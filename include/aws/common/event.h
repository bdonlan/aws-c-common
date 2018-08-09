#ifndef AWS_COMMON_EVENT_H
#define AWS_COMMON_EVENT_H

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

#include <stdlib.h>

#include <aws/common/exports.h>
#include <aws/common/common.h>
#include <aws/common/mutex.h>
#include <aws/common/linked_list.h>

struct aws_event;

/**
 * Creates an event object. An event object is a thread synchronization object
 * that contains a single boolean flag; threads can await the flag becoming true.
 * Optionally, if autoreset is set, the flag can be reset when a thread successfully
 * waits for the event.
 *
 * When autoreset is on, and the event is signalled while being waited upon, the wakeup
 * and reset happens atomically with the signal. This means that we do not lose wakeups
 * if multiple signals happen in short succession while threads are waiting. However, the
 * flag is not a counter; if multiple signals occur while autoreset is on and no threads
 * are waiting, the flag is signalled only once.
 *
 * Arguments:
 * @param ev        - a pointer to the event to initialize
 * @param autoreset - true to reset the event when waited on
 * @param signalled - the initial state of the event flag
 */
AWS_COMMON_API
int aws_event_init(struct aws_event *ev, bool autoreset, bool signalled);

/*
 * Implementation notes:
 *
 * This event object combines a state value with a linked list of waiting threads.
 * Designs that used a single counter to convey both the presence/absence of waiters
 * and the current state were considered, but I was unable to find a design that would
 * both avoid a thundering herd of threads on wakeup of autoreset events, and also avoid
 * losing events when a wait timeout raced with a signal. As such, it uses a linked list
 * to precisely track which threads are waiting, and which is to receive the autoreset
 * wakeup. This design has the nice side effect of providing fairness as well.
 *
 * Wait entries (see event.c) contain a flag that indicates whether the thread has been
 * awoken. If a thread times out on its wait, it will stick check its flag (holding the
 * mutex) before returning; if it was in fact signalled, it will discard the timeout and
 * report a successful wakeup instead.
 *
 * The actual wait entries are allocated on the stack of the thread that is waiting,
 * so heap allocation is not required in the critical path.
 *
 * Note that this design means that when we have a non-autoreset event we have to iterate
 * through the waiter list on the signalling thread - but we'd probably have to do that 
 * in the kernel somewhere anyway. In the future we could perhaps optimize this to use a
 * different design for the non-autoreset case instead.
 *
 * This header has inlined variants of the signal/reset/is_signalled functions that
 * are intended to avoid expensive work when the event is not currently being waited
 * upon.
 */

struct aws_event {
    /*
     *   0 - unsignalled
     *   1 - signalled, no waiters
     *   2 - unsignalled, threads waiting
     *
     * Transitions to/from 2 must occur with the mutex held.
     */
    volatile unsigned int state;
    bool autoreset;
    struct aws_linked_list_node wait_head;
    struct aws_mutex mutex;
};

#define AWS_EVENT_PRIV_OS_INIT \
    { .mutex = PTHREAD_MUTEX_INITIALIZER }

#define AWS_EVENT_INIT(autoreset_v, signalled)  \
    { .state        = !!(signalled),            \
      .autoreset    = !!(autoreset_v),          \
      .wait_head    = { NULL, NULL }            \
      .mutex        = AWS_MUTEX_INIT            \
    }

static inline bool aws_atomic_compare_exchange(volatile unsigned int *pval, unsigned int *expect, unsigned int newval) {
    bool success;
#ifdef _WIN32
    unsigned int initial = InterlockedCompareExchange(pval, newval, *expect);
    success = (*expect == initial);
    *expect = initial;
#else
    success = __atomic_compare_exchange_n(pval, expect, newval, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
#endif
    return success;
}

AWS_COMMON_API
int aws_event_signal_priv_slowpath(struct aws_event *ev);

static inline AWS_COMMON_API
void aws_event_clean_up(struct aws_event *ev) {
    assert(ev->state != 2);
    aws_mutex_clean_up(&ev->mutex);
}

AWS_COMMON_API
bool aws_event_wait(struct aws_event *ev, int64_t time_to_wait);

static inline AWS_COMMON_API
int aws_event_signal(struct aws_event *ev) {
    unsigned int curval = ev->state;
    unsigned int newval;

    do {
        switch (curval) {
            case 0:
                newval = 1;
                break;
            case 1:
                return AWS_OP_SUCCESS;
            case 2:
                return aws_event_signal_priv_slowpath(ev);
            default:
                abort();
        }
    } while (!aws_atomic_compare_exchange(&ev->state, &curval, newval));

    return AWS_OP_SUCCESS;
}

static inline AWS_COMMON_API
void aws_event_reset(struct aws_event *ev) {
    unsigned int curval = ev->state;
    unsigned int newval = 0;

    do {
        if (curval != 1) {
            return; // already unsignalled
        }
    } while (!aws_atomic_compare_exchange(&ev->state, &curval, newval));
}

static inline AWS_COMMON_API
bool aws_event_is_signalled(const struct aws_event *ev) {
    return ev->state == 1;
}

#endif /* AWS_COMMON_EVENT_H */
