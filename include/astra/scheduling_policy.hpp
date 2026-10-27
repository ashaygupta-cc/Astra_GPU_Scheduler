#pragma once

#include "astra/job.hpp"
#include <string_view>
#include <vector>
#include <optional>
#include <memory>
#include <chrono>

namespace astra {

struct SchedulingDecision {
    const Job* chosen_job = nullptr;
    std::string reason;
};

class SchedulingPolicy {
public:
    virtual ~SchedulingPolicy() = default;

    virtual std::string_view name() const noexcept = 0;

    virtual std::optional<SchedulingDecision> choose_next(
        const std::vector<Job>& eligible_jobs,
        std::chrono::system_clock::time_point current_time = std::chrono::system_clock::now()) const = 0;
};

class FcfsPolicy : public SchedulingPolicy {
public:
    std::string_view name() const noexcept override { return "fcfs"; }

    std::optional<SchedulingDecision> choose_next(
        const std::vector<Job>& eligible_jobs,
        std::chrono::system_clock::time_point current_time = std::chrono::system_clock::now()) const override;
};

class PriorityPolicy : public SchedulingPolicy {
public:
    std::string_view name() const noexcept override { return "priority"; }

    std::optional<SchedulingDecision> choose_next(
        const std::vector<Job>& eligible_jobs,
        std::chrono::system_clock::time_point current_time = std::chrono::system_clock::now()) const override;
};

class PriorityAgingPolicy : public SchedulingPolicy {
public:
    explicit PriorityAgingPolicy(unsigned int aging_interval_ms = 2000, int aging_max_priority = 10)
        : aging_interval_ms_(aging_interval_ms), aging_max_priority_(aging_max_priority) {}

    std::string_view name() const noexcept override { return "priority_aging"; }

    std::optional<SchedulingDecision> choose_next(
        const std::vector<Job>& eligible_jobs,
        std::chrono::system_clock::time_point current_time = std::chrono::system_clock::now()) const override;

    int compute_effective_priority(const Job& job, std::chrono::system_clock::time_point current_time) const;

private:
    unsigned int aging_interval_ms_;
    int aging_max_priority_;
};

class SjfPolicy : public SchedulingPolicy {
public:
    std::string_view name() const noexcept override { return "sjf"; }

    std::optional<SchedulingDecision> choose_next(
        const std::vector<Job>& eligible_jobs,
        std::chrono::system_clock::time_point current_time = std::chrono::system_clock::now()) const override;
};

std::unique_ptr<SchedulingPolicy> create_scheduling_policy(
    std::string_view name, 
    unsigned int aging_interval_ms = 2000, 
    int aging_max_priority = 10);

} // namespace astra
