# L6-TASK1: Latency Investigation and Tracing Report

## 1. Workload Description & 10 ms Start-Latency Guarantee

The application workload consists of two concurrent threads driven by a periodic timer interrupt on `stm32f4_disco`:
- **`event_timer`**: Fires every 250 ms (`EVENT_PERIOD_MS = 250`), enqueues a `struct control_event` (`seq`, `ready_ms`) into `control_queue`, signals `maintenance_start`, and emits the application trace event `event_ready`.
- **`control` thread**: Blocks on `control_queue`. When an event arrives, it measures start latency, verifies the 10 ms deadline guarantee, simulates control workload via `k_busy_wait(1000)`, and logs the outcome.
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

Below is the `kernel thread list` output pasted directly as printed by the Zephyr shell on `stm32f4_disco`:

```text
rtt:~$ kernel thread list
Scheduler: 6 since last call
Threads:
 0x20001508 maintenance
	options: 0x0, priority: 4 timeout: 0
	state: pending, entry: 0x8000649
	stack size 2048, unused 1760, usage 288 / 2048 (14 %)

 0x200015c0 control   
	options: 0x0, priority: 7 timeout: 0
	state: pending, entry: 0x800067d
	stack size 2048, unused 1672, usage 376 / 2048 (18 %)

 0x20001bc8 tracing_thread
	options: 0x0, priority: 14 timeout: 0
	state: pending, entry: 0x8002195
	stack size 1024, unused 720, usage 304 / 1024 (29 %)

 0x20001d58 shell_uart
	options: 0x0, priority: 14 timeout: 0
	state: pending, entry: 0x8005bf9
	stack size 2048, unused 1576, usage 472 / 2048 (23 %)

 0x20002a18           
	options: 0x1, priority: -14 timeout: 0
	state: pending, entry: 0x80082f1
	stack size 1024, unused 720, usage 304 / 1024 (29 %)

 0x20001c80 usbworkq  
	options: 0x0, priority: -1 timeout: 0
	state: pending, entry: 0x800ad19
	stack size 1024, unused 776, usage 248 / 1024 (24 %)

*0x200020d8 shell_rtt 
	options: 0x0, priority: 14 timeout: 0
	state: queued, entry: 0x8005bf9
	stack size 2048, unused 992, usage 1056 / 2048 (51 %)

 0x20001688 logging   
	options: 0x0, priority: 14 timeout: 512
	state: pending, entry: 0x8001719
	stack size 768, unused 336, usage 432 / 768 (56 %)

 0x20002e78 sysworkq  
	options: 0x1, priority: -1 timeout: 0
	state: pending, entry: 0x800ad19
	stack size 1024, unused 768, usage 256 / 1024 (25 %)

 0x20002c30 idle      
	options: 0x1, priority: 15 timeout: 0
	state: , entry: 0x8011833
	stack size 320, unused 168, usage 152 / 320 (47 %)
```

### Analysis of Thread Priorities
In Zephyr RTOS, numerically smaller priority values correspond to higher scheduling priority:
- `maintenance` has priority **4** (higher execution priority)
- `control` has priority **7** (lower execution priority)

---

## 3. CTF Trace Analysis (Decoded with Babeltrace)

The RAM-backed CTF traces (`CONFIG_TRACING_BACKEND_RAM=y`) from both the original failing run and the fixed run were dumped from SRAM and decoded directly with `babeltrace2` without editing in `app/traces/l6_scheduler_events.txt`.

### Trace Evidence (Original Program)
In Trace 1 (`CONTROL_PRIORITY = 7`, `MAINTENANCE_PRIORITY = 4`):
1. At `[00:00:00.500523857]`, `control` becomes ready (`thread_sched_ready`), followed at `[00:00:00.500546202]` by `maintenance` (`thread_sched_ready`), and `event_ready` is emitted at `[00:00:00.500562136]`.
2. At `[00:00:00.500587297]`, the scheduler switches in `maintenance` (`prio 4 < 7`) instead of `control`.
3. `maintenance` executes `k_busy_wait(45000)` (`thread_busy_wait_enter` at `0.500598970` to `thread_busy_wait_exit` at `0.545606345`).
4. Only after `maintenance` pends at `[00:00:00.545628357]` does the scheduler switch in `control` at `[00:00:00.545647041]`.
5. `control_start` is emitted at `[00:00:00.545661904]` with `arg1 = 45` ms — violating the 10 ms start-latency guarantee.

