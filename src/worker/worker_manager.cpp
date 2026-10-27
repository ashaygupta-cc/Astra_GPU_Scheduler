#include "astra/worker_manager.hpp"
#include "astra/worker.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <nlohmann/json.hpp>

namespace astra {

namespace {

uint64_t hash_job_id(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

WorkerManager::WorkerManager(unsigned int max_workers) : max_workers_(max_workers) {}

WorkerManager::~WorkerManager() {
    shutdown_all(false);
}

bool WorkerManager::init(unsigned int pool_size) {
    max_workers_ = pool_size;
    workers_.clear();
    workers_.resize(pool_size);

    for (unsigned int i = 0; i < pool_size; ++i) {
        workers_[i].id = static_cast<int>(i);
        if (!start_worker(static_cast<int>(i))) {
            return false;
        }
    }
    return true;
}

bool WorkerManager::start_worker(int worker_id) {
    if (worker_id < 0 || static_cast<size_t>(worker_id) >= workers_.size()) {
        return false;
    }

    ManagedWorker& w = workers_[static_cast<size_t>(worker_id)];
    w.id = worker_id;

    auto [parent_fd, child_fd] = IpcChannel::create_socketpair();
    if (!parent_fd.valid() || !child_fd.valid()) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        // Child Worker Process
        parent_fd.reset();
        int rc = run_worker_process(worker_id, child_fd.release());
        _exit(rc);
    }

    // Parent Scheduler Process
    child_fd.reset();
    w.pid = pid;
    w.channel = std::make_unique<IpcChannel>(std::move(parent_fd));
    w.state = WorkerState::Idle;
    w.current_job_id = "";
    w.active_workload_pid = -1;
    w.started_time = std::chrono::system_clock::now();
    w.last_heartbeat = w.started_time;

    return true;
}

bool WorkerManager::stop_worker(int worker_id, bool immediate) {
    if (worker_id < 0 || static_cast<size_t>(worker_id) >= workers_.size()) {
        return false;
    }

    ManagedWorker& w = workers_[static_cast<size_t>(worker_id)];
    if (w.pid <= 0) return true;

    if (immediate) {
        kill(w.pid, SIGKILL);
    } else {
        if (w.channel && w.channel->is_open()) {
            w.channel->send(MessageType::SHUTDOWN, 0, 0, 0, "{}");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        kill(w.pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        kill(w.pid, SIGKILL);
    }

    int status = 0;
    waitpid(w.pid, &status, 0);
    w.state = WorkerState::Terminated;
    w.pid = -1;
    w.channel.reset();
    return true;
}

int WorkerManager::find_idle_worker() const {
    for (size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i].state == WorkerState::Idle && workers_[i].pid > 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

size_t WorkerManager::active_worker_count() const {
    size_t count = 0;
    for (const auto& w : workers_) {
        if (w.pid > 0 && w.state != WorkerState::Terminated) {
            ++count;
        }
    }
    return count;
}

bool WorkerManager::dispatch_job(int worker_id, const Job& job) {
    if (worker_id < 0 || static_cast<size_t>(worker_id) >= workers_.size()) {
        return false;
    }

    ManagedWorker& w = workers_[static_cast<size_t>(worker_id)];
    if (w.state != WorkerState::Idle || !w.channel || !w.channel->is_open()) {
        return false;
    }

    int timeout_sec = 0;
    if (job.deadline.has_value()) {
        auto dur = std::chrono::duration_cast<std::chrono::seconds>(*job.deadline - std::chrono::system_clock::now()).count();
        if (dur > 0) timeout_sec = static_cast<int>(dur);
    }

    nlohmann::json payload{
        {"job_id", job.id},
        {"executable", job.executable_path},
        {"argv", job.argv},
        {"timeout_seconds", timeout_sec}
    };

    uint64_t job_hash = hash_job_id(job.id);
    if (!w.channel->send(MessageType::RUN_JOB, job_hash, 1, 0, payload.dump())) {
        return false;
    }

    w.state = WorkerState::Busy;
    w.current_job_id = job.id;
    return true;
}

bool WorkerManager::cancel_job(const std::string& job_id) {
    for (auto& w : workers_) {
        if (w.current_job_id == job_id && w.channel && w.channel->is_open()) {
            uint64_t job_hash = hash_job_id(job_id);
            return w.channel->send(MessageType::CANCEL_JOB, job_hash, 0, 0, "{}");
        }
    }
    return false;
}

void WorkerManager::poll_events(int timeout_ms) {
    std::vector<pollfd> pfds;
    std::vector<int> worker_indices;

    for (size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i].channel && workers_[i].channel->is_open()) {
            pollfd pfd{};
            pfd.fd = workers_[i].channel->fd();
            pfd.events = POLLIN;
            pfds.push_back(pfd);
            worker_indices.push_back(static_cast<int>(i));
        }
    }

    if (pfds.empty()) return;

    int rc = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);
    if (rc <= 0) return;

    for (size_t k = 0; k < pfds.size(); ++k) {
        if (pfds[k].revents & POLLIN) {
            int idx = worker_indices[k];
            ManagedWorker& w = workers_[static_cast<size_t>(idx)];

            MessageHeader header;
            std::string payload;
            while (w.channel->receive(header, payload, 0)) {
                if (header.type == static_cast<uint32_t>(MessageType::JOB_STARTED)) {
                    auto j = nlohmann::json::parse(payload, nullptr, false);
                    if (!j.is_discarded()) {
                        w.active_workload_pid = j.value("workload_pid", -1);
                        if (on_job_started) {
                            on_job_started(w.id, w.current_job_id, w.active_workload_pid);
                        }
                    }
                } else if (header.type == static_cast<uint32_t>(MessageType::JOB_COMPLETED)) {
                    auto j = nlohmann::json::parse(payload, nullptr, false);
                    std::string j_id = w.current_job_id;
                    int exit_code = j.value("exit_code", 0);
                    std::string logs = j.value("output", "");
                    w.state = WorkerState::Idle;
                    w.current_job_id = "";
                    w.active_workload_pid = -1;
                    if (on_job_completed) {
                        on_job_completed(w.id, j_id, exit_code, logs);
                    }
                } else if (header.type == static_cast<uint32_t>(MessageType::JOB_FAILED)) {
                    auto j = nlohmann::json::parse(payload, nullptr, false);
                    std::string j_id = w.current_job_id;
                    int exit_code = j.value("exit_code", -1);
                    std::string logs = j.value("output", "");
                    w.state = WorkerState::Idle;
                    w.current_job_id = "";
                    w.active_workload_pid = -1;
                    if (on_job_failed) {
                        on_job_failed(w.id, j_id, exit_code, logs);
                    }
                } else if (header.type == static_cast<uint32_t>(MessageType::HEARTBEAT)) {
                    w.last_heartbeat = std::chrono::system_clock::now();
                }
            }
        }
    }
}

void WorkerManager::reap_children() {
    int status = 0;
    while (true) {
        pid_t dead_pid = waitpid(-1, &status, WNOHANG);
        if (dead_pid <= 0) {
            break;
        }

        // Match against workers
        for (auto& w : workers_) {
            if (w.pid == dead_pid) {
                if (w.state != WorkerState::Terminated) {
                    w.state = WorkerState::Unhealthy;
                    w.pid = -1;
                    w.channel.reset();
                    if (on_worker_crashed) {
                        on_worker_crashed(w.id, w.restart_count);
                    }
                }
                break;
            }
        }
    }
}

void WorkerManager::check_health_and_restart() {
    for (auto& w : workers_) {
        if (w.state == WorkerState::Unhealthy || w.pid <= 0) {
            if (w.restart_count < 5) {
                w.restart_count++;
                start_worker(w.id);
            }
        }
    }
}

void WorkerManager::shutdown_all(bool immediate) {
    for (auto& w : workers_) {
        if (w.pid > 0) {
            stop_worker(w.id, immediate);
        }
    }
}

} // namespace astra
