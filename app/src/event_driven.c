/*
 * Zephyr Course: Intermediate - L4-TASK1: zbus listener and subscriber
 *
 * Architecture:
 * - Channel: sensor_chan carrying struct sensor_msg
 * - Publisher: sensor_thread publishes simulated sensor samples every 100 ms
 * - Listener: display_lis performs immediate display update in the publisher's context
 * - Subscriber: logger_sub thread uses zbus_sub_wait + zbus_chan_read for slower logging
 * - Observers: Both display_lis and logger_sub are registered via ZBUS_OBSERVERS on sensor_chan
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

LOG_MODULE_REGISTER(l4_task1, LOG_LEVEL_INF);

#define STACK_SIZE         2048
#define SENSOR_PERIOD_MS    100
#define SENSOR_SAMPLE_COUNT  20
#define PUBLISHER_PRIORITY    5
#define SUBSCRIBER_PRIORITY   6

/* Sensor message structure */
struct sensor_msg {
    uint32_t timestamp_ms;
    int32_t temperature_mc;
    uint8_t seq;
};

/* Forward declaration for listener callback */
static void display_listener_cb(const struct zbus_channel *chan);

/* Fast display listener: executes synchronously in publisher's context */
ZBUS_LISTENER_DEFINE(display_lis, display_listener_cb);

/* Slower logging subscriber: uses message queue to unblock subscriber thread */
ZBUS_SUBSCRIBER_DEFINE(logger_sub, 8);

/*
 * ZBUS Channel Definition:
 * Carries struct sensor_msg with both display_lis and logger_sub observers
 */
ZBUS_CHAN_DEFINE(sensor_chan,
                 struct sensor_msg,
                 NULL,
                 NULL,
                 ZBUS_OBSERVERS(display_lis, logger_sub),
                 ZBUS_MSG_INIT(.timestamp_ms = 0,
                               .temperature_mc = 0,
                               .seq = 0));

/* Listener callback: runs in publisher thread context for fast notification */
static void display_listener_cb(const struct zbus_channel *chan)
{
    const struct sensor_msg *msg = zbus_chan_const_msg(chan);

    LOG_INF("[DISPLAY-LIS] (in %s context) seq=%u temp=%d mC",
            k_thread_name_get(k_current_get()),
            msg->seq,
            msg->temperature_mc);
}

/* Publisher thread: publishes sensor readings every 100 ms */
static void publisher_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "publisher");

    LOG_INF("[PUBLISHER] Thread started, publishing %d samples every %d ms",
            SENSOR_SAMPLE_COUNT, SENSOR_PERIOD_MS);

    for (uint8_t i = 0; i < SENSOR_SAMPLE_COUNT; i++) {
        struct sensor_msg sample = {
            .timestamp_ms = k_uptime_get_32(),
            .temperature_mc = 22000 + (i * 450), /* Simulated temp: 22.0°C to 30.55°C */
            .seq = i,
        };

        LOG_INF("[PUBLISHER] Publishing sample seq=%u temp=%d mC",
                sample.seq, sample.temperature_mc);

        int ret = zbus_chan_pub(&sensor_chan, &sample, K_MSEC(100));
        if (ret != 0) {
            LOG_WRN("[PUBLISHER] zbus_chan_pub failed: %d", ret);
        }

        k_msleep(SENSOR_PERIOD_MS);
    }

    LOG_INF("[PUBLISHER] All samples published, publisher done");
}

/* Subscriber thread: performs slower decoupled logging */
static void subscriber_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "logger");

    LOG_INF("[LOGGER-SUB] Thread started, waiting for channel events");

    const struct zbus_channel *chan = NULL;

    while (true) {
        /* Wait for notification on logger_sub */
        int ret = zbus_sub_wait(&logger_sub, &chan, K_MSEC(2000));
        if (ret != 0) {
            LOG_INF("[LOGGER-SUB] Timeout waiting for samples, subscriber completed");
            break;
        }

        /* Read the message from the channel */
        struct sensor_msg msg;
        ret = zbus_chan_read(chan, &msg, K_MSEC(100));
        if (ret != 0) {
            LOG_WRN("[LOGGER-SUB] zbus_chan_read failed: %d", ret);
            continue;
        }

        uint32_t latency_ms = k_uptime_get_32() - msg.timestamp_ms;
        LOG_INF("[LOGGER-SUB] (in %s context) seq=%u temp=%d mC latency=%u ms",
                k_thread_name_get(k_current_get()),
                msg.seq,
                msg.temperature_mc,
                latency_ms);

        /* Simulate slower logging I/O (e.g. disk or serial flash write) */
        k_msleep(180);
    }

    LOG_INF("[LOGGER-SUB] Logging thread exited cleanly");
}

/* Thread definitions */
K_THREAD_DEFINE(pub_tid, STACK_SIZE, publisher_thread_fn,
                NULL, NULL, NULL, PUBLISHER_PRIORITY, 0, 0);

K_THREAD_DEFINE(sub_tid, STACK_SIZE, subscriber_thread_fn,
                NULL, NULL, NULL, SUBSCRIBER_PRIORITY, 0, 0);

int main(void)
{
    LOG_INF("=================================================");
    LOG_INF("=== L4-TASK1: zbus Listener and Subscriber ===");
    LOG_INF("Publisher: publishes simulated sensor every %d ms", SENSOR_PERIOD_MS);
    LOG_INF("Listener: fast simulated display in publisher context");
    LOG_INF("Subscriber: slow decoupled logging via zbus_sub_wait");
    LOG_INF("=================================================");

    return 0;
}
