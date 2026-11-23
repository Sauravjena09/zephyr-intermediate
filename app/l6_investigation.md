# L6-TASK1: Latency Investigation and Tracing Report

## 1. Workload Description & 10 ms Start-Latency Guarantee

The application workload consists of two concurrent threads driven by a periodic timer interrupt:
- **`event_timer`**: Fires every 250 ms (`EVENT_PERIOD_MS = 250`), enqueues a `struct control_event` (`seq`, `ready_ms`) into `control_queue`, signals `maintenance_start`, and emits the application trace event `event_ready`.
- **`control` thread**: Blocks on `control_queue`. When an event arrives, it measures latency, verifies the deadline guarantee, simulates control workload via `k_busy_wait(1000)`, and logs the outcome.
- **`maintenance` thread**: Blocks on `maintenance_start`. When unblocked, it performs a 45 ms CPU-bound maintenance routine (`k_busy_wait(45000)`).

### Start-Latency Guarantee vs Full Processing Time
- **Start Latency ($T_{\text{start\_latency}}$)**:
  $$T_{\text{start\_latency}} = T_{\text{start}} - T_{\text{ready}}$$
  The elapsed time from when an event is placed into the queue by the timer interrupt ($T_{\text{ready}}$) until the `control` thread actually begins execution ($T_{\text{start}}$). This isolates the **scheduler dispatch latency** (time spent waiting in `READY` state).
  - **Guarantee Requirement**: Start latency must be **$\le 10\text{ ms}$** (`DEADLINE_MS = 10`).
  - **Start Event**: Emitted via `sys_trace_named_event("control_start", event.seq, start_latency)`.

- **Full Processing Time ($T_{\text{processing}}$)**:
  $$T_{\text{processing}} = T_{\text{done}} - T_{\text{start}}$$
  The actual execution duration of the control algorithm itself (~1.0 ms).
  - **End Event**: Emitted via `sys_trace_named_event("control_done", event.seq, processing_time)`.

- **Total Response Time ($T_{\text{response}}$)**:
  $$T_{\text{response}} = T_{\text{done}} - T_{\text{ready}} = T_{\text{start\_latency}} + T_{\text{processing}}$$

---

## 2. Kernel Thread List Output

Below is the kernel thread list output pasted directly as printed by the shell command `kernel thread list`:

```text
uart:~$ kernel thread list
Scheduler: 12423 since last call
Threads:
 0x20001850 control   
	options: 0x0, priority: 7 timeout: 0
	state: pending, entry: 0x08001249
	stack size 2048, unused 1784, usage 264 / 2048 (12 %)

 0x200020a0 maintenance
	options: 0x0, priority: 4 timeout: 0
	state: pending, entry: 0x08001351
	stack size 2048, unused 1856, usage 192 / 2048 (9 %)

 0x20000a60 sysworkq  
	options: 0x1, priority: -1 timeout: 0
	state: pending, entry: 0x08002a15
	stack size 1024, unused 768, usage 256 / 1024 (25 %)

*0x20000ec0 shell_uart
	options: 0x0, priority: 14 timeout: 0
	state: queued, entry: 0x08003185
	stack size 2048, unused 1420, usage 628 / 2048 (30 %)

 0x20000410 idle      
	options: 0x1, priority: 15 timeout: 0
	state: ready, entry: 0x08002bbd
	stack size 320, unused 248, usage 72 / 320 (22 %)

 0x20000cd8 main      
	options: 0x0, priority: 0 timeout: 0
	state: suspended, entry: 0x0800142d
	stack size 2048, unused 1816, usage 232 / 2048 (11 %)
```

### Analysis of Thread Priorities
In Zephyr RTOS, thread priorities follow the standard convention where **numerically smaller priority values correspond to higher scheduling priority**:
- `maintenance` has priority **4** (Higher execution priority)
- `control` has priority **7** (Lower execution priority)

---

## 3. CTF Trace Analysis (Decoded with Babeltrace)

The CTF trace decoded with Babeltrace 2 (`babeltrace2`) shows the scheduler events in `app/traces/l6_scheduler_events.txt`.

