#include "astra/scheduler.hpp"

#include <chrono>
#include <iostream>

namespace astra {

Scheduler::Scheduler(JobStore& store, 
                     WorkerManager& worker_manager, 
                     ResourceManager& resource_manager, 
                     const AstraConfig& config)
    : store_(store), 
      worker_manager_(worker_manager), 
      resource_manager_(resource_manager), 
      config_(config),
      policy_(create_scheduling_policy(config.default_policy, config.aging_interval_ms, config.aging_max_priority)) {
    setup_callbacks();
}

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::set_policy(std::unique_ptr<SchedulingPolicy> policy) {
    std::lock_guard<std::mutex> lock(step_mtx_);
    policy_ = std::move(policy);
}

void Scheduler::setup_callbacks() {
    worker_manager_.on_job_started = [this](int worker_id, const std::string& job_id, pid_t workload_pid) {
        store_.update_job_worker(job_id, worker_id, workload_pid);
        Event ev{0, std::chrono::system_clock::now(), EventType::JOB_STARTED, job_id, worker_id, 
                 "{\"workload_pid\": " + std::to_string(workload_pid) + "}"};
        store_.record_event(ev);
    };

    worker_manager_.on_job_completed = [this](int worker_id, const std::string& job_id, int exit_code, const std::string& logs) {
        resource_manager_.release(job_id);

        JobAttempt attempt;
        attempt.job_id = job_id;
        attempt.worker_id = worker_id;
        attempt.exit_code = exit_code;
        attempt.start_time = std::chrono::system_clock::now();
        attempt.end_time = attempt.start_time;
        store_.record_attempt(attempt);

        store_.update_job_state(job_id, JobState::Succeeded, exit_code, logs);

        Event ev_rel{0, std::chrono::system_clock::now(), EventType::RESOURCE_RELEASED, job_id, worker_id, "{}"};
        store_.record_event(ev_rel);

        Event ev_comp{0, std::chrono::system_clock::now(), EventType::JOB_COMPLETED, job_id, worker_id, 
                      "{\"exit_code\": " + std::to_string(exit_code) + "}"};
        store_.record_event(ev_comp);
    };

    worker_manager_.on_job_failed = [this](int worker_id, const std::string& job_id, int exit_code, const std::string& logs) {
        resource_manager_.release(job_id);

        JobAttempt attempt;
        attempt.job_id = job_id;
        attempt.worker_id = worker_id;
        attempt.exit_code = exit_code;
        attempt.error_message = logs;
        attempt.start_time = std::chrono::system_clock::now();
        attempt.end_time = attempt.start_time;
        store_.record_attempt(attempt);

        auto job_opt = store_.get_job(job_id);
        if (job_opt && job_opt->retry_count < job_opt->max_retries) {
            // Trigger bounded retry
            Job updated = *job_opt;
            updated.retry_count++;
            updated.state = JobState::Queued;
            updated.enqueued_time = std::chrono::system_clock::now();
            store_.save_job(updated);

            Event ev_ret{0, std::chrono::system_clock::now(), EventType::JOB_RETRIED, job_id, worker_id, 
                         "{\"retry_count\": " + std::to_string(updated.retry_count) + "}"};
            store_.record_event(ev_ret);

            Event ev_q{0, updated.enqueued_time, EventType::JOB_QUEUED, job_id, -1, "{}"};
            store_.record_event(ev_q);
        } else {
            store_.update_job_state(job_id, JobState::Failed, exit_code, logs);

            Event ev_rel{0, std::chrono::system_clock::now(), EventType::RESOURCE_RELEASED, job_id, worker_id, "{}"};
            store_.record_event(ev_rel);

            Event ev_fail{0, std::chrono::system_clock::now(), EventType::JOB_FAILED, job_id, worker_id, 
                          "{\"exit_code\": " + std::to_string(exit_code) + "}"};
            store_.record_event(ev_fail);
        }
    };
}

void Scheduler::reconcile_dependencies() {
    auto waiting_jobs = store_.get_jobs_by_state(JobState::WaitingForDependencies);
    for (auto& job : waiting_jobs) {
        bool all_satisfied = true;
        bool dependency_failed = false;
        std::string failed_dep_id;

        for (const auto& dep_id : job.dependencies) {
            auto dep_opt = store_.get_job(dep_id);
            if (!dep_opt) {
                dependency_failed = true;
                failed_dep_id = dep_id;
                break;
            }
            if (dep_opt->state == JobState::Failed || dep_opt->state == JobState::Cancelled) {
                dependency_failed = true;
                failed_dep_id = dep_id;
                break;
            }
            if (dep_opt->state != JobState::Succeeded) {
                all_satisfied = false;
            }
        }

        if (dependency_failed) {
            store_.update_job_state(job.id, JobState::Blocked, -1, "Prerequisite dependency '" + failed_dep_id + "' failed");
        } else if (all_satisfied) {
            store_.update_job_state(job.id, JobState::Queued);
            Event ev_q{0, std::chrono::system_clock::now(), EventType::JOB_QUEUED, job.id, -1, "{}"};
            store_.record_event(ev_q);
        }
    }
}

bool Scheduler::step() {
    std::lock_guard<std::mutex> lock(step_mtx_);

    // 1. Process active child workers and poll IPC
    worker_manager_.reap_children();
    worker_manager_.poll_events(0);
    worker_manager_.check_health_and_restart();

    // 2. Unblock jobs whose dependencies have succeeded
    reconcile_dependencies();

    // 3. Query queued candidate jobs
    auto queued_jobs = store_.get_jobs_by_state(JobState::Queued);
    if (queued_jobs.empty()) {
        return true;
    }

    // 4. Dispatch eligible jobs while idle workers and resources exist
    while (!queued_jobs.empty()) {
        int idle_worker = worker_manager_.find_idle_worker();
        if (idle_worker < 0) {
            break; // All workers are currently busy
        }

        auto decision = policy_->choose_next(queued_jobs);
        if (!decision || !decision->chosen_job) {
            break;
        }

        const Job candidate = *decision->chosen_job;

        std::string admission_reason;
        if (!resource_manager_.can_admit(candidate.resources, &admission_reason)) {
            // Cannot admit candidate right now due to limits
            break;
        }

        // Reserve logical resources
        std::string res_err;
        if (!resource_manager_.reserve(candidate.id, candidate.resources, &res_err)) {
            break;
        }

        // Record decision and reservation events
        Event ev_dec{0, std::chrono::system_clock::now(), EventType::SCHEDULER_DECISION, 
                     candidate.id, idle_worker, "{\"reason\": \"" + decision->reason + "\"}"};
        store_.record_event(ev_dec);

        Event ev_res{0, std::chrono::system_clock::now(), EventType::RESOURCE_RESERVED, 
                     candidate.id, idle_worker, "{}"};
        store_.record_event(ev_res);

        // Transition job: Queued -> Admitted -> Running
        store_.update_job_state(candidate.id, JobState::Admitted);
        store_.update_job_state(candidate.id, JobState::Running);
        store_.update_job_worker(candidate.id, idle_worker, -1);

        // Dispatch job over IPC to worker
        if (!worker_manager_.dispatch_job(idle_worker, candidate)) {
            // Dispatch failed; release reservation and restore queued state
            resource_manager_.release(candidate.id);
            store_.update_job_state(candidate.id, JobState::Queued);
            break;
        }

        // Remove candidate from local queued list
        for (auto it = queued_jobs.begin(); it != queued_jobs.end(); ++it) {
            if (it->id == candidate.id) {
                queued_jobs.erase(it);
                break;
            }
        }
    }

    return true;
}

void Scheduler::start() {
    if (running_.exchange(true)) return;

    loop_thread_ = std::thread([this]() {
        while (running_.load()) {
            step();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
}

void Scheduler::stop() {
    if (!running_.exchange(false)) return;

    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
}

} // namespace astra
