#pragma once

#include "astra/job_state.hpp"

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <cstdint>
#include <sys/types.h>
#include <nlohmann/json.hpp>

namespace astra {

enum class DeviceRequirement {
    CpuOnly,
    GpuPreferred,
    GpuRequired,
    Either
};

inline constexpr std::string_view to_string(DeviceRequirement dev) noexcept {
    switch (dev) {
        case DeviceRequirement::CpuOnly:      return "CPU_ONLY";
        case DeviceRequirement::GpuPreferred: return "GPU_PREFERRED";
        case DeviceRequirement::GpuRequired:  return "GPU_REQUIRED";
        case DeviceRequirement::Either:       return "EITHER";
    }
    return "UNKNOWN";
}

inline std::optional<DeviceRequirement> device_requirement_from_string(std::string_view s) noexcept {
    if (s == "CPU_ONLY" || s == "cpu" || s == "CpuOnly")           return DeviceRequirement::CpuOnly;
    if (s == "GPU_PREFERRED" || s == "GpuPreferred")               return DeviceRequirement::GpuPreferred;
    if (s == "GPU_REQUIRED" || s == "gpu" || s == "GpuRequired")   return DeviceRequirement::GpuRequired;
    if (s == "EITHER" || s == "either" || s == "Either")           return DeviceRequirement::Either;
    return std::nullopt;
}

struct ResourceRequest {
    unsigned int cpu_slots = 1;
    std::uint64_t ram_mb = 128;
    std::uint64_t gpu_memory_mb = 0;
    DeviceRequirement device = DeviceRequirement::CpuOnly;

    bool operator==(const ResourceRequest&) const = default;
};

inline void to_json(nlohmann::json& j, const ResourceRequest& r) {
    j = nlohmann::json{
        {"cpu_slots", r.cpu_slots},
        {"ram_mb", r.ram_mb},
        {"gpu_memory_mb", r.gpu_memory_mb},
        {"device", std::string(to_string(r.device))}
    };
}

inline void from_json(const nlohmann::json& j, ResourceRequest& r) {
    j.at("cpu_slots").get_to(r.cpu_slots);
    j.at("ram_mb").get_to(r.ram_mb);
    j.at("gpu_memory_mb").get_to(r.gpu_memory_mb);
    std::string dev_str;
    j.at("device").get_to(dev_str);
    r.device = device_requirement_from_string(dev_str).value_or(DeviceRequirement::CpuOnly);
}

struct JobAttempt {
    std::uint64_t attempt_id = 0;
    std::string job_id;
    int attempt_number = 1;
    int worker_id = -1;
    pid_t pid = -1;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    int exit_code = 0;
    std::string error_message;

    bool operator==(const JobAttempt&) const = default;
};

inline void to_json(nlohmann::json& j, const JobAttempt& a) {
    j = nlohmann::json{
        {"attempt_id", a.attempt_id},
        {"job_id", a.job_id},
        {"attempt_number", a.attempt_number},
        {"worker_id", a.worker_id},
        {"pid", a.pid},
        {"start_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(a.start_time.time_since_epoch()).count()},
        {"end_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(a.end_time.time_since_epoch()).count()},
        {"exit_code", a.exit_code},
        {"error_message", a.error_message}
    };
}

inline void from_json(const nlohmann::json& j, JobAttempt& a) {
    j.at("attempt_id").get_to(a.attempt_id);
    j.at("job_id").get_to(a.job_id);
    j.at("attempt_number").get_to(a.attempt_number);
    j.at("worker_id").get_to(a.worker_id);
    j.at("pid").get_to(a.pid);
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    j.at("start_time_ms").get_to(start_ms);
    j.at("end_time_ms").get_to(end_ms);
    a.start_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(start_ms));
    a.end_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(end_ms));
    j.at("exit_code").get_to(a.exit_code);
    j.at("error_message").get_to(a.error_message);
}

struct Job {
    std::string id;
    std::string name;
    std::string executable_path;
    std::vector<std::string> argv;
    int priority = 5;
    std::optional<std::chrono::system_clock::time_point> deadline;
    ResourceRequest resources;
    std::vector<std::string> dependencies;
    JobState state = JobState::Submitted;
    int max_retries = 1;
    int retry_count = 0;
    bool checkpointable = false;
    int assigned_worker_id = -1;
    pid_t worker_pid = -1;
    int exit_code = 0;
    std::string failure_reason;
    std::string workflow_id;

