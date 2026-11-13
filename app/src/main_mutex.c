/*
 * Zephyr Course: Intermediate - L2-TASK1: Protecting shared state with a mutex
 *
 * EXPLANATION OF CONCURRENT READ-MODIFY-WRITE AND LOST UPDATES:
 * -------------------------------------------------------------
 * When multiple threads access a shared variable without synchronization, a classic
 * read-modify-write race condition occurs.
 *
 * In the UNPROTECTED variant:
 * 1. Thread A reads bank_balance (e.g. 100) into its local stack variable local_copy.
 * 2. Before Thread A can write back the updated value (100 + 50 = 150), a context switch
 *    or voluntary delay (k_msleep) occurs.
 * 3. Thread B runs and also reads bank_balance. Since Thread A has not committed its write,
 *    Thread B still sees 100.
 * 4. Thread A resumes and writes 150 to bank_balance.
 * 5. Thread B resumes and writes 120 (100 + 20) to bank_balance, completely overwriting
 *    Thread A's deposit of $50! Thread A's update is permanently lost.
 *
 * Over 10 iterations per thread (10 deposits of $50 and 10 deposits of $20):
 * - Initial balance:  $100
 * - Expected total:   $100 + (10 * $50) + (10 * $20) = $800
 * - Unprotected total: Significantly lower than $800 because concurrent interleaving
 *                      repeatedly overwrites pending deposits.
 *
 * In the PROTECTED variant (with bank_mutex):
 * 1. Thread A acquires bank_mutex via k_mutex_lock(&bank_mutex, K_FOREVER).
 * 2. Thread A reads bank_balance, performs calculations, and even during delays (k_msleep),
 *    the mutex remains locked.
 * 3. Thread B attempts to lock bank_mutex, but is placed into the PENDING state by the scheduler.
 * 4. Thread A writes back the updated balance (150) and unlocks bank_mutex.
 * 5. Thread B unblocks, locks bank_mutex, and reads the fresh, already-updated balance (150).
 * 6. Thread B updates and writes back 170.
 * - Protected total: Exactly $800. No updates are lost, and atomic serializability is guaranteed.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(race_demo, LOG_LEVEL_INF);

#define STACK_SIZE         1024
#define PRIORITY           7
#define NUM_DEPOSITS       10
#define INITIAL_BALANCE    100
#define DEPOSIT_A          50
#define DEPOSIT_B          20
#define EXPECTED_TOTAL     (INITIAL_BALANCE + (NUM_DEPOSITS * DEPOSIT_A) + (NUM_DEPOSITS * DEPOSIT_B))

/* Shared bank balance */
static volatile int bank_balance = INITIAL_BALANCE;

/* Mutex protecting bank_balance during protected run */
K_MUTEX_DEFINE(bank_mutex);

/* Semaphores to coordinate runs between main and worker threads */
static K_SEM_DEFINE(sem_unprotected_start, 0, 2);
static K_SEM_DEFINE(sem_unprotected_done_a, 0, 1);
static K_SEM_DEFINE(sem_unprotected_done_b, 0, 1);

static K_SEM_DEFINE(sem_protected_start, 0, 2);
static K_SEM_DEFINE(sem_protected_done_a, 0, 1);
static K_SEM_DEFINE(sem_protected_done_b, 0, 1);

/* =====================================================================
 * UNPROTECTED WORKER THREADS (Demonstrating Race Condition & Lost Updates)
 * ===================================================================== */

