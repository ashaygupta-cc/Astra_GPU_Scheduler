#include "astra/deadlock_detector.hpp"

#include <iostream>
#include <cassert>
#include <chrono>

void test_bankers_algorithm_safety() {
    using namespace astra;

    // 1. Provably Safe System
    // Resources: Available = [CPU=3, RAM=3000, GPU=2]
    BankersSystemState safe_state;
    safe_state.job_ids = {"P0", "P1", "P2"};
    safe_state.available = {3, 3000, 2};

    // Allocation
    safe_state.allocation["P0"] = {0, 1000, 0};
    safe_state.allocation["P1"] = {2, 0, 0};
    safe_state.allocation["P2"] = {3, 2000, 1};

    // Max Claim
    safe_state.max_claim["P0"] = {7, 5000, 3};
    safe_state.max_claim["P1"] = {3, 2000, 2};
    safe_state.max_claim["P2"] = {6, 2000, 2};

    // Need = Max - Allocation
    safe_state.need["P0"] = safe_state.max_claim["P0"] - safe_state.allocation["P0"]; // [7, 4000, 3]
    safe_state.need["P1"] = safe_state.max_claim["P1"] - safe_state.allocation["P1"]; // [1, 2000, 2]
    safe_state.need["P2"] = safe_state.max_claim["P2"] - safe_state.allocation["P2"]; // [3, 0, 1]

    auto safe_res = DeadlockDetector::check_safety(safe_state);
    assert(safe_res.is_safe);
    assert(safe_res.safe_sequence.size() == 3);
    assert(safe_res.safe_sequence[0] == "P1"); // P1 finishes: Work = [5, 3000, 2]
    assert(safe_res.safe_sequence[1] == "P2"); // P2 finishes: Work = [8, 5000, 3]
    assert(safe_res.safe_sequence[2] == "P0"); // P0 finishes: Work = [8, 6000, 3]
    assert(safe_res.deadlocked_jobs.empty());

    // 2. Deadlocked / Unsafe State
    // Decrease available so no job's need can be satisfied
    BankersSystemState unsafe_state = safe_state;
    unsafe_state.available = {0, 500, 0};

    auto unsafe_res = DeadlockDetector::check_safety(unsafe_state);
    assert(!unsafe_res.is_safe);
    assert(unsafe_res.safe_sequence.empty());
    assert(unsafe_res.deadlocked_jobs.size() == 3);
    assert(unsafe_res.diagnostic_explanation.find("UNSAFE STATE") != std::string::npos);

    std::cout << "[PASS] Banker's algorithm safe sequence computation and deadlock proof\n";
}

void test_wait_for_graph_cycle_detection() {
    using namespace astra;

    // 3-node cycle: J1 -> J2 -> J3 -> J1
    std::unordered_map<std::string, std::vector<std::string>> wfg;
    wfg["J1"] = {"J2"};
    wfg["J2"] = {"J3"};
    wfg["J3"] = {"J1"};

    auto cycle_opt = DeadlockDetector::detect_wait_for_cycle(wfg);
    assert(cycle_opt.has_value());
    assert(cycle_opt->cycle_path.size() >= 4);
    assert(cycle_opt->cycle_path.front() == cycle_opt->cycle_path.back());

    // Acyclic graph: J1 -> J2, J1 -> J3
    std::unordered_map<std::string, std::vector<std::string>> acyclic;
    acyclic["J1"] = {"J2", "J3"};
    acyclic["J2"] = {};
    acyclic["J3"] = {};

    auto no_cycle = DeadlockDetector::detect_wait_for_cycle(acyclic);
    assert(!no_cycle.has_value());

    std::cout << "[PASS] Wait-For Graph (WFG) DFS cycle detection\n";
}

void test_explainable_victim_selection() {
    using namespace astra;

    Job j_high;
    j_high.id = "j_high";
    j_high.priority = 9;
    j_high.started_time = std::chrono::system_clock::now() - std::chrono::milliseconds(2000);

    Job j_low_long;
    j_low_long.id = "j_low_long";
    j_low_long.priority = 2;
    j_low_long.started_time = std::chrono::system_clock::now() - std::chrono::milliseconds(1500);

    Job j_low_short;
    j_low_short.id = "j_low_short";
    j_low_short.priority = 2;
    j_low_short.started_time = std::chrono::system_clock::now() - std::chrono::milliseconds(200);

    std::unordered_map<std::string, const Job*> meta;
    meta["j_high"] = &j_high;
    meta["j_low_long"] = &j_low_long;
    meta["j_low_short"] = &j_low_short;

    std::vector<std::string> candidates = {"j_high", "j_low_long", "j_low_short"};
    auto [victim, reason] = DeadlockDetector::select_victim(candidates, meta);

    // Should choose j_low_short: lowest priority (2) and least sunk runtime (200ms vs 1500ms)
    assert(victim == "j_low_short");
    assert(reason.find("j_low_short") != std::string::npos);
    assert(reason.find("lowest base priority (2)") != std::string::npos);

    std::cout << "[PASS] Explainable victim selection by priority and minimal sunk work\n";
}

void test_deadlock_simulation_demo_output() {
    using namespace astra;

    std::string report = DeadlockDetector::run_deadlock_simulation_demo();
    assert(!report.empty());
    assert(report.find("ASTRA OPERATING SYSTEM DEADLOCK LABORATORY") != std::string::npos);
    assert(report.find("UNSAFE STATE") != std::string::npos);
    assert(report.find("Circular Wait Detected in Resource Graph") != std::string::npos);
    assert(report.find("Selected victim 'Job-Beta' for preemption") != std::string::npos);
    assert(report.find("provably SAFE state") != std::string::npos);
    assert(report.find("Deadlock successfully broken") != std::string::npos);

    std::cout << "[PASS] Educational deadlock lab simulation and report generation\n";
}

int main() {
    std::cout << "Running ASTRA Phase 7 (Reliability & Deadlock Lab) Tests...\n";
    test_bankers_algorithm_safety();
    test_wait_for_graph_cycle_detection();
    test_explainable_victim_selection();
    test_deadlock_simulation_demo_output();
    std::cout << "All Phase 7 tests passed successfully!\n";
    return 0;
}
