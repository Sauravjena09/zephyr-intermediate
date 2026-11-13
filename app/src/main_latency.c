/*
 * Zephyr Course: Intermediate - L6-TASK1: Latency Investigation and Tracing
 *
 * WORKLOAD ARCHITECTURE & LATENCY GUARANTEE:
 * ------------------------------------------
 * 1. Timer expiry (event_timer_expiry) fires every 250 ms (EVENT_PERIOD_MS).
 *    - Posts control_event (seq, ready_ms = k_uptime_get_32()) into control_queue.
 *    - Unblocks maintenance thread via k_sem_give(&maintenance_start).
 *    - Emits application trace event: "event_ready".
 *
 * 2. 10 ms Start-Latency Guarantee:
 *    - Start Latency = start_time (when control thread begins running) - ready_ms (when event was generated).
 *    - Guarantee: Start latency must NOT exceed 10 ms (DEADLINE_MS = 10).
 *    - Distinguishing start latency from full processing time:
 *        * Start latency: time spent waiting in READY state before scheduler dispatch.
 *        * Full processing time: execution time of control algorithm (done_time - start_time).
 *        * Total response time: done_time - ready_ms = start_latency + processing_time.
 *    - Emits trace events: "control_start" (start event) and "control_done" (end event).
 *
 * 3. Failing vs Corrected Priority Assignment:
 *    - In Zephyr RTOS, lower numerical priority value = HIGHER execution priority.
 *    - FAILING MODE (#define FIX_SCHEDULING_PRIORITY 0):
 *        * CONTROL_PRIORITY = 7 (lower priority)
 *        * MAINTENANCE_PRIORITY = 4 (higher priority)
 *        * Root Cause: When timer fires, both threads become ready. Scheduler picks
 *          maintenance (prio 4), which runs k_busy_wait(45000) for 45 ms.
 *          Control thread is starved for 45 ms, causing consistent 10 ms deadline misses!
 *    - CORRECTED MODE (#define FIX_SCHEDULING_PRIORITY 1):
 *        * CONTROL_PRIORITY = 3 (higher priority)
 *        * MAINTENANCE_PRIORITY = 7 (lower priority)
 *        * Result: When timer fires, control thread runs immediately (latency < 1 ms).
 *          Maintenance runs afterwards. 0 deadline misses!
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/tracing/tracing.h>

LOG_MODULE_REGISTER(l6_latency, LOG_LEVEL_INF);

#define STACK_SIZE               2048
#define EVENT_PERIOD_MS           250
#define MAINTENANCE_LOAD_US     45000   /* 45 ms maintenance CPU load */
#define DEADLINE_MS                10   /* 10 ms start-latency guarantee */

/* Set to 1 to apply the scheduling priority correction */
#ifndef FIX_SCHEDULING_PRIORITY
#define FIX_SCHEDULING_PRIORITY     1
#endif

#if FIX_SCHEDULING_PRIORITY
#define CONTROL_PRIORITY            3   /* High priority: runs before maintenance */
#define MAINTENANCE_PRIORITY        7   /* Low priority: background workload */
#else
#define CONTROL_PRIORITY            7   /* Low priority: preempted/delayed by maintenance */
#define MAINTENANCE_PRIORITY        4   /* High priority: causes 45 ms delay */
#endif

struct control_event {
    uint32_t seq;
    uint32_t ready_ms;
};

K_MSGQ_DEFINE(control_queue, sizeof(struct control_event), 4, 4);
K_SEM_DEFINE(maintenance_start, 0, 1);

/* ================================================================== */
/*  Timer Expiry: generates control event and triggers maintenance    */
/* ================================================================== */

static void event_timer_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    static uint32_t seq = 0;
    struct control_event event = {
        .seq = seq++,
        .ready_ms = k_uptime_get_32(),
    };

    /* Timer expiry runs in ISR context; never wait */
    int ret = k_msgq_put(&control_queue, &event, K_NO_WAIT);
    if (ret != 0) {
        LOG_WRN("[TIMER] Queue full, dropped seq=%u", event.seq);
        return;
    }

    /* Trigger background maintenance */
    k_sem_give(&maintenance_start);

    /* Application trace event for event availability */
    sys_trace_named_event("event_ready", event.seq, k_msgq_num_used_get(&control_queue));
}

