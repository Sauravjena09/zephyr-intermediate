

#include <zephyr/kernel.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/tracing/tracing.h>




LOG_MODULE_REGISTER(pipeline_test, LOG_LEVEL_INF); /* Added semicolon */

#define QUEUE_SIZE 8
#define WDT_TIMEOUT_MS 2000
#define DEADLINE_MS 50

struct pipeline_data {
    int value;
    uint32_t timestamps_ms;
};
K_MSGQ_DEFINE(consumer_queue, sizeof(struct pipeline_data), QUEUE_SIZE, 4);

void zbus_listener_callback(const struct zbus_channel *chan) {
    /* Bug 1 Fix: use zbus_chan_const_msg() directly — avoids corrupting the
     * pointer by passing &received_data (address of a pointer) into
     * zbus_chan_read(), which overwrote it with raw bytes causing
     * garbage latency (281162 ms) and corrupted SEQ numbers. */
    const struct pipeline_data *msg = zbus_chan_const_msg(chan);
    if (k_msgq_put(&consumer_queue, msg, K_NO_WAIT) != 0) {
        LOG_WRN("Consumer Inbox FULL! Dropping data: %d", msg->value);
    }
}
ZBUS_LISTENER_DEFINE(pipeline_listener, zbus_listener_callback);

ZBUS_CHAN_DEFINE(sensor_data_chan,
                 struct pipeline_data,
                 NULL,
                 NULL,
                 ZBUS_OBSERVERS(pipeline_listener), /* Fixed: Added 'S' */
                 ZBUS_MSG_INIT(0)
);

void wdt_callback(int channel_id, void *user_data) {
    LOG_ERR("FATAL: WATCHDOG TIMER EXPIRED, CHANNEL ID %d is unresponsive", channel_id);
    LOG_ERR("SYSTEM WILL NOW HARDWARE REBOOT");
}

void producer_thread(void) {
    struct pipeline_data data = { .value = 0,.timestamps_ms=0 };
    while(1) {
        data.timestamps_ms=k_uptime_get_32();
        zbus_chan_pub(&sensor_data_chan, &data, K_NO_WAIT);
        LOG_INF("event_ready seq= %d ,queue =%u", data.value,k_msgq_num_used_get(&consumer_queue));
        sys_trace_named_event("event_ready",data.value,k_msgq_num_used_get(&consumer_queue));
        data.value++;
        k_msleep(200);
    }
}

void consumer_thread(void) {
    struct pipeline_data data_in={0}; /* Fixed: struct typo */
    int process_count = 0;
    static uint32_t deadline_miss=0;

    int wdt_channel = task_wdt_add(WDT_TIMEOUT_MS, wdt_callback, NULL);
    if(wdt_channel < 0) {
        LOG_ERR("FAILED TO ADD TASK WDT CHANNEL");
        return;
    }
    
  while (1) {
        if (k_msgq_get(&consumer_queue, &data_in, K_MSEC(100)) == 0) {
            uint32_t latency_ms = k_uptime_get_32() - data_in.timestamps_ms;

            if (latency_ms > DEADLINE_MS) {
                deadline_miss++;
                LOG_WRN_RATELIMIT("deadline_miss count=%u, SEQ=%d, latency_ms=%u",
                                  deadline_miss, data_in.value, latency_ms);
            } else {
                LOG_INF("event_done seq=%d latency_ms=%u result=OK", 
                        data_in.value, latency_ms);
                        sys_trace_named_event("event_done", data_in.value, latency_ms);
                process_count++;
            }
        }

        /* Feed the watchdog to prove this thread is not frozen */
        task_wdt_feed(wdt_channel);

        /* SIMULATE A STUCK CONSUMER */
        if (process_count == 600) {
            LOG_WRN("Simulating a catastrophic consumer freeze...");
            k_msleep(10000); 
        }
    }
}

void health_thread(void) {
    while(1) {
        uint32_t used = k_msgq_num_used_get(&consumer_queue);
        if (used >= (QUEUE_SIZE * 3 / 4)) {
            LOG_WRN("QUEUE CAPACITY IS CRITICAL %d filled", used);
        }
        k_msleep(500);
    }
}

K_THREAD_DEFINE(prod_id, 1024, (k_thread_entry_t)producer_thread, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(cons_id, 1024, (k_thread_entry_t)consumer_thread, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(hlth_id, 1024, (k_thread_entry_t)health_thread, NULL, NULL, NULL, 4, 0, 0);

int main(void) {
    LOG_INF("STARTING THE ZBUS pipeline & watchdog test");
    const struct device *hw_wdt = DEVICE_DT_GET(DT_NODELABEL(iwdg));
    k_thread_name_set(prod_id, "producer");
    k_thread_name_set(cons_id, "consumer");
    k_thread_name_set(hlth_id, "health");
    if(!device_is_ready(hw_wdt)) {
        LOG_WRN("HARDWARE NOT READY, RUNNING TASK WDT IN SOFTWARE ONLY MODE");
        task_wdt_init(NULL);
    } else {
        task_wdt_init(hw_wdt);
    }
    return 0;
}