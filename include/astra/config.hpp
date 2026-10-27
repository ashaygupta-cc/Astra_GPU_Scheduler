#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace astra {

struct AstraConfig {
    unsigned int max_workers = 6;
    unsigned int max_cpu_slots = 6;
    std::string default_policy = "priority_aging";
    unsigned int worker_start_timeout_ms = 3000;
    unsigned int heartbeat_interval_ms = 1000;
    unsigned int heartbeat_timeout_ms = 5000;
    std::string shutdown_mode = "drain";
    int default_max_retries = 1;
    std::string workload_root = "./workloads";
    std::string database_path = "astra.db";
    std::string event_log_path = "astra_events.jsonl";
    unsigned int aging_interval_ms = 2000;
    int aging_max_priority = 10;

    bool validate(std::string* error_out = nullptr) const;

    static AstraConfig load_from_file(const std::string& path);
    static AstraConfig load_from_json_string(const std::string& json_str);
};

void to_json(nlohmann::json& j, const AstraConfig& c);
void from_json(const nlohmann::json& j, AstraConfig& c);

} // namespace astra