K_TIMER_DEFINE(event_timer, event_timer_expiry, NULL);

/* ================================================================== */
/*  Control Thread: latency measurement & deadline monitoring        */
/* ================================================================== */

static void control_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "control");

    uint32_t deadline_miss_count = 0;
    uint32_t total_events = 0;

    LOG_INF("[CONTROL] Thread started (Priority=%d, Start-Latency Deadline=%d ms)",
            k_thread_priority_get(k_current_get()), DEADLINE_MS);

    while (true) {
        struct control_event event;
        int ret = k_msgq_get(&control_queue, &event, K_FOREVER);
        if (ret != 0) {
            LOG_ERR("[CONTROL] receive failed: %d", ret);
            continue;
        }

        /* START EVENT: Marks the instant control work actually begins executing */
        uint32_t start_time = k_uptime_get_32();
        uint32_t start_latency_ms = start_time - event.ready_ms;
        total_events++;

        sys_trace_named_event("control_start", event.seq, start_latency_ms);

        /* Evaluate start-latency guarantee */
        if (start_latency_ms > DEADLINE_MS) {
            deadline_miss_count++;
            LOG_WRN_RATELIMIT("[CONTROL] DEADLINE MISS: seq=%u start_latency=%u ms (> %u ms) misses=%u/%u",
                              event.seq, start_latency_ms, DEADLINE_MS,
                              deadline_miss_count, total_events);
        } else {
            LOG_INF("[CONTROL] OK: seq=%u start_latency=%u ms (<= %u ms guarantee) total=%u",
                    event.seq, start_latency_ms, DEADLINE_MS, total_events);
        }

        /* Full processing time: simulated control algorithm execution */
        k_busy_wait(1500); /* 1.5 ms processing load */

        /* END EVENT: Marks completion of full control processing */
        uint32_t done_time = k_uptime_get_32();
        uint32_t processing_time_ms = done_time - start_time;
        uint32_t total_response_ms = done_time - event.ready_ms;

        sys_trace_named_event("control_done", event.seq, processing_time_ms);

        LOG_DBG("[CONTROL] seq=%u done: proc=%u ms total_resp=%u ms",
                event.seq, processing_time_ms, total_response_ms);
    }
}

/* ================================================================== */
/*  Maintenance Thread: background CPU-bound maintenance workload    */
/* ================================================================== */

static void maintenance_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "maintenance");

    LOG_INF("[MAINTENANCE] Thread started (Priority=%d, Load=%u ms)",
            k_thread_priority_get(k_current_get()), MAINTENANCE_LOAD_US / 1000U);

    while (true) {
        k_sem_take(&maintenance_start, K_FOREVER);

        /* Simulates non-urgent but CPU-intensive maintenance workload */
        k_busy_wait(MAINTENANCE_LOAD_US);
    }
}

K_THREAD_DEFINE(control_tid, STACK_SIZE, control_fn,
                NULL, NULL, NULL, CONTROL_PRIORITY, 0, 0);

K_THREAD_DEFINE(maintenance_tid, STACK_SIZE, maintenance_fn,
                NULL, NULL, NULL, MAINTENANCE_PRIORITY, 0, 0);

int main(void)
{
    LOG_INF("=================================================");
    LOG_INF("=== L6-TASK1: Latency Investigation & Tracing ===");
    LOG_INF("Configuration: %s",
            FIX_SCHEDULING_PRIORITY ? "CORRECTED (Priority Fix Applied)" : "FAILING (Original Workload)");
    LOG_INF("Control Priority:     %d", CONTROL_PRIORITY);
    LOG_INF("Maintenance Priority: %d", MAINTENANCE_PRIORITY);
    LOG_INF("Guarantee: Control start latency <= %d ms", DEADLINE_MS);
    LOG_INF("Maintenance Load:  %u ms", MAINTENANCE_LOAD_US / 1000U);
    LOG_INF("=================================================");

    k_timer_start(&event_timer, K_MSEC(500), K_MSEC(EVENT_PERIOD_MS));

    return 0;
}
