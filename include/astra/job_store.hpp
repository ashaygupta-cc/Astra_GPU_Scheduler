#pragma once

#include "astra/job.hpp"
#include "astra/worker_info.hpp"
#include "astra/event.hpp"
#include "astra/workflow.hpp"

#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace astra {

class JobStore {
public:
    JobStore();
    ~JobStore();

    JobStore(const JobStore&) = delete;
    JobStore& operator=(const JobStore&) = delete;

    JobStore(JobStore&&) noexcept;
    JobStore& operator=(JobStore&&) noexcept;

    bool open(const std::string& db_path, std::string* error_out = nullptr);
    void close();
    bool is_open() const;

    int get_schema_version(std::string* error_out = nullptr);

    // Job persistence
    bool save_job(const Job& job, std::string* error_out = nullptr);
    bool update_job_state(const std::string& job_id, JobState state, int exit_code = 0, 
                          const std::string& failure_reason = "", std::string* error_out = nullptr);
    bool update_job_worker(const std::string& job_id, int worker_id, pid_t pid, std::string* error_out = nullptr);
    std::optional<Job> get_job(const std::string& job_id, std::string* error_out = nullptr);
    std::vector<Job> get_all_jobs(std::string* error_out = nullptr);
    std::vector<Job> get_jobs_by_state(JobState state, std::string* error_out = nullptr);
    bool delete_job(const std::string& job_id, std::string* error_out = nullptr);

    // Dependencies
    bool add_dependency(const std::string& job_id, const std::string& depends_on_job_id, std::string* error_out = nullptr);
    std::vector<std::string> get_dependencies(const std::string& job_id, std::string* error_out = nullptr);

    // Execution attempts
    bool record_attempt(const JobAttempt& attempt, std::string* error_out = nullptr);
    std::vector<JobAttempt> get_attempts_for_job(const std::string& job_id, std::string* error_out = nullptr);

    // Worker tracking
    bool save_worker(const WorkerInfo& worker, std::string* error_out = nullptr);
    std::optional<WorkerInfo> get_worker(int worker_id, std::string* error_out = nullptr);
    std::vector<WorkerInfo> get_all_workers(std::string* error_out = nullptr);

    // Event audit logs
    bool record_event(const Event& event, std::uint64_t* out_event_id = nullptr, std::string* error_out = nullptr);
    std::vector<Event> get_events(const std::string& job_id = "", size_t limit = 100, std::string* error_out = nullptr);

    // Workflow persistence
    bool save_workflow(const WorkflowRecord& wf, std::string* error_out = nullptr);
    bool update_workflow_state(const std::string& wf_id, const std::string& state, std::string* error_out = nullptr);
    std::optional<WorkflowRecord> get_workflow(const std::string& wf_id, std::string* error_out = nullptr);
    std::vector<WorkflowRecord> get_all_workflows(std::string* error_out = nullptr);
    std::vector<Job> get_jobs_by_workflow(const std::string& wf_id, std::string* error_out = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra
