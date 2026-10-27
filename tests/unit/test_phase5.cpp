#include "astra/tui.hpp"
#include "astra/tui_snapshot.hpp"
#include "astra/job_store.hpp"
#include "astra/resource_manager.hpp"
#include "astra/worker_manager.hpp"

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

    for (auto v : {TuiView::Jobs, TuiView::Workers, TuiView::Resources, 
                   TuiView::Events, TuiView::JobDetails, TuiView::Logs}) {
        tui.set_view(v);
        tui.render_frame(dummy_snap); // Must not crash
    }

    tui.shutdown();
    std::cout << "[PASS] Tui view transitions and headless rendering stability\n";
}

int main() {
    std::cout << "Running ASTRA Phase 5 (TUI & Telemetry) Unit Tests...\n";
    test_tui_snapshot_aggregation();
    test_tui_view_transitions_and_headless_render();
    std::cout << "All Phase 5 tests passed successfully!\n";
    return 0;
}
