#include "astra/cli_handler.hpp"
#include "astra/command_parser.hpp"
#include "astra/version.hpp"
#include "astra/tui.hpp"
#include "astra/tui_snapshot.hpp"
#include "astra/resource_manager.hpp"
#include "astra/worker_manager.hpp"
#include "astra/workflow.hpp"

#include <iomanip>
#include <sstream>
#include <filesystem>
#include <atomic>
#include <chrono>

namespace astra {

namespace {

static std::atomic<uint32_t> g_job_counter{1};

std::string format_duration(std::chrono::milliseconds ms) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(ms).count();
    if (secs < 60) {
        return std::to_string(secs) + "s";
    }
    auto mins = secs / 60;
    secs = secs % 60;
    return std::to_string(mins) + "m " + std::to_string(secs) + "s";
}

std::string format_time_point(const std::chrono::system_clock::time_point& tp) {
    if (tp == std::chrono::system_clock::time_point{}) {
        return "-";
    }
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
    localtime_r(&time_t_val, &tm_buf);
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

} // namespace

CliHandler::CliHandler(JobStore& store, AstraConfig& config)
    : store_(store), config_(config) {}

std::string CliHandler::generate_job_id() {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    uint32_t counter = g_job_counter.fetch_add(1);
    std::ostringstream ss;
    ss << "job-" << (now_ms % 1000000) << "-" << counter;
    return ss.str();
}

int CliHandler::execute(const Command& cmd, std::ostream& out, std::ostream& err) {
    return std::visit([this, &out, &err](const auto& c) -> int {
        using T = std::decay_t<decltype(c)>;

        if constexpr (std::is_same_v<T, HelpCommand>) {
            out << astra::ASTRA_BANNER << "\n"
                << "ASTRA - Autonomous CPU/GPU Job Scheduler (v" << astra::ASTRA_VERSION << ")\n\n"
                << "COMMANDS:\n"
                << "  help                      Show this help manual\n"
                << "  version                   Display version and system diagnostics\n"
                << "  submit <workload> [opts]  Submit a workload binary for execution\n"
                << "  ps [--all]                List active / completed scheduled jobs\n"
                << "  inspect <job-id>          Inspect complete metadata and execution history\n"
                << "  cancel <job-id>           Cancel a queued or running job\n"
                << "  logs <job-id>             View captured execution stdout/stderr\n"
                << "  workers                   Show worker process pool status\n"
                << "  policy show|set <name>    View or update scheduling policy\n"
                << "  config show               Display current runtime configuration\n"
                << "  events [--job <id>]       Query append-only audit event history\n"
                << "  workflow submit <file>    Submit a multi-job DAG workflow\n"
                << "  deadlock demo             Run Banker's Algorithm deadlock simulation\n"
                << "  demo                      Execute automated scheduling demo\n"
                << "  top                       Open interactive full-screen ncurses TUI\n"
                << "  shutdown [--immediate]    Gracefully terminate the scheduler daemon\n\n"
                << "SUBMIT OPTIONS:\n"
                << "  --cpu-slots N             CPU core slots to allocate (default: 1)\n"
                << "  --ram-mb N                Estimated memory requirement in MB (default: 128)\n"
                << "  --gpu                     Require GPU device execution\n"
                << "  --gpu-preferred           Prefer GPU with CPU fallback support\n"
                << "  --gpu-memory-mb N         Estimated GPU VRAM in MB\n"
                << "  --priority N              Job priority (default: 5, higher runs first)\n"
                << "  --max-retries N           Maximum failure retry attempts (default: 1)\n"
                << "  --timeout SECONDS         Job execution deadline timeout\n"
                << "  --depends-on ID1,ID2      Comma-separated list of prerequisite job IDs\n"
                << "  --checkpointable          Enable cooperative checkpoint support\n"
                << "  --args <args...>          Arguments passed directly to workload binary\n";
            return 0;
        }

        else if constexpr (std::is_same_v<T, VersionCommand>) {
            out << astra::ASTRA_BANNER << "\n"
                << "ASTRA Scheduler Version " << astra::ASTRA_VERSION << " (C++20 Linux / WSL2)\n"
                << "Build: Release-Ready POSIX Systems Engine\n";
            return 0;
        }

        else if constexpr (std::is_same_v<T, SubmitCommand>) {
            Job job;
            job.id = generate_job_id();
            job.executable_path = c.workload;

            // Extract filename from workload path as friendly job name
            std::filesystem::path p(c.workload);
            job.name = p.stem().string();
            job.argv = c.args;
            job.resources.cpu_slots = c.cpu_slots;
            job.resources.ram_mb = c.ram_mb;
            job.resources.gpu_memory_mb = c.gpu_memory_mb;
            job.resources.device = c.device;
            job.priority = c.priority;
            job.max_retries = c.max_retries;
            job.checkpointable = c.checkpointable;
            job.dependencies = c.depends_on;

            if (c.timeout_seconds > 0) {
                job.deadline = std::chrono::system_clock::now() + std::chrono::seconds(c.timeout_seconds);
            }

            job.submitted_time = std::chrono::system_clock::now();
            if (job.dependencies.empty()) {
                job.state = JobState::Queued;
                job.enqueued_time = job.submitted_time;
            } else {
                job.state = JobState::WaitingForDependencies;
            }

            std::string val_err;
            if (!job.validate(&val_err)) {
                err << "[ERROR] Job validation failed: " << val_err << "\n";
                return 1;
            }

            std::string db_err;
            if (!store_.save_job(job, &db_err)) {
                err << "[ERROR] Failed to persist job: " << db_err << "\n";
                return 1;
            }

            // Emit audit events
            Event ev_sub{0, job.submitted_time, EventType::JOB_SUBMITTED, job.id, -1, "{}"};
            store_.record_event(ev_sub);

            Event ev_val{0, job.submitted_time, EventType::JOB_VALIDATED, job.id, -1, "{}"};
            store_.record_event(ev_val);

            if (job.state == JobState::Queued) {
                Event ev_q{0, job.enqueued_time, EventType::JOB_QUEUED, job.id, -1, "{}"};
                store_.record_event(ev_q);
            }

            out << "[SUBMITTED] Job ID: " << job.id << "\n"
                << "  Workload:    " << job.executable_path << "\n"
                << "  Initial:     " << to_string(job.state) << "\n"
                << "  Priority:    " << job.priority << "\n"
                << "  Device:      " << to_string(job.resources.device) << "\n"
                << "  Resources:   CPU=" << job.resources.cpu_slots 
                << " slots, RAM=" << job.resources.ram_mb << " MB\n";
            if (!job.dependencies.empty()) {
                out << "  Dependencies: ";
                for (size_t d = 0; d < job.dependencies.size(); ++d) {
                    out << (d > 0 ? ", " : "") << job.dependencies[d];
                }
                out << "\n";
            }
            return 0;
        }

        else if constexpr (std::is_same_v<T, PsCommand>) {
            std::string err_str;
            auto jobs = store_.get_all_jobs(&err_str);
            if (!err_str.empty()) {
                err << "[ERROR] Failed to query jobs: " << err_str << "\n";
                return 1;
            }

            if (jobs.empty()) {
                out << "No jobs currently in scheduler queue.\n";
                return 0;
            }

            out << std::left 
                << std::setw(16) << "JOB ID"
                << std::setw(18) << "NAME"
                << std::setw(15) << "STATE"
                << std::setw(14) << "DEVICE"
                << std::setw(6)  << "PRI"
                << std::setw(14) << "WORKER/PID"
                << std::setw(10) << "ELAPSED"
                << std::setw(6)  << "EXIT"
                << "\n";
            out << std::string(99, '-') << "\n";

            auto now = std::chrono::system_clock::now();
            for (const auto& j : jobs) {
                if (!c.all && is_terminal_state(j.state)) {
                    continue;
                }

                std::string worker_str = "-";
                if (j.assigned_worker_id >= 0) {
                    worker_str = "w" + std::to_string(j.assigned_worker_id);
                    if (j.worker_pid > 0) {
                        worker_str += ":" + std::to_string(j.worker_pid);
                    }
                }

                std::string elapsed_str = "-";
                if (j.started_time != std::chrono::system_clock::time_point{}) {
                    auto end_tp = (j.completed_time != std::chrono::system_clock::time_point{}) ? j.completed_time : now;
                    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end_tp - j.started_time);
                    elapsed_str = format_duration(dur);
                } else if (j.submitted_time != std::chrono::system_clock::time_point{}) {
                    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(now - j.submitted_time);
                    elapsed_str = format_duration(dur);
                }

                std::string exit_str = is_terminal_state(j.state) ? std::to_string(j.exit_code) : "-";

                out << std::left
                    << std::setw(16) << j.id
                    << std::setw(18) << (j.name.size() > 16 ? j.name.substr(0, 15) + "…" : j.name)
                    << std::setw(15) << to_string(j.state)
                    << std::setw(14) << to_string(j.resources.device)
                    << std::setw(6)  << j.priority
                    << std::setw(14) << worker_str
                    << std::setw(10) << elapsed_str
                    << std::setw(6)  << exit_str
                    << "\n";
            }
            return 0;
        }

        else if constexpr (std::is_same_v<T, InspectCommand>) {
            std::string err_str;
            auto job_opt = store_.get_job(c.job_id, &err_str);
            if (!job_opt) {
                err << "[ERROR] Job not found: " << c.job_id << "\n";
                return 1;
            }

            const auto& j = *job_opt;
            nlohmann::json json_view = j;
            out << "Job Inspection Report: " << j.id << "\n"
                << "==================================================\n"
                << "Name:               " << j.name << "\n"
                << "Executable Path:    " << j.executable_path << "\n"
                << "State:              " << to_string(j.state) << "\n"
                << "Priority:           " << j.priority << "\n"
                << "Device Requirement: " << to_string(j.resources.device) << "\n"
                << "Resources:          CPU=" << j.resources.cpu_slots 
                << " slots, RAM=" << j.resources.ram_mb << " MB, GPU Mem=" << j.resources.gpu_memory_mb << " MB\n"
                << "Max Retries:        " << j.max_retries << " (Attempts so far: " << j.attempts.size() << ")\n"
                << "Assigned Worker:    " << (j.assigned_worker_id >= 0 ? std::to_string(j.assigned_worker_id) : "-")
                << " (PID: " << (j.worker_pid > 0 ? std::to_string(j.worker_pid) : "-") << ")\n"
                << "Exit Code:          " << j.exit_code << "\n"
                << "Failure Reason:     " << (j.failure_reason.empty() ? "None" : j.failure_reason) << "\n"
                << "Submitted At:       " << format_time_point(j.submitted_time) << "\n"
                << "Enqueued At:        " << format_time_point(j.enqueued_time) << "\n"
                << "Started At:         " << format_time_point(j.started_time) << "\n"
                << "Completed At:       " << format_time_point(j.completed_time) << "\n";

            if (!j.dependencies.empty()) {
                out << "Dependencies:       ";
                for (size_t idx = 0; idx < j.dependencies.size(); ++idx) {
                    out << (idx > 0 ? ", " : "") << j.dependencies[idx];
                }
                out << "\n";
            }

            if (!j.attempts.empty()) {
                out << "\nAttempts History:\n";
                for (const auto& att : j.attempts) {
                    out << "  - Attempt #" << att.attempt_number 
                        << " on worker #" << att.worker_id << " (PID " << att.pid << ")"
                        << " -> Exit Code: " << att.exit_code;
                    if (!att.error_message.empty()) out << " (" << att.error_message << ")";
                    out << "\n";
                }
            }

            out << "==================================================\n";
            return 0;
        }

        else if constexpr (std::is_same_v<T, CancelCommand>) {
            std::string err_str;
            auto job_opt = store_.get_job(c.job_id, &err_str);
            if (!job_opt) {
                err << "[ERROR] Job not found: " << c.job_id << "\n";
                return 1;
            }

            if (is_terminal_state(job_opt->state)) {
                err << "[ERROR] Job " << c.job_id << " is already in terminal state " 
                    << to_string(job_opt->state) << "\n";
                return 1;
            }

            if (!store_.update_job_state(c.job_id, JobState::Cancelled, -1, "Cancelled by user via CLI", &err_str)) {
                err << "[ERROR] Failed to cancel job: " << err_str << "\n";
                return 1;
            }

            Event ev{0, std::chrono::system_clock::now(), EventType::JOB_CANCELLED, c.job_id, -1, "{\"reason\": \"user_cancel\"}"};
            store_.record_event(ev);

            out << "[CANCELLED] Job " << c.job_id << " cancelled successfully.\n";
            return 0;
        }

        else if constexpr (std::is_same_v<T, LogsCommand>) {
            std::string err_str;
            auto job_opt = store_.get_job(c.job_id, &err_str);
            if (!job_opt) {
                err << "[ERROR] Job not found: " << c.job_id << "\n";
                return 1;
            }
            out << "--- Output logs for Job " << c.job_id << " (" << job_opt->name << ") ---\n";
            out << "[STATUS: " << to_string(job_opt->state) << "]\n";
            if (!job_opt->failure_reason.empty()) {
                out << "Diagnostic Log: " << job_opt->failure_reason << "\n";
            } else {
                out << "(No recorded stdout/stderr output yet for in-flight/queued job)\n";
            }
            return 0;
        }

        else if constexpr (std::is_same_v<T, WorkersCommand>) {
            std::string err_str;
            auto workers = store_.get_all_workers(&err_str);
            if (workers.empty()) {
                out << "Worker pool is currently idle. (Configured max_workers: " 
                    << config_.max_workers << ")\n";
                return 0;
            }

            out << std::left
                << std::setw(12) << "WORKER ID"
                << std::setw(10) << "PID"
                << std::setw(14) << "STATE"
                << std::setw(16) << "CURRENT JOB"
                << std::setw(10) << "RESTARTS"
                << std::setw(20) << "LAST HEARTBEAT"
                << "\n";
            out << std::string(82, '-') << "\n";

            for (const auto& w : workers) {
                out << std::left
                    << std::setw(12) << ("worker-" + std::to_string(w.id))
                    << std::setw(10) << (w.pid > 0 ? std::to_string(w.pid) : "-")
                    << std::setw(14) << to_string(w.state)
                    << std::setw(16) << (w.current_job_id.empty() ? "-" : w.current_job_id)
                    << std::setw(10) << w.restart_count
                    << std::setw(20) << format_time_point(w.last_heartbeat)
                    << "\n";
            }
            return 0;
        }

        else if constexpr (std::is_same_v<T, PolicyCommand>) {
            if (c.action == PolicyCommand::Action::Show) {
                out << "Current Scheduling Policy: " << config_.default_policy << "\n";
                out << "Supported policies: fcfs, priority, priority_aging, sjf\n";
                return 0;
            } else {
                config_.default_policy = c.policy_name;
                out << "[OK] Scheduling policy updated to: " << config_.default_policy << "\n";
                return 0;
            }
        }

        else if constexpr (std::is_same_v<T, ConfigCommand>) {
            nlohmann::json j = config_;
            out << "Active ASTRA Configuration:\n" << j.dump(2) << "\n";
            return 0;
        }

        else if constexpr (std::is_same_v<T, EventsCommand>) {
            std::string err_str;
            auto events = store_.get_events(c.job_id, c.limit, &err_str);
            if (!err_str.empty()) {
                err << "[ERROR] Failed to query events: " << err_str << "\n";
                return 1;
            }

            if (events.empty()) {
                out << "No audit events recorded" << (c.job_id.empty() ? "" : " for job " + c.job_id) << ".\n";
                return 0;
            }

            out << std::left
                << std::setw(8)  << "EVENT#"
                << std::setw(22) << "TIMESTAMP"
                << std::setw(22) << "TYPE"
                << std::setw(16) << "JOB ID"
                << std::setw(10) << "WORKER"
                << "PAYLOAD"
                << "\n";
            out << std::string(90, '-') << "\n";

            for (const auto& ev : events) {
                out << std::left
                    << std::setw(8)  << ev.id
                    << std::setw(22) << format_time_point(ev.timestamp)
                    << std::setw(22) << to_string(ev.type)
                    << std::setw(16) << (ev.job_id.empty() ? "-" : ev.job_id)
                    << std::setw(10) << (ev.worker_id >= 0 ? std::to_string(ev.worker_id) : "-")
                    << ev.payload_json
                    << "\n";
            }
            return 0;
        }

        else if constexpr (std::is_same_v<T, WorkflowCommand>) {
            if (c.action == WorkflowCommand::Action::Submit) {
                std::string parse_err;
                auto wf_def = DagEngine::parse_json_file(c.target, &parse_err);
                if (!wf_def) {
                    err << "[ERROR] Failed to parse workflow: " << parse_err << "\n";
                    return 1;
                }

                std::vector<std::string> topo_order;
                std::string dag_err;
                if (!DagEngine::validate_dag(*wf_def, topo_order, &dag_err)) {
                    err << "[ERROR] Workflow validation failed: " << dag_err << "\n";
                    return 1;
                }

                std::unordered_map<std::string, std::string> key_to_job_id;
                std::string inst_err;
                auto jobs = DagEngine::instantiate_jobs(*wf_def, config_.workload_root, key_to_job_id, &inst_err);
                if (jobs.empty() && !wf_def->tasks.empty()) {
                    err << "[ERROR] Failed to instantiate workflow jobs: " << inst_err << "\n";
                    return 1;
                }

                WorkflowRecord rec;
                rec.id = wf_def->id;
                rec.name = wf_def->name;
                rec.state = "Running";
                rec.created_time = std::chrono::system_clock::now();
                store_.save_workflow(rec);

                for (const auto& j : jobs) {
                    store_.save_job(j);
                    Event ev_sub{0, j.submitted_time, EventType::JOB_SUBMITTED, j.id, -1, 
                                 "{\"workflow_id\": \"" + wf_def->id + "\", \"task_name\": \"" + j.name + "\"}"};
                    store_.record_event(ev_sub);

                    if (j.state == JobState::Queued) {
                        Event ev_q{0, j.enqueued_time, EventType::JOB_QUEUED, j.id, -1, "{}"};
                        store_.record_event(ev_q);
                    }
                }

                out << "Workflow submitted successfully!\n"
                    << "Workflow ID:   " << wf_def->id << "\n"
                    << "Pipeline Name: " << wf_def->name << "\n"
                    << "Total Tasks:   " << jobs.size() << "\n"
                    << "Topological:   ";
                for (size_t i = 0; i < topo_order.size(); ++i) {
                    out << topo_order[i] << (i + 1 < topo_order.size() ? " -> " : "\n");
                }
                out << "Jobs:\n";
                for (const auto& j : jobs) {
                    out << "  - " << std::left << std::setw(15) << j.name
                        << " -> " << std::setw(25) << j.id
                        << " (" << to_string(j.state);
                    if (!j.dependencies.empty()) {
                        out << ", Depends: [";
                        for (size_t d = 0; d < j.dependencies.size(); ++d) {
                            out << j.dependencies[d] << (d + 1 < j.dependencies.size() ? ", " : "");
                        }
                        out << "]";
                    }
                    out << ")\n";
                }
                return 0;
            } else if (c.action == WorkflowCommand::Action::Status) {
                auto wf_opt = store_.get_workflow(c.target);
                if (!wf_opt) {
                    err << "[ERROR] Workflow not found: " << c.target << "\n";
                    return 1;
                }

                auto wf_jobs = store_.get_jobs_by_workflow(c.target);
                size_t succeeded_cnt = 0;
                size_t failed_cnt = 0;
                size_t running_cnt = 0;
                for (const auto& j : wf_jobs) {
                    if (j.state == JobState::Succeeded) succeeded_cnt++;
                    else if (j.state == JobState::Failed || j.state == JobState::Cancelled || j.state == JobState::Blocked) failed_cnt++;
                    else if (j.state == JobState::Running) running_cnt++;
                }

                std::string overall = wf_opt->state;
                if (succeeded_cnt == wf_jobs.size() && !wf_jobs.empty()) {
                    overall = "Succeeded";
                    store_.update_workflow_state(c.target, "Succeeded");
                } else if (failed_cnt > 0 && running_cnt == 0 && (succeeded_cnt + failed_cnt == wf_jobs.size())) {
                    overall = "Failed";
                    store_.update_workflow_state(c.target, "Failed");
                } else if (running_cnt > 0) {
                    overall = "Running";
                }

                out << "Workflow Status: " << wf_opt->id << " (" << wf_opt->name << ")\n"
                    << "Overall State:   " << overall 
                    << " (" << succeeded_cnt << "/" << wf_jobs.size() << " completed, "
                    << failed_cnt << " failed/blocked)\n";
                out << std::string(80, '-') << "\n";
                out << std::left
                    << std::setw(20) << "TASK"
                    << std::setw(26) << "JOB ID"
                    << std::setw(18) << "STATE"
                    << std::setw(10) << "ELAPSED"
                    << "EXIT\n";
                out << std::string(80, '-') << "\n";

                auto now = std::chrono::system_clock::now();
                for (const auto& j : wf_jobs) {
                    std::string elapsed = "-";
                    if (j.started_time != std::chrono::system_clock::time_point{}) {
                        auto end_tp = (j.completed_time != std::chrono::system_clock::time_point{}) ? j.completed_time : now;
                        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end_tp - j.started_time);
                        elapsed = format_duration(dur);
                    }
                    std::string exit_str = is_terminal_state(j.state) ? std::to_string(j.exit_code) : "-";

                    out << std::left
                        << std::setw(20) << (j.name.size() > 18 ? j.name.substr(0, 17) + "…" : j.name)
                        << std::setw(26) << j.id
                        << std::setw(18) << to_string(j.state)
                        << std::setw(10) << elapsed
                        << exit_str << "\n";
                }
                return 0;
            } else if (c.action == WorkflowCommand::Action::List) {
                auto wfs = store_.get_all_workflows();
                if (wfs.empty()) {
                    out << "No workflows found in database.\n";
                    return 0;
                }

                out << std::left
                    << std::setw(24) << "WORKFLOW ID"
                    << std::setw(24) << "NAME"
                    << std::setw(14) << "STATE"
                    << "CREATED\n";
                out << std::string(80, '-') << "\n";
                for (const auto& wf : wfs) {
                    out << std::left
                        << std::setw(24) << wf.id
                        << std::setw(24) << (wf.name.size() > 22 ? wf.name.substr(0, 21) + "…" : wf.name)
                        << std::setw(14) << wf.state
                        << format_time_point(wf.created_time) << "\n";
                }
                return 0;
            }
            return 0;
        }

        else if constexpr (std::is_same_v<T, TopCommand>) {
            ResourceManager rm(config_.max_cpu_slots, 4096);
            WorkerManager wm(config_.max_workers);
            auto start_tp = std::chrono::system_clock::now();
            Tui tui;
            tui.run(
                [&]() -> TuiSnapshot {
                    return collect_tui_snapshot(store_, rm, wm, config_.default_policy, start_tp);
                },
                [&](const std::string& job_id) -> bool {
                    std::string err_str;
                    return store_.update_job_state(job_id, JobState::Cancelled, 0, "Cancelled via TUI", &err_str);
                },
                nullptr
            );
            return 0;
        }

        else if constexpr (std::is_same_v<T, ShutdownCommand>) {
            out << "[SHUTDOWN] Scheduler daemon shutdown requested ("
                << (c.immediate ? "immediate" : "drain mode") << ").\n";
            return 0;
        }

        else if constexpr (std::is_same_v<T, InvalidCommand>) {
            err << "[ERROR] " << c.error_message << "\n";
            return 1;
        }

        else {
            out << "[INFO] Command accepted.\n";
            return 0;
        }
    }, cmd);
}

int CliHandler::run_repl(std::istream& in, std::ostream& out, std::ostream& err) {
    out << astra::ASTRA_BANNER << "\n"
        << "ASTRA Interactive Control REPL (v" << astra::ASTRA_VERSION << ")\n"
        << "Type 'help' for available commands, 'exit' or 'quit' to terminate.\n\n";

    std::string line;
    while (true) {
        out << "astra> ";
        out.flush();

        if (!std::getline(in, line)) {
            break;
        }

        auto tokens = CommandParser::tokenize(line);
        if (tokens.empty()) {
            continue;
        }

        if (tokens[0] == "exit" || tokens[0] == "quit") {
            out << "Exiting ASTRA REPL.\n";
            break;
        }

        Command cmd = CommandParser::parse(tokens, config_.workload_root);
        execute(cmd, out, err);
    }

    return 0;
}

} // namespace astra
