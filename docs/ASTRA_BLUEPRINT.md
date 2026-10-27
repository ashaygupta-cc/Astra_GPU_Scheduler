# ASTRA — Autonomous CPU/GPU Job Scheduler
## Complete Engineering Blueprint & Antigravity Master Prompt

**Project:** Operating Systems · C++20 System Programming · Systems Design  
**Timeline:** 6 weeks / approximately 1.5 months  
**Platform:** Ubuntu 24.04 LTS / Linux (WSL2 supported)  
**Interface:** CLI + ncurses terminal UI  
**Core:** POSIX processes, IPC, synchronization, scheduling  
**Optional:** CUDA GPU workloads when supported  
**Scope:** Single-machine scheduler; distributed execution is out of scope for the first release.  

**Implementation principle:** Build a correct CPU-only MVP first, then add advanced capabilities in tested milestones. Never represent simulated GPU data as real hardware telemetry.

---

## 1. Product Vision

Build ASTRA, a terminal-native autonomous compute scheduler that accepts CPU/GPU workloads, chooses when and where eligible jobs run, launches real worker processes, monitors their state, coordinates communication using POSIX IPC, protects shared state with synchronization primitives, detects selected deadlock scenarios, and recovers from controlled worker failures.

ASTRA is **not** an operating-system kernel and does not replace Linux's CPU scheduler. Linux schedules processes and threads on CPU cores. ASTRA schedules application jobs onto its worker pool. CUDA or another GPU runtime executes GPU kernels.

### A Typical Demo:
1. User submits CPU and GPU jobs.
2. ASTRA validates executable, arguments, dependencies, and resource request.
3. Jobs enter the queue and are persisted in SQLite.
4. The scheduler checks priority, dependencies, CPU/GPU eligibility, and configured limits.
5. A worker receives the job over an IPC socket/pipe.
6. The child process executes it and reports status/exit code.
7. ASTRA records timing, events, and available telemetry.
8. Failed jobs may be retried under a bounded policy; cooperative jobs may resume from checkpoints.
9. The TUI shows jobs, workers, decisions, and execution history.

---

## 2. Scope & Honest Claims