### Trace Evidence
1. The periodic timer interrupt fires and generates `event_ready`.
2. Both `maintenance` (priority 4) and `control` (priority 7) enter the ready state.
3. The scheduler context-switches to `maintenance` because its numerical priority (4) is higher than `control` (7).
4. `maintenance` runs uninterruptibly for 45 ms in `k_busy_wait(45000)`.
5. `control` is starved and sits in the `READY` queue for 45 ms.
6. When `maintenance` finally pends on `maintenance_start`, `control` is switched in, emitting `control_start` with `start_latency = 45 ms`, which violates the 10 ms start-latency guarantee.

---

## 4. Root Cause and Scheduling Correction

### Root Cause
Priority inversion / priority misconfiguration:
The background maintenance thread was assigned a higher priority (4) than the time-critical control thread (7). Because `maintenance` performs 45 ms of uninterruptible CPU work, it monopolizes the CPU core and delays the control thread by 45 ms, causing repeated 10 ms deadline misses.

### Scheduling Correction Applied
Inverted the priority assignment:
- **`CONTROL_PRIORITY` = 3** (High priority, preempts background tasks)
- **`MAINTENANCE_PRIORITY` = 7** (Low priority background worker)

---

## 5. Latency Logs

### Original Program (Failing: CONTROL_PRIORITY = 7, MAINTENANCE_PRIORITY = 4)
```text
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.005,000] <inf> homework: === L6 Homework: Runtime Investigation ===
[00:00:00.005,000] <inf> homework: Control work must start within 10 ms
[00:00:00.005,000] <inf> homework: Inspect, measure, trace, explain, and correct the delay
[00:00:00.545,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=0 start_latency=45 ms (> 10 ms guarantee) total_misses=1
[00:00:00.546,000] <inf> homework: [CONTROL] processed seq=0
[00:00:00.795,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=1 start_latency=45 ms (> 10 ms guarantee) total_misses=2
[00:00:00.796,000] <inf> homework: [CONTROL] processed seq=1
[00:00:01.045,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=2 start_latency=45 ms (> 10 ms guarantee) total_misses=3
[00:00:01.046,000] <inf> homework: [CONTROL] processed seq=2
[00:00:01.295,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=3 start_latency=45 ms (> 10 ms guarantee) total_misses=4
[00:00:01.296,000] <inf> homework: [CONTROL] processed seq=3
[00:00:01.545,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=4 start_latency=45 ms (> 10 ms guarantee) total_misses=5
[00:00:01.546,000] <inf> homework: [CONTROL] processed seq=4
```

### Fixed Version (Corrected: CONTROL_PRIORITY = 3, MAINTENANCE_PRIORITY = 7)
```text
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.005,000] <inf> homework: === L6 Homework: Runtime Investigation ===
[00:00:00.005,000] <inf> homework: Control work must start within 10 ms
[00:00:00.005,000] <inf> homework: Inspect, measure, trace, explain, and correct the delay
[00:00:00.500,000] <inf> homework: [CONTROL] OK: seq=0 start_latency=0 ms (<= 10 ms guarantee) total=1
[00:00:00.501,000] <inf> homework: [CONTROL] processed seq=0
[00:00:00.750,000] <inf> homework: [CONTROL] OK: seq=1 start_latency=0 ms (<= 10 ms guarantee) total=2
[00:00:00.751,000] <inf> homework: [CONTROL] processed seq=1
[00:00:01.000,000] <inf> homework: [CONTROL] OK: seq=2 start_latency=0 ms (<= 10 ms guarantee) total=3
[00:00:01.001,000] <inf> homework: [CONTROL] processed seq=2
[00:00:01.250,000] <inf> homework: [CONTROL] OK: seq=3 start_latency=0 ms (<= 10 ms guarantee) total=4
[00:00:01.251,000] <inf> homework: [CONTROL] processed seq=3
[00:00:01.500,000] <inf> homework: [CONTROL] OK: seq=4 start_latency=0 ms (<= 10 ms guarantee) total=5
[00:00:01.501,000] <inf> homework: [CONTROL] processed seq=4
```

---

## 6. Summary of Evidence
| Metric | Original Program (Failing) | Fixed Version (Corrected) |
| :--- | :--- | :--- |
| **Control Priority** | 7 (Low) | 3 (High) |
| **Maintenance Priority** | 4 (High) | 7 (Low) |
| **Start Latency** | **45 ms** | **0 - 1 ms** |
| **10 ms Guarantee** | **Violated (100% deadline misses)** | **Satisfied (0 misses)** |
| **Root Cause** | Maintenance runs first, delaying Control by 45 ms | Control preempts Maintenance and starts immediately |
