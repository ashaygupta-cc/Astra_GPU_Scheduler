#pragma once

#include <string>
#include <string_view>
#include <chrono>
#include <optional>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace astra {

enum class EventType {
    JOB_SUBMITTED,
    JOB_VALIDATED,
    JOB_QUEUED,
    SCHEDULER_DECISION,
    RESOURCE_RESERVED,
    WORKER_STARTED,
    JOB_STARTED,
    JOB_PROGRESS,
    JOB_CHECKPOINTED,
    WORKER_CRASHED,
    JOB_RETRIED,
    DEADLOCK_DETECTED,
    RECOVERY_APPLIED,
    JOB_COMPLETED,
    JOB_FAILED,
    JOB_CANCELLED,
    RESOURCE_RELEASED
};

inline constexpr std::string_view to_string(EventType type) noexcept {
    switch (type) {
        case EventType::JOB_SUBMITTED:      return "JOB_SUBMITTED";
        case EventType::JOB_VALIDATED:      return "JOB_VALIDATED";
        case EventType::JOB_QUEUED:         return "JOB_QUEUED";
        case EventType::SCHEDULER_DECISION: return "SCHEDULER_DECISION";
        case EventType::RESOURCE_RESERVED:  return "RESOURCE_RESERVED";
        case EventType::WORKER_STARTED:     return "WORKER_STARTED";
        case EventType::JOB_STARTED:        return "JOB_STARTED";
        case EventType::JOB_PROGRESS:       return "JOB_PROGRESS";
        case EventType::JOB_CHECKPOINTED:   return "JOB_CHECKPOINTED";
        case EventType::WORKER_CRASHED:     return "WORKER_CRASHED";
        case EventType::JOB_RETRIED:        return "JOB_RETRIED";
        case EventType::DEADLOCK_DETECTED:  return "DEADLOCK_DETECTED";
        case EventType::RECOVERY_APPLIED:   return "RECOVERY_APPLIED";
        case EventType::JOB_COMPLETED:      return "JOB_COMPLETED";
        case EventType::JOB_FAILED:         return "JOB_FAILED";
        case EventType::JOB_CANCELLED:      return "JOB_CANCELLED";
        case EventType::RESOURCE_RELEASED:  return "RESOURCE_RELEASED";
    }
    return "UNKNOWN";
}

inline std::optional<EventType> event_type_from_string(std::string_view s) noexcept {
    if (s == "JOB_SUBMITTED")      return EventType::JOB_SUBMITTED;
    if (s == "JOB_VALIDATED")      return EventType::JOB_VALIDATED;
    if (s == "JOB_QUEUED")         return EventType::JOB_QUEUED;
    if (s == "SCHEDULER_DECISION") return EventType::SCHEDULER_DECISION;
    if (s == "RESOURCE_RESERVED")  return EventType::RESOURCE_RESERVED;
    if (s == "WORKER_STARTED")     return EventType::WORKER_STARTED;
    if (s == "JOB_STARTED")        return EventType::JOB_STARTED;
    if (s == "JOB_PROGRESS")       return EventType::JOB_PROGRESS;
    if (s == "JOB_CHECKPOINTED")   return EventType::JOB_CHECKPOINTED;
    if (s == "WORKER_CRASHED")     return EventType::WORKER_CRASHED;
    if (s == "JOB_RETRIED")        return EventType::JOB_RETRIED;
    if (s == "DEADLOCK_DETECTED")  return EventType::DEADLOCK_DETECTED;
    if (s == "RECOVERY_APPLIED")   return EventType::RECOVERY_APPLIED;
    if (s == "JOB_COMPLETED")      return EventType::JOB_COMPLETED;
    if (s == "JOB_FAILED")         return EventType::JOB_FAILED;
    if (s == "JOB_CANCELLED")      return EventType::JOB_CANCELLED;
    if (s == "RESOURCE_RELEASED")  return EventType::RESOURCE_RELEASED;
    return std::nullopt;
}

struct Event {
    std::uint64_t id = 0;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    EventType type = EventType::JOB_SUBMITTED;
    std::string job_id;
    int worker_id = -1;
    std::string payload_json;

    bool operator==(const Event&) const = default;
};

inline void to_json(nlohmann::json& j, const Event& e) {
    j = nlohmann::json{
        {"id", e.id},
        {"timestamp_ms", std::chrono::duration_cast<std::chrono::milliseconds>(e.timestamp.time_since_epoch()).count()},
        {"type", std::string(to_string(e.type))},
        {"job_id", e.job_id},
        {"worker_id", e.worker_id},
        {"payload_json", e.payload_json}
    };
}

inline void from_json(const nlohmann::json& j, Event& e) {
    j.at("id").get_to(e.id);
    int64_t ts_ms = 0;
    j.at("timestamp_ms").get_to(ts_ms);
    e.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ts_ms));
    std::string type_str;
    j.at("type").get_to(type_str);
    e.type = event_type_from_string(type_str).value_or(EventType::JOB_SUBMITTED);
    j.at("job_id").get_to(e.job_id);
    j.at("worker_id").get_to(e.worker_id);
    j.at("payload_json").get_to(e.payload_json);
}

} // namespace astra
