#include "astra/tui.hpp"
#include "astra/tui_snapshot.hpp"
#include "astra/job_store.hpp"
#include "astra/resource_manager.hpp"
#include "astra/worker_manager.hpp"
#include "astra/gantt.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>

void test_tui_snapshot_aggregation() {
    using namespace astra;

    const std::string test_db = "test_tui_snapshot.db";
    std::filesystem::remove(test_db);

    JobStore store;
    std::string err;
    assert(store.open(test_db, &err));

    auto now = std::chrono::system_clock::now();

    // 1. Insert jobs with diverse states
    Job j1; j1.id = "j-queued"; j1.name = "queued_job"; j1.state = JobState::Queued;
    assert(store.save_job(j1));

    Job j2; j2.id = "j-running"; j2.name = "running_job"; j2.state = JobState::Running;
    assert(store.save_job(j2));

    Job j3; j3.id = "j-succeeded"; j3.name = "success_job"; j3.state = JobState::Succeeded;
    assert(store.save_job(j3));

    Job j4; j4.id = "j-failed"; j4.name = "failed_job"; j4.state = JobState::Failed;
    assert(store.save_job(j4));

    // Record an event
    Event ev; ev.type = EventType::JOB_STARTED; ev.job_id = "j-running";
    assert(store.record_event(ev));

    ResourceManager rm(4, 4096);
    WorkerManager wm(2);

    auto start_time = now - std::chrono::seconds(45);
    TuiSnapshot snap = collect_tui_snapshot(store, rm, wm, "priority_aging", start_time);

    // Verify snapshot calculations
    assert(snap.policy_name == "priority_aging");
    assert(snap.uptime_seconds >= 44);
    assert(snap.jobs.size() == 4);
    assert(snap.queued_jobs_count == 1);
    assert(snap.running_jobs_count == 1);
    assert(snap.succeeded_jobs_count == 1);
    assert(snap.failed_jobs_count == 1);
    assert(snap.resources.total_cpu_slots == 4);
    assert(!snap.recent_events.empty());
    assert(!snap.resources.telemetry_source_label.empty());

    store.close();
    std::filesystem::remove(test_db);

    std::cout << "[PASS] TuiSnapshot collection, metric aggregation, and telemetry labeling\n";
}

void test_tui_view_transitions_and_headless_render() {
    using namespace astra;

    Tui tui;
    assert(tui.init());
    assert(tui.is_initialized());

    // Test view transitions
    assert(tui.current_view() == TuiView::Jobs);
    tui.set_view(TuiView::Workers);
    assert(tui.current_view() == TuiView::Workers);
    tui.set_view(TuiView::Resources);
    assert(tui.current_view() == TuiView::Resources);
    tui.set_view(TuiView::Events);
    assert(tui.current_view() == TuiView::Events);
    tui.set_view(TuiView::JobDetails);
    assert(tui.current_view() == TuiView::JobDetails);
    tui.set_view(TuiView::Logs);
    assert(tui.current_view() == TuiView::Logs);

    // Render frame across all views in headless mode
    TuiSnapshot dummy_snap;
    dummy_snap.policy_name = "priority_aging";
    dummy_snap.uptime_seconds = 120;
    dummy_snap.total_workers = 4;
    dummy_snap.active_workers = 2;

    Job test_job;
    test_job.id = "test-job-01";
    test_job.name = "matrix_mul";
    test_job.state = JobState::Running;
    test_job.priority = 5;
    test_job.resources.cpu_slots = 2;
    test_job.resources.device = DeviceRequirement::CpuOnly;
    dummy_snap.jobs.push_back(test_job);

    for (auto v : {TuiView::Jobs, TuiView::Gantt, TuiView::HostPs, TuiView::Deadlock, TuiView::Workers, TuiView::Resources, 
                   TuiView::Events, TuiView::JobDetails, TuiView::Logs}) {
        tui.set_view(v);
        tui.render_frame(dummy_snap); // Must not crash
    }

    tui.shutdown();
    std::cout << "[PASS] Tui view transitions and headless rendering stability\n";
}

