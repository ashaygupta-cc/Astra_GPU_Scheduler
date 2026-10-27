#pragma once

#include "astra/job.hpp"
#include "astra/worker_info.hpp"
#include "astra/resource_manager.hpp"
#include "astra/event.hpp"

#include <string>
#include <vector>
#include <chrono>

namespace astra {

struct TuiSnapshot {
    std::string policy_name = "priority_aging";
    unsigned int active_workers = 0;
    unsigned int total_workers = 0;
    size_t queued_jobs_count = 0;
    size_t running_jobs_count = 0;
    size_t succeeded_jobs_count = 0;
    size_t failed_jobs_count = 0;
    uint64_t uptime_seconds = 0;

    std::vector<Job> jobs;
    std::vector<WorkerInfo> workers;
    ResourceSnapshot resources;
    std::vector<Event> recent_events;

    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

class JobStore;
class WorkerManager;

TuiSnapshot collect_tui_snapshot(
    JobStore& store,
    ResourceManager& rm,
    WorkerManager& wm,
    const std::string& policy_name,
    std::chrono::system_clock::time_point start_time);

} // namespace astra
