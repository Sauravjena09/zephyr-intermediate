#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(demo_kernel, LOG_LEVEL_INF);

#define STACK_SIZE 1024

#define PRIO_A 7
#define PRIO_B 5
#define PRIO_C 3
#define PRIO_D -1
void t_low_fn(void *p1, void *p2, void *p3)
{
    while (1) {
        LOG_INF("T_LOW RUNNING");
        k_msleep(300);
    }
}

void t_med_fn(void *p1, void *p2, void *p3)
{
    while (1) {
        LOG_INF("T_MED_RUNNING");
        k_msleep(200);
    }
}
void t_high_fn(void *p1, void *p2, void *p3)
{
    while (1) {
        LOG_INF("T_HIGH_RUNNING");
        k_msleep(100);
    }
}

void t_cooperative_fn(void *p1,void *p2,void *p3){
    while(1){
        int count=0;
        while(count<=5){
            LOG_INF("T_COOPERATIVE_RUNNING");
            count++;
        }
     LOG_INF("TASK COMPLETED BY THE COOPERATIVE THREAD");
     k_yield();

    }
}
/***
 *  when k_yield() is called, the scheduler looks for another "Ready" thread of equal or higher priority to hand the CPU to.
 *  Since -1 is the highest priority in your system, it instantly hands the CPU right back to
 *  the cooperative thread, locking up the system
 */
K_THREAD_DEFINE(thread_a, STACK_SIZE, t_low_fn,
                NULL, NULL, NULL, PRIO_A, 0, 0);
K_THREAD_DEFINE(thread_b, STACK_SIZE, t_med_fn,
                NULL, NULL, NULL, PRIO_B, 0, 0);
K_THREAD_DEFINE(thread_c,STACK_SIZE,t_high_fn,
                NULL,NULL,NULL,PRIO_C,0,0);
K_THREAD_DEFINE(thread_d,STACK_SIZE,t_cooperative_fn,
                NULL,NULL,NULL,PRIO_D,0,0);

int main(void){  
    LOG_INF("MAIN BOOT:RTOS SCHEDULER HAS STARTED !");

    return 0;
}

