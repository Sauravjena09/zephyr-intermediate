#include<zephyr/kernel.h>
#include<zephyr/logging/log.h>
#include<zephyr/zbus/zbus.h>


LOG_MODULE_REGISTER(task4,LOG_LEVEL_INF);

#define STACK_SIZE        2048U
#define SENSOR_PERIOD_MS   100U
#define SENSOR_COUNT       18
#define TEMP_ALARM_MC   27000

struct sensor_data{
    int32_t temperature_mc;
    int32_t timestamp_ms;
};

static void display_listener_cb(const struct zbus_channel *chan);
/*listener*/
ZBUS_LISTENER_DEFINE(display_lis,display_listener_cb);
/*for the slow logging*/
ZBUS_SUBSCRIBER_DEFINE(alarm_sub,9);
ZBUS_CHAN_DEFINE(sensor_chan,
    struct sensor_data,
    NULL,
    NULL,
    ZBUS_OBSERVERS(display_lis,alarm_sub),
    ZBUS_MSG_INIT(.temperature_mc=0,
        .timestamp_ms=0,
    ));
static void display_listener_cb(const struct zbus_channel *chan){
    const struct sensor_data *msg= (const struct sensor_data *)zbus_chan_const_msg(chan);

   LOG_INF("[DISPLAY-LIS] thread=%s  temp=%d mC",
            k_thread_name_get(k_current_get()),
            msg->temperature_mc);
    
}

static void sensor_thread_fn(void *p1,void *p2,void *p3){
    ARG_UNUSED(p1);ARG_UNUSED(p2);ARG_UNUSED(p3);
    k_thread_name_set(k_current_get(),"sensor");
 for (int i = 0; i < SENSOR_COUNT; i++) {
        struct sensor_data data = {
            .temperature_mc = 24000 + (i * 350),
            .timestamp_ms = k_uptime_get_32(),
        };

        LOG_INF("[SENSOR] publish  temp=%d mC",
                data.temperature_mc);

        int ret = zbus_chan_pub(&sensor_chan, &data, K_MSEC(100));
        if (ret != 0) {
            LOG_WRN("[SENSOR] publish failed ret=%d", ret);
        }

        k_msleep(SENSOR_PERIOD_MS);
    }

    LOG_INF("[SENSOR] done");
}

/*sensor alarm**/

static void alarm_thread_fn(void *p1,void *p2,void *p3){
    ARG_UNUSED(p1);ARG_UNUSED(p2);ARG_UNUSED(p3);
    k_thread_name_set(k_current_get(),"alarm");
    const struct zbus_channel*chan=NULL;
    int alarms=0;
    while(true){
      int ret = zbus_sub_wait(&alarm_sub, &chan, K_MSEC(3000));
        if (ret != 0) {
            LOG_INF("[ALARM-SUB] timeout, done");
            break;
        }

        struct sensor_data msg;

        ret = zbus_chan_read(chan, &msg, K_MSEC(100));
        if (ret != 0) {
            LOG_WRN("[ALARM-SUB] read failed ret=%d", ret);
            continue;
        }

        if (msg.temperature_mc >= TEMP_ALARM_MC) {
            alarms++;

            LOG_WRN("[ALARM-SUB] HIGH TEMP  temp=%d mC alarms=%d",
                    msg.temperature_mc,
                    alarms);
        } else {
            LOG_INF("[ALARM-SUB] ok temp=%d mC",
                    msg.temperature_mc);
        }  
      }
}
K_THREAD_DEFINE(sensor_thread, STACK_SIZE, sensor_thread_fn,
                NULL, NULL, NULL, 5, 0, 0);


K_THREAD_DEFINE(alarm_thread, STACK_SIZE, alarm_thread_fn,
                NULL, NULL, NULL, 6, 0, 0);
                int main(void)
{
    LOG_INF("=== L4 Demo 2: Zbus Pub-Sub ===");
    LOG_INF("sensor publishes every %dms", SENSOR_PERIOD_MS);
    LOG_INF("display listener runs in publisher context");
    LOG_INF("logger uses message subscriber copies");
    LOG_INF("alarm uses a regular subscriber");
    LOG_INF("alarm threshold: %d mC", TEMP_ALARM_MC);

    return 0;
}