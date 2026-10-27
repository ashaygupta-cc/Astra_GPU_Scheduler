#include "astra/config.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace astra {

bool AstraConfig::validate(std::string* error_out) const {
    if (max_workers == 0) {
        if (error_out) *error_out = "max_workers must be greater than 0";
        return false;
    }
    if (max_cpu_slots == 0) {
        if (error_out) *error_out = "max_cpu_slots must be greater than 0";
        return false;
    }
    if (default_policy != "fcfs" && default_policy != "priority" &&
        default_policy != "priority_aging" && default_policy != "sjf") {
        if (error_out) *error_out = "default_policy must be 'fcfs', 'priority', 'priority_aging', or 'sjf'";
        return false;
    }
    if (worker_start_timeout_ms == 0) {
        if (error_out) *error_out = "worker_start_timeout_ms must be greater than 0";
        return false;
    }
    if (heartbeat_interval_ms == 0) {
        if (error_out) *error_out = "heartbeat_interval_ms must be greater than 0";
        return false;
    }
    if (heartbeat_timeout_ms <= heartbeat_interval_ms) {
        if (error_out) *error_out = "heartbeat_timeout_ms must be greater than heartbeat_interval_ms";
        return false;
    }
    if (shutdown_mode != "drain" && shutdown_mode != "immediate") {
        if (error_out) *error_out = "shutdown_mode must be 'drain' or 'immediate'";
        return false;
    }
    if (default_max_retries < 0) {
        if (error_out) *error_out = "default_max_retries must be non-negative";
        return false;
    }
    if (workload_root.empty()) {
        if (error_out) *error_out = "workload_root path cannot be empty";
        return false;
    }
    if (database_path.empty()) {
        if (error_out) *error_out = "database_path cannot be empty";
        return false;
    }
    return true;
}

AstraConfig AstraConfig::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open config file: " + path);
    }
    nlohmann::json j;
    file >> j;
    AstraConfig config = j.get<AstraConfig>();
    std::string err;
    if (!config.validate(&err)) {
        throw std::runtime_error("Invalid configuration: " + err);
    }
    return config;
}

AstraConfig AstraConfig::load_from_json_string(const std::string& json_str) {
    nlohmann::json j = nlohmann::json::parse(json_str);
    AstraConfig config = j.get<AstraConfig>();
    std::string err;
    if (!config.validate(&err)) {
        throw std::runtime_error("Invalid configuration: " + err);
    }
    return config;
}

void to_json(nlohmann::json& j, const AstraConfig& c) {
    j = nlohmann::json{
        {"max_workers", c.max_workers},
        {"max_cpu_slots", c.max_cpu_slots},
        {"default_policy", c.default_policy},
        {"worker_start_timeout_ms", c.worker_start_timeout_ms},
        {"heartbeat_interval_ms", c.heartbeat_interval_ms},
        {"heartbeat_timeout_ms", c.heartbeat_timeout_ms},
        {"shutdown_mode", c.shutdown_mode},
        {"default_max_retries", c.default_max_retries},
        {"workload_root", c.workload_root},
        {"database_path", c.database_path},
        {"event_log_path", c.event_log_path},
        {"aging_interval_ms", c.aging_interval_ms},
        {"aging_max_priority", c.aging_max_priority}
    };
}

void from_json(const nlohmann::json& j, AstraConfig& c) {
    if (j.contains("max_workers")) j.at("max_workers").get_to(c.max_workers);
    if (j.contains("max_cpu_slots")) j.at("max_cpu_slots").get_to(c.max_cpu_slots);
    if (j.contains("default_policy")) j.at("default_policy").get_to(c.default_policy);
    if (j.contains("worker_start_timeout_ms")) j.at("worker_start_timeout_ms").get_to(c.worker_start_timeout_ms);
    if (j.contains("heartbeat_interval_ms")) j.at("heartbeat_interval_ms").get_to(c.heartbeat_interval_ms);
    if (j.contains("heartbeat_timeout_ms")) j.at("heartbeat_timeout_ms").get_to(c.heartbeat_timeout_ms);
    if (j.contains("shutdown_mode")) j.at("shutdown_mode").get_to(c.shutdown_mode);
    if (j.contains("default_max_retries")) j.at("default_max_retries").get_to(c.default_max_retries);
    if (j.contains("workload_root")) j.at("workload_root").get_to(c.workload_root);
    if (j.contains("database_path")) j.at("database_path").get_to(c.database_path);
    if (j.contains("event_log_path")) j.at("event_log_path").get_to(c.event_log_path);
    if (j.contains("aging_interval_ms")) j.at("aging_interval_ms").get_to(c.aging_interval_ms);
    if (j.contains("aging_max_priority")) j.at("aging_max_priority").get_to(c.aging_max_priority);
}

} // namespace astra
