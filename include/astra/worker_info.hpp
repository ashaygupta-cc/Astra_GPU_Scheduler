#pragma once

#include <string>
#include <string_view>
#include <chrono>
#include <optional>
#include <sys/types.h>
#include <nlohmann/json.hpp>

namespace astra {

enum class WorkerState {
    Idle,
    Busy,
    Unhealthy,
    Terminated
};

inline constexpr std::string_view to_string(WorkerState state) noexcept {
    switch (state) {
        case WorkerState::Idle:       return "IDLE";
        case WorkerState::Busy:       return "BUSY";
        case WorkerState::Unhealthy:  return "UNHEALTHY";
        case WorkerState::Terminated: return "TERMINATED";
    }
    return "UNKNOWN";
}

inline std::optional<WorkerState> worker_state_from_string(std::string_view s) noexcept {
    if (s == "IDLE" || s == "Idle")             return WorkerState::Idle;
    if (s == "BUSY" || s == "Busy")             return WorkerState::Busy;
    if (s == "UNHEALTHY" || s == "Unhealthy")   return WorkerState::Unhealthy;
    if (s == "TERMINATED" || s == "Terminated") return WorkerState::Terminated;
    return std::nullopt;
}

struct WorkerInfo {
    int id = -1;
    pid_t pid = -1;
    WorkerState state = WorkerState::Idle;
    std::string current_job_id;
    int restart_count = 0;
    std::chrono::system_clock::time_point last_heartbeat = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point started_time = std::chrono::system_clock::now();

    bool operator==(const WorkerInfo&) const = default;
};

inline void to_json(nlohmann::json& j, const WorkerInfo& w) {
    j = nlohmann::json{
        {"id", w.id},
        {"pid", w.pid},
        {"state", std::string(to_string(w.state))},
        {"current_job_id", w.current_job_id},
        {"restart_count", w.restart_count},
        {"last_heartbeat_ms", std::chrono::duration_cast<std::chrono::milliseconds>(w.last_heartbeat.time_since_epoch()).count()},
        {"started_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(w.started_time.time_since_epoch()).count()}
    };
}

inline void from_json(const nlohmann::json& j, WorkerInfo& w) {
    j.at("id").get_to(w.id);
    j.at("pid").get_to(w.pid);
    std::string state_str;
    j.at("state").get_to(state_str);
    w.state = worker_state_from_string(state_str).value_or(WorkerState::Idle);
    j.at("current_job_id").get_to(w.current_job_id);
    j.at("restart_count").get_to(w.restart_count);
    int64_t hb_ms = 0, st_ms = 0;
    j.at("last_heartbeat_ms").get_to(hb_ms);
    j.at("started_time_ms").get_to(st_ms);
    w.last_heartbeat = std::chrono::system_clock::time_point(std::chrono::milliseconds(hb_ms));
    w.started_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(st_ms));
}

} // namespace astra
