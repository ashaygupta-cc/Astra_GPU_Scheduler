#pragma once

#include "astra/job.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <sstream>

namespace astra {

struct ResourceVector {
    unsigned int cpu_slots = 0;
    uint64_t ram_mb = 0;
    uint64_t gpu_slots = 0;

    bool operator<=(const ResourceVector& other) const {
        return cpu_slots <= other.cpu_slots &&
               ram_mb <= other.ram_mb &&
               gpu_slots <= other.gpu_slots;
    }

    ResourceVector operator+(const ResourceVector& other) const {
        return {cpu_slots + other.cpu_slots, ram_mb + other.ram_mb, gpu_slots + other.gpu_slots};
    }

    ResourceVector operator-(const ResourceVector& other) const {
        return {
            cpu_slots >= other.cpu_slots ? cpu_slots - other.cpu_slots : 0,
            ram_mb >= other.ram_mb ? ram_mb - other.ram_mb : 0,
            gpu_slots >= other.gpu_slots ? gpu_slots - other.gpu_slots : 0
        };
    }
};

struct BankersSystemState {
    std::vector<std::string> job_ids;
    ResourceVector available;
    std::unordered_map<std::string, ResourceVector> max_claim;
    std::unordered_map<std::string, ResourceVector> allocation;
    std::unordered_map<std::string, ResourceVector> need;
};

struct BankersSafetyResult {
    bool is_safe = false;
    std::vector<std::string> safe_sequence;
    std::vector<std::string> deadlocked_jobs;
    std::string diagnostic_explanation;
};

struct WaitForCycle {
    std::vector<std::string> cycle_path; // e.g. ["J1", "J2", "J1"]
    std::string victim_job_id;
    std::string victim_reason;
};

class DeadlockDetector {
public:
    // 1. Classical Banker's Safety Algorithm
    static BankersSafetyResult check_safety(const BankersSystemState& state);

    // 2. Wait-For Graph (WFG) Cycle Detection
    // adj: holding_job_id -> [jobs waiting for resources held by holding_job_id]
    static std::optional<WaitForCycle> detect_wait_for_cycle(
        const std::unordered_map<std::string, std::vector<std::string>>& wait_for_graph,
        const std::unordered_map<std::string, const Job*>& job_metadata = {});

    // 3. Explainable Victim Selection
    // Selects the optimal victim to abort based on lowest priority, lowest runtime, and resource recovery
    static std::pair<std::string, std::string> select_victim(
        const std::vector<std::string>& candidates,
        const std::unordered_map<std::string, const Job*>& job_metadata);

    // 4. Run full educational deadlock lab simulation
    static std::string run_deadlock_simulation_demo();
};

} // namespace astra