### In Scope
- Interactive CLI and ncurses dashboard.
- Multiple real worker processes.
- POSIX process creation and monitoring: `fork`, `execvp`/`posix_spawn`, `waitpid`, signals (`SIGTERM`, `SIGKILL`, `SIGCHLD`).
- IPC through dedicated bidirectional `socketpair` (`AF_UNIX`, `SOCK_STREAM`) or pipes per worker.
- In-process synchronization using `std::mutex` and `std::condition_variable`; process-shared primitives only where truly needed.
- FCFS, priority, priority aging, and optional SJF.
- Resource admission for CPU slots, estimated RAM, GPU availability, and estimated GPU memory.
- SQLite job metadata/history; JSON configuration/workflows; JSONL event logs.
- DAG workflows, cycle detection (Kahn's algorithm / DFS), and dependency management.
- Controlled fault injection, bounded retry, worker restart.
- Deadlock lab using a wait-for graph and Banker's Algorithm for simulated resources.
- Optional CUDA sample when hardware/toolkit are available.
- Tests, benchmarks, scripts, docs, and reproducible demos.

### Not in the MVP
- Production cluster scheduling or multi-machine execution.
- Arbitrary live migration of a running process.
- Guaranteed checkpointing of arbitrary native processes.
- Arbitrary CUDA-kernel preemption.
- A web dashboard.
- A claim that ASTRA replaces the Linux kernel scheduler.
- A database server; SQLite is sufficient.

---

## 3. Feature Priorities

### P0 — Must Ship
1. CLI/help and command parser.
2. Job model and validated state machine.
3. Submit/list/inspect/cancel/logs.
4. Worker manager using real child processes.
5. Robust IPC and error handling.
6. FCFS and priority scheduling.
7. CPU resource limits/admission.
8. Thread-safe event queue.
9. SQLite persistence.
10. ncurses dashboard with jobs, workers, events, basic metrics.
11. Success/failure/cancel/queueing integration tests.
12. Build/run docs and scripted demo.

### P1 — Differentiators
- Priority aging and starvation prevention.
- DAG workflow engine.
- Worker crash detection, bounded retry, restart.
- Fault-injection commands.
- Resource-aware CPU/GPU eligibility.
- Event timeline replay.
- Deadlock lab and victim recovery.
- Bounded worker-pool auto-scaling.
- JSON config and structured logs.
- Scheduling-policy benchmarks.

### P2 — Stretch Goals
- CUDA GPU workload and GPU telemetry.
- Cooperative checkpoint/resume.
- GPU-memory admission estimates.
- Shared-memory telemetry ring buffer.
- SJF using historical estimates.
- Polished ASCII graphs.
- Versioned plugin ABI for trusted workloads.

**Rule:** P0 first. Do not start P2 until P0 is stable.

---

## 4. Architecture

```
                 CLI / ncurses TUI
                         │
             Command parser + validation
                         │
  Scheduler Core: policy, queue, dependencies, admission
         ├── Resource Manager (track CPU slots, RAM, GPU)
         ├── SQLite Job Store (durable job/run history)
         └── Event Recorder (JSONL / timeline replay)
                         │
  Worker Manager: launch, monitor, cancel, retry, reap
                         │
          Per-worker pipes or socketpair
           ├── Worker 1 (CPU job)
           ├── Worker 2 (CPU job)
           └── Worker N (GPU job -> CUDA runtime)
```

### Component Rules
- **TUI:** Displays snapshots and submits commands; does not mutate internal scheduler state directly.
- **Scheduler Core:** Owns job transitions and scheduling decisions.
- **Resource Manager:** Tracks reservations and clearly distinguishes estimates from measured values.
- **Worker Manager:** Owns child lifecycle, IPC, signals, waitpid, restart decisions.
- **Job Store:** Persists definitions and state snapshots in SQLite.
- **Event Recorder:** Appends immutable events for replay/auditing.
- **GPU adapter:** Optional CUDA backend and sample workloads.

---

## 5. Technology Stack

| Layer | Choice | Purpose |
|---|---|---|
| Language | C++20 | Core implementation |
| OS | Ubuntu 24.04 / Linux (WSL2) | POSIX APIs |
| Build | CMake + Ninja | Reproducible builds |
| CLI | Custom parser or CLI11 | Command parsing |
| TUI | ncurses / ncursesw | Full-screen terminal UI |
| Persistence | SQLite3 | Durable job history |
| Config | nlohmann/json | Config/workflow JSON |
| Logging | Custom structured logger / spdlog | Diagnostic logs |
| Tests | CTest + custom test harness / Catch2 | Unit/integration tests |
| Debugging | gdb, strace, valgrind | Process/IPC debugging |
| Sanitizers | ASan, UBSan, TSan | Memory/concurrency testing |
| GPU | CUDA Toolkit, optional | NVIDIA GPU jobs |
| Version control | Git | Collaboration and milestones |

*Do not add a web backend, frontend framework, Redis, Kafka, or remote database in the MVP.*

---

## 6. Job Model and State Machine

A job contains: stable ID, name, executable path, argv array, priority, optional deadline, resource request, dependencies, state, retry policy, worker/PID, exit code, timestamps, and attempt history.

### Job States
```
enum class JobState {
    Submitted,
    WaitingForDependencies,
    Queued,
    Admitted,
    Running,
    Checkpointing,
    Paused,
    Retrying,
    Succeeded,
    Failed,
    Cancelled,
    Blocked
};
```

### Legal Transitions
- `Submitted` -> `WaitingForDependencies` or `Queued`
- `WaitingForDependencies` -> `Queued` when all dependencies succeed
- `Queued` -> `Admitted` when resource checks pass
- `Admitted` -> `Running` when a worker accepts the job
- `Running` -> `Succeeded` or `Failed` after completion
- `Running` -> `Retrying` -> `Queued` when retry policy allows
- `Queued` -> `Cancelled` immediately
- `Running` -> `Cancelled` only after termination is confirmed
- `Running` -> `Checkpointing` -> `Paused` only for checkpoint-aware workloads
- `Blocked`: unsatisfied/failed dependency or invalid policy condition; stores reason.

*Enforce transitions in one central function. Emit an event for every valid transition and reject illegal transitions.*

---

## 7. CLI Specification

Support one-shot commands and an interactive REPL:
```
astra help
astra submit <workload> [options]
astra ps [--all]
astra inspect <job-id>
astra cancel <job-id>
astra logs <job-id>
astra top
astra workers
astra policy show
astra policy set <fcfs|priority|sjf>
astra config show
astra workflow submit <file.json>
astra events [--job <id>]
astra replay <run-id>
astra deadlock demo
astra demo
astra shutdown
```

### Options
```
--cpu-slots N
--ram-mb N
--gpu
--gpu-preferred
--gpu-memory-mb N
--priority N
--max-retries N
--timeout SECONDS
--depends-on ID,ID,...
--checkpointable
```

---

## 8. IPC Protocol

- Bidirectional `socketpair(AF_UNIX, SOCK_STREAM, 0)` per worker.
- Framed binary / text messages with fixed header:
  1. Protocol version (`uint32_t`)
  2. Message type (`uint32_t`)
  3. Job ID (`uint64_t`) and Request ID (`uint64_t`)
  4. Payload length (`uint32_t`)
  5. Status / error code (`int32_t`)
  6. Bounded payload buffer
- Message types: `RUN_JOB`, `JOB_STARTED`, `JOB_PROGRESS`, `JOB_COMPLETED`, `JOB_FAILED`, `CANCEL_JOB`, `HEARTBEAT`, `SHUTDOWN`, `ERROR`.
- Exact-length reads/writes handling `EINTR` and partial transfers.

---

## 9. Antigravity Master Instructions

Follow phased implementation strictly:
- Phase 0: Inspect and Plan (this milestone)
- Phase 1: Domain and Storage
- Phase 2: CLI
- Phase 3: Workers and IPC
- Phase 4: Scheduler
- Phase 5: TUI and Telemetry
- Phase 6: DAG
- Phase 7: Reliability / Deadlock
- Phase 8: GPU / Checkpoint
- Phase 9: Replay / Release
