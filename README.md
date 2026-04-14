# Multi-Container Runtime

## 1. Team Information

| Name | SRN |
|------|-----|
| Subramani B M | PES1UG24CS473 |
| Sujal Sachin Yadavi | PES1UG24CS475 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites
- Ubuntu 22.04 or 24.04 VM
- Secure Boot OFF
- No WSL

### Install Dependencies
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build
```bash
cd boilerplate
make
```

### Prepare Root Filesystem
```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Load Kernel Module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
sudo dmesg | tail -5
```

### Start Supervisor
```bash
sudo ./engine supervisor ./rootfs-base
```

### Launch Containers (in a second terminal)
```bash
# Start Alpha with logging loop
sudo ./engine start alpha ./rootfs-alpha "/bin/sh -c 'i=1; while [ \$i -le 100 ]; do echo \"LOG_TEST_LINE_\$i\"; i=\$((i+1)); sleep 1; done'"

# Start Beta as an idle task
sudo ./engine start beta ./rootfs-beta "/bin/sh -c 'sleep 1000'"
```

### Use the CLI
```bash
# List containers and verify global PIDs
sudo ./engine ps

# View real-time logs through the bounded-buffer
sudo ./engine logs alpha

# Stop a container and trigger clean teardown
sudo ./engine stop alpha
```

### Run Memory Test
```bash
cp memory_hog ./rootfs-alpha/
# 10MB Soft Limit / 20MB Hard Limit
sudo ./engine start memtest ./rootfs-alpha "/memory_hog" --soft-mib 10 --hard-mib 20
sudo dmesg -w | grep container_monitor
```

### Run Scheduling Experiment
```bash
# Demonstrate CFS fairness by pitting high priority against low priority
sudo ./engine start cpuhi ./rootfs-alpha "/bin/sh -c 'while true; do true; done'" --nice -10
sudo ./engine start cpulo ./rootfs-beta "/bin/sh -c 'while true; do true; done'" --nice 10
top -bn1 | head -n 20
```

### Cleanup
```bash
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine ps
ps aux | grep engine
sudo rmmod monitor
```

---

## 3. Demo Screenshots

### Screenshot 1 — Multi-container supervision
<img width="737" height="470" alt="image" src="https://github.com/user-attachments/assets/63347569-6ab7-41fe-97dc-6753af1b2005" />
<img width="1398" height="273" alt="image" src="https://github.com/user-attachments/assets/081e6337-5d67-4e6d-a29b-372fef71ff3b" />

Demonstration of the Jackfruit Supervisor managing multiple isolated containers concurrently. The terminal logs verify interleaved output from Container Alpha and Container Beta, proving successful thread synchronization in the logging system. The engine ps output confirms that both containers are assigned unique PIDs and isolated root filesystems.

### Screenshot 2 — Metadata tracking
<img width="1398" height="273" alt="image" src="https://github.com/user-attachments/assets/4f6ff637-b006-47db-99f2-b48ae4f5d2df" />

Validation of the internal `container_record_t` metadata tracking. The screenshot shows the CLI successfully querying the supervisor's linked list to display real-time host PIDs, container states, and the specific `rootfs` paths associated with each active environment.

### Screenshot 3 — Bounded-buffer logging
<img width="1430" height="555" alt="image" src="https://github.com/user-attachments/assets/700aac89-6dfc-480f-bc4d-0fbc16a905ae" />

Evidence of the producer-consumer logging architecture in action. The log file contents demonstrate that data streamed from the container's `stdout` was correctly intercepted, buffered, and written to the host filesystem with high-resolution timestamps, confirming no data loss during high-volume output.

### Screenshot 4 — CLI and IPC
<img width="669" height="172" alt="image" src="https://github.com/user-attachments/assets/c995262a-af7e-4775-8da7-34baccd269dc" />

Verification of Inter-Process Communication (IPC) via Unix Domain Sockets. The sequence shows a CLI stop command being issued and the supervisor immediately responding, proving robust message passing between the control plane and the background daemon via `/tmp/jackfruit.sock`.

### Screenshot 5 — Soft-limit warning
<img width="1314" height="72" alt="image" src="https://github.com/user-attachments/assets/789c68fe-df28-48ad-be96-ae7a4d1c95b8" />

Observation of kernel-level resource monitoring. The dmesg output displays the `SOFT LIMIT` warning triggered by the kernel module's timer callback when a container's Resident Set Size (RSS) exceeds the defined threshold without terminating the process.

### Screenshot 6 — Hard-limit enforcement
<img width="1316" height="77" alt="image" src="https://github.com/user-attachments/assets/b8dae856-9d13-4a5c-b2c9-9085b57a4f59" />

Demonstration of immediate kernel-space enforcement. Upon breaching the hard memory limit, the kernel monitor issues an unblockable `SIGKILL`, transitioning the container to the `KILLED` state as seen in the supervisor metadata, preventing host memory exhaustion.

### Screenshot 7 — Scheduling experiment
<img width="897" height="470" alt="image" src="https://github.com/user-attachments/assets/e377cc42-1af8-4335-a92a-f9f5e40e195a" />

Analysis of scheduler fairness using the Completely Fair Scheduler (CFS). The top output verifies that the high-priority container (`nice -10`) receives a significantly larger CPU share than the low-priority container (`nice 10`), validating our implementation of process weighting and CFS weight theory.

### Screenshot 8 — Clean teardown
<img width="957" height="188" alt="image" src="https://github.com/user-attachments/assets/d915b74b-813c-4a53-8a7a-3a055e14b4e8" />

