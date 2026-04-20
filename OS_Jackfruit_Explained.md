# OS-Jackfruit: Repository Explained

> **Team:** Subramani B M (PES1UG24CS473) · Sujal Sachin Yadavi (PES1UG24CS475)  
> **Language:** C (user-space) + C (Linux Kernel Module)  
> **Target OS:** Ubuntu 22.04 / 24.04 VM (native Linux, no WSL, Secure Boot OFF)

---

## Table of Contents

1. [What Is This Project?](#1-what-is-this-project)
2. [Repository Layout](#2-repository-layout)
3. [High-Level Architecture](#3-high-level-architecture)
4. [Source Files — Deep Dive](#4-source-files--deep-dive)
   - [engine.c — The Runtime & Supervisor](#enginec--the-runtime--supervisor)
   - [logger.h — Shared Ring Buffer Definition](#loggerh--shared-ring-buffer-definition)
   - [monitor_ioctl.h — Kernel/User-Space ABI](#monitor_ioctlh--kerneluser-space-abi)
   - [monitor.c — Linux Kernel Module (LKM)](#monitorc--linux-kernel-module-lkm)
   - [memory_hog.c — Memory Pressure Workload](#memory_hogc--memory-pressure-workload)
   - [cpu_hog.c — CPU Burn Workload](#cpu_hogc--cpu-burn-workload)
   - [io_pulse.c — I/O Workload](#io_pulsec--io-workload)
   - [Makefile](#makefile)
5. [Data Flow: End-to-End](#5-data-flow-end-to-end)
6. [Concurrency & Synchronization Design](#6-concurrency--synchronization-design)
7. [Kernel Module Internals](#7-kernel-module-internals)
8. [CLI Command Reference](#8-cli-command-reference)
9. [Scheduler Experiments](#9-scheduler-experiments)
10. [Design Decisions & Tradeoffs](#10-design-decisions--tradeoffs)
11. [Build & Run Cheatsheet](#11-build--run-cheatsheet)

---

## 1. What Is This Project?

**OS-Jackfruit** is a student-built, lightweight **Linux container runtime** written in C. It is an OS coursework project that demonstrates multiple core operating-system concepts in one integrated system:

| Concept | How It's Demonstrated |
|---|---|
| Process isolation | `clone()` with PID, UTS, and mount namespaces |
| Filesystem jailing | `chroot()` into a per-container Alpine rootfs |
| IPC — pipes | Container stdout → supervisor via pipe |
| IPC — UNIX sockets | CLI client ↔ supervisor control channel |
| Shared memory | `mmap` of a POSIX shared-memory ring buffer |
| Producer-consumer | Proxy threads (producers) + logger thread (consumer) |
| Mutex & cond vars | POSIX `pthread_mutex_t` + `pthread_cond_t` |
| Linux Kernel Module | Custom `monitor.ko` for memory enforcement |
| `ioctl` ABI | User-space registers PIDs with the kernel module |
| CFS scheduling | `setpriority()` / nice values alter CPU share |
| Signal handling | `SIGCHLD` reaping, `SIGINT`/`SIGTERM` teardown |

---

## 2. Repository Layout

```
OS-Jackfruit/
├── README.md                  ← Full project documentation (screenshots, analysis)
├── project-guide.md           ← Assignment specification (6 tasks, grading rules)
├── .gitignore
├── .github/
│   └── workflows/             ← GitHub Actions CI smoke-check
├── images/                    ← Demo screenshots (1a.png … 8.png)
└── boilerplate/               ← ALL SOURCE CODE lives here
    ├── engine.c               ← User-space runtime + supervisor (≈395 lines)
    ├── monitor.c              ← Linux Kernel Module – memory enforcer (≈123 lines)
    ├── logger.h               ← Shared ring-buffer struct definition
    ├── monitor_ioctl.h        ← ioctl command ABI shared by engine ↔ module
    ├── memory_hog.c           ← Workload: allocates RAM to hit soft/hard limits
    ├── cpu_hog.c              ← Workload: burns 100% CPU for scheduler tests
    ├── io_pulse.c             ← Workload: writes + syncs in bursts for I/O tests
    ├── hog.c                  ← (helper / alternate hog binary)
    ├── environment-check.sh   ← Preflight script (checks kernel headers, etc.)
    └── Makefile               ← Builds all targets + kernel module
```

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                        Host Linux Kernel                         │
│   ┌──────────────────────┐    ┌──────────────────────────────┐  │
│   │   monitor.ko (LKM)   │    │         CFS Scheduler        │  │
│   │  • /dev/container_   │    │  • vruntime tracks each PID  │  │
│   │    monitor           │    │  • nice value → weight       │  │
│   │  • 1-sec timer loop  │    └──────────────────────────────┘  │
│   │  • send_sig(SIGKILL) │                                       │
│   └──────────┬───────────┘                                       │
│              │ ioctl (register/unregister PID)                   │
│   ┌──────────┴───────────────────────────────────────────────┐  │
│   │              engine (supervisor daemon)                   │  │
│   │                                                           │  │
│   │  ┌────────────┐  ┌────────────┐  ┌──────────────────┐   │  │
│   │  │ UNIX socket│  │ SIGCHLD    │  │  shared-memory   │   │  │
│   │  │ listener   │  │ handler    │  │  ring buffer     │   │  │
│   │  │ (accept)   │  │ (waitpid) │  │  (mmap, 100 slots│   │  │
│   │  └─────┬──────┘  └────────────┘  └──────┬───────────┘   │  │
│   │        │                                  │               │  │
│   │  ┌─────▼──────┐                    ┌─────▼──────┐        │  │
│   │  │ CLI client │                    │ logger     │        │  │
│   │  │ (ps/start/ │                    │ thread     │        │  │
│   │  │  stop/logs)│                    │ (consumer) │        │  │
│   │  └────────────┘                    └────────────┘        │  │
│   │                                                           │  │
│   │  Per container:                                           │  │
│   │  ┌───────────────────────────────────┐                   │  │
│   │  │ clone(CLONE_NEWPID|NEWUTS|NEWNS)  │                   │  │
│   │  │  → container child process        │                   │  │
│   │  │     chroot(rootfs-alpha)          │                   │  │
│   │  │     stdout/stderr → pipe          │                   │  │
│   │  │  ← proxy thread (producer)        │                   │  │
│   │  └───────────────────────────────────┘                   │  │
│   └───────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

There are **two separate IPC paths**:

- **Path A (Logging):** `container stdout` → `pipe` → `proxy thread (producer)` → `ring buffer` → `logger thread (consumer)` → `container_<id>.log` file.
- **Path B (Control):** `CLI client` → `UNIX Domain Socket (/tmp/jackfruit.sock)` → `supervisor accept() loop` → action + response.

---

## 4. Source Files — Deep Dive

### `engine.c` — The Runtime & Supervisor

This is the main binary (`engine`). It serves **dual roles** depending on the first argument:

#### Role 1: Supervisor Daemon (`engine supervisor <rootfs>`)

```
main() when argv[1] == "supervisor"
  │
  ├─ signal(SIGCHLD, handle_sigchld)   ← reaps zombie children
  ├─ signal(SIGINT/SIGTERM, handle_sigint) ← clean shutdown
  ├─ init_log_buffer()                 ← creates POSIX shared memory ring buffer
  ├─ pthread_create(logger_thread)     ← starts the consumer thread
  └─ socket(AF_UNIX) → bind → listen  ← opens /tmp/jackfruit.sock
       └─ accept() loop
              ├─ "ps"    → print all container_t metadata
              ├─ "start" → start_container(is_run=0) → non-blocking
              ├─ "run"   → start_container(is_run=1) → blocks until exit
              ├─ "stop"  → kill(SIGKILL, host_pid)
              └─ "logs"  → open container_<id>.log, read, send back
```

#### Role 2: CLI Client (any other subcommand)

```
main() else
  │
  ├─ socket(AF_UNIX) → connect(/tmp/jackfruit.sock)
  ├─ write(cmd string assembled from argv)
  └─ read + print response → exit
```

#### Key Data Structures

**`container_t`** — one slot per container, up to `MAX_CONTAINERS = 10`:

```c
typedef struct {
    char id[32];           // human name, e.g. "alpha"
    pid_t host_pid;        // PID as seen by the host kernel
    char state[16];        // "running" / "stopped" / "killed"
    char rootfs[256];      // path to per-container rootfs copy
    char command[256];     // shell command to exec inside
    int pipe_fd;           // write-end of the pipe, given to container
    time_t start_time;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_val;
} container_t;
```

**`proxy_args_t`** — argument packet forwarded to each proxy thread:

```c
typedef struct {
    int pipe_fd;          // read-end of the pipe
    char container_id[32];
} proxy_args_t;
```

#### Container Launch — `start_container()`

1. Find a free slot in `containers[]`.
2. `pipe(pipefd)` — creates a pipe; write-end goes to the container, read-end stays in the supervisor.
3. `clone(container_main, stack, CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, config)` — forks into new namespaces.
4. `register_kernel_monitor(pid, soft, hard, id)` — opens `/dev/container_monitor` and calls `ioctl(MONITOR_REGISTER)`.
5. Spawns a **proxy thread** (`container_proxy_thread`) that reads the pipe and calls `push_log()`.
6. For `start`: immediately sends `[OK]` back to CLI. For `run`: blocks with `waitpid()`.

#### Inside the Container — `container_main()`

```
container_main(config)
  ├─ sethostname(config->id)     ← UTS namespace: sets the hostname
  ├─ dup2(pipe_fd, 1)            ← redirect stdout → pipe
  ├─ dup2(pipe_fd, 2)            ← redirect stderr → pipe
  ├─ setpriority(PRIO_PROCESS, 0, nice_val)  ← CFS weight
  ├─ chroot(config->rootfs)      ← jail filesystem to rootfs copy
  ├─ chdir("/")
  ├─ mount("proc", "/proc", "proc", ...)  ← make ps/top work inside
  └─ execl("/bin/sh", "sh", "-c", command)  ← replace process image
```

#### Signal Handlers

- **`handle_sigchld`** — called when any child exits. Loops `waitpid(-1, WNOHANG)` to reap all done children, marks their `state` as `"stopped"` or `"killed"`, and calls `unregister_kernel_monitor()`.
- **`handle_sigint`** — sends `SIGKILL` to all running containers, unlinks the socket and shared memory, then exits cleanly.

---

### `logger.h` — Shared Ring Buffer Definition

Defines the **bounded buffer** used to pass log lines between proxy threads and the logger thread.

```c
#define SHM_NAME     "/jackfruit_logger"
#define MAX_LOGS     100          // buffer capacity
#define LOG_MSG_SIZE 256          // max bytes per message

typedef struct {
    char container_id[32];
    char message[LOG_MSG_SIZE];
} log_entry_t;

typedef struct {
    log_entry_t buffer[MAX_LOGS]; // circular array
    int head;                      // consumer reads here
    int tail;                      // producer writes here
    int count;                     // current occupancy

    pthread_mutex_t lock;          // mutual exclusion
    pthread_cond_t  not_empty;     // consumer waits here when empty
    pthread_cond_t  not_full;      // producer waits here when full
} ring_buffer_t;
```

The mutex and cond vars have `PTHREAD_PROCESS_SHARED` set so they work across processes (through the `mmap`'d shared memory).

---

### `monitor_ioctl.h` — Kernel/User-Space ABI

Defines the structures and `ioctl` command codes shared between `engine.c` and `monitor.c`. This header is included by both user-space and kernel-space code (gated via `#ifdef __KERNEL__`).

```c
struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[32];
};

#define MONITOR_MAGIC      'M'
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)
```

- `MONITOR_REGISTER` — tells the kernel module to start tracking this PID with these memory limits.
- `MONITOR_UNREGISTER` — removes a PID from kernel tracking (called on container stop/kill).

---

### `monitor.c` — Linux Kernel Module (LKM)

This is the **kernel-space component**. It compiles into `monitor.ko` and is loaded with `sudo insmod monitor.ko`.

#### What it registers
- A **character device** at major number assigned by `register_chrdev()`, exposed as `/dev/container_monitor`.
- An **`ioctl` handler** (`dev_ioctl`) to receive register/unregister requests from user-space.
- A **kernel timer** (`monitor_timer`) that fires every **1 second**.

#### Internal State

```c
struct target_info {
    pid_t pid;
    unsigned long soft_limit;   // in KB
    unsigned long hard_limit;   // in KB
    int in_use;
};

static struct target_info targets[MAX_TARGETS];  // up to 16 containers
static DEFINE_SPINLOCK(targets_lock);            // protects the array
```

A **spinlock** (not a mutex) is used because the timer fires in an interrupt context where sleeping is not allowed.

#### Timer Callback — `monitor_check()`

Every second, for each registered PID:

```
1. find_get_pid(pid)           ← get kernel PID struct
2. get_pid_task(pid_struct)    ← get task_struct
3. get_mm_rss(task->mm)        ← read Resident Set Size in pages
4. convert to KB (× PAGE_SIZE/1024)
5. if rss > hard_limit  → pr_alert(...) + send_sig(SIGKILL, task, 1)
   elif rss > soft_limit → pr_warn(...)
6. put_task_struct() + put_pid()
7. mod_timer(..., jiffies + 1000ms)   ← re-arm for next second
```

The critical difference from user-space cgroups: `send_sig(SIGKILL, task, 1)` issues the kill **directly to the kernel `task_struct`** — no user-space involvement needed.

---

### `memory_hog.c` — Memory Pressure Workload

Designed to trigger soft and hard memory limits in the LKM.

```
Behavior:
  Loop forever:
    malloc(chunk_mb × 1MiB)
    memset(mem, 'A', ...)   ← actually touches pages, so RSS grows
    print progress
    sleep(sleep_ms)
```

- Default: **8 MiB per second**.
- Arguments: `chunk_mb` and `sleep_ms` are configurable.
- Linked **statically** (`-static`) so it can run inside the Alpine rootfs without glibc.

**Why `memset`?** — `malloc` alone does not increase RSS; pages are only allocated on first write (demand paging). `memset` forces every byte to be touched, so the kernel actually maps physical pages and RSS grows.

---

### `cpu_hog.c` — CPU Burn Workload

Used to demonstrate CFS (Completely Fair Scheduler) behavior at different nice values.

```
Behavior:
  time_limit = seconds (default 10)
  while elapsed < time_limit:
    accumulator = accumulator * 1664525 + 1013904223   ← trivial LCG, keeps CPU busy
    if 1 second passed: print progress
```

- No I/O, no sleep — purely CPU-bound, meaning the CFS `vruntime` advances quickly.
- Used in pairs: one container with `--nice -10` (high priority) vs one with `--nice 10` (low priority).

---

### `io_pulse.c` — I/O Workload

Complements `cpu_hog` for the scheduler comparison experiment.

```
Behavior:
  open /tmp/io_pulse.out
  for i in 1..iterations (default 20):
    write "io_pulse iteration=N\n"
    fsync(fd)            ← forces disk flush, blocks on I/O
    print to stdout
    sleep(sleep_ms)      ← voluntarily gives up CPU
```

- Because it sleeps and blocks on I/O, its CFS `vruntime` accumulates **slowly** — when it wakes up and becomes runnable, CFS schedules it ahead of the CPU-bound container (which has a large `vruntime`).
- This demonstrates **CFS's preference for I/O-bound tasks** for responsiveness.

---

### `Makefile`

```makefile
all:  builds engine, memory_hog, cpu_hog, io_pulse  +  monitor.ko
ci:   builds only user-space targets (no kernel headers required — for GitHub CI)
clean: removes all built artifacts and log files
```

Key build details:
- `engine` links with `-lpthread` for POSIX threads.
- `memory_hog`, `cpu_hog`, `io_pulse` link with `-static` by default so they can run inside the Alpine rootfs without the host's glibc.
- `monitor.ko` is built via the standard kernel build system: `$(MAKE) -C $(KDIR) M=$(PWD) modules`.

---

## 5. Data Flow: End-to-End

### Container Lifecycle

```
CLI:  sudo ./engine start alpha ./rootfs-alpha "/bin/sh -c 'echo hello'"
         │
         └─ connect(jackfruit.sock) → send "start alpha ./rootfs-alpha ..."
                                                │
Supervisor:                      accept() → parse → start_container()
                                                │
                                    pipe(pipefd) created
                                                │
                              clone(CLONE_NEWPID|NEWUTS|NEWNS)
                                    │                │
                            (supervisor side)   (container child)
                                    │           sethostname("alpha")
                             close(pipefd[1])   dup2(pipefd[1], stdout)
                                    │           chroot(rootfs-alpha)
                             register_kernel_monitor(pid)   mount(/proc)
                                    │           exec(/bin/sh -c 'echo hello')
                             proxy_thread spawned  │
                                    │          stdout output → pipe
                                    │                │
                             read(pipefd[0]) ←───────┘
                                    │
                             push_log("alpha", "hello\n")
                                    │
                             ring_buffer[tail++]   ← with mutex + cond broadcast
                                    │
                             logger_thread wakes (cond wait → signal)
                                    │
                             fopen("container_alpha.log", "a")
                             fprintf(log_file, "[timestamp] hello\n")
```

### Memory Limit Enforcement

```
monitor.ko timer fires every 1 second:
  → get_mm_rss(task->mm) for each registered PID
  → if RSS > soft_limit: pr_warn("[container_monitor] WARNING: ...")
  → if RSS > hard_limit: send_sig(SIGKILL, task, 1)
                                    │
                            container process killed
                                    │
Supervisor: SIGCHLD received
  → waitpid() reaps the child
  → containers[i].state = "killed"
  → unregister_kernel_monitor(pid)
```

---

## 6. Concurrency & Synchronization Design

### Actors

| Thread | Role |
|---|---|
| Main supervisor thread | `accept()` loop, spawns containers |
| `logger_thread` | **Consumer** — dequeues from ring buffer, writes to log files |
| `container_proxy_thread` (one per container) | **Producer** — reads pipe from container, enqueues into ring buffer |

### Shared Resources & Protection

| Resource | Shared between | Protection |
|---|---|---|
| `ring_buffer_t` (in shared memory) | All proxy threads + logger thread | `pthread_mutex_t lock` |
| `buffer->count == 0` state | Producer checking full/empty | `pthread_cond_t not_empty` |
| `buffer->count == MAX_LOGS` state | Consumer checking full | `pthread_cond_t not_full` |
| `containers[]` array | Main thread + SIGCHLD handler | De-facto safe (signal handler is atomic) |

### Producer (`push_log`) Pattern

```c
pthread_mutex_lock(&buf->lock);
while (buf->count == MAX_LOGS)           // WAIT if full (blocks, no spin)
    pthread_cond_wait(&buf->not_full, &buf->lock);
// critical section: write entry at tail
buf->tail = (buf->tail + 1) % MAX_LOGS;
buf->count++;
pthread_cond_signal(&buf->not_empty);    // wake consumer
pthread_mutex_unlock(&buf->lock);
```

### Consumer (`logger_thread`) Pattern

```c
pthread_mutex_lock(&buf->lock);
while (buf->count == 0)                  // WAIT if empty
    pthread_cond_wait(&buf->not_empty, &buf->lock);
// critical section: read entry at head
buf->head = (buf->head + 1) % MAX_LOGS;
buf->count--;
pthread_cond_signal(&buf->not_full);     // wake any blocked producer
pthread_mutex_unlock(&buf->lock);
```

**Why condition variables instead of busy-waiting?**  
Busy-waiting (spin-checking `count`) wastes CPU cycles. Condition variables suspend the thread in the kernel until signaled — zero CPU cost while waiting.

---

## 7. Kernel Module Internals

### Initialization (`monitor_init`)

1. Zeroes all `targets[]` slots.
2. Calls `register_chrdev(0, "container_monitor", &fops)` — kernel assigns a dynamic major number and creates the char device.
3. `timer_setup` + `mod_timer` — arms the 1-second recurring timer.

### Cleanup (`monitor_exit`)

1. `del_timer` — cancels the timer so no callbacks fire after unload.
2. `unregister_chrdev` — removes the device.

### Spinlock vs Mutex in the Kernel

The timer callback (`monitor_check`) runs in a **softirq (interrupt) context** — it cannot sleep, so `mutex_lock` (which can sleep) is forbidden. A **`spinlock_t`** is used instead: it busy-spins briefly without sleeping, safe in interrupt context.

The `ioctl` handler (`dev_ioctl`) also holds `targets_lock` (spinlock) briefly while reading or writing the `targets[]` array.

---

## 8. CLI Command Reference

| Command | Behavior |
|---|---|
| `engine supervisor <rootfs>` | Starts the long-running daemon; blocks and listens |
| `engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]` | Launches container in background; returns immediately |
| `engine run <id> <rootfs> <cmd> [...]` | Launches container and blocks until it exits; returns exit code |
| `engine ps` | Lists all tracked containers (ID, host PID, state) |
| `engine logs <id>` | Streams the `container_<id>.log` file |
| `engine stop <id>` | Sends `SIGKILL` to the container's host PID |

**Default memory limits**: 40 MiB soft, 64 MiB hard (if `--soft-mib`/`--hard-mib` not provided).

---

## 9. Scheduler Experiments

The project includes two controlled experiments that use the workload binaries to probe Linux CFS behavior.

### Experiment 1 — Priority Difference (CPU vs CPU)

```bash
sudo ./engine start cpuhi ./rootfs-alpha "/bin/sh -c 'while true; do true; done'" --nice -10
sudo ./engine start cpulo ./rootfs-beta  "/bin/sh -c 'while true; do true; done'" --nice 10
top -bn1 | head -n 20
```

| Container | Nice | Observed CPU% | CFS Weight |
|---|---|---|---|
| `cpuhi` | −10 | ~75–80% | 9544 |
| `cpulo` | +10 | ~20–25% | 10 |

**Why?** CFS tracks each task's `vruntime`. For a task with nice −10, `vruntime` increments at a fraction of real time (weight 9544 vs default 1024). The task always has the smallest `vruntime` in the red-black tree, so the scheduler picks it most often.

### Experiment 2 — CPU-Bound vs I/O-Bound

| Container | Workload | Observed Behavior |
|---|---|---|
| `alpha` | `cpu_hog` | Consistently high CPU% |
| `beta` | `io_pulse` | Low average CPU%, but scheduled immediately on I/O completion |

**Why?** While `io_pulse` sleeps (waiting for `fsync` or `usleep`), its `vruntime` stays low. When it wakes, CFS finds it has the smallest `vruntime` and schedules it ahead of `cpu_hog` — demonstrating the scheduler's natural bias toward interactive/I/O-bound processes for responsiveness.

---

## 10. Design Decisions & Tradeoffs

| Decision | Choice Made | Tradeoff |
|---|---|---|
| **Network isolation** | `CLONE_NEWNET` omitted | Containers share host IP stack — simpler to implement, but no real network isolation |
| **Control IPC** | Single-threaded blocking `accept()` on UNIX socket | Simple synchronization; slight risk of stall if a client hangs |
| **CPU priority** | `setpriority()` / nice values | Less strict than cgroup CPU quotas, but maps directly to CFS weights and is easy to observe in `top` |
| **Memory enforcement** | Custom LKM with 1-second `mod_timer` | No user-space polling needed; direct kernel `send_sig`; but 1-second granularity means a process could exceed limits momentarily |
| **Spinlock in LKM** | `DEFINE_SPINLOCK` | Ultra-low latency in interrupt/timer context; busy-waits CPU on contention (acceptable for very short critical sections) |
| **Logging pipeline** | Shared-memory ring buffer via `mmap` + POSIX cond vars | Zero-copy between producer and consumer; safe for concurrent access; avoids busy-waiting |
| **Filesystem isolation** | `chroot()` | Simpler than `pivot_root`; note: `chroot` can be escaped via `..` traversal if the container has `CAP_SYS_CHROOT` |

---

## 11. Build & Run Cheatsheet

```bash
# 1. Install dependencies
sudo apt update && sudo apt install -y build-essential linux-headers-$(uname -r)

# 2. Build everything
cd boilerplate
make

# 3. Prepare rootfs
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# 4. Load the kernel module
sudo insmod monitor.ko
ls -l /dev/container_monitor   # verify device exists
sudo dmesg | tail -5           # should say "Loaded successfully"

# 5. Start supervisor (Terminal 1 — blocks here)
sudo ./engine supervisor ./rootfs-base

# 6. Launch containers (Terminal 2)
sudo ./engine start alpha ./rootfs-alpha "/bin/sh -c '...'"
sudo ./engine start beta  ./rootfs-beta  "/bin/sh -c 'sleep 1000'"
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha

# 7. Memory limit test
cp memory_hog ./rootfs-alpha/
sudo ./engine start memtest ./rootfs-alpha "/memory_hog" --soft-mib 10 --hard-mib 20
sudo dmesg -w | grep container_monitor

# 8. Cleanup
sudo ./engine stop beta
sudo rmmod monitor
```

---

*Document generated from source: `OS-Jackfruit/boilerplate/` (engine.c, monitor.c, logger.h, monitor_ioctl.h, memory_hog.c, cpu_hog.c, io_pulse.c, Makefile) and `README.md` / `project-guide.md`.*
