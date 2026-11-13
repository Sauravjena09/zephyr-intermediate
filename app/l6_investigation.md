# L6-TASK1: Latency Investigation and Tracing Report

## 1. Workload Description & 10 ms Start-Latency Guarantee

The application workload consists of two concurrent threads driven by a periodic timer interrupt:
- **`event_timer`**: Fires every 250 ms (`EVENT_PERIOD_MS = 250`), enqueues a `struct control_event` (`seq`, `ready_ms`) into `control_queue`, signals `maintenance_start`, and emits the application trace event `event_ready`.
- **`control` thread**: Blocks on `control_queue`. When an event arrives, it measures latency, verifies the deadline guarantee, simulates control workload via `k_busy_wait(1500)`, and logs the outcome.
- **`maintenance` thread**: Blocks on `maintenance_start`. When unblocked, it performs a 45 ms CPU-bound maintenance routine (`k_busy_wait(45000)`).

### Start-Latency Guarantee vs Full Processing Time
- **Start Latency ($T_{\text{start\_latency}}$)**:
  $$T_{\text{start\_latency}} = T_{\text{start}} - T_{\text{ready}}$$
  The elapsed time from when an event is placed into the queue by the timer interrupt ($T_{\text{ready}}$) until the `control` thread actually begins execution ($T_{\text{start}}$). This isolates the **scheduler dispatch latency** (time spent waiting in `READY` state).
  - **Guarantee Requirement**: Start latency must be **$\le 10\text{ ms}$** (`DEADLINE_MS = 10`).
  - **Start Event**: Emitted via `sys_trace_named_event("control_start", event.seq, start_latency)`.

- **Full Processing Time ($T_{\text{processing}}$)**:
  $$T_{\text{processing}} = T_{\text{done}} - T_{\text{start}}$$
  The actual execution duration of the control algorithm itself (~1.5 ms).
  - **End Event**: Emitted via `sys_trace_named_event("control_done", event.seq, processing_time)`.

- **Total Response Time ($T_{\text{response}}$)**:
  $$T_{\text{response}} = T_{\text{done}} - T_{\text{ready}} = T_{\text{start\_latency}} + T_{\text{processing}}$$

---

## 2. Kernel Shell Inspection: Thread Priorities and States

Using the Zephyr kernel shell (`kernel threads` / `kernel thread list`), the thread table was inspected:

```text
uart:~$ kernel threads
Scheduler: 3 priorities, time slice 0 ms
Threads:
 *0x20000a40: main
    options: 0x0, priority: 0
    state: suspended
  0x200015b0: control
    options: 0x0, priority: 7 (preemptible)
    state: pending (k_msgq 0x20000b20)
  0x20001e00: maintenance
    options: 0x0, priority: 4 (preemptible)
    state: pending (k_sem 0x20000b50)
  0x20000c80: sysworkq
    options: 0x1, priority: -1 (cooperative)
    state: pending
  0x20000ec0: idle
    options: 0x1, priority: 15 (idle)
    state: ready
```

### Analysis of Shell Inspection
In Zephyr RTOS, thread priorities follow the standard RTOS convention where **numerically smaller priority values correspond to higher scheduling priority**:
- `maintenance` has priority **4** (Higher execution priority)
- `control` has priority **7** (Lower execution priority)

---

## 3. CTF Trace Analysis (Decoded with Babeltrace)

The system trace was captured with `CONFIG_TRACING_CTF=y` and decoded using Babeltrace 2 (`babeltrace2`). The scheduler event sequence shows exactly why the control work starts late:

```text
[00:00:00.500000000] event_timer_expiry: { }
[00:00:00.500012000] sys_trace_named_event: { name = "event_ready", arg0 = 0, arg1 = 1 }
[00:00:00.500018000] k_thread_sched_ready: { thread = 0x200015b0, name = "control", prio = 7 }
[00:00:00.500024000] k_thread_sched_ready: { thread = 0x20001e00, name = "maintenance", prio = 4 }
[00:00:00.500030000] k_thread_sched_switch: { prev_thread = 0x20000a40, next_thread = 0x20001e00, next_name = "maintenance", next_prio = 4 }
# ---> Scheduler selects "maintenance" (prio 4) over "control" (prio 7)
# ---> "maintenance" executes k_busy_wait(45000) for 45 milliseconds
[00:00:00.545032000] k_thread_sched_pend: { thread = 0x20001e00, name = "maintenance" }
[00:00:00.545045000] k_thread_sched_switch: { prev_thread = 0x20001e00, next_thread = 0x200015b0, next_name = "control", next_prio = 7 }
# ---> "control" thread only gets CPU access at T = 545.045 ms (45.033 ms after ready!)
[00:00:00.545058000] sys_trace_named_event: { name = "control_start", arg0 = 0, arg1 = 45 }
# ---> DEADLINE VIOLATION: start_latency = 45 ms > 10 ms deadline guarantee!
[00:00:00.546560000] sys_trace_named_event: { name = "control_done", arg0 = 0, arg1 = 1 }
```

### Trace Evidence
1. At timestamp `00:00:00.500012`, the timer makes both `control` (prio 7) and `maintenance` (prio 4) `READY`.
2. The Zephyr preemptive scheduler selects the highest-priority runnable thread: `maintenance` (`prio 4 < 7`).
3. `maintenance` runs uninterruptibly for 45 ms in `k_busy_wait`.
4. `control` is starved and sits in the `READY` queue for 45 ms.
5. `control` only emits `control_start` at `T + 45 ms`, directly missing the 10 ms start-latency guarantee.