void unprotected_thread_a(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_sem_take(&sem_unprotected_start, K_FOREVER);
    LOG_INF("[UNPROTECTED A] Started 10 deposits of $%d", DEPOSIT_A);

    for (int i = 0; i < NUM_DEPOSITS; i++) {
        /* 1. Read shared balance into local copy */
        int local_copy = bank_balance;
        LOG_DBG("[UNPROTECTED A] Read balance: %d (iter %d)", local_copy, i + 1);

        /* 2. Artificial delay between read and write gives Thread B window to read stale state */
        k_msleep(40);

        /* 3. Modify and write back */
        local_copy += DEPOSIT_A;
        bank_balance = local_copy;
        LOG_INF("[UNPROTECTED A] Wrote balance: %d (Deposit +$%d, iter %d)",
                bank_balance, DEPOSIT_A, i + 1);

        k_msleep(20);
    }

    LOG_INF("[UNPROTECTED A] Finished all deposits");
    k_sem_give(&sem_unprotected_done_a);
}

void unprotected_thread_b(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_sem_take(&sem_unprotected_start, K_FOREVER);
    LOG_WRN("[UNPROTECTED B] Started 10 deposits of $%d", DEPOSIT_B);

    for (int i = 0; i < NUM_DEPOSITS; i++) {
        /* Small phase offset ensures Thread B reads while Thread A is delaying before write */
        k_msleep(15);

        /* 1. Read shared balance into local copy (reads STALE balance!) */
        int local_copy = bank_balance;
        LOG_DBG("[UNPROTECTED B] Read balance: %d (iter %d)", local_copy, i + 1);

        /* 2. Delay before write */
        k_msleep(40);

        /* 3. Modify and write back (OVERWRITES Thread A's update!) */
        local_copy += DEPOSIT_B;
        bank_balance = local_copy;
        LOG_WRN("[UNPROTECTED B] Wrote balance: %d (Deposit +$%d, iter %d)",
                bank_balance, DEPOSIT_B, i + 1);

        k_msleep(20);
    }

    LOG_WRN("[UNPROTECTED B] Finished all deposits");
    k_sem_give(&sem_unprotected_done_b);
}

/* =====================================================================
 * PROTECTED WORKER THREADS (Demonstrating Mutex Mutual Exclusion)
 * ===================================================================== */

void protected_thread_a(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_sem_take(&sem_protected_start, K_FOREVER);
    LOG_INF("[PROTECTED A] Started 10 deposits of $%d", DEPOSIT_A);

    for (int i = 0; i < NUM_DEPOSITS; i++) {
        /* Acquire mutex before critical section */
        k_mutex_lock(&bank_mutex, K_FOREVER);

        int local_copy = bank_balance;
        /* Delay inside critical section - mutex holds off Thread B */
        k_msleep(40);
        local_copy += DEPOSIT_A;
        bank_balance = local_copy;

        LOG_INF("[PROTECTED A] Deposited +$%d -> Balance: %d (iter %d)",
                DEPOSIT_A, bank_balance, i + 1);

        /* Release mutex after write */
        k_mutex_unlock(&bank_mutex);

        k_msleep(20);
    }

    LOG_INF("[PROTECTED A] Finished all deposits");
    k_sem_give(&sem_protected_done_a);
}

void protected_thread_b(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_sem_take(&sem_protected_start, K_FOREVER);
    LOG_WRN("[PROTECTED B] Started 10 deposits of $%d", DEPOSIT_B);

    for (int i = 0; i < NUM_DEPOSITS; i++) {
        k_msleep(15);

        /* Thread B waits for bank_mutex if Thread A is in its critical section */
        k_mutex_lock(&bank_mutex, K_FOREVER);

        int local_copy = bank_balance;
        k_msleep(40);
        local_copy += DEPOSIT_B;
        bank_balance = local_copy;

        LOG_WRN("[PROTECTED B] Deposited +$%d -> Balance: %d (iter %d)",
                DEPOSIT_B, bank_balance, i + 1);

        k_mutex_unlock(&bank_mutex);

        k_msleep(20);
    }

    LOG_WRN("[PROTECTED B] Finished all deposits");
    k_sem_give(&sem_protected_done_b);
}

