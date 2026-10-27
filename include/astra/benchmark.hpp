#pragma once

#include "astra/scheduling_policy.hpp"
#include <string>
#include <vector>
#include <chrono>
#include <memory>

namespace astra {

struct BenchmarkJob {
    std::string id;
    std::string name;
    int base_priority = 5;
    double burst_seconds = 1.0;
    double arrival_seconds = 0.0;
    
    // Results populated during simulation
    double start_time = 0.0;
    double completion_time = 0.0;
    double wait_time = 0.0;
    double turnaround_time = 0.0;
};

struct PolicyBenchmarkResult {
    std::string policy_name;
    size_t total_jobs = 0;
    double avg_wait_time = 0.0;
    double max_wait_time = 0.0;
    double avg_turnaround_time = 0.0;
    double max_turnaround_time = 0.0;
    double total_makespan = 0.0;
    size_t starved_jobs = 0; // jobs that waited > threshold
    std::vector<BenchmarkJob> completed_jobs;
};

class PolicyBenchmarkSuite {
public:
    // Generate standard synthetic workload trace (mix of short, long, high, low prio jobs)
    static std::vector<BenchmarkJob> generate_synthetic_workload(size_t job_count = 20, unsigned int seed = 42);

    // Run simulated execution of a workload trace using the specified scheduling policy
    static PolicyBenchmarkResult evaluate_policy(const std::string& policy_name, 
                                                 std::vector<BenchmarkJob> workload,
                                                 int concurrency_slots = 2);

    // Run full comparative benchmark across FCFS, Priority, Priority Aging, and SJF
    static std::vector<PolicyBenchmarkResult> run_comparison(size_t job_count = 30, int concurrency_slots = 2);

    // Format comparative ASCII table for CLI and report
    static std::string format_benchmark_report(const std::vector<PolicyBenchmarkResult>& results);
};

} // namespace astra
