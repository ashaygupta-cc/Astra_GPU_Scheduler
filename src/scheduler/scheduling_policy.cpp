#include "astra/scheduling_policy.hpp"

#include <algorithm>
#include <sstream>

namespace astra {

std::optional<SchedulingDecision> FcfsPolicy::choose_next(
    const std::vector<Job>& eligible_jobs,
    std::chrono::system_clock::time_point /*current_time*/) const {
    if (eligible_jobs.empty()) {
        return std::nullopt;
    }

    const Job* best = &eligible_jobs[0];
    for (size_t i = 1; i < eligible_jobs.size(); ++i) {
        if (eligible_jobs[i].enqueued_time < best->enqueued_time) {
            best = &eligible_jobs[i];
        } else if (eligible_jobs[i].enqueued_time == best->enqueued_time && eligible_jobs[i].id < best->id) {
            best = &eligible_jobs[i];
        }
    }

    std::ostringstream ss;
    ss << "FCFS: Selected job '" << best->id << "' with earliest enqueue timestamp";
    return SchedulingDecision{best, ss.str()};
}

std::optional<SchedulingDecision> PriorityPolicy::choose_next(
    const std::vector<Job>& eligible_jobs,
    std::chrono::system_clock::time_point /*current_time*/) const {
    if (eligible_jobs.empty()) {
        return std::nullopt;
    }

    const Job* best = &eligible_jobs[0];
    for (size_t i = 1; i < eligible_jobs.size(); ++i) {
        if (eligible_jobs[i].priority > best->priority) {
            best = &eligible_jobs[i];
        } else if (eligible_jobs[i].priority == best->priority) {
            if (eligible_jobs[i].enqueued_time < best->enqueued_time) {
                best = &eligible_jobs[i];
            }
        }
    }

    std::ostringstream ss;
    ss << "Priority: Selected job '" << best->id << "' with highest priority (" 
       << best->priority << ")";
    return SchedulingDecision{best, ss.str()};
}

int PriorityAgingPolicy::compute_effective_priority(
    const Job& job, 
    std::chrono::system_clock::time_point current_time) const {
    if (aging_interval_ms_ == 0) {
        return job.priority;
    }

    int64_t wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        current_time - job.enqueued_time).count();
    if (wait_ms < 0) wait_ms = 0;

    int boost = static_cast<int>(wait_ms / aging_interval_ms_);
    int effective = job.priority + boost;
    if (effective > aging_max_priority_) {
        effective = aging_max_priority_;
    }
    return effective;
}

std::optional<SchedulingDecision> PriorityAgingPolicy::choose_next(
    const std::vector<Job>& eligible_jobs,
    std::chrono::system_clock::time_point current_time) const {
    if (eligible_jobs.empty()) {
        return std::nullopt;
    }

    const Job* best = &eligible_jobs[0];
    int best_effective = compute_effective_priority(*best, current_time);

    for (size_t i = 1; i < eligible_jobs.size(); ++i) {
        int cand_effective = compute_effective_priority(eligible_jobs[i], current_time);
        if (cand_effective > best_effective) {
            best = &eligible_jobs[i];
            best_effective = cand_effective;
        } else if (cand_effective == best_effective) {
            if (eligible_jobs[i].enqueued_time < best->enqueued_time) {
                best = &eligible_jobs[i];
                best_effective = cand_effective;
            }
        }
    }

    std::ostringstream ss;
    ss << "PriorityAging: Selected job '" << best->id << "' with effective priority " 
       << best_effective << " (base: " << best->priority << ")";
    return SchedulingDecision{best, ss.str()};
}

std::optional<SchedulingDecision> SjfPolicy::choose_next(
    const std::vector<Job>& eligible_jobs,
    std::chrono::system_clock::time_point /*current_time*/) const {
    if (eligible_jobs.empty()) {
        return std::nullopt;
    }

    auto get_estimate = [](const Job& j) -> uint64_t {
        if (j.deadline.has_value()) {
            auto d = std::chrono::duration_cast<std::chrono::seconds>(*j.deadline - j.enqueued_time).count();
            if (d > 0) return static_cast<uint64_t>(d);
        }
        return j.resources.cpu_slots; // proxy estimate
    };

    const Job* best = &eligible_jobs[0];
    uint64_t best_est = get_estimate(*best);

    for (size_t i = 1; i < eligible_jobs.size(); ++i) {
        uint64_t cand_est = get_estimate(eligible_jobs[i]);
        if (cand_est < best_est) {
            best = &eligible_jobs[i];
            best_est = cand_est;
        } else if (cand_est == best_est && eligible_jobs[i].enqueued_time < best->enqueued_time) {
            best = &eligible_jobs[i];
            best_est = cand_est;
        }
    }

    std::ostringstream ss;
    ss << "SJF: Selected job '" << best->id << "' with shortest estimated runtime (" << best_est << ")";
    return SchedulingDecision{best, ss.str()};
}

std::unique_ptr<SchedulingPolicy> create_scheduling_policy(
    std::string_view name, 
    unsigned int aging_interval_ms, 
    int aging_max_priority) {
    if (name == "fcfs") {
        return std::make_unique<FcfsPolicy>();
    }
    if (name == "priority") {
        return std::make_unique<PriorityPolicy>();
    }
    if (name == "sjf") {
        return std::make_unique<SjfPolicy>();
    }
    // Default to priority_aging
    return std::make_unique<PriorityAgingPolicy>(aging_interval_ms, aging_max_priority);
}

} // namespace astra