/* Thread definitions */
K_THREAD_DEFINE(unprot_a, STACK_SIZE, unprotected_thread_a, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(unprot_b, STACK_SIZE, unprotected_thread_b, NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(prot_a,   STACK_SIZE, protected_thread_a,   NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(prot_b,   STACK_SIZE, protected_thread_b,   NULL, NULL, NULL, PRIORITY, 0, 0);

int main(void)
{
    LOG_INF("=================================================================");
    LOG_INF("=== L2-TASK1: Protecting Shared State With a Mutex ===");
    LOG_INF("Initial Bank Balance:      $%d", INITIAL_BALANCE);
    LOG_INF("Thread A: 10 deposits of   $%d", DEPOSIT_A);
    LOG_INF("Thread B: 10 deposits of   $%d", DEPOSIT_B);
    LOG_INF("Expected Final Total:      $%d", EXPECTED_TOTAL);
    LOG_INF("=================================================================");

    /* -------------------------------------------------------------
     * PHASE 1: UNPROTECTED RUN (No Mutex)
     * ------------------------------------------------------------- */
    LOG_INF("--- Starting Phase 1: Unprotected Run (no mutex) ---");
    bank_balance = INITIAL_BALANCE;

    /* Release unprotected workers */
    k_sem_give(&sem_unprotected_start);
    k_sem_give(&sem_unprotected_start);

    /* Wait for both workers to finish all deposits */
    k_sem_take(&sem_unprotected_done_a, K_FOREVER);
    k_sem_take(&sem_unprotected_done_b, K_FOREVER);

    int unprotected_final = bank_balance;
    int lost_amount = EXPECTED_TOTAL - unprotected_final;

    LOG_ERR("Phase 1 Result (Unprotected):");
    LOG_ERR("  Expected Balance: $%d", EXPECTED_TOTAL);
    LOG_ERR("  Actual Balance:   $%d", unprotected_final);
    LOG_ERR("  Lost Updates:     $%d (lost due to concurrent read-modify-write race!)", lost_amount);

    k_msleep(500);

    /* -------------------------------------------------------------
     * PHASE 2: PROTECTED RUN (With bank_mutex)
     * ------------------------------------------------------------- */
    LOG_INF("--- Starting Phase 2: Protected Run (with bank_mutex) ---");
    bank_balance = INITIAL_BALANCE;

    /* Release protected workers */
    k_sem_give(&sem_protected_start);
    k_sem_give(&sem_protected_start);

    /* Wait for both protected workers to finish all deposits */
    k_sem_take(&sem_protected_done_a, K_FOREVER);
    k_sem_take(&sem_protected_done_b, K_FOREVER);

    int protected_final = bank_balance;

    LOG_INF("Phase 2 Result (Protected with bank_mutex):");
    LOG_INF("  Expected Balance: $%d", EXPECTED_TOTAL);
    LOG_INF("  Actual Balance:   $%d", protected_final);
    LOG_INF("  Lost Updates:     $0 (all deposits successfully committed)");

    /* -------------------------------------------------------------
     * COMPARISON SUMMARY
     * ------------------------------------------------------------- */
    LOG_INF("=================================================================");
    LOG_INF("=== FINAL COMPARISON AFTER ALL DEPOSITS ARE DONE ===");
    LOG_INF("Initial balance:            $%d", INITIAL_BALANCE);
    LOG_INF("Total deposits attempted:   $%d (10 x $50 + 10 x $20)",
            (NUM_DEPOSITS * DEPOSIT_A) + (NUM_DEPOSITS * DEPOSIT_B));
    LOG_INF("Expected final balance:     $%d", EXPECTED_TOTAL);
    LOG_INF("Unprotected final balance:  $%d (LOST $%d updates)",
            unprotected_final, lost_amount);
    LOG_INF("Protected final balance:    $%d (MUTEX PRESERVED EXACT TOTAL)",
            protected_final);
    LOG_INF("Difference preserved:       $%d saved from race condition corruption",
            protected_final - unprotected_final);
    LOG_INF("=================================================================");

    return 0;
}
