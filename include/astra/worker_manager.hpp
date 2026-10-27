#pragma once

#include "astra/ipc_channel.hpp"
#include "astra/worker_info.hpp"
#include "astra/job.hpp"

#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <string>

namespace astra {

struct ManagedWorker {
    int id = -1;
    pid_t pid = -1;
    std::unique_ptr<IpcChannel> channel;
    WorkerState state = WorkerState::Idle;
    std::string current_job_id;
    pid_t active_workload_pid = -1;
    int restart_count = 0;
    std::chrono::system_clock::time_point last_heartbeat = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point started_time = std::chrono::system_clock::now();
};

class WorkerManager {
public:
    explicit WorkerManager(unsigned int max_workers = 4);
    ~WorkerManager();

    WorkerManager(const WorkerManager&) = delete;
    WorkerManager& operator=(const WorkerManager&) = delete;

    bool init(unsigned int pool_size);
    bool start_worker(int worker_id);
    bool stop_worker(int worker_id, bool immediate = false);

    int find_idle_worker() const;
    size_t active_worker_count() const;
    const std::vector<ManagedWorker>& workers() const { return workers_; }

    bool dispatch_job(int worker_id, const Job& job);
    bool cancel_job(const std::string& job_id);

    // Non-blocking poll for IPC messages from workers
    void poll_events(int timeout_ms = 0);

    // Non-blocking reap for terminated child processes
    void reap_children();

    // Health monitoring & automated recovery
    void check_health_and_restart();

    // Orderly shutdown of all child processes
    void shutdown_all(bool immediate = false);

    // Event callbacks
    std::function<void(int worker_id, const std::string& job_id, pid_t workload_pid)> on_job_started;
    std::function<void(int worker_id, const std::string& job_id, int exit_code, const std::string& logs)> on_job_completed;
    std::function<void(int worker_id, const std::string& job_id, int exit_code, const std::string& logs)> on_job_failed;
    std::function<void(int worker_id, int restart_count)> on_worker_crashed;

private:
    unsigned int max_workers_;
    std::vector<ManagedWorker> workers_;
};

} // namespace astra