### Trace Evidence (Fixed Version)
In Trace 2 (`CONTROL_PRIORITY = 3`, `MAINTENANCE_PRIORITY = 7`):
1. At `[00:00:00.500562178]`, `event_ready` is emitted.
2. The scheduler immediately switches in `control` (`[00:00:00.500587255]`), and `control_start` is emitted at `[00:00:00.500602184]` with `arg1 = 0` ms (~40 µs dispatch latency).
3. `control` completes (`control_done` at `[00:00:00.501647458]`), and only then does `maintenance` run its 45 ms background work.

---

## 4. Root Cause and Scheduling Correction

### Root Cause
Priority inversion / misconfiguration: `maintenance` was assigned priority 4 while `control` was assigned priority 7. When both threads become ready on the periodic timer expiry, the preemptive scheduler selects `maintenance`, which holds the CPU for 45 ms in `k_busy_wait`, starving `control` past its 10 ms start-latency deadline.

### Scheduling Correction Applied
Inverted the priority assignment in `app/src/main_latency.c`:
- **`CONTROL_PRIORITY` = 3** (higher priority)
- **`MAINTENANCE_PRIORITY` = 7** (lower priority)

---

## 5. Latency Logs

### Original Program (Failing: `CONTROL_PRIORITY = 7`, `MAINTENANCE_PRIORITY = 4`)
```text
*** Booting Zephyr OS build v4.4.0-9765-gfa3c2981db16 ***
[00:00:00.013,000] <inf> homework: === L6 Homework: Runtime Investigation ===
[00:00:00.013,000] <inf> homework: Control work must start within 10 ms
[00:00:00.013,000] <inf> homework: Inspect, measure, trace, explain, and correct the delay
[00:00:00.560,000] <inf> homework: [CONTROL] processed seq=0
[00:00:00.810,000] <inf> homework: [CONTROL] processed seq=1
[00:00:01.059,000] <wrn> homework: Skipped 2 messages
[00:00:01.059,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=2 start_latency=45 ms (> 10 ms guarantee) total_misses=3
[00:00:01.060,000] <inf> homework: [CONTROL] processed seq=2
[00:00:01.310,000] <inf> homework: [CONTROL] processed seq=3
[00:00:01.560,000] <inf> homework: [CONTROL] processed seq=4
[00:00:01.810,000] <inf> homework: [CONTROL] processed seq=5
[00:00:02.059,000] <wrn> homework: Skipped 3 messages
[00:00:02.059,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=6 start_latency=45 ms (> 10 ms guarantee) total_misses=7
[00:00:02.060,000] <inf> homework: [CONTROL] processed seq=6
[00:00:02.310,000] <inf> homework: [CONTROL] processed seq=7
[00:00:02.560,000] <inf> homework: [CONTROL] processed seq=8
[00:00:02.810,000] <inf> homework: [CONTROL] processed seq=9
[00:00:03.059,000] <wrn> homework: Skipped 3 messages
[00:00:03.059,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=10 start_latency=45 ms (> 10 ms guarantee) total_misses=11
[00:00:03.060,000] <inf> homework: [CONTROL] processed seq=10
[00:00:03.310,000] <inf> homework: [CONTROL] processed seq=11
[00:00:03.560,000] <inf> homework: [CONTROL] processed seq=12
[00:00:03.810,000] <inf> homework: [CONTROL] processed seq=13
[00:00:04.059,000] <wrn> homework: Skipped 3 messages
[00:00:04.059,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=14 start_latency=45 ms (> 10 ms guarantee) total_misses=15
[00:00:04.060,000] <inf> homework: [CONTROL] processed seq=14
[00:00:04.310,000] <inf> homework: [CONTROL] processed seq=15
[00:00:04.560,000] <inf> homework: [CONTROL] processed seq=16
[00:00:04.810,000] <inf> homework: [CONTROL] processed seq=17
[00:00:05.059,000] <wrn> homework: Skipped 3 messages
[00:00:05.059,000] <wrn> homework: [CONTROL] DEADLINE MISS: seq=18 start_latency=45 ms (> 10 ms guarantee) total_misses=19
[00:00:05.060,000] <inf> homework: [CONTROL] processed seq=18
[00:00:05.310,000] <inf> homework: [CONTROL] processed seq=19
```

