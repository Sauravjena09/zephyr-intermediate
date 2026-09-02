#include<zephyr/kernel.h>
#include<zephyr/logging/log.h>

LOG_MODULE_REGISTER(task3_work1,LOG_LEVEL_INF);
#define STACK_SIZE 1024U
#define SENSOR_MS 100U

static void sensor_work_handler(struct k_work *work){
    LOG_INF("WORK HANDLER EXCUTED,EVENT HAPPEND");
}
K_WORK_DEFINE(sensor_work,sensor_work_handler);
static void sensor_sim_fn(void *p1,void *p2,void *p3){
    while(1){
        LOG_INF("SENSOR FIRED :SUMMBITING THE WORK TO THE SYSTEM WORKQUEUE");
        k_work_submit(&sensor_work);
        /*
        k_work_reschedule(&sensor_debounce_work, K_MSEC(20));
        */
        k_msleep(SENSOR_MS);
    }
}
K_THREAD_DEFINE(sensor_thread, STACK_SIZE, sensor_sim_fn, NULL, NULL, NULL, 5, 0, 0);

int main(void) {
    LOG_INF("=== L3 Homework: Workqueue Solution ===");
    return 0;
}