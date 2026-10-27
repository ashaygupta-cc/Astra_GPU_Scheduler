# ASTRA: Autonomous CPU/GPU Job Scheduler

ASTRA is a terminal-native, single-machine autonomous compute scheduler designed for Linux. It manages heterogeneous workloads across CPU and GPU worker processes, enforces resource admission limits, coordinates IPC via dedicated UNIX socket pairs, persists state in SQLite, and features a live full-screen terminal UI (ncurses).

```
   ___    ____  ______  ____    ___ 
  / _ |  / __/_/_  __/ / _  |  / _ |
 / __ | _\ \   / /   / , _/  / __ |
/_/ |_|/___/  /_/   /_/|_|  /_/ |_|
 Autonomous CPU/GPU Job Scheduler
```

---

## Key Features

- **POSIX Process & Worker Model:** Manages real child processes using `fork`/`execvp`/`posix_spawn`, `waitpid`, and POSIX signal management.
- **Dedicated IPC Protocol:** Bidirectional `AF_UNIX` stream socket pairs with framed, versioned binary headers and payload validation.
- **Pluggable Scheduling Engine:** FCFS, Strict Priority, Priority Aging (starvation prevention), and Shortest Job First (SJF).
- **Resource Admission Control:** Tracks logical CPU slots, estimated RAM, and GPU device availability to prevent overcommitment.
- **Durable Storage & Persistence:** SQLite3 engine with schema migrations for jobs, attempts, workers, and events.
- **DAG Workflow Engine:** Dependency graph execution with cycle detection (Kahn's algorithm) and automated downstream unlocking.
- **Resilience & Deadlock Lab:** Controlled worker fault injection, bounded automatic retries, and Banker's Algorithm deadlock simulation.
- **Full-Screen Terminal UI:** Live ncurses dashboard with tabbed views (Jobs, Workers, Resources, Events, Replay).
- **CPU-First Architecture:** Runs on standard Linux / WSL2 environments; automatically detects and leverages NVIDIA GPU hardware when present.

---

## Quick Start (Ubuntu 24.04 / WSL2)

### 1. Prerequisites
```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git \
  pkg-config libncurses-dev libsqlite3-dev nlohmann-json3-dev
```

### 2. Build with CMake & Ninja
```bash
cmake -B build -G Ninja
cmake --build build
```

### 3. Run Tests
```bash
ctest --test-dir build --output-on-failure
```

### 4. Run ASTRA
```bash
./build/astra --version
./build/astra --help
```

---

## Architecture & Documentation

- [Complete Engineering Blueprint](docs/ASTRA_BLUEPRINT.md)
- [Implementation Plan & Phased Roadmap](docs/implementation-plan.md)