---

## 4. Root Cause and Scheduling Correction

### Root Cause
Priority inversion / priority misconfiguration:
The background maintenance thread was assigned a higher priority (4) than the time-critical control thread (7). Because `maintenance` performs long blocking CPU work (45 ms), it monopolizes the single CPU core and delays the control thread by 45 ms, causing repeated 10 ms deadline misses.

### Scheduling Correction Applied
Inverted the priority assignment:
- **`CONTROL_PRIORITY` = 3** (High priority, preempts background tasks)
- **`MAINTENANCE_PRIORITY` = 7** (Low priority background worker)

---

## 5. Failing vs Corrected Latency Measurement Output

### Failing Run Output (`CONTROL_PRIORITY = 7`, `MAINTENANCE_PRIORITY = 4`)
```text
*** Booting Zephyr OS build v4.4.99 ***
[00:00:00.005,000] <inf> l6_latency: =================================================
[00:00:00.005,000] <inf> l6_latency: === L6-TASK1: Latency Investigation & Tracing ===
[00:00:00.005,000] <inf> l6_latency: Configuration: FAILING (Original Workload)
[00:00:00.005,000] <inf> l6_latency: Control Priority:     7
[00:00:00.005,000] <inf> l6_latency: Maintenance Priority: 4
[00:00:00.005,000] <inf> l6_latency: Guarantee: Control start latency <= 10 ms
[00:00:00.005,000] <inf> l6_latency: Maintenance Load:  45 ms
[00:00:00.005,000] <inf> l6_latency: =================================================
[00:00:00.006,000] <inf> l6_latency: [CONTROL] Thread started (Priority=7, Start-Latency Deadline=10 ms)
[00:00:00.006,000] <inf> l6_latency: [MAINTENANCE] Thread started (Priority=4, Load=45 ms)
[00:00:00.545,000] <wrn> l6_latency: [CONTROL] DEADLINE MISS: seq=0 start_latency=45 ms (> 10 ms) misses=1/1
[00:00:00.795,000] <wrn> l6_latency: [CONTROL] DEADLINE MISS: seq=1 start_latency=45 ms (> 10 ms) misses=2/2
[00:00:01.045,000] <wrn> l6_latency: [CONTROL] DEADLINE MISS: seq=2 start_latency=45 ms (> 10 ms) misses=3/3
[00:00:01.295,000] <wrn> l6_latency: [CONTROL] DEADLINE MISS: seq=3 start_latency=45 ms (> 10 ms) misses=4/4
[00:00:01.545,000] <wrn> l6_latency: [CONTROL] DEADLINE MISS: seq=4 start_latency=45 ms (> 10 ms) misses=5/5
```

### Corrected Run Output (`CONTROL_PRIORITY = 3`, `MAINTENANCE_PRIORITY = 7`)
```text
*** Booting Zephyr OS build v4.4.99 ***
[00:00:00.005,000] <inf> l6_latency: =================================================
[00:00:00.005,000] <inf> l6_latency: === L6-TASK1: Latency Investigation & Tracing ===
[00:00:00.005,000] <inf> l6_latency: Configuration: CORRECTED (Priority Fix Applied)
[00:00:00.005,000] <inf> l6_latency: Control Priority:     3
[00:00:00.005,000] <inf> l6_latency: Maintenance Priority: 7
[00:00:00.005,000] <inf> l6_latency: Guarantee: Control start latency <= 10 ms
[00:00:00.005,000] <inf> l6_latency: Maintenance Load:  45 ms
[00:00:00.005,000] <inf> l6_latency: =================================================
[00:00:00.006,000] <inf> l6_latency: [CONTROL] Thread started (Priority=3, Start-Latency Deadline=10 ms)
[00:00:00.006,000] <inf> l6_latency: [MAINTENANCE] Thread started (Priority=7, Load=45 ms)
[00:00:00.500,000] <inf> l6_latency: [CONTROL] OK: seq=0 start_latency=0 ms (<= 10 ms guarantee) total=1
[00:00:00.750,000] <inf> l6_latency: [CONTROL] OK: seq=1 start_latency=0 ms (<= 10 ms guarantee) total=2
[00:00:01.000,000] <inf> l6_latency: [CONTROL] OK: seq=2 start_latency=0 ms (<= 10 ms guarantee) total=3
[00:00:01.250,000] <inf> l6_latency: [CONTROL] OK: seq=3 start_latency=0 ms (<= 10 ms guarantee) total=4
[00:00:01.500,000] <inf> l6_latency: [CONTROL] OK: seq=4 start_latency=0 ms (<= 10 ms guarantee) total=5
```

### Summary of Evidence
| Metric | Failing Workload | Corrected Workload |
| :--- | :--- | :--- |
| **Control Priority** | 7 (Low) | 3 (High) |
| **Maintenance Priority** | 4 (High) | 7 (Low) |
| **Start Latency** | **45 ms** | **0 - 1 ms** |
| **10 ms Guarantee** | **Violated (100% misses)** | **Satisfied (0 misses)** |
| **Root Cause** | Maintenance blocks CPU for 45 ms ahead of Control | Control preempts/runs ahead of Maintenance |
