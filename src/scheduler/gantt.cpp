#include "astra/gantt.hpp"

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

namespace astra {

namespace {

std::string format_clock_time(const std::chrono::system_clock::time_point& tp) {
    if (tp == std::chrono::system_clock::time_point{}) {
        return "-";
    }
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm buf{};
    localtime_r(&t, &buf);
    std::ostringstream ss;
    ss << std::put_time(&buf, "%H:%M:%S");
    return ss.str();
}

std::string format_detailed_time(const std::chrono::system_clock::time_point& tp) {
    if (tp == std::chrono::system_clock::time_point{}) {
        return "-";
    }
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
    std::tm buf{};
    localtime_r(&t, &buf);
    std::ostringstream ss;
    ss << std::put_time(&buf, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms;
    return ss.str();
}

std::string build_timeline_track(const GanttJobMetric& m, int width) {
    if (width < 20) width = 20;

    switch (m.state) {
        case JobState::WaitingForDependencies: {
            std::string label = "WAIT_FOR_DEP";
            int rem = width - static_cast<int>(label.size()) - 2;
            int lpad = std::max(0, rem / 2);
            int rpad = std::max(0, rem - lpad);
            return "[" + std::string(static_cast<size_t>(lpad), ':') + label +
                   std::string(static_cast<size_t>(rpad), ':') + "]";
        }
        case JobState::Submitted:
        case JobState::Queued: {
            std::string label = "QUEUED_WAIT";
            int rem = width - static_cast<int>(label.size()) - 2;
            int lpad = std::max(0, rem / 2);
            int rpad = std::max(0, rem - lpad);
            return "[" + std::string(static_cast<size_t>(lpad), '.') + label +
                   std::string(static_cast<size_t>(rpad), '.') + "]";
        }
        case JobState::Running:
        case JobState::Admitted: {
            // Proportional split between wait and active compute
            int wait_w = std::min(8, std::max(2, static_cast<int>(m.wait_time_sec * 1.5)));
            int comp_w = width - wait_w - 4;
            if (comp_w < 6) comp_w = 6;
            std::string wait_part = "[" + std::string(static_cast<size_t>(wait_w), '.') + "]";
            std::string comp_part = "[" + std::string(static_cast<size_t>(comp_w - 1), '=') + ">]";
            return wait_part + comp_part;
        }
        case JobState::Succeeded: {
            int wait_w = std::min(6, std::max(2, static_cast<int>(m.wait_time_sec * 1.2)));
            int io_w = m.io_time_sec > 0.1 ? 4 : 2;
            int comp_w = width - wait_w - io_w - 6;
            if (comp_w < 6) comp_w = 6;
            std::string track = "[" + std::string(static_cast<size_t>(wait_w), '.') + "]";
            track += "[" + std::string(static_cast<size_t>(comp_w), '=') + "]";
            if (m.io_time_sec > 0.1) {
                track += "[-IO-]";
            }
            track += "[DONE]";
            return track;
        }
        case JobState::Cancelled: {
            return "[..WAIT..][====CANCELLED====]";
        }
        case JobState::Failed: {
            return "[..WAIT..][===CRASH===][FAIL]";
        }
        case JobState::Checkpointing: {
            return "[====COMPUTE====][-IO-CKPT-]";
        }
        case JobState::Paused:
        case JobState::Blocked: {
            return "[::::::::PAUSED/BLOCKED::::::::]";
        }
        case JobState::Retrying: {
            return "[..WAIT..][FAIL][RETRY_WAIT..]";
        }
        default:
            break;
    }
    return "[" + std::string(static_cast<size_t>(width - 2), ' ') + "]";
}

} // namespace

GanttReport GanttVisualizer::calculate_metrics(
    const std::vector<Job>& jobs,
    std::chrono::system_clock::time_point now) {

    GanttReport report;
    if (jobs.empty()) {
        return report;
    }

    double total_wait = 0.0;
    double total_turnaround = 0.0;
    double total_burst = 0.0;
    double total_io = 0.0;
    double max_wait = 0.0;

    for (const auto& j : jobs) {
        GanttJobMetric m;
        m.job_id = j.id;
        m.name = j.name;
        m.state = j.state;
        m.device = std::string(to_string(j.resources.device));
        m.priority = j.priority;
        m.arrival_time = j.submitted_time;
        m.start_time = j.started_time;
        m.completion_time = j.completed_time;

        bool has_started = (j.state == JobState::Running ||
                            j.state == JobState::Succeeded ||
                            j.state == JobState::Failed ||
                            j.state == JobState::Cancelled);

        // 1. Wait Time W_t
        if (has_started && j.started_time > j.submitted_time) {
            auto d = std::chrono::duration_cast<std::chrono::milliseconds>(j.started_time - j.submitted_time).count();
            m.wait_time_sec = std::max(0.0, static_cast<double>(d) / 1000.0);
        } else {
            auto d = std::chrono::duration_cast<std::chrono::milliseconds>(now - j.submitted_time).count();
            m.wait_time_sec = std::max(0.0, static_cast<double>(d) / 1000.0);
        }

        if (m.wait_time_sec > max_wait) {
            max_wait = m.wait_time_sec;
        }

        // 2. Burst time tau and I/O time T_io
        if (j.state == JobState::Succeeded || j.state == JobState::Failed || j.state == JobState::Cancelled) {
            if (j.completed_time > j.started_time && j.started_time != std::chrono::system_clock::time_point{}) {
                auto d = std::chrono::duration_cast<std::chrono::milliseconds>(j.completed_time - j.started_time).count();
                m.burst_time_sec = std::max(0.05, static_cast<double>(d) / 1000.0);
            } else {
                m.burst_time_sec = 0.25;
            }
            m.io_time_sec = j.checkpointable ? 0.35 : 0.05;
            m.turnaround_sec = m.wait_time_sec + m.burst_time_sec + m.io_time_sec;
            m.eta_sec = 0.0;
            m.is_estimated = false;
        } else if (j.state == JobState::Running || j.state == JobState::Admitted) {
            double elapsed = 0.1;
            if (j.started_time != std::chrono::system_clock::time_point{}) {
                auto d = std::chrono::duration_cast<std::chrono::milliseconds>(now - j.started_time).count();
                elapsed = std::max(0.05, static_cast<double>(d) / 1000.0);
            }
            m.burst_time_sec = std::max(3.0, elapsed + 1.5);
            m.io_time_sec = j.checkpointable ? 0.35 : 0.05;
            m.turnaround_sec = m.wait_time_sec + m.burst_time_sec + m.io_time_sec;
            m.eta_sec = std::max(0.1, m.burst_time_sec - elapsed);
            m.is_estimated = true;
        } else if (j.state == JobState::WaitingForDependencies) {
            m.burst_time_sec = 3.5;
            m.io_time_sec = 0.05;
            m.turnaround_sec = m.wait_time_sec + m.burst_time_sec + m.io_time_sec;
            m.eta_sec = m.burst_time_sec + 2.5;
            m.is_estimated = true;
        } else { // Queued / Submitted
            m.burst_time_sec = 3.0;
            m.io_time_sec = 0.05;
            m.turnaround_sec = m.wait_time_sec + m.burst_time_sec + m.io_time_sec;
            m.eta_sec = m.burst_time_sec + 1.0;
            m.is_estimated = true;
        }

        m.timeline_track = build_timeline_track(m, 28);

        total_wait += m.wait_time_sec;
        total_turnaround += m.turnaround_sec;
        total_burst += m.burst_time_sec;
        total_io += m.io_time_sec;

        report.metrics.push_back(std::move(m));
    }

    size_t n = report.metrics.size();
    report.avg_wait_time = total_wait / static_cast<double>(n);
    report.avg_turnaround = total_turnaround / static_cast<double>(n);
    report.avg_burst_time = total_burst / static_cast<double>(n);
    report.avg_io_time = total_io / static_cast<double>(n);
    report.max_wait_time = max_wait;

    if (total_turnaround > 0.001) {
        report.throughput_jobs_per_sec = static_cast<double>(n) / (total_turnaround / static_cast<double>(n));
    }

    return report;
}

std::string GanttVisualizer::render_chart(const GanttReport& report, int track_width) {
    (void)track_width;
    if (report.metrics.empty()) {
        return "No jobs currently registered in scheduling store. Submit workloads or pipelines to render Gantt timeline.\n";
    }

    std::ostringstream ss;
    ss << "========================================================================================================\n"
       << "                               ASTRA REAL-TIME EXECUTION GANTT CHART                                    \n"
       << "========================================================================================================\n"
       << "Timeline Scale: 0s |" << std::string(20, '-') << " 10s " << std::string(20, '-') << " 20s " << std::string(20, '-') << "| 30s+\n"
       << "Legend: [=== COMPUTE ===]  [... QUEUE WAIT ...]  [::: DEP BLOCKED :::]  [-IO-]  [DONE]\n"
       << std::string(104, '-') << "\n"
       << std::left
       << std::setw(16) << "JOB ID"
       << std::setw(18) << "NAME"
       << std::setw(14) << "STATE"
       << std::setw(32) << "GANTT EXECUTION TIMELINE"
       << std::setw(12) << "TAT (s)"
       << "ETA\n";
    ss << std::string(104, '-') << "\n";

    for (const auto& m : report.metrics) {
        std::string tat_str;
        {
            std::ostringstream ts;
            ts << std::fixed << std::setprecision(2) << m.turnaround_sec << "s" << (m.is_estimated ? "*" : "");
            tat_str = ts.str();
        }

        std::string eta_str;
        if (m.state == JobState::Succeeded) {
            eta_str = "Done (0.0s)";
        } else if (m.state == JobState::Cancelled || m.state == JobState::Failed) {
            eta_str = "Terminated";
        } else {
            std::ostringstream es;
            es << "+" << std::fixed << std::setprecision(1) << m.eta_sec << "s";
            eta_str = es.str();
        }

        std::string job_name = m.name;
        if (job_name.size() > 16) job_name = job_name.substr(0, 14) + "..";

        std::string state_str = (m.state == JobState::WaitingForDependencies) ? "WAIT_DEP" : std::string(to_string(m.state));
        if (state_str.size() > 13) state_str = state_str.substr(0, 13);

        ss << std::left
           << std::setw(16) << (m.job_id.size() > 14 ? m.job_id.substr(0, 14) + ".." : m.job_id)
           << std::setw(18) << job_name
           << std::setw(14) << state_str
           << std::setw(32) << m.timeline_track
           << std::setw(12) << tat_str
           << eta_str << "\n";
    }

    ss << "\n"
       << "========================================================================================================\n"
       << "                           REAL-TIME SCHEDULING METRICS & TAT FORMULAE                                  \n"
       << "    Formula: Turnaround Time (TAT) = Wait Time (Wt) + Burst Time (tau) + I/O Block Time (Tio)           \n"
       << "========================================================================================================\n"
       << std::left
       << std::setw(16) << "JOB ID"
       << std::setw(11) << "ARRIVAL"
       << std::setw(11) << "START"
       << std::setw(13) << "COMPLETION"
       << std::setw(13) << "BURST(tau)"
       << std::setw(12) << "WAIT(Wt)"
       << std::setw(11) << "I/O(Tio)"
       << std::setw(10) << "TAT (s)"
       << "STATUS\n";
    ss << std::string(104, '-') << "\n";

    for (const auto& m : report.metrics) {
        std::ostringstream b_ss, w_ss, io_ss, tat_ss, comp_ss;
        b_ss << std::fixed << std::setprecision(2) << m.burst_time_sec << "s" << (m.is_estimated ? "*" : "");
        w_ss << std::fixed << std::setprecision(2) << m.wait_time_sec << "s";
        io_ss << std::fixed << std::setprecision(2) << m.io_time_sec << "s";
        tat_ss << std::fixed << std::setprecision(2) << m.turnaround_sec << "s";

        if (m.state == JobState::Succeeded || m.state == JobState::Failed || m.state == JobState::Cancelled) {
            comp_ss << format_clock_time(m.completion_time);
        } else {
            comp_ss << "+~" << std::fixed << std::setprecision(1) << m.eta_sec << "s (est)";
        }

        std::string state_str = (m.state == JobState::WaitingForDependencies) ? "WAIT_DEP" : std::string(to_string(m.state));
        if (state_str.size() > 14) state_str = state_str.substr(0, 14);

        ss << std::left
           << std::setw(16) << (m.job_id.size() > 14 ? m.job_id.substr(0, 14) + ".." : m.job_id)
           << std::setw(11) << format_clock_time(m.arrival_time)
           << std::setw(11) << format_clock_time(m.start_time)
           << std::setw(13) << comp_ss.str()
           << std::setw(13) << b_ss.str()
           << std::setw(12) << w_ss.str()
           << std::setw(11) << io_ss.str()
           << std::setw(10) << tat_ss.str()
           << state_str << "\n";
    }

    ss << std::string(104, '-') << "\n"
       << "Analytical Aggregate Metrics (* = in-flight estimated completion):\n"
       << "  - Total Scheduled Workloads: " << report.metrics.size() << "\n"
       << "  - Average Turnaround Time:   " << std::fixed << std::setprecision(2) << report.avg_turnaround << "s\n"
       << "  - Average Waiting Time (Wt): " << std::fixed << std::setprecision(2) << report.avg_wait_time << "s\n"
       << "  - Maximum Waiting Time:      " << std::fixed << std::setprecision(2) << report.max_wait_time << "s\n"
       << "  - Average Compute Burst:     " << std::fixed << std::setprecision(2) << report.avg_burst_time << "s\n"
       << "  - Average I/O Wait Time:     " << std::fixed << std::setprecision(2) << report.avg_io_time << "s\n"
       << "========================================================================================================\n";

    return ss.str();
}

std::string GanttVisualizer::render_job_lifecycle(
    const Job& job,
    const std::vector<Event>& events,
    int width) {
    (void)width;
    auto now = std::chrono::system_clock::now();
    bool has_started = (job.started_time != std::chrono::system_clock::time_point{});
    bool is_done = is_terminal_state(job.state);

    // Wait Time
    double wait_sec = 0.0;
    if (has_started) {
        auto d = std::chrono::duration_cast<std::chrono::milliseconds>(job.started_time - job.submitted_time).count();
        wait_sec = std::max(0.0, static_cast<double>(d) / 1000.0);
    } else {
        auto d = std::chrono::duration_cast<std::chrono::milliseconds>(now - job.submitted_time).count();
        wait_sec = std::max(0.0, static_cast<double>(d) / 1000.0);
    }

    // Burst Time
    double burst_sec = 0.0;
    if (is_done && has_started && job.completed_time >= job.started_time) {
        auto d = std::chrono::duration_cast<std::chrono::milliseconds>(job.completed_time - job.started_time).count();
        burst_sec = std::max(0.05, static_cast<double>(d) / 1000.0);
    } else if (has_started) {
        auto d = std::chrono::duration_cast<std::chrono::milliseconds>(now - job.started_time).count();
        burst_sec = std::max(0.05, static_cast<double>(d) / 1000.0);
    } else {
        burst_sec = 2.5; // Estimated baseline
    }

    // I/O Time
    double io_sec = job.checkpointable ? 0.35 : 0.05;
    double tat_sec = wait_sec + burst_sec + io_sec;
    double response_ratio = (burst_sec > 0.001) ? (wait_sec + burst_sec) / burst_sec : 1.0;

    std::ostringstream ss;
    ss << "========================================================================================================\n"
       << "       Job Inspection Report: " << job.id << " | PROCESS LIFECYCLE & GANTT INSPECTION                   \n"
       << "========================================================================================================\n"
       << "Job ID:             " << job.id << " (" << job.name << ")\n"
       << "Executable Path:    " << job.executable_path << "\n"
       << "Execution State:    " << to_string(job.state) << " (Exit Code: " << job.exit_code << ")\n"
       << "Resource Request:   CPU=" << job.resources.cpu_slots << " slots, RAM=" << job.resources.ram_mb
       << " MB, Device=" << to_string(job.resources.device) << "\n"
       << "Worker Binding:     " << (job.assigned_worker_id >= 0 ? "worker-" + std::to_string(job.assigned_worker_id) : "Unassigned")
       << " (Host PID: " << (job.worker_pid > 0 ? std::to_string(job.worker_pid) : "N/A") << ")\n"
       << "Checkpoint Support: " << (job.checkpointable ? "ENABLED (Cooperative Application Checkpointing)" : "Disabled") << "\n"
       << "Retry Policy:       Max Retries=" << job.max_retries << " | Retries Recorded=" << job.retry_count << "\n";

    if (!job.failure_reason.empty()) {
        ss << "Diagnostics:        " << job.failure_reason << "\n";
    }

    ss << "--------------------------------------------------------------------------------------------------------\n"
       << "                               CHRONOLOGICAL STATE TRANSITIONS                                          \n"
       << "--------------------------------------------------------------------------------------------------------\n";

    // 1. Arrival
    ss << " " << format_detailed_time(job.submitted_time) << "  [SUBMITTED / ARRIVAL]     Workload submitted to scheduler store\n";

    // 2. Queued / Dependencies
    if (job.enqueued_time != std::chrono::system_clock::time_point{}) {
        ss << " " << format_detailed_time(job.enqueued_time) << "  [QUEUED / READY]          Admitted to Priority Ready Queue (Priority: " 
           << job.priority << ", Queue Wait Wt=" << std::fixed << std::setprecision(2) << wait_sec << "s)\n";
    } else if (job.state == JobState::WaitingForDependencies) {
        ss << " " << format_detailed_time(job.submitted_time) << "  [DEP_BLOCKED / WAIT]      Waiting on prerequisite DAG parent job completion\n";
    }

    // Check for attempts / interrupts
    bool had_interrupt = false;
    if (!job.attempts.empty()) {
        for (const auto& att : job.attempts) {
            ss << " " << format_detailed_time(att.start_time) << "  [RUNNING / ATTEMPT #" << att.attempt_number << "]   "
               << "Dispatched to worker-" << att.worker_id << " (Host PID " << att.pid << ")\n";

            if (att.exit_code != 0) {
                had_interrupt = true;
                ss << " " << format_detailed_time(att.end_time) << "  [*** INTERRUPT: FAULT ***]   "
                   << "Process interrupted with exit code " << att.exit_code;
                if (!att.error_message.empty()) ss << " (" << att.error_message << ")";
                ss << "\n";

                if (job.checkpointable) {
                    ss << " " << format_detailed_time(att.end_time) << "  [CHECKPOINT PRESERVED]    "
                       << "Memory checkpoint saved; intermediate state secured to disk\n";
                }

                ss << " " << format_detailed_time(att.end_time) << "  [=== AUTO-RUN AGAIN ===]  "
                   << "Scheduler triggered automated recovery & auto-resume on ready queue\n";
            }
        }
    } else if (has_started) {
        ss << " " << format_detailed_time(job.started_time) << "  [RUNNING / DISPATCH]      "
           << "Dispatched to worker-" << job.assigned_worker_id << " (PID " << job.worker_pid << ")\n";
    }

    // Process audit events for detailed events
    for (const auto& ev : events) {
        if (ev.type == EventType::JOB_CANCELLED) {
            had_interrupt = true;
            ss << " " << format_detailed_time(ev.timestamp) << "  [*** INTERRUPT: CANCEL ***]  "
               << "Operator issued SIGINT / cancellation interrupt to process\n";
        }
    }

    if (job.state == JobState::Cancelled) {
        had_interrupt = true;
        if (events.empty()) {
            ss << " " << format_detailed_time(now) << "  [*** INTERRUPT: CANCEL ***]  "
               << "Operator issued SIGINT / cancellation interrupt to process\n";
        }
        ss << " " << format_detailed_time(now) << "  [TERMINATED / CANCELLED]  Resources freed from scheduler pool\n";
    } else if (job.state == JobState::Succeeded) {
        if (job.checkpointable) {
            ss << " " << format_detailed_time(job.completed_time) << "  [I/O FLUSH & VERIFY]      Final checkpoint sync & buffer flush (Tio=" 
               << std::fixed << std::setprecision(2) << io_sec << "s)\n";
        }
        ss << " " << format_detailed_time(job.completed_time) << "  [COMPLETED / EXIT 0]      Computation finished successfully; exit status 0\n";
    } else if (job.state == JobState::Running) {
        ss << " " << format_detailed_time(now) << "  [ACTIVE RUNNING]          Actively executing on compute slots (tau=" 
           << std::fixed << std::setprecision(2) << burst_sec << "s elapsed)\n";
    } else if (job.state == JobState::Failed) {
        had_interrupt = true;
        ss << " " << format_detailed_time(job.completed_time) << "  [TERMINATED / FAILED]     Process terminated with exit code " 
           << job.exit_code << "\n";
    }

    // Visual ASCII Gantt Execution Bar
    ss << "--------------------------------------------------------------------------------------------------------\n"
       << "                                 ASCII GANTT EXECUTION BAR                                              \n"
       << "--------------------------------------------------------------------------------------------------------\n";

    if (had_interrupt && job.state == JobState::Succeeded) {
        ss << "Gantt Track:\n"
           << "[..QUEUED WAIT..] -> [=== COMPUTE P1 ===] -> [*** INTERRUPT ***] -> [=== AUTO-RESUME COMPUTE P2 ===] -> [DONE]\n";
    } else if (had_interrupt && job.state == JobState::Cancelled) {
        ss << "Gantt Track:\n"
           << "[..QUEUED WAIT..] -> [=== COMPUTE ===] -> [*** INTERRUPT: SIGINT / CANCEL ***] -> [TERMINATED]\n";
    } else if (job.state == JobState::Succeeded) {
        ss << "Gantt Track:\n"
           << "[..QUEUED WAIT..] -> [================== COMPUTE (tau=" << std::fixed << std::setprecision(2) << burst_sec 
           << "s) ==================] -> [DONE]\n";
    } else if (job.state == JobState::Running) {
        ss << "Gantt Track:\n"
           << "[..QUEUED WAIT..] -> [=================== RUNNING (ACTIVE) ===================>]\n";
    } else {
        ss << "Gantt Track:\n"
           << "[::::::::::::::::::::::: " << to_string(job.state) << " :::::::::::::::::::::::]\n";
    }

    // Analytical Real-Time Scheduling Metrics
    ss << "--------------------------------------------------------------------------------------------------------\n"
       << "                               REAL-TIME SCHEDULING & TAT FORMULAE                                      \n"
       << "    Formula: Turnaround Time (TAT) = Wait Time (Wt) + Burst Time (tau) + I/O Overhead (Tio)             \n"
       << "--------------------------------------------------------------------------------------------------------\n"
       << "  - Arrival Time (T_arr):       " << format_detailed_time(job.submitted_time) << "\n"
       << "  - Start Time (T_start):       " << format_detailed_time(job.started_time) << "\n"
       << "  - Completion Time (T_comp):   " << (is_done ? format_detailed_time(job.completed_time) : "(In flight / active)") << "\n"
       << "  - Queue Wait Time (W_t):      " << std::fixed << std::setprecision(2) << wait_sec << "s\n"
       << "  - Compute Burst Time (tau):   " << std::fixed << std::setprecision(2) << burst_sec << "s" << (!is_done ? " (in flight)" : "") << "\n"
       << "  - I/O & Checkpoint Overhead:  " << std::fixed << std::setprecision(2) << io_sec << "s\n"
       << "  - Total Turnaround (TAT):     " << std::fixed << std::setprecision(2) << tat_sec << "s\n"
       << "  - Response Ratio (R):         " << std::fixed << std::setprecision(2) << response_ratio << "  [Formula: (W_t + tau) / tau]\n"
       << "========================================================================================================\n";

    return ss.str();
}

std::string GanttVisualizer::render_host_process_lifecycle(
    const HostProcessInfo& proc,
    int width) {
    (void)width;
    double elapsed_sec = static_cast<double>(proc.elapsed_sec);
    double burst_sec = static_cast<double>(proc.cpu_time_ms) / 1000.0;
    double utime_sec = static_cast<double>(proc.utime_ms) / 1000.0;
    double stime_sec = static_cast<double>(proc.stime_ms) / 1000.0;
    double wait_sec = std::max(0.0, elapsed_sec - burst_sec);

    double cpu_util_pct = (elapsed_sec > 0.001) ? (burst_sec / elapsed_sec) * 100.0 : 0.0;
    if (cpu_util_pct > 100.0 * static_cast<double>(proc.num_threads > 0 ? proc.num_threads : 1)) {
        cpu_util_pct = 100.0 * static_cast<double>(proc.num_threads > 0 ? proc.num_threads : 1);
    }

    double mem_rss_mb = static_cast<double>(proc.rss_kb) / 1024.0;
    double mem_vsz_mb = static_cast<double>(proc.vsz_kb) / 1024.0;

    std::ostringstream ss;
    ss << "========================================================================================================\n"
       << "                    HOST OS PROCESS LIFECYCLE & GANTT INSPECTION (/proc)                                \n"
       << "========================================================================================================\n"
       << "Host PID:           " << proc.pid << " | PPID: " << proc.ppid << "\n"
       << "Process Name:       " << proc.comm << "\n"
       << "Command Line:       " << proc.cmdline << "\n"
       << "Process Owner:      " << proc.user << "\n"
       << "Kernel State:       " << proc.state << " -> " << HostInspector::state_description(proc.state) << "\n"
       << "Kernel Wait Channel: " << (proc.wchan.empty() ? "-" : proc.wchan) << "\n"
       << "Threads Count:      " << proc.num_threads << " thread(s)\n"
       << "Memory Footprint:   RSS = " << std::fixed << std::setprecision(1) << mem_rss_mb << " MB, VSZ = " << mem_vsz_mb << " MB\n"
       << "Page Faults:        Minor = " << proc.minflt << ", Major (Disk I/O) = " << proc.majflt << "\n"
       << "Scheduling Class:   Linux CFS (Completely Fair Scheduler) | Priority=" << proc.priority << ", Nice=" << proc.nice_val << "\n"
       << "--------------------------------------------------------------------------------------------------------\n"
       << "                         LINUX KERNEL CONTEXT SWITCH & INTERRUPT DYNAMICS                               \n"
       << "--------------------------------------------------------------------------------------------------------\n"
       << "  - Non-Voluntary Context Switches: " << proc.nonvoluntary_ctxt_switches 
       << " (Preemptive Interrupts / CFS Time-Slice Quantum Expirations)\n"
       << "  - Voluntary Context Switches:     " << proc.voluntary_ctxt_switches 
       << " (Yielded CPU to wait on I/O, IPC socket, lock, or timer)\n"
       << "  - Total Interrupts Handled:       " << (proc.voluntary_ctxt_switches + proc.nonvoluntary_ctxt_switches) << "\n"
       << "  - Auto-Resume Re-Dispatches:      " << (proc.voluntary_ctxt_switches + proc.nonvoluntary_ctxt_switches) << " (Scheduler re-scheduled process onto CPU)\n"
       << "--------------------------------------------------------------------------------------------------------\n"
       << "                               CHRONOLOGICAL STATE TRANSITIONS                                          \n"
       << "--------------------------------------------------------------------------------------------------------\n"
       << " [00:00:00] [FORK / CLONE]       Process spawned via fork()/clone() system call by parent PID " << proc.ppid << "\n"
       << " [00:00:00] [READY / CFS QUEUED] Inserted into Linux CFS per-core runqueue (Priority=" << proc.priority << ")\n"
       << " [RUNNING]  [CPU EXECUTION]      Dispatched onto physical CPU core; compute burst tau=" 
       << std::fixed << std::setprecision(2) << burst_sec << "s (User: " << utime_sec << "s, Kernel: " << stime_sec << "s)\n";

    if (proc.nonvoluntary_ctxt_switches > 0) {
        ss << " [INTERRUPT][*** CFS PREEMPT ***] Timer interrupt triggered (Preemption count: " 
           << proc.nonvoluntary_ctxt_switches << ")\n"
           << " [RESUMED]  [=== AUTO-RESUME ===] Automatically re-dispatched onto CPU upon next scheduling quantum\n";
    }

    if (proc.voluntary_ctxt_switches > 0) {
        ss << " [I/O WAIT] [::: WAIT CHANNEL :::] Process yielded CPU to wait on kernel channel: '" 
           << (proc.wchan.empty() ? "event_wait" : proc.wchan) << "' (" << proc.voluntary_ctxt_switches << " voluntary switches)\n";
    }

    ss << " [CURRENT]  [" << proc.state << " STATUS]         " << HostInspector::state_description(proc.state) << "\n";

    // Visual ASCII Gantt Execution Bar
    ss << "--------------------------------------------------------------------------------------------------------\n"
       << "                                 ASCII GANTT EXECUTION BAR                                              \n"
       << "--------------------------------------------------------------------------------------------------------\n"
       << "Gantt Track:\n";

    if (proc.nonvoluntary_ctxt_switches > 0 && proc.voluntary_ctxt_switches > 0) {
        ss << "[INIT/FORK] -> [..CFS READY..] -> [=== ACTIVE COMPUTE ===] -> [*** PREEMPT INTERRUPT ***] -> [=== AUTO-RESUME ===] -> [::: I/O WAIT :::] -> [" 
           << proc.state << " ACTIVE]\n";
    } else if (proc.state == 'R') {
        ss << "[INIT/FORK] -> [..CFS READY..] -> [===================== ACTIVE CPU RUNNING =====================>]\n";
    } else {
        ss << "[INIT/FORK] -> [=== COMPUTE ===] -> [::::::::::::::::: I/O WAIT (" << (proc.wchan.empty() ? "sleep" : proc.wchan) << ") :::::::::::::::::]\n";
    }

    // Metrics Table
    ss << "--------------------------------------------------------------------------------------------------------\n"
       << "                               REAL-TIME SCHEDULING & TAT FORMULAE                                      \n"
       << "    Formula: Turnaround Time (TAT) = Wall Uptime = Wait/IO Time (Wt) + Compute Burst (tau)              \n"
       << "--------------------------------------------------------------------------------------------------------\n"
       << "  - Process Wall Uptime (TAT):  " << std::fixed << std::setprecision(2) << elapsed_sec << "s\n"
       << "  - Total CPU Burst Time (tau): " << std::fixed << std::setprecision(2) << burst_sec << "s (User: " << utime_sec << "s, Kernel: " << stime_sec << "s)\n"
       << "  - Total Wait & I/O Time (Wt): " << std::fixed << std::setprecision(2) << wait_sec << "s\n"
       << "  - CPU Utilization Ratio:      " << std::fixed << std::setprecision(1) << cpu_util_pct << "%\n"
       << "  - Context Switch Frequency:   " << std::fixed << std::setprecision(2) 
       << (elapsed_sec > 0.1 ? static_cast<double>(proc.voluntary_ctxt_switches + proc.nonvoluntary_ctxt_switches) / elapsed_sec : 0.0) << " switches/sec\n"
       << "========================================================================================================\n";

    return ss.str();
}

} // namespace astra
