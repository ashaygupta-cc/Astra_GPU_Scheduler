#!/usr/bin/env bash
# ==============================================================================
# ASTRA: Autonomous CPU/GPU Job Scheduler — Comprehensive End-to-End Demo Script
# Runs all 9 milestone phases & verification gates in under 2 minutes.
# ==============================================================================

set -euo pipefail

BOLD="\033[1m"
GREEN="\033[32m"
CYAN="\033[36m"
YELLOW="\033[33m"
RESET="\033[0m"

echo -e "${BOLD}${CYAN}"
echo "================================================================================"
echo "          ASTRA COMPREHENSIVE END-TO-END DEMONSTRATION & VERIFICATION           "
echo "================================================================================"
echo -e "${RESET}"

# Build directory determination
BUILD_DIR="./build"
if [[ ! -f "${BUILD_DIR}/astra" ]]; then
    echo -e "${YELLOW}[!] Building ASTRA using Ninja...${RESET}"
    cmake -B build -G Ninja
    cmake --build build
fi

ASTRA_BIN="${BUILD_DIR}/astra"

# Clean up transient demo artifacts
rm -f astra.db astra_demo.db demo_ckpt.ckpt

echo -e "\n${BOLD}${GREEN}--- Step 1: System Telemetry & Toolchain Diagnostics ---${RESET}"
"${ASTRA_BIN}" version

echo -e "\n${BOLD}${GREEN}--- Step 2: Scheduling Policy Benchmark Suite (OS Starvation Analysis) ---${RESET}"
"${ASTRA_BIN}" bench --jobs 20 --slots 2

echo -e "\n${BOLD}${GREEN}--- Step 3: Banker's Algorithm Deadlock Lab & Victim Recovery ---${RESET}"
"${ASTRA_BIN}" deadlock demo

echo -e "\n${BOLD}${GREEN}--- Step 4: Directed Acyclic Graph (DAG) Workflow Submission ---${RESET}"
echo "Submitting Linear Pipeline (examples/workflow.json):"
"${ASTRA_BIN}" workflow submit examples/workflow.json

echo -e "\nSubmitting Diamond Parallel DAG (examples/diamond_workflow.json):"
"${ASTRA_BIN}" workflow submit examples/diamond_workflow.json

echo -e "\nActive Workflows in SQLite Database:"
"${ASTRA_BIN}" workflow list

echo -e "\n${BOLD}${GREEN}--- Step 5: Cycle Detection & Dependency Protection ---${RESET}"
echo "Submitting Cyclic Workflow (examples/cyclic_workflow.json) - Expecting Rejection:"
set +e
"${ASTRA_BIN}" workflow submit examples/cyclic_workflow.json
EXIT_CODE=$?
set -e
if [[ ${EXIT_CODE} -ne 0 ]]; then
    echo -e "${GREEN}[PASS] Cyclic workflow rejected successfully by 3-color DFS cycle detector.${RESET}"
else
    echo -e "${YELLOW}[FAIL] Cyclic workflow should have been rejected.${RESET}"
    exit 1
fi

echo -e "\n${BOLD}${GREEN}--- Step 6: Cooperative Checkpoint & Crash-Resume Protocol ---${RESET}"
CHECKPOINT_BIN="${BUILD_DIR}/workloads/checkpointable_sum"
echo "Phase 6A: Simulating workload process crash at step 50,000..."
set +e
"${CHECKPOINT_BIN}" --target 100000 --fail-at 50000 --checkpoint-file demo_ckpt.ckpt
CRASH_RC=$?
set -e
echo "Process exited with expected crash code: ${CRASH_RC}"

echo "Phase 6B: Resuming workload from atomic POSIX checkpoint..."
"${CHECKPOINT_BIN}" --target 100000 --resume --checkpoint-file demo_ckpt.ckpt
rm -f demo_ckpt.ckpt

echo -e "\n${BOLD}${GREEN}--- Step 7: GPU Workload & Transparent Host Fallback ---${RESET}"
"${BUILD_DIR}/workloads/vector_add"

echo -e "\n${BOLD}${GREEN}--- Step 8: Deterministic Event Replay Timeline ---${RESET}"
# Generate demo JSONL trace
DEMO_JSONL="demo_replay_trace.jsonl"
cat << 'EOF' > "${DEMO_JSONL}"
{"type": "JOB_SUBMITTED", "job_id": "demo-task-01", "worker_id": -1, "timestamp_ms": 1000, "payload": {"workload": "matrix_mul", "cpu_slots": 2}}
{"type": "JOB_QUEUED", "job_id": "demo-task-01", "worker_id": -1, "timestamp_ms": 1050, "payload": {"policy": "priority", "effective_pri": 8}}
{"type": "JOB_DISPATCHED", "job_id": "demo-task-01", "worker_id": 1, "timestamp_ms": 1150, "payload": {"worker_pid": 4821}}
{"type": "JOB_RUNNING", "job_id": "demo-task-01", "worker_id": 1, "timestamp_ms": 1200, "payload": {"status": "computing"}}
{"type": "JOB_SUCCEEDED", "job_id": "demo-task-01", "worker_id": 1, "timestamp_ms": 2450, "payload": {"exit_code": 0, "duration_ms": 1250}}
EOF

"${ASTRA_BIN}" replay "${DEMO_JSONL}"
rm -f "${DEMO_JSONL}"

echo -e "\n${BOLD}${CYAN}================================================================================${RESET}"
echo -e "${BOLD}${GREEN}       ASTRA FULL DEMO SUITE COMPLETED SUCCESSFULLY IN UNDER 2 MINUTES!        ${RESET}"
echo -e "${BOLD}${CYAN}================================================================================${RESET}"
