#include "astra/workflow.hpp"
#include "astra/command_parser.hpp"

#include <fstream>
#include <sstream>
#include <unordered_set>
#include <queue>
#include <algorithm>
#include <filesystem>
#include <atomic>
#include <functional>

namespace astra {

namespace {

static std::atomic<uint32_t> g_wf_counter{1};

std::string generate_workflow_id() {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "wf_" + std::to_string(now_ms % 1000000) + "_" + std::to_string(g_wf_counter.fetch_add(1));
}

} // namespace

std::optional<WorkflowDefinition> DagEngine::parse_json_file(const std::string& path, std::string* error_out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        if (error_out) *error_out = "Cannot open workflow file: " + path;
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_json_string(buffer.str(), error_out);
}

std::optional<WorkflowDefinition> DagEngine::parse_json_string(const std::string& json_str, std::string* error_out) {
    nlohmann::json root = nlohmann::json::parse(json_str, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        if (error_out) *error_out = "Invalid JSON format: expected root object";
        return std::nullopt;
    }

    WorkflowDefinition def;
    if (root.contains("id") && root["id"].is_string()) {
        def.id = root["id"].get<std::string>();
    } else {
        def.id = generate_workflow_id();
    }

    if (root.contains("name") && root["name"].is_string()) {
        def.name = root["name"].get<std::string>();
    } else {
        def.name = "unnamed_workflow";
    }

    std::string task_field;
    if (root.contains("jobs") && root["jobs"].is_array()) {
        task_field = "jobs";
    } else if (root.contains("tasks") && root["tasks"].is_array()) {
        task_field = "tasks";
    } else {
        if (error_out) *error_out = "Workflow JSON missing 'jobs' or 'tasks' array";
        return std::nullopt;
    }

    const auto& tasks_json = root[task_field];
    for (size_t idx = 0; idx < tasks_json.size(); ++idx) {
        const auto& t_json = tasks_json[idx];
        if (!t_json.is_object()) {
            if (error_out) *error_out = "Task entry #" + std::to_string(idx) + " is not an object";
            return std::nullopt;
        }

        WorkflowTaskDefinition task;
        if (t_json.contains("key") && t_json["key"].is_string()) {
            task.key = t_json["key"].get<std::string>();
        } else if (t_json.contains("id") && t_json["id"].is_string()) {
            task.key = t_json["id"].get<std::string>();
        } else {
            if (error_out) *error_out = "Task #" + std::to_string(idx) + " is missing required 'key' identifier";
            return std::nullopt;
        }

        if (t_json.contains("name") && t_json["name"].is_string()) {
            task.name = t_json["name"].get<std::string>();
        } else {
            task.name = task.key;
        }

        if (t_json.contains("workload") && t_json["workload"].is_string()) {
            task.workload = t_json["workload"].get<std::string>();
        } else if (t_json.contains("executable_path") && t_json["executable_path"].is_string()) {
            task.workload = t_json["executable_path"].get<std::string>();
        } else {
            if (error_out) *error_out = "Task '" + task.key + "' is missing required 'workload' or 'executable_path'";
            return std::nullopt;
        }

        if (t_json.contains("args") && t_json["args"].is_array()) {
            for (const auto& arg : t_json["args"]) {
                if (arg.is_string()) {
                    task.args.push_back(arg.get<std::string>());
                } else {
                    task.args.push_back(arg.dump());
                }
            }
        }

        if (t_json.contains("priority") && t_json["priority"].is_number_integer()) {
            task.priority = t_json["priority"].get<int>();
        }

        if (t_json.contains("max_retries") && t_json["max_retries"].is_number_integer()) {
            task.max_retries = t_json["max_retries"].get<int>();
        }

        if (t_json.contains("resources") && t_json["resources"].is_object()) {
            const auto& r = t_json["resources"];
            if (r.contains("cpu_slots") && r["cpu_slots"].is_number_unsigned()) {
                task.resources.cpu_slots = r["cpu_slots"].get<unsigned int>();
            }
            if (r.contains("ram_mb") && r["ram_mb"].is_number()) {
                task.resources.ram_mb = r["ram_mb"].get<uint64_t>();
            }
            if (r.contains("gpu_memory_mb") && r["gpu_memory_mb"].is_number()) {
                task.resources.gpu_memory_mb = r["gpu_memory_mb"].get<uint64_t>();
            }
            if (r.contains("device") && r["device"].is_string()) {
                auto dev = device_requirement_from_string(r["device"].get<std::string>());
                if (dev.has_value()) {
                    task.resources.device = *dev;
                }
            }
        }

        std::string dep_field;
        if (t_json.contains("depends_on") && t_json["depends_on"].is_array()) {
            dep_field = "depends_on";
        } else if (t_json.contains("dependencies") && t_json["dependencies"].is_array()) {
            dep_field = "dependencies";
        }

        if (!dep_field.empty()) {
            for (const auto& dep : t_json[dep_field]) {
                if (dep.is_string()) {
                    task.depends_on.push_back(dep.get<std::string>());
                }
            }
        }

        def.tasks.push_back(std::move(task));
    }

    return def;
}

bool DagEngine::validate_dag(const WorkflowDefinition& def,
                             std::vector<std::string>& topo_order_out,
                             std::string* error_out) {
    if (def.tasks.empty()) {
        if (error_out) *error_out = "Workflow contains no tasks/jobs";
        return false;
    }

    std::unordered_set<std::string> keys;
    std::unordered_map<std::string, std::vector<std::string>> adj; // u -> [downstream dependents]
    std::unordered_map<std::string, int> in_degree;

    for (const auto& task : def.tasks) {
        if (task.key.empty()) {
            if (error_out) *error_out = "Task key cannot be empty";
            return false;
        }
        if (keys.count(task.key)) {
            if (error_out) *error_out = "Duplicate task key in workflow: '" + task.key + "'";
            return false;
        }
        keys.insert(task.key);
        in_degree[task.key] = 0;
    }

    for (const auto& task : def.tasks) {
        for (const auto& dep : task.depends_on) {
            if (!keys.count(dep)) {
                if (error_out) *error_out = "Task '" + task.key + "' references unknown prerequisite dependency: '" + dep + "'";
                return false;
            }
            if (dep == task.key) {
                if (error_out) *error_out = "Cycle detected: task '" + task.key + "' depends on itself";
                return false;
            }
            adj[dep].push_back(task.key);
            in_degree[task.key]++;
        }
    }

    // Kahn's algorithm for topological sorting
    std::queue<std::string> q;
    for (const auto& [k, deg] : in_degree) {
        if (deg == 0) {
            q.push(k);
        }
    }

    topo_order_out.clear();
    while (!q.empty()) {
        std::string u = q.front();
        q.pop();
        topo_order_out.push_back(u);

        if (adj.count(u)) {
            for (const auto& v : adj[u]) {
                in_degree[v]--;
                if (in_degree[v] == 0) {
                    q.push(v);
                }
            }
        }
    }

    if (topo_order_out.size() != def.tasks.size()) {
        // Cycle detected. Trace exact cycle using 3-color DFS for diagnostic reporting.
        std::string cycle_str;
        std::unordered_map<std::string, int> color; // 0 = unvisited, 1 = in-stack, 2 = finished
        std::vector<std::string> trace;
        bool found = false;

        std::function<bool(const std::string&)> dfs = [&](const std::string& u) -> bool {
            color[u] = 1;
            trace.push_back(u);

            if (adj.count(u)) {
                for (const auto& v : adj[u]) {
                    if (color[v] == 1) {
                        // Cycle back-edge found: find v in trace
                        auto it = std::find(trace.begin(), trace.end(), v);
                        std::ostringstream ss;
                        while (it != trace.end()) {
                            ss << *it << " -> ";
                            ++it;
                        }
                        ss << v;
                        cycle_str = ss.str();
                        return true;
                    }
                    if (color[v] == 0) {
                        if (dfs(v)) return true;
                    }
                }
            }

            trace.pop_back();
            color[u] = 2;
            return false;
        };

        for (const auto& task : def.tasks) {
            if (color[task.key] == 0) {
                if (dfs(task.key)) {
                    found = true;
                    break;
                }
            }
        }

        if (error_out) {
            if (found && !cycle_str.empty()) {
                *error_out = "Cycle detected in workflow: " + cycle_str;
            } else {
                *error_out = "Cycle detected in workflow dependencies";
            }
        }
        return false;
    }

    return true;
}

std::vector<Job> DagEngine::instantiate_jobs(
    const WorkflowDefinition& def,
    const std::string& workload_root,
    std::unordered_map<std::string, std::string>& key_to_job_id_out,
    std::string* error_out) {

    std::vector<std::string> topo_order;
    if (!validate_dag(def, topo_order, error_out)) {
        return {};
    }

    // Build key -> job_id mapping
    key_to_job_id_out.clear();
    for (const auto& task : def.tasks) {
        key_to_job_id_out[task.key] = def.id + "_" + task.key;
    }

    std::vector<Job> jobs;
    jobs.reserve(def.tasks.size());

    auto now = std::chrono::system_clock::now();

    for (const auto& task : def.tasks) {
        Job job;
        job.id = key_to_job_id_out[task.key];
        job.name = task.name.empty() ? task.key : task.name;

        // Resolve workload executable path
        std::filesystem::path p(task.workload);
        if (p.is_absolute()) {
            job.executable_path = p.string();
        } else {
            std::filesystem::path full = std::filesystem::path(workload_root) / p;
            if (std::filesystem::exists(full)) {
                job.executable_path = full.string();
            } else if (std::filesystem::exists(p)) {
                job.executable_path = p.string();
            } else {
                job.executable_path = full.string();
            }
        }

        // Validate security check
        std::string path_err;
        if (!CommandParser::is_safe_workload_path(job.executable_path, workload_root, &path_err)) {
            if (error_out) {
                *error_out = "Task '" + task.key + "' rejected: " + path_err;
            }
            return {};
        }

        job.argv = task.args;
        job.priority = task.priority;
        job.resources = task.resources;
        job.max_retries = task.max_retries;
        job.workflow_id = def.id;
        job.submitted_time = now;

        for (const auto& dep_key : task.depends_on) {
            job.dependencies.push_back(key_to_job_id_out[dep_key]);
        }

        if (job.dependencies.empty()) {
            job.state = JobState::Queued;
            job.enqueued_time = now;
        } else {
            job.state = JobState::WaitingForDependencies;
        }

        jobs.push_back(std::move(job));
    }

    return jobs;
}

} // namespace astra
