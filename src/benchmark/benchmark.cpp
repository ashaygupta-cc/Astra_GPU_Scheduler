#include "astra/benchmark.hpp"
#include "astra/scheduling_policy.hpp"

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <random>
#include <cmath>

namespace astra {

std::vector<BenchmarkJob> PolicyBenchmarkSuite::generate_synthetic_workload(size_t job_count, unsigned int seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> burst_dist(0.5, 4.0);
    std::uniform_int_distribution<int> prio_dist(1, 10);
    std::uniform_real_distribution<double> arrival_dist(0.0, 5.0);

    std::vector<BenchmarkJob> jobs;
    jobs.reserve(job_count);

    // Job 0: Long low-priority job arriving at t=0 (classic convoy effect trigger)
    jobs.push_back({
        "bench-0", "convoy_seed_job",
        /* priority */ 2,
        /* burst */ 6.0,
        /* arrival */ 0.0
    });

    for (size_t i = 1; i < job_count; ++i) {
        BenchmarkJob bj;
        bj.id = "bench-" + std::to_string(i);
        bj.name = "task_" + std::to_string(i);
        bj.burst_seconds = std::round(burst_dist(rng) * 10.0) / 10.0;
        bj.base_priority = prio_dist(rng);
        bj.arrival_seconds = std::round(arrival_dist(rng) * 10.0) / 10.0;
        jobs.push_back(bj);
    }

    std::sort(jobs.begin(), jobs.end(), [](const BenchmarkJob& a, const BenchmarkJob& b) {
        if (a.arrival_seconds != b.arrival_seconds) return a.arrival_seconds < b.arrival_seconds;
        return a.id < b.id;
    });

    return jobs;
}

PolicyBenchmarkResult PolicyBenchmarkSuite::evaluate_policy(const std::string& policy_name,
                                                            std::vector<BenchmarkJob> workload,
                                                            int concurrency_slots) {
    PolicyBenchmarkResult result;
    result.policy_name = policy_name;
    result.total_jobs = workload.size();
    if (workload.empty() || concurrency_slots <= 0) {
        return result;
    }

    auto policy = create_scheduling_policy(policy_name);
    if (!policy) {
        policy = std::make_unique<FcfsPolicy>();
    }

    // Event-driven discrete simulation
    double current_time = 0.0;
    std::vector<BenchmarkJob*> pending;
    for (auto& bj : workload) {
        pending.push_back(&bj);
    }

    // Active execution slots: {job_ptr, finish_time}
    struct RunningSlot {
        BenchmarkJob* job = nullptr;
        double finish_time = 0.0;
    };
    std::vector<RunningSlot> active_slots;
    active_slots.reserve(concurrency_slots);

    std::vector<BenchmarkJob*> queue;
    std::vector<BenchmarkJob> finished;
    finished.reserve(workload.size());

    const auto base_time = std::chrono::system_clock::now();

    while (finished.size() < workload.size()) {
        // 1. Advance time to next event (either new arrival or next slot completion)
        double next_event_time = 1e9;

        // Next arrival
        for (auto* bj : pending) {
            if (bj->arrival_seconds > current_time) {
                next_event_time = std::min(next_event_time, bj->arrival_seconds);
            }
        }
        // Next slot completion
        for (const auto& slot : active_slots) {
            next_event_time = std::min(next_event_time, slot.finish_time);
        }

        if (next_event_time > current_time && next_event_time < 1e9) {
            current_time = next_event_time;
        }

        // 2. Free completed slots
        auto slot_it = active_slots.begin();
        while (slot_it != active_slots.end()) {
            if (slot_it->finish_time <= current_time) {
                auto* j = slot_it->job;
                j->completion_time = slot_it->finish_time;
                j->turnaround_time = j->completion_time - j->arrival_seconds;
                j->wait_time = j->turnaround_time - j->burst_seconds;
                finished.push_back(*j);
                slot_it = active_slots.erase(slot_it);
            } else {
                ++slot_it;
            }
        }

        // 3. Move newly arrived jobs from pending to queue
        auto pend_it = pending.begin();
        while (pend_it != pending.end()) {
            if ((*pend_it)->arrival_seconds <= current_time) {
                queue.push_back(*pend_it);
                pend_it = pending.erase(pend_it);
            } else {
                ++pend_it;
            }
        }

        // 4. Dispatch jobs from queue to available slots using policy
        while (active_slots.size() < static_cast<size_t>(concurrency_slots) && !queue.empty()) {
            // Convert queue to astra::Job list to invoke actual scheduling policy
            std::vector<Job> candidate_jobs;
            candidate_jobs.reserve(queue.size());

            for (const auto* bj : queue) {
                Job j;
                j.id = bj->id;
                j.name = bj->name;
                j.priority = bj->base_priority;
                j.state = JobState::Queued;
                j.submitted_time = base_time + std::chrono::milliseconds(static_cast<int64_t>(bj->arrival_seconds * 1000.0));
                j.enqueued_time = j.submitted_time;
                j.deadline = j.enqueued_time + std::chrono::milliseconds(static_cast<int64_t>(bj->burst_seconds * 1000.0));
                candidate_jobs.push_back(j);
            }

            auto cur_tp = base_time + std::chrono::milliseconds(static_cast<int64_t>(current_time * 1000.0));
            auto decision_opt = policy->choose_next(candidate_jobs, cur_tp);
            if (!decision_opt.has_value() || !decision_opt->chosen_job) {
                break;
            }

            std::string chosen_id = decision_opt->chosen_job->id;

            // Find chosen job in queue
            auto q_it = std::find_if(queue.begin(), queue.end(), [&](const BenchmarkJob* bj) {
                return bj->id == chosen_id;
            });

            if (q_it != queue.end()) {
                BenchmarkJob* chosen = *q_it;
                queue.erase(q_it);

                chosen->start_time = current_time;
                active_slots.push_back({chosen, current_time + chosen->burst_seconds});
            } else {
                break;
            }
        }

        // If no active slots and queue is empty, leap forward to next pending arrival
        if (active_slots.empty() && queue.empty() && !pending.empty()) {
            double next_arr = 1e9;
            for (auto* bj : pending) {
                next_arr = std::min(next_arr, bj->arrival_seconds);
            }
            if (next_arr < 1e9) {
                current_time = next_arr;
            }
        }
    }

    // Compute aggregated metrics
    double sum_wait = 0.0;
    double sum_turnaround = 0.0;
    double max_wait = 0.0;
    double max_turnaround = 0.0;
    size_t starved = 0;

    const double STARVATION_THRESHOLD = 5.0; // Wait time > 5.0s classified as starved

    for (const auto& j : finished) {
        sum_wait += j.wait_time;
        sum_turnaround += j.turnaround_time;
        max_wait = std::max(max_wait, j.wait_time);
        max_turnaround = std::max(max_turnaround, j.turnaround_time);
        if (j.wait_time > STARVATION_THRESHOLD && j.base_priority <= 4) {
            starved++;
        }
    }

    result.avg_wait_time = sum_wait / finished.size();
    result.max_wait_time = max_wait;
    result.avg_turnaround_time = sum_turnaround / finished.size();
    result.max_turnaround_time = max_turnaround;
    result.total_makespan = current_time;
    result.starved_jobs = starved;
    result.completed_jobs = std::move(finished);

    return result;
}

std::vector<PolicyBenchmarkResult> PolicyBenchmarkSuite::run_comparison(size_t job_count, int concurrency_slots) {
    auto workload = generate_synthetic_workload(job_count, 42);

    std::vector<std::string> policies = {"fcfs", "priority", "aging", "sjf"};
    std::vector<PolicyBenchmarkResult> results;
    results.reserve(policies.size());

    for (const auto& p : policies) {
        results.push_back(evaluate_policy(p, workload, concurrency_slots));
    }

    return results;
}

std::string PolicyBenchmarkSuite::format_benchmark_report(const std::vector<PolicyBenchmarkResult>& results) {
    std::ostringstream ss;
    ss << "=======================================================================================================\n"
       << "                               ASTRA SCHEDULING POLICY BENCHMARK REPORT                                \n"
       << "=======================================================================================================\n";
    ss << std::left
       << std::setw(16) << "POLICY"
       << std::setw(8)  << "JOBS"
       << std::setw(16) << "AVG WAIT (s)"
       << std::setw(16) << "MAX WAIT (s)"
       << std::setw(18) << "AVG TURNAROUND(s)"
       << std::setw(14) << "MAKESPAN (s)"
       << "STARVATION\n";
    ss << std::string(96, '-') << "\n";

    for (const auto& r : results) {
        std::string p_name = r.policy_name;
        if (p_name == "fcfs") p_name = "FCFS (Baseline)";
        else if (p_name == "priority") p_name = "Strict Priority";
        else if (p_name == "aging") p_name = "Priority Aging";
        else if (p_name == "sjf") p_name = "Shortest Job First";

        ss << std::left
           << std::setw(16) << p_name
           << std::setw(8)  << r.total_jobs
           << std::setw(16) << (std::to_string(std::round(r.avg_wait_time * 1000.0) / 1000.0).substr(0, 6) + "s")
           << std::setw(16) << (std::to_string(std::round(r.max_wait_time * 1000.0) / 1000.0).substr(0, 6) + "s")
           << std::setw(18) << (std::to_string(std::round(r.avg_turnaround_time * 1000.0) / 1000.0).substr(0, 6) + "s")
           << std::setw(14) << (std::to_string(std::round(r.total_makespan * 1000.0) / 1000.0).substr(0, 6) + "s")
           << (r.starved_jobs > 0 ? (std::to_string(r.starved_jobs) + " jobs") : "0 (Protected)") << "\n";
    }

    ss << std::string(96, '-') << "\n"
       << "OPERATING SYSTEMS THEORETICAL ANALYSIS & EMPIRICAL FINDINGS:\n"
       << " 1. FCFS (First-Come First-Served): Simple and starvation-free, but vulnerable to Convoy Effects\n"
       << "    where short jobs get queued behind long-running compute bursts.\n"
       << " 2. Strict Priority: Fast-tracks mission-critical high-priority workloads, but causes unbounded\n"
       << "    starvation on low-priority tasks when high-priority jobs arrive continuously.\n"
       << " 3. Priority Aging: Solves the priority inversion/starvation anomaly by monotonically elevating\n"
       << "    effective priority proportional to queuing delay, capping maximum wait time.\n"
       << " 4. SJF (Shortest Job First): Provably minimizes average wait and turnaround time across all\n"
       << "    non-preemptive scheduling algorithms.\n"
       << "=======================================================================================================\n";

    return ss.str();
}

} // namespace astra