void test_per_process_gantt_lifecycle_visualizer() {
    using namespace astra;

    // 1. Test Host Process Inspection & Lifecycle Gantt
    pid_t self_pid = ::getpid();
    auto host_opt = HostInspector::inspect_process(self_pid);
    assert(host_opt.has_value());
    assert(host_opt->pid == self_pid);
    assert(host_opt->state == 'R' || host_opt->state == 'S');

    std::string host_gantt = GanttVisualizer::render_host_process_lifecycle(*host_opt);
    assert(!host_gantt.empty());
    assert(host_gantt.find("HOST OS PROCESS LIFECYCLE & GANTT INSPECTION") != std::string::npos);
    assert(host_gantt.find("Turnaround Time (TAT)") != std::string::npos);
    assert(host_gantt.find("Gantt Track:") != std::string::npos);

    // 2. Test Astra Workload Lifecycle Gantt with Crash Interrupt & Auto-Resume
    Job job;
    job.id = "job-ckpt-recovery-test";
    job.name = "checkpointable_sum";
    job.executable_path = "./workloads/checkpointable_sum";
    job.state = JobState::Succeeded;
    job.checkpointable = true;
    job.max_retries = 2;
    job.retry_count = 1;
    job.exit_code = 0;

    auto t0 = std::chrono::system_clock::now() - std::chrono::seconds(10);
    auto t1 = t0 + std::chrono::milliseconds(200);
    auto t2 = t1 + std::chrono::milliseconds(500);
    auto t3 = t2 + std::chrono::seconds(3);
    auto t4 = t3 + std::chrono::milliseconds(200);
    auto t5 = t4 + std::chrono::seconds(4);

    job.submitted_time = t0;
    job.enqueued_time = t1;
    job.started_time = t2;
    job.completed_time = t5;

    // Attempt #1: Crashed at step 50000 with exit code 42
    JobAttempt att1;
    att1.attempt_number = 1;
    att1.worker_id = 0;
    att1.pid = 9012;
    att1.start_time = t2;
    att1.end_time = t3;
    att1.exit_code = 42;
    att1.error_message = "Simulated crash at step 50000";
    job.attempts.push_back(att1);

    // Attempt #2: Auto-resumed from checkpoint and completed
    JobAttempt att2;
    att2.attempt_number = 2;
    att2.worker_id = 0;
    att2.pid = 9088;
    att2.start_time = t4;
    att2.end_time = t5;
    att2.exit_code = 0;
    job.attempts.push_back(att2);

    std::string job_gantt = GanttVisualizer::render_job_lifecycle(job);
    assert(!job_gantt.empty());
    assert(job_gantt.find("Job Inspection Report: job-ckpt-recovery-test") != std::string::npos);
    assert(job_gantt.find("[*** INTERRUPT: FAULT ***]") != std::string::npos);
    assert(job_gantt.find("[CHECKPOINT PRESERVED]") != std::string::npos);
    assert(job_gantt.find("[=== AUTO-RUN AGAIN ===]") != std::string::npos);
    assert(job_gantt.find("[=== AUTO-RESUME COMPUTE P2 ===]") != std::string::npos);
    assert(job_gantt.find("Turnaround Time (TAT)") != std::string::npos);

    std::cout << "[PASS] Per-process lifecycle timeline, crash interrupt detection, and auto-resume Gantt visualization\n";
}

int main() {
    std::cout << "Running ASTRA Phase 5 (TUI & Telemetry) Unit Tests...\n";
    test_tui_snapshot_aggregation();
    test_tui_view_transitions_and_headless_render();
    test_per_process_gantt_lifecycle_visualizer();
    std::cout << "All Phase 5 tests passed successfully!\n";
    return 0;
}
