#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <ostream>

namespace astra {

enum class JobState {
    Submitted,
    WaitingForDependencies,
    Queued,
    Admitted,
    Running,
    Checkpointing,
    Paused,
    Retrying,
    Succeeded,
    Failed,
    Cancelled,
    Blocked
};

inline constexpr std::string_view to_string(JobState state) noexcept {
    switch (state) {
        case JobState::Submitted:              return "SUBMITTED";
        case JobState::WaitingForDependencies: return "WAITING_FOR_DEPENDENCIES";
        case JobState::Queued:                 return "QUEUED";
        case JobState::Admitted:               return "ADMITTED";
        case JobState::Running:                return "RUNNING";
        case JobState::Checkpointing:          return "CHECKPOINTING";
        case JobState::Paused:                 return "PAUSED";
        case JobState::Retrying:               return "RETRYING";
        case JobState::Succeeded:              return "SUCCEEDED";
        case JobState::Failed:                 return "FAILED";
        case JobState::Cancelled:              return "CANCELLED";
        case JobState::Blocked:                return "BLOCKED";
    }
    return "UNKNOWN";
}

inline std::optional<JobState> job_state_from_string(std::string_view s) noexcept {
    if (s == "SUBMITTED")                 return JobState::Submitted;
    if (s == "WAITING_FOR_DEPENDENCIES") return JobState::WaitingForDependencies;
    if (s == "QUEUED")                    return JobState::Queued;
    if (s == "ADMITTED")                  return JobState::Admitted;
    if (s == "RUNNING")                   return JobState::Running;
    if (s == "CHECKPOINTING")             return JobState::Checkpointing;
    if (s == "PAUSED")                    return JobState::Paused;
    if (s == "RETRYING")                  return JobState::Retrying;
    if (s == "SUCCEEDED")                 return JobState::Succeeded;
    if (s == "FAILED")                    return JobState::Failed;
    if (s == "CANCELLED")                 return JobState::Cancelled;
    if (s == "BLOCKED")                   return JobState::Blocked;
    return std::nullopt;
}

inline bool is_terminal_state(JobState state) noexcept {
    return state == JobState::Succeeded ||
           state == JobState::Failed ||
           state == JobState::Cancelled;
}

inline bool is_valid_transition(JobState from, JobState to) noexcept {
    if (from == to) {
        return true;
    }

    // Terminal states cannot transition to any other state
    if (is_terminal_state(from)) {
        return false;
    }

    switch (from) {
        case JobState::Submitted:
            return to == JobState::WaitingForDependencies ||
                   to == JobState::Queued ||
                   to == JobState::Cancelled ||
                   to == JobState::Blocked;

        case JobState::WaitingForDependencies:
            return to == JobState::Queued ||
                   to == JobState::Cancelled ||
                   to == JobState::Blocked;

        case JobState::Queued:
            return to == JobState::Admitted ||
                   to == JobState::Cancelled ||
                   to == JobState::Blocked;

        case JobState::Admitted:
            return to == JobState::Running ||
                   to == JobState::Queued ||    // Preemption / yield before dispatch
                   to == JobState::Cancelled ||
                   to == JobState::Blocked;

        case JobState::Running:
            return to == JobState::Succeeded ||
                   to == JobState::Failed ||
                   to == JobState::Retrying ||
                   to == JobState::Cancelled ||
                   to == JobState::Checkpointing ||
                   to == JobState::Blocked;

        case JobState::Checkpointing:
            return to == JobState::Paused ||
                   to == JobState::Running ||
                   to == JobState::Failed ||
                   to == JobState::Cancelled;

        case JobState::Paused:
            return to == JobState::Running ||
                   to == JobState::Queued ||
                   to == JobState::Cancelled;

        case JobState::Retrying:
            return to == JobState::Queued ||
                   to == JobState::Failed ||
                   to == JobState::Cancelled;

        case JobState::Blocked:
            return to == JobState::Queued ||    // Dependency or resource resolved
                   to == JobState::Cancelled;

        case JobState::Succeeded:
        case JobState::Failed:
        case JobState::Cancelled:
            return false;
    }

    return false;
}

inline bool validate_and_transition(JobState& current, JobState target, std::string* error_out = nullptr) {
    if (is_valid_transition(current, target)) {
        current = target;
        return true;
    }
    if (error_out != nullptr) {
        *error_out = "Illegal state transition from " + std::string(to_string(current)) +
                     " to " + std::string(to_string(target));
    }
    return false;
}

inline std::ostream& operator<<(std::ostream& os, JobState state) {
    return os << to_string(state);
}

} // namespace astra
