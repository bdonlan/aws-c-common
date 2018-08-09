/*
 * Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/common/event.h>
#include <aws/common/condition_variable.h>

struct aws_event_wait_entry {
    struct aws_linked_list_node node;
    struct aws_condition_variable cvar;
    bool awoken;
};

static bool wake_predicate(void *pentry) {
    struct aws_event_wait_entry *entry = (struct aws_event_wait_entry *)pentry;

    return entry->awoken;
}

static int start_waiting(struct aws_event *ev, struct aws_event_wait_entry *entry) {
    assert(ev->state == 2);

    if (ev->wait_head.next == NULL) {
        ev->wait_head.next = ev->wait_head.prev = &ev->wait_head;
    }

    entry->node.next = entry->node.prev = &entry->node;

    if (aws_condition_variable_init(&entry->cvar)) return AWS_OP_ERR;

    entry->awoken = false;
    aws_linked_list_insert_after(&ev->wait_head, &entry->node);

    return AWS_OP_SUCCESS;
}

static void stop_waiting(struct aws_event *ev, struct aws_event_wait_entry *entry) {
    entry->node.prev->next = entry->node.next;
    entry->node.next->prev = entry->node.prev;

    aws_condition_variable_clean_up(&entry->cvar);
}

static int awaken_one(struct aws_event *ev) {
    assert(ev->wait_head.next != &ev->wait_head);

    struct aws_event_wait_entry *entry = AWS_CONTAINER_OF(ev->wait_head.next, struct aws_event_wait_entry, node);

    if (aws_condition_variable_notify_one(&entry->cvar)) return AWS_OP_ERR;

    entry->awoken = true;
    aws_linked_list_remove(&entry->node);

    /* Make the unlink operation in stop_waiting a no-op */
    entry->node.next = entry->node.prev = &entry->node;

    return AWS_OP_SUCCESS;
}

static int awaken_all(struct aws_event *ev) {
    /* 
     * We'll notify all the cvars, then after that set the awoken flag and tear down the list;
     * this way, if any notify fails, we can avoid any visible effects.
     */
    for (struct aws_linked_list_node *node = ev->wait_head.next; node != &ev->wait_head; node = node->next) {
        struct aws_event_wait_entry *entry = AWS_CONTAINER_OF(ev->wait_head.next, struct aws_event_wait_entry, node);

        if (aws_condition_variable_notify_all(&entry->cvar)) return AWS_OP_ERR;
    }

    for (struct aws_linked_list_node *node = ev->wait_head.next, *next = node->next; node != &ev->wait_head; node = next, next = node->next) {
        struct aws_event_wait_entry *entry = AWS_CONTAINER_OF(ev->wait_head.next, struct aws_event_wait_entry, node);

        entry->awoken = true;
        node->next = node->prev = node;
    }

    ev->wait_head.next = ev->wait_head.prev = &ev->wait_head;

    return AWS_OP_SUCCESS;
}

AWS_COMMON_API
int aws_event_init(struct aws_event *ev, bool autoreset, bool signalled) {
    ev->state = !!signalled;
    ev->autoreset = autoreset;
    ev->wait_head.next = ev->wait_head.prev = NULL;
    return aws_mutex_init(&ev->mutex);
}

AWS_COMMON_API
int aws_event_signal_priv_slowpath(struct aws_event *ev) {
    if (aws_mutex_lock(&ev->mutex)) return AWS_OP_ERR;

    int result = AWS_OP_ERR;
    bool autoreset = ev->autoreset;

    unsigned int curval = ev->state;
    unsigned int newval;

    do {
        switch (curval) {
            case 0:
                newval = 1;
                break;
            case 1:
                return true;
            case 2:
                if (autoreset) {
                    if (awaken_one(ev)) goto out;
                    newval = ev->wait_head.next ? 2 : 0;
                } else {
                    if (awaken_all(ev)) goto out;
                    newval = 1;
                }
                result = AWS_OP_SUCCESS;
                goto out;
            default:
                break;
        }
    } while (!aws_atomic_compare_exchange(&ev->state, &curval, newval));

    result = AWS_OP_SUCCESS;

out:
    aws_mutex_unlock(&ev->mutex);
    return result;
}

AWS_COMMON_API
bool aws_event_wait(struct aws_event *ev, int64_t time_to_wait) {
    /* Fast path: If it's already signalled, we might not need to lock anything */
    bool autoreset = ev->autoreset;
    bool result = false;

    unsigned int curval = ev->state;
    unsigned int newval;

    do {
        switch (curval) {
            case 0:
                goto slowpath;
            case 1:
                if (autoreset) {
                    newval = 0;
                    break;
                } else {
                    /* Don't need to change the value */
                    return true;
                }
            case 2:
                goto slowpath;
            default:
                abort();
        }
    } while (!aws_atomic_compare_exchange(&ev->state, &curval, newval));

    /* We transitioned 1->0, we're signalled and done */
    return true;

slowpath:
    /* We need to sleep, so we'll need the mutex */
    aws_mutex_lock(&ev->mutex);

    do {
        switch (curval) {
            case 0:
                newval = 2;
                break;
            case 1:
                if (autoreset) {
                    newval = 0;
                } else {
                    /* Already signalled with no autoreset */
                    result = true;
                    goto done_looping;
                }
                break;
            case 2:
                newval = 2;
                goto done_looping;
            default:
                break;
        }
    } while (!aws_atomic_compare_exchange(&ev->state, &curval, newval));

done_looping:
    if (curval == 1) {
        /* Signalled, and autoreset. */
        result = true;
        goto out;
    } else {
        assert(newval == 2);

        struct aws_event_wait_entry entry;
        if (start_waiting(ev, &entry)) {
            /* Initialization failed? */
            goto out;
        }

        aws_condition_variable_wait_for_pred(
            &entry.cvar,
            &ev->mutex,
            time_to_wait,
            wake_predicate,
            &entry
        );

        /* If we were signalled, awoken should be true now */
        result = entry.awoken;

        stop_waiting(ev, &entry);
    }

out:
    /* 
     * We have the mutex held, so we're allowed to transition
     * 2->0 without a CAS (any other mutators need the mutex to change it)
     */
    if (ev->wait_head.next == &ev->wait_head && ev->state == 2) {
        ev->state = 0;
    }

    aws_mutex_unlock(&ev->mutex);

    return result;
}