### Fixed Version (Corrected: `CONTROL_PRIORITY = 3`, `MAINTENANCE_PRIORITY = 7`)
```text
*** Booting Zephyr OS build v4.4.0-9765-gfa3c2981db16 ***
[00:00:00.013,000] <inf> homework: === L6 Homework: Runtime Investigation ===
[00:00:00.013,000] <inf> homework: Control work must start within 10 ms
[00:00:00.013,000] <inf> homework: Inspect, measure, trace, explain, and correct the delay
[00:00:00.514,000] <inf> homework: [CONTROL] OK: seq=0 start_latency=0 ms (<= 10 ms guarantee) total=1
[00:00:00.515,000] <inf> homework: [CONTROL] processed seq=0
[00:00:00.764,000] <inf> homework: [CONTROL] OK: seq=1 start_latency=0 ms (<= 10 ms guarantee) total=2
[00:00:00.765,000] <inf> homework: [CONTROL] processed seq=1
[00:00:01.014,000] <inf> homework: [CONTROL] OK: seq=2 start_latency=0 ms (<= 10 ms guarantee) total=3
[00:00:01.015,000] <inf> homework: [CONTROL] processed seq=2
[00:00:01.264,000] <inf> homework: [CONTROL] OK: seq=3 start_latency=0 ms (<= 10 ms guarantee) total=4
[00:00:01.265,000] <inf> homework: [CONTROL] processed seq=3
[00:00:01.514,000] <inf> homework: [CONTROL] OK: seq=4 start_latency=0 ms (<= 10 ms guarantee) total=5
[00:00:01.515,000] <inf> homework: [CONTROL] processed seq=4
[00:00:01.764,000] <inf> homework: [CONTROL] OK: seq=5 start_latency=0 ms (<= 10 ms guarantee) total=6
[00:00:01.765,000] <inf> homework: [CONTROL] processed seq=5
[00:00:02.014,000] <inf> homework: [CONTROL] OK: seq=6 start_latency=0 ms (<= 10 ms guarantee) total=7
[00:00:02.015,000] <inf> homework: [CONTROL] processed seq=6
[00:00:02.264,000] <inf> homework: [CONTROL] OK: seq=7 start_latency=0 ms (<= 10 ms guarantee) total=8
[00:00:02.265,000] <inf> homework: [CONTROL] processed seq=7
[00:00:02.514,000] <inf> homework: [CONTROL] OK: seq=8 start_latency=0 ms (<= 10 ms guarantee) total=9
[00:00:02.515,000] <inf> homework: [CONTROL] processed seq=8
[00:00:02.764,000] <inf> homework: [CONTROL] OK: seq=9 start_latency=0 ms (<= 10 ms guarantee) total=10
[00:00:02.765,000] <inf> homework: [CONTROL] processed seq=9
[00:00:03.014,000] <inf> homework: [CONTROL] OK: seq=10 start_latency=0 ms (<= 10 ms guarantee) total=11
[00:00:03.015,000] <inf> homework: [CONTROL] processed seq=10
[00:00:03.264,000] <inf> homework: [CONTROL] OK: seq=11 start_latency=0 ms (<= 10 ms guarantee) total=12
[00:00:03.265,000] <inf> homework: [CONTROL] processed seq=11
[00:00:03.514,000] <inf> homework: [CONTROL] OK: seq=12 start_latency=0 ms (<= 10 ms guarantee) total=13
[00:00:03.515,000] <inf> homework: [CONTROL] processed seq=12
[00:00:03.764,000] <inf> homework: [CONTROL] OK: seq=13 start_latency=0 ms (<= 10 ms guarantee) total=14
[00:00:03.765,000] <inf> homework: [CONTROL] processed seq=13
```

---

## 6. Summary of Evidence
| Metric | Original Program (Failing) | Fixed Version (Corrected) |
| :--- | :--- | :--- |
| **Control Priority** | 7 (Low) | 3 (High) |
| **Maintenance Priority** | 4 (High) | 7 (Low) |
| **Start Latency** | **45 ms** | **0 ms (~40 µs)** |
| **10 ms Guarantee** | **Violated (100% deadline misses)** | **Satisfied (0 misses)** |
| **Root Cause** | Maintenance runs first, delaying Control by 45 ms | Control preempts Maintenance and starts immediately |
