# ASTRA Implementation Plan & Architectural Blueprint
**Phase 0: Environment Inspection, System Architecture, & Phased Roadmap**

---

## 1. Environment & Toolchain Inspection Report

An exhaustive system inspection was conducted on the host machine and WSL2 subsystem:

| Component | Target Requirement | Detected Environment | Status | Notes |
|---|---|---|---|---|
| **Host OS** | Windows 11 / WSL2 | Windows x86_64 + WSL2 | Verified | Seamless WSL2 execution |
| **Linux Subsystem** | Ubuntu 24.04 LTS (Noble) | Ubuntu 24.04 LTS (Linux 6.18.54.1-microsoft-standard-WSL2) | Verified | Exact target OS |
| **Compiler** | C++20 compliant (GCC >= 13 or Clang >= 16) | GCC 13.3.0 (`/usr/bin/g++`) | Verified | Full C++20 feature support (`<chrono>`, concepts, `std::jthread`, `std::format`) |
| **Build System** | CMake >= 3.24 + Ninja | CMake 3.28.3, Ninja 1.11.1 | Verified | Fast, reproducible Ninja builds |
| **TUI Library** | ncurses / ncursesw | `libncurses-dev` 6.4.20240113 | Verified | Supports wide-character / color rendering |
| **Database** | SQLite3 >= 3.35 | `libsqlite3-dev` 3.45.1 | Verified | In-process relational storage with ACID guarantees |
| **JSON Library** | nlohmann/json | `nlohmann-json3-dev` 3.11.3 | Verified | Header-only modern C++ JSON parser |
| **GPU Telemetry / Hardware** | Optional NVIDIA GPU | NVIDIA GeForce RTX 3050 Laptop GPU (4096 MiB) | Verified | Driver 591.84, CUDA 13.1 driver capability |
| **CUDA Toolkit (nvcc)** | Optional for P2 | Not installed | Verified (P0/CPU-First) | System follows strict CPU-first design; GPU scheduler testing can use real hardware detection or transparent simulated pool |

---

## 2. Architecture & Ownership Model

ASTRA adheres to a single-responsibility, modular architecture with clear process and thread boundaries.

```
+-------------------------------------------------------------------------------+
|                             ASTRA Process Architecture                        |
|                                                                               |
|  +--------------------+     Shared Read Snapshots     +--------------------+  |
|  |     TUI View       |<------------------------------|   Scheduler Core   |  |
|  | (ncurses terminal) |                               | (Event Loop Thread)|  |
|  +---------+----------+                               +---------+----------+  |
|            | Submits Commands                                   |             |
|            v                                                    v             |
|  +--------------------+                               +--------------------+  |
|  |   Command Parser   |                               |  Resource Manager  |  |
|  |   & CLI Dispatch   |                               | (Slots, RAM, GPU)  |  |
|  +--------------------+                               +---------+----------+  |
|                                                                 |             |
|  +--------------------+                               +--------------------+  |
|  | SQLite Storage Eng |<====== Thread-safe Queue =====|   Worker Manager   |  |
|  |   (Persistence)    |                               |   (Process Supv)   |  |
|  +--------------------+                               +---------+----------+  |
+-----------------------------------------------------------------|-------------+
                                                                  | AF_UNIX socketpairs
                     +--------------------------------------------+
                     |                            |
                     v                            v
          +--------------------+       +--------------------+
          |   Worker Child 1   |       |   Worker Child N   |
          |  (CPU Workload)    |       |  (GPU / Workload)  |
          +--------------------+       +--------------------+
```

### 2.1 Thread Boundaries & Concurrency
- **Scheduler Event Thread:** The sole authority for state transitions (`JobState`), policy decisions (FCFS, Priority, Aging), and admission control. No background thread mutates job state directly.
- **Worker Monitor / I/O Threads:** Dedicated poll/epoll or non-blocking threads read IPC messages from child workers and push typed event packets into a thread-safe bounded event queue.
- **Storage Subsystem:** SQLite access is channeled through a thread-safe storage abstraction (`JobStore`) that serializes transactional writes while permitting fast reads.
- **TUI Thread:** Operates on immutable state snapshots taken at fixed refresh intervals (e.g. 10 Hz), preventing lock contention with time-critical scheduling loops.

### 2.2 Memory and Resource RAII
- **File Descriptors:** Encapsulated in `UniqueFd` / `FdWrapper` RAII classes to eliminate socket/pipe descriptor leaks.
- **Child Bookkeeping:** Child processes are strictly registered in an active process table. Signal handling (`SIGCHLD`) or explicit `waitpid(WNOHANG)` reaps terminated processes immediately, preventing zombie leaks.
- **No Cross-Process STL Pointers:** Inter-process communication strictly uses serialized binary/JSON payloads across UNIX domain sockets, never raw pointers or STL containers in shared memory.

---

## 3. IPC Protocol Design

Communication between the ASTRA daemon and child worker processes occurs over dedicated bidirectional UNIX domain socket pairs (`socketpair(AF_UNIX, SOCK_STREAM, 0)`).

