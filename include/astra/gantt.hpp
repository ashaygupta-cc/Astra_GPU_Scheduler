#pragma once

#include "astra/job.hpp"
#include "astra/event.hpp"
#include "astra/host_inspector.hpp"
#include <vector>
#include <string>
#include <chrono>

namespace astra {

struct GanttJobMetric {
    std::string job_id;
    std::string name;
    JobState state = JobState::Submitted;
    std::string device = "CPU";
    int priority = 5;

    std::chrono::system_clock::time_point arrival_time;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point completion_time;

    double wait_time_sec = 0.0;     // W_t: time spent in queue or blocked waiting
    double burst_time_sec = 0.0;    // tau: compute execution time on CPU/GPU slot
    double io_time_sec = 0.0;       // T_io: checkpoint, disk I/O, IPC synchronization
    double turnaround_sec = 0.0;    // TAT = W_t + tau + T_io
    double eta_sec = 0.0;           // Estimated remaining time to completion
    bool is_estimated = false;

    std::string timeline_track;     // Visual ASCII timeline track
};

struct GanttReport {
    std::vector<GanttJobMetric> metrics;
    double avg_wait_time = 0.0;
    double avg_turnaround = 0.0;
    double max_wait_time = 0.0;
    double avg_burst_time = 0.0;
    double avg_io_time = 0.0;
    double throughput_jobs_per_sec = 0.0;
};

class GanttVisualizer {
public:
    static GanttReport calculate_metrics(
        const std::vector<Job>& jobs,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    static std::string render_chart(const GanttReport& report, int track_width = 30);

    // Detailed per-process lifecycle timeline, state progression & Gantt chart for an Astra job
    static std::string render_job_lifecycle(
        const Job& job,
        const std::vector<Event>& events = {},
        int width = 50);

    // Detailed per-process lifecycle timeline, interrupt analysis & Gantt chart for a Host OS process
    static std::string render_host_process_lifecycle(
        const HostProcessInfo& proc,
        int width = 50);
};

} // namespace astra
