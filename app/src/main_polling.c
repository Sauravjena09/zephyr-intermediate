#include <zephyr/logging/log.h>
#include <stdbool.h>

LOG_MODULE_REGISTER(task3,LOG_LEVEL_INF);
K_SEM_DEFINE(my_sem,1,1);

#define STACK_SIZE 1024U
#define SENSOR_MS 100U
#define POLL_MS 10U
#define EVENT_COUNT 10U

static volatile bool sensor_flag = false;
static int wasted_wakeups = 0;

static void sensor_sim_fn(void *p1, void *p2,void *p3){
    while(1){
        k_sem_take(&my_sem,K_FOREVER);
        sensor_flag=true;
        k_sem_give(&my_sem);
        LOG_INF("THE FLAG IS SET AND SEM IS GIVEN");
        k_msleep(SENSOR_MS);
    }
}

static void polling_fn(void *p1, void *p2, void *p3) {
    bool current_flag;

    while(1) {
        /* Take semaphore to safely read and clear the flag */
        k_sem_take(&my_sem, K_FOREVER);
        
        current_flag = sensor_flag;
        if (current_flag == true) {
            sensor_flag = false;
        }
        
        /* ALWAYS give the semaphore back */
        k_sem_give(&my_sem);

        /* Process outside the lock to minimize blocking */
        if (current_flag) {
            LOG_INF("POLLING: Event processed. (Wasted wakeups since last event: %d)", wasted_wakeups);
            wasted_wakeups = 0;
        } else {
            wasted_wakeups++;
        }
        
        k_msleep(POLL_MS);
    }
}

K_THREAD_DEFINE(sensor_thread,  STACK_SIZE, sensor_sim_fn, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(polling_thread, STACK_SIZE, polling_fn,    NULL, NULL, NULL, 5, 0, 0);int main(void)
{
    LOG_INF("=== L3 Homework: Polling to Workqueue ===");
    LOG_INF("Starter: polling every %dms, sensor fires every %dms",
            POLL_MS, SENSOR_MS);
    LOG_INF("Expected wasted wakeups: ~%d per event",
            (SENSOR_MS / POLL_MS) - 1);
    LOG_INF("Run this, count wakeups, then convert to workqueue.");

    /* Wait long enough for all events to complete */
    

    return 0;
}