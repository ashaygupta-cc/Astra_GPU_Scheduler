#pragma once

#include "astra/job.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <nlohmann/json.hpp>

namespace astra {

struct WorkflowRecord {
    std::string id;
    std::string name;
    std::string state = "Pending"; // Pending, Running, Succeeded, Failed
    std::chrono::system_clock::time_point created_time = std::chrono::system_clock::now();
};

struct WorkflowTaskDefinition {
    std::string key;
    std::string name;
    std::string workload;
    std::vector<std::string> args;
    int priority = 5;
    ResourceRequest resources;
    int max_retries = 1;
    std::vector<std::string> depends_on;
};

struct WorkflowDefinition {
    std::string id;
    std::string name;
    std::vector<WorkflowTaskDefinition> tasks;
};

class DagEngine {
public:
    // Parse workflow JSON definition from file or raw string
    static std::optional<WorkflowDefinition> parse_json_file(const std::string& path, std::string* error_out = nullptr);
    static std::optional<WorkflowDefinition> parse_json_string(const std::string& json_str, std::string* error_out = nullptr);

    // Validate DAG structure using Kahn's algorithm and DFS cycle detection
    // Returns topological ordering in topo_order_out
    static bool validate_dag(const WorkflowDefinition& def,
                             std::vector<std::string>& topo_order_out,
                             std::string* error_out = nullptr);

    // Instantiate concrete Job domain objects with translated dependencies
    static std::vector<Job> instantiate_jobs(const WorkflowDefinition& def,
                                            const std::string& workload_root,
                                            std::unordered_map<std::string, std::string>& key_to_job_id_out,
                                            std::string* error_out = nullptr);
};

} // namespace astra
