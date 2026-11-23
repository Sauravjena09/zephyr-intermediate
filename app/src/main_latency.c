/*
 * Zephyr Course: Intermediate - L6-TASK1: Latency Investigation and Tracing
 *
 * 10 ms Start-Latency Guarantee:
 * - Start Latency: Elapsed time from when an event is queued by event_timer_expiry
 *   (ready_ms) until the control thread wakes up and begins execution (start_time).
 * - Guarantee: Start latency must not exceed 10 ms (DEADLINE_MS = 10).
 * - Full Processing Time: Duration of the control algorithm itself (done_time - start_time).
 * - Events:
 *     * "event_ready"  : emitted on timer expiry when control_event is posted.
 *     * "control_start": start event, emitted when control thread begins execution.
 *     * "control_done" : end event, emitted when control processing completes.
 *
 * Scheduling Root Cause & Fix:
 * - Original (Failing): CONTROL_PRIORITY = 7, MAINTENANCE_PRIORITY = 4.
 *   Since 4 < 7 in Zephyr, maintenance preempts/runs ahead of control when both become
 *   ready on timer expiry. Maintenance runs k_busy_wait(45000), causing control to start
 *   ~45 ms late and consistently violate the 10 ms guarantee.
 * - Corrected (Fixed): CONTROL_PRIORITY = 3, MAINTENANCE_PRIORITY = 7.
 *   Control runs immediately (start latency 0-1 ms <= 10 ms), and maintenance executes
 *   during subsequent idle time. 0 deadline misses.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/tracing/tracing.h>

LOG_MODULE_REGISTER(homework, LOG_LEVEL_INF);

#define STACK_SIZE            2048
#define EVENT_PERIOD_MS        250
#define MAINTENANCE_LOAD_US  45000
#define DEADLINE_MS             10

/* Set to 1 for the corrected priority assignment, 0 for original failing workload */
#ifndef FIX_SCHEDULING_PRIORITY
#define FIX_SCHEDULING_PRIORITY 1
#endif

#if FIX_SCHEDULING_PRIORITY
#define CONTROL_PRIORITY         3
#define MAINTENANCE_PRIORITY     7
#else
#define CONTROL_PRIORITY         7
#define MAINTENANCE_PRIORITY     4
#endif

struct control_event {
    uint32_t seq;
    uint32_t ready_ms;
};

K_MSGQ_DEFINE(control_queue, sizeof(struct control_event), 4, 4);
K_SEM_DEFINE(maintenance_start, 0, 1);

/* ================================================================== */
/*  Timer expiry: creates one control event                           */
/* ================================================================== */

static void event_timer_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    static uint32_t seq;
    struct control_event event = {
        .seq = seq++,
        .ready_ms = k_uptime_get_32(),
    };

    /* Timer expiry runs in interrupt context, so never wait here. */
    int ret = k_msgq_put(&control_queue, &event, K_NO_WAIT);

    if (ret != 0) {
        return;
    }

    /* Both threads become ready when the timer interrupt returns. */
    k_sem_give(&maintenance_start);

    /* Application trace event for event availability */
    sys_trace_named_event("event_ready", event.seq, k_msgq_num_used_get(&control_queue));
}

K_TIMER_DEFINE(event_timer, event_timer_expiry, NULL);

/* ================================================================== */
/*  Control thread                                                   */
/* ================================================================== */

static void control_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "control");

    uint32_t deadline_misses = 0;
    uint32_t total_events = 0;

    while (true) {
        struct control_event event;
        int ret = k_msgq_get(&control_queue, &event, K_FOREVER);

        if (ret != 0) {
            LOG_ERR("[CONTROL] receive failed: %d", ret);
            continue;
        }

        /* Start event: records the instant control execution begins */
        uint32_t start_time = k_uptime_get_32();
        uint32_t start_latency = start_time - event.ready_ms;
        total_events++;

        sys_trace_named_event("control_start", event.seq, start_latency);

        /* Evaluate 10 ms start-latency guarantee */
        if (start_latency > DEADLINE_MS) {
            deadline_misses++;
            LOG_WRN_RATELIMIT("[CONTROL] DEADLINE MISS: seq=%u start_latency=%u ms (> %u ms guarantee) total_misses=%u",
                              event.seq, start_latency, DEADLINE_MS, deadline_misses);
        } else {
            LOG_INF("[CONTROL] OK: seq=%u start_latency=%u ms (<= %u ms guarantee) total=%u",
                    event.seq, start_latency, DEADLINE_MS, total_events);
        }

        /* Full processing workload: simulated control processing */
        k_busy_wait(1000);

        uint32_t done_time = k_uptime_get_32();
        uint32_t processing_time = done_time - start_time;

        /* End event: records completion of control processing */
        sys_trace_named_event("control_done", event.seq, processing_time);

        LOG_INF("[CONTROL] processed seq=%u", event.seq);
    }
}

/* ================================================================== */
/*  Background maintenance thread                                    */
/* ================================================================== */

static void maintenance_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "maintenance");

    while (true) {
        k_sem_take(&maintenance_start, K_FOREVER);

        /* This work is important, but it has no short deadline. */
        k_busy_wait(MAINTENANCE_LOAD_US);
    }
}

K_THREAD_DEFINE(control, STACK_SIZE, control_fn,
                NULL, NULL, NULL, CONTROL_PRIORITY, 0, 0);

K_THREAD_DEFINE(maintenance, STACK_SIZE, maintenance_fn,
                NULL, NULL, NULL, MAINTENANCE_PRIORITY, 0, 0);

int main(void)
{
    LOG_INF("=== L6 Homework: Runtime Investigation ===");
    LOG_INF("Control work must start within 10 ms");
    LOG_INF("Inspect, measure, trace, explain, and correct the delay");

    k_timer_start(&event_timer, K_MSEC(500), K_MSEC(EVENT_PERIOD_MS));

    return 0;
}
