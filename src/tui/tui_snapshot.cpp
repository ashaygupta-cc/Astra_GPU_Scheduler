#include "astra/tui_snapshot.hpp"
#include "astra/job_store.hpp"
#include "astra/worker_manager.hpp"

namespace astra {

TuiSnapshot collect_tui_snapshot(
    JobStore& store,
    ResourceManager& rm,
    WorkerManager& wm,
    const std::string& policy_name,
    std::chrono::system_clock::time_point start_time) {
    TuiSnapshot snap;
    snap.policy_name = policy_name;
    snap.timestamp = std::chrono::system_clock::now();

    auto uptime_dur = std::chrono::duration_cast<std::chrono::seconds>(snap.timestamp - start_time);
    snap.uptime_seconds = (uptime_dur.count() > 0) ? static_cast<uint64_t>(uptime_dur.count()) : 0;

    // 1. Collect Jobs & Compute States
    snap.jobs = store.get_all_jobs();
    for (const auto& j : snap.jobs) {
        if (j.state == JobState::Queued || j.state == JobState::WaitingForDependencies) {
            snap.queued_jobs_count++;
        } else if (j.state == JobState::Running || j.state == JobState::Admitted) {
            snap.running_jobs_count++;
        } else if (j.state == JobState::Succeeded) {
            snap.succeeded_jobs_count++;
        } else if (j.state == JobState::Failed || j.state == JobState::Cancelled) {
            snap.failed_jobs_count++;
        }
    }

    // 2. Collect Workers
    const auto& managed_workers = wm.workers();
    snap.total_workers = static_cast<unsigned int>(managed_workers.size());
    snap.active_workers = 0;

    for (const auto& mw : managed_workers) {
        WorkerInfo wi;
        wi.id = mw.id;
        wi.pid = mw.pid;
        wi.state = mw.state;
        wi.current_job_id = mw.current_job_id;
        wi.restart_count = mw.restart_count;
        wi.last_heartbeat = mw.last_heartbeat;
        wi.started_time = mw.started_time;
        snap.workers.push_back(wi);

        if (mw.pid > 0 && mw.state != WorkerState::Terminated && mw.state != WorkerState::Unhealthy) {
            snap.active_workers++;
        }
    }

    // 3. Collect Resources Snapshot
    snap.resources = rm.get_snapshot();

    // 4. Collect Recent Audit Events
    snap.recent_events = store.get_events("", 10);

    return snap;
}

} // namespace astra
