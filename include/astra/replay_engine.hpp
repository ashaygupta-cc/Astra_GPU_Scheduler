#pragma once

#include "astra/event.hpp"
#include <string>
#include <vector>
#include <chrono>
#include <iostream>

namespace astra {

class JobStore;

struct ReplayStep {
    uint64_t step_index = 0;
    std::chrono::system_clock::time_point timestamp;
    double delta_seconds = 0.0;
    EventType event_type = EventType::JOB_SUBMITTED;
    std::string job_id;
    int worker_id = -1;
    std::string payload;
};

class ReplayEngine {
public:
    // Load events from database store and reconstruct timeline
    static std::vector<ReplayStep> load_from_store(JobStore& store, const std::string& job_id = "", size_t limit = 100);

    // Load events from a raw JSONL event audit log file
    static std::vector<ReplayStep> load_from_jsonl(const std::string& jsonl_path);

    // Format timeline table
    static std::string render_timeline_table(const std::vector<ReplayStep>& steps);

    // Playback timeline with optional simulated time delays
    static void playback(const std::vector<ReplayStep>& steps, std::ostream& out, double speed_multiplier = 1.0);
};

} // namespace astra