### 3.1 Frame Structure (Fixed 28-Byte Header + Variable Bounded Payload)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Magic (0x41535452 = 'ASTR')      |    Protocol Version  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Message Type         |          Status / Error       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          Job ID (High)                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          Job ID (Low)                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Request ID                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Length                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Payload Data (Length bytes) ...           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 3.2 Message Types
1. `MSG_RUN_JOB`: Parent -> Worker. Workload path, arguments, resource limits, checkpoint paths.
2. `MSG_JOB_STARTED`: Worker -> Parent. Child PID, start timestamp.
3. `MSG_JOB_PROGRESS`: Worker -> Parent. Percent complete, metrics, current step.
4. `MSG_JOB_COMPLETED`: Worker -> Parent. Exit code, execution duration, output summary.
5. `MSG_JOB_FAILED`: Worker -> Parent. Exit code, termination signal, error diagnostic.
6. `MSG_CANCEL_JOB`: Parent -> Worker. Request orderly termination (`SIGTERM` followed by timeout and `SIGKILL`).
7. `MSG_HEARTBEAT`: Bidirectional. Liveness probe and health verification.
8. `MSG_SHUTDOWN`: Parent -> Worker. Drain in-flight work and exit.

### 3.3 Framing Guarantees
- Handles partial reads and writes gracefully using looped `read_exact` / `write_exact` wrappers.
- Automatically handles `EINTR` interrupts.
- Rejects frames exceeding `MAX_PAYLOAD_SIZE` (64 KB) to protect against malformed or malicious messages.

---

## 4. Error-Handling Conventions

1. **System Calls & POSIX APIs:**
   Wrapped in RAII helpers. Any system call failure captures `errno` and formats an `AstraStatus` or throws a typed `SystemException`.
2. **Result Pattern (`AstraResult<T>`):**
   Non-exceptional control flow errors (validation, admission failures, DAG cycle rejection) use an explicit `AstraResult<T>` or `std::expected`-style return type.
3. **Graceful Degradation:**
   - Missing CUDA runtime -> fallback to CPU-only execution with explicit diagnostic indicator.
   - Child process crash -> intercepted via `SIGCHLD` / `waitpid`, logged as a failure attempt, and worker pool restored.

---

## 5. Phased Roadmap & Acceptance Criteria

### Phase 0: Inspect & Plan (Current)
- **Goal:** Verify compiler and dependencies; establish repository skeleton, CMake build system, core documentation, and minimal executable.
- **Acceptance:** Minimal executable compiles under C++20 with CMake and Ninja, prints system info, and exits 0.

### Phase 1: Domain & Storage
- **Goal:** Define core types (`Job`, `ResourceRequest`, `JobState`, `JobAttempt`, `WorkerInfo`, `Event`), implement atomic state machine transitions, and SQLite storage engine with migration support.
- **Acceptance:** Unit tests verify valid transitions, reject illegal transitions, and confirm SQLite round-trip persistence across restarts.

### Phase 2: CLI Interface
- **Goal:** Comprehensive CLI command parser (`submit`, `ps`, `inspect`, `cancel`, `logs`, `workers`, `policy`, `events`, `shutdown`) and interactive REPL mode.
- **Acceptance:** Command line accepts valid arguments, rejects malformed inputs, and executes commands safely without shell injection.

### Phase 3: Workers & IPC
- **Goal:** Worker lifecycle manager, process creation via `posix_spawn` / `fork`+`execvp`, bidirectional IPC socketpairs, stdout/stderr capture, process reaping, and signal management.
- **Acceptance:** Real processes are launched, stdout/stderr captured, cancellation terminates processes cleanly, and no zombie processes remain.

### Phase 4: Scheduler Core
- **Goal:** Scheduling policies (FCFS, Priority, Priority Aging), admission control with CPU slot reservations, thread-safe event queue.
- **Acceptance:** Jobs are dispatched in policy order, respects CPU limits, and starvation prevention elevates aged jobs.

### Phase 5: Terminal UI & Telemetry
- **Goal:** ncurses-based interactive dashboard with tabbed views (Jobs, Workers, Resources, Events, Details), keyboard navigation, and live metrics.
- **Acceptance:** TUI updates smoothly at 10 Hz without blocking the scheduler, correctly displays live worker states, and allows interactive job cancellation.

### Phase 6: DAG Workflows
- **Goal:** Workflow definition parser (JSON), Kahn's algorithm for dependency ordering and cycle detection, automated dependency unlocking upon job success.
- **Acceptance:** Complex DAG pipelines execute in correct topological order; circular dependencies are detected and rejected at submission.

### Phase 7: Reliability & Deadlock Lab
- **Goal:** Fault injection (worker kill, child SIGSEGV), automatic bounded retry, Banker's algorithm simulation lab, and wait-for graph cycle detection.
- **Acceptance:** Crashed workers are detected and revived; Banker's algorithm correctly classifies safe vs unsafe resource request states.

### Phase 8: GPU & Cooperative Checkpointing
- **Goal:** Optional CUDA vector-addition workload, hardware GPU query via NVML/CUDA driver API, and cooperative checkpoint/restore API.
- **Acceptance:** Works reliably on systems with or without CUDA. Checkpointed workloads can pause and resume from saved state.

### Phase 9: Replay, Benchmarks, & Release
- **Goal:** Event timeline replay (inspect historic executions without re-running workloads), scheduling policy benchmark suites, comprehensive user documentation, and clean demo scripts.
- **Acceptance:** Full end-to-end demo script runs cleanly in under 2 minutes, passing all verification gates.