Proof of robust resource deallocation and process reaping. Following a global shutdown, the `ps aux grep` confirms the complete absence of container PIDs, proving that the supervisor successfully utilized `waitpid()` to prevent zombie processes and closed all IPC pipes and sockets cleanly.

---

## 4. Engineering Analysis

### 1. Isolation Mechanisms

The runtime achieves isolation using three Linux namespaces created via the `clone()` system call with `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS` flags.

**PID namespace:** Virtualizes process IDs so the container sees itself as `PID 1`, preventing visibility of host processes.

**UTS namespace:** Isolates the hostname, set via `sethostname()` inside the child process.

**Mount namespace:** Combined with `chroot()`, isolates the filesystem view so the container sees only its assigned directory as `root` (/).

### 2. Supervisor and Process Lifecycle

A persistent supervisor is required to act as the parent process to prevent zombie processes by reaping exited children via `waitpid()`. The supervisor manages a linked list of `container_record_t` to track real-time metadata and handle `SIGCHLD` signals for orderly state updates.

### 3. IPC, Threads, and Synchronization

The project uses two distinct IPC mechanisms:

**Logging (Pipes):** Redirects `stdout` to a bounded buffer where a dedicated logger thread (Consumer) writes to host files.

**Control (UNIX Sockets):** CLI processes communicate with the supervisor over `/tmp/jackfruit.sock` using `control_request_t` structures.

**Synchronization:** Uses `pthread_mutex_t` and `pthread_cond_t (not_empty, not_full)` for thread-safe buffer management, avoiding CPU-intensive busy-waiting.

### 4. Memory Management and Enforcement

The kernel monitor tracks Resident Set Size (RSS)—the physical RAM occupied by a process—via a fixed-interval timer callback.

**Soft Limit**: Triggers a non-fatal warning in kernel logs.

**Hard Limit:** Enforces an absolute ceiling by issuing a `SIGKILL` directly from kernel space, ensuring reliability that user-space monitors cannot provide.

### 5. Scheduling Behavior

The project demonstrates the Completely Fair Scheduler (CFS). Processes with lower nice values (higher priority) accumulate virtual runtime (`vruntime`) more slowly, allowing them to receive a significantly larger share of CPU cycles. The experiments confirm that CFS rewards I/O-bound tasks with lower scheduling lag while maintaining weighted fairness for CPU hogs.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** Utilized `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS`.

**Tradeoff:** The network namespace (`CLONE_NEWNET`) was intentionally omitted. Consequently, containers share the host’s network stack, which can lead to port conflicts.

**Justification:** Full network virtualization requires complex virtual ethernet (`veth`) pair bridging and IP management. `PID`, `UTS`, and mount isolation are sufficient for demonstrating the core principles of process and filesystem isolation.

### Supervisor Architecture
**Choice:** Implemented a long-running supervisor using a single-threaded event loop with blocking `accept()` for the control plane.

**Tradeoff:** A single slow CLI command could momentarily block the supervisor from processing other incoming requests.

**Justification:** This drastically simplifies synchronization logic. Since container workloads are handled in separate processes, the slight latency in the control plane—which handles infrequent human commands—is an acceptable compromise.

### IPC and Logging
**Choice:** Used Unix Domain Sockets for the control channel and Pipes for logging.

**Tradeoff:** Pipes are unidirectional, requiring the supervisor to manage multiple file descriptors (one per container).

**Justification:**Unix Domain Sockets provide reliable, connection-oriented communication for discrete commands. The bounded buffer with condition variables handles backpressure naturally, preventing the logger from falling behind.

### Kernel Monitor
**Choice:** Used a mutex instead of a spinlock for the monitored container list in the kernel.

**Tradeoff:** A mutex has slightly higher overhead than a spinlock for extremely short critical sections.

**Justification:** The timer callback iterates the entire list and calls `get_mm_rss()` per entry. Because this is not a short critical section, a spinlock would hold off other CPUs for too long. A mutex allows the kernel to schedule other work while waiting.

### Scheduling Experiments
**Choice:** Used `setpriority()` with nice values rather than Linux cgroups for CPU shares.

**Tradeoff:** Nice values affect the entire process tree, whereas cgroups allow for more granular per-container control.

**Justification:** Nice values are directly observable in the top utility and map cleanly to Completely Fair Scheduler (CFS) weight theory, making the results easier to analyze and verify.

---

## 6. Scheduler Experiment Results

### Experiment 1 — CPU-bound vs CPU-bound with different priorities

| Container | Nice value | Observed CPU% | Weight Ratio (Approx.)
|-----------|------------|---------------|----------------------|
| `cpuhi` | -10 | ~75-80% | 9544 |
| `cpulo` | +10 | ~20-25% | 10 |

**Analysis:** CFS assigns CPU time based on a virtual runtime counter. Processes with lower nice values accumulate virtual runtime more slowly, allowing them to be scheduled more frequently. The observed ~4x difference in CPU share confirms that the high-priority container successfully dominated cycles, consistent with CFS weight theory.

### Experiment 2 — CPU-bound vs I/O-bound

| Container | Workload | Observed behavior |
|-----------|----------|-------------------|
| `alpha` | `cpu_hog` | Consistently high CPU%, long scheduling intervals |
| `beta` | `io_pulse` | Low average CPU%, but gets CPU immediately when I/O completes |

**Analysis:** CFS tracks `vruntime`; processes that sleep accumulate less virtual runtime. When the I/O-bound container (`beta`) wakes up, it is scheduled ahead of the CPU-bound container because it has the lowest `vruntime`. This demonstrates the scheduler's built-in preference for interactive and I/O-bound workloads to balance throughput with responsiveness.
