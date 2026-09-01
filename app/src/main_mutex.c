#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(race_demo,LOG_LEVEL_INF);

int bank_balance=100;
K_MUTEX_DEFINE(bank_mutex);
void thread_a_entry(void){
    while(1){
        k_mutex_lock(&bank_mutex,K_FOREVER);

        LOG_INF("Thread A :READING BALANCE");
        int local_copy = bank_balance;
        local_copy = local_copy+50;
        k_msleep(100);
        bank_balance =local_copy;
        LOG_INF("THREAD A : DEPOSITED 50 ,FINAL BALANCE %d",bank_balance);
        k_mutex_unlock(&bank_mutex);
        k_msleep(2000);
    }
}

void thread_b_entry(void){
    while(1){
        k_msleep(50);
        k_mutex_lock(&bank_mutex,K_FOREVER);
        LOG_WRN("THREAD B : READING BALANCE....");
        int local_copy =bank_balance;
        local_copy=local_copy+20;
        bank_balance=local_copy;
        LOG_WRN("Thread B: Deposited $20. Final Balance: $%d", bank_balance);
        k_mutex_unlock(&bank_mutex);
        k_msleep(2000);
    }
}

K_THREAD_DEFINE(tid_a,1024,thread_a_entry,NULL,NULL,NULL,7,0,0);
K_THREAD_DEFINE(tid_b,1024,thread_b_entry,NULL,NULL,NULL,7,0,0);

int main(void){
    return 0;
}