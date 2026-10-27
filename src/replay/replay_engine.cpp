#include "astra/replay_engine.hpp"
#include "astra/job_store.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace astra {

std::vector<ReplayStep> ReplayEngine::load_from_store(JobStore& store, const std::string& job_id, size_t limit) {
    auto events = store.get_events(job_id, limit);
    if (events.empty()) {
        return {};
    }

    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.id < b.id;
    });

    std::vector<ReplayStep> steps;
    steps.reserve(events.size());

    auto base_time = events.front().timestamp;

    for (size_t i = 0; i < events.size(); ++i) {
        const auto& ev = events[i];
        ReplayStep s;
        s.step_index = i + 1;
        s.timestamp = ev.timestamp;
        auto diff = std::chrono::duration_cast<std::chrono::microseconds>(ev.timestamp - base_time).count();
        s.delta_seconds = static_cast<double>(diff) / 1000000.0;
        s.event_type = ev.type;
        s.job_id = ev.job_id;
        s.worker_id = ev.worker_id;
        s.payload = ev.payload_json;
        steps.push_back(std::move(s));
    }

    return steps;
}

std::vector<ReplayStep> ReplayEngine::load_from_jsonl(const std::string& jsonl_path) {
    std::ifstream file(jsonl_path);
    if (!file.is_open()) {
        return {};
    }

    std::vector<Event> events;
    std::string line;
    uint64_t counter = 1;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) continue;

        Event ev;
        ev.id = counter++;
        if (parsed.contains("type") && parsed["type"].is_string()) {
            ev.type = event_type_from_string(parsed["type"].get<std::string>()).value_or(EventType::JOB_SUBMITTED);
        }
        if (parsed.contains("job_id") && parsed["job_id"].is_string()) {
            ev.job_id = parsed["job_id"].get<std::string>();
        }
        if (parsed.contains("worker_id") && parsed["worker_id"].is_number()) {
            ev.worker_id = parsed["worker_id"].get<int>();
        }
        if (parsed.contains("timestamp_ms") && parsed["timestamp_ms"].is_number()) {
            int64_t ms = parsed["timestamp_ms"].get<int64_t>();
            ev.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
        } else {
            ev.timestamp = std::chrono::system_clock::now();
        }
        if (parsed.contains("payload")) {
            ev.payload_json = parsed["payload"].dump();
        }

        events.push_back(std::move(ev));
    }

    if (events.empty()) return {};

    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.id < b.id;
    });

    std::vector<ReplayStep> steps;
    steps.reserve(events.size());
    auto base_time = events.front().timestamp;

    for (size_t i = 0; i < events.size(); ++i) {
        const auto& ev = events[i];
        ReplayStep s;
        s.step_index = i + 1;
        s.timestamp = ev.timestamp;
        auto diff = std::chrono::duration_cast<std::chrono::microseconds>(ev.timestamp - base_time).count();
        s.delta_seconds = static_cast<double>(diff) / 1000000.0;
        s.event_type = ev.type;
        s.job_id = ev.job_id;
        s.worker_id = ev.worker_id;
        s.payload = ev.payload_json;
        steps.push_back(std::move(s));
    }

    return steps;
}

std::string ReplayEngine::render_timeline_table(const std::vector<ReplayStep>& steps) {
    if (steps.empty()) {
        return "No audit events recorded for replay timeline.\n";
    }

    std::ostringstream ss;
    ss << "========================================================================================\n"
       << "                       ASTRA DETERMINISTIC EVENT REPLAY TIMELINE                        \n"
       << "========================================================================================\n";
    ss << std::left
       << std::setw(6)  << "STEP"
       << std::setw(12) << "+DELTA"
       << std::setw(24) << "EVENT TYPE"
       << std::setw(18) << "JOB ID"
       << std::setw(10) << "WORKER"
       << "PAYLOAD DETAILS\n";
    ss << std::string(88, '-') << "\n";

    for (const auto& s : steps) {
        std::ostringstream delta_ss;
        delta_ss << "+" << std::fixed << std::setprecision(3) << s.delta_seconds << "s";

        std::string worker_str = (s.worker_id >= 0) ? ("w" + std::to_string(s.worker_id)) : "-";

        ss << std::left
           << std::setw(6)  << s.step_index
           << std::setw(12) << delta_ss.str()
           << std::setw(24) << to_string(s.event_type)
           << std::setw(18) << (s.job_id.empty() ? "-" : s.job_id)
           << std::setw(10) << worker_str
           << s.payload << "\n";
    }

    ss << std::string(88, '-') << "\n"
       << "Total Replay Events: " << steps.size() << " | Sunk Timeline Duration: +" 
       << std::fixed << std::setprecision(3) << steps.back().delta_seconds << "s\n";

    return ss.str();
}

void ReplayEngine::playback(const std::vector<ReplayStep>& steps, std::ostream& out, double speed_multiplier) {
    if (steps.empty()) {
        out << "No replay events available.\n";
        return;
    }

    out << render_timeline_table(steps);

    if (speed_multiplier <= 0.0) return;

    // Simulated playback delay demonstration
    for (size_t i = 1; i < steps.size() && i < 10; ++i) {
        double gap_ms = (steps[i].delta_seconds - steps[i - 1].delta_seconds) * 1000.0 / speed_multiplier;
        if (gap_ms > 0 && gap_ms < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(gap_ms)));
        }
    }
}

} // namespace astra