    std::chrono::system_clock::time_point submitted_time = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point enqueued_time;
    std::chrono::system_clock::time_point started_time;
    std::chrono::system_clock::time_point completed_time;

    std::vector<JobAttempt> attempts;

    bool validate(std::string* error_out = nullptr) const {
        if (id.empty()) {
            if (error_out) *error_out = "Job ID cannot be empty";
            return false;
        }
        if (name.empty()) {
            if (error_out) *error_out = "Job name cannot be empty";
            return false;
        }
        if (executable_path.empty()) {
            if (error_out) *error_out = "Job executable path cannot be empty";
            return false;
        }
        if (resources.cpu_slots == 0) {
            if (error_out) *error_out = "Job cpu_slots must be at least 1";
            return false;
        }
        if (priority < 0) {
            if (error_out) *error_out = "Job priority must be non-negative";
            return false;
        }
        if (max_retries < 0) {
            if (error_out) *error_out = "Job max_retries must be non-negative";
            return false;
        }
        return true;
    }
};

inline void to_json(nlohmann::json& j, const Job& job) {
    j = nlohmann::json{
        {"id", job.id},
        {"name", job.name},
        {"executable_path", job.executable_path},
        {"argv", job.argv},
        {"priority", job.priority},
        {"resources", job.resources},
        {"dependencies", job.dependencies},
        {"state", std::string(to_string(job.state))},
        {"max_retries", job.max_retries},
        {"retry_count", job.retry_count},
        {"checkpointable", job.checkpointable},
        {"assigned_worker_id", job.assigned_worker_id},
        {"worker_pid", job.worker_pid},
        {"exit_code", job.exit_code},
        {"failure_reason", job.failure_reason},
        {"workflow_id", job.workflow_id},
        {"submitted_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(job.submitted_time.time_since_epoch()).count()},
        {"enqueued_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(job.enqueued_time.time_since_epoch()).count()},
        {"started_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(job.started_time.time_since_epoch()).count()},
        {"completed_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(job.completed_time.time_since_epoch()).count()},
        {"attempts", job.attempts}
    };
    if (job.deadline.has_value()) {
        j["deadline_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(job.deadline->time_since_epoch()).count();
    } else {
        j["deadline_ms"] = nullptr;
    }
}

inline void from_json(const nlohmann::json& j, Job& job) {
    j.at("id").get_to(job.id);
    j.at("name").get_to(job.name);
    j.at("executable_path").get_to(job.executable_path);
    j.at("argv").get_to(job.argv);
    j.at("priority").get_to(job.priority);
    j.at("resources").get_to(job.resources);
    j.at("dependencies").get_to(job.dependencies);
    std::string state_str;
    j.at("state").get_to(state_str);
    job.state = job_state_from_string(state_str).value_or(JobState::Submitted);
    j.at("max_retries").get_to(job.max_retries);
    j.at("retry_count").get_to(job.retry_count);
    j.at("checkpointable").get_to(job.checkpointable);
    j.at("assigned_worker_id").get_to(job.assigned_worker_id);
    j.at("worker_pid").get_to(job.worker_pid);
    j.at("exit_code").get_to(job.exit_code);
    j.at("failure_reason").get_to(job.failure_reason);
    j.at("workflow_id").get_to(job.workflow_id);

    int64_t sub_ms = 0, enq_ms = 0, start_ms = 0, comp_ms = 0;
    j.at("submitted_time_ms").get_to(sub_ms);
    j.at("enqueued_time_ms").get_to(enq_ms);
    j.at("started_time_ms").get_to(start_ms);
    j.at("completed_time_ms").get_to(comp_ms);
    job.submitted_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(sub_ms));
    job.enqueued_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(enq_ms));
    job.started_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(start_ms));
    job.completed_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(comp_ms));

    if (j.contains("attempts") && !j["attempts"].is_null()) {
        j.at("attempts").get_to(job.attempts);
    }
    if (j.contains("deadline_ms") && !j["deadline_ms"].is_null()) {
        int64_t d_ms = j["deadline_ms"].get<int64_t>();
        job.deadline = std::chrono::system_clock::time_point(std::chrono::milliseconds(d_ms));
    } else {
        job.deadline = std::nullopt;
    }
}

} // namespace astra
