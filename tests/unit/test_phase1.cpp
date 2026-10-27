#include "astra/job_state.hpp"
#include "astra/job.hpp"
#include "astra/worker_info.hpp"
#include "astra/event.hpp"
#include "astra/config.hpp"
#include "astra/job_store.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <chrono>

void test_job_state_transitions() {
    using namespace astra;

    // Test to_string and from_string
    assert(to_string(JobState::Submitted) == "SUBMITTED");
    assert(job_state_from_string("RUNNING") == JobState::Running);
    assert(!job_state_from_string("INVALID_STATE").has_value());

    // Valid transitions
    assert(is_valid_transition(JobState::Submitted, JobState::Queued));
    assert(is_valid_transition(JobState::Submitted, JobState::WaitingForDependencies));
    assert(is_valid_transition(JobState::WaitingForDependencies, JobState::Queued));
    assert(is_valid_transition(JobState::Queued, JobState::Admitted));
    assert(is_valid_transition(JobState::Admitted, JobState::Running));
    assert(is_valid_transition(JobState::Running, JobState::Succeeded));
    assert(is_valid_transition(JobState::Running, JobState::Failed));
    assert(is_valid_transition(JobState::Running, JobState::Retrying));
    assert(is_valid_transition(JobState::Retrying, JobState::Queued));
    assert(is_valid_transition(JobState::Queued, JobState::Cancelled));
    assert(is_valid_transition(JobState::Running, JobState::Cancelled));
    assert(is_valid_transition(JobState::Running, JobState::Checkpointing));
    assert(is_valid_transition(JobState::Checkpointing, JobState::Paused));
    assert(is_valid_transition(JobState::Paused, JobState::Running));

    // Invalid transitions
    assert(!is_valid_transition(JobState::Succeeded, JobState::Running));
    assert(!is_valid_transition(JobState::Failed, JobState::Queued));
    assert(!is_valid_transition(JobState::Cancelled, JobState::Running));
    assert(!is_valid_transition(JobState::Queued, JobState::Running)); // Must pass through Admitted!

    // Validate helper with error reporting
    JobState s = JobState::Queued;
    std::string err;
    assert(validate_and_transition(s, JobState::Admitted, &err));
    assert(s == JobState::Admitted);
    assert(!validate_and_transition(s, JobState::Succeeded, &err));
    assert(!err.empty());

    std::cout << "[PASS] JobState transitions and validation\n";
}

void test_job_model_validation() {
    using namespace astra;

    Job valid_job;
    valid_job.id = "job-001";
    valid_job.name = "matrix_mul";
    valid_job.executable_path = "./workloads/matrix_mul_cpu";
    valid_job.argv = {"--size", "512"};
    valid_job.resources.cpu_slots = 2;
    valid_job.priority = 5;

    std::string err;
    assert(valid_job.validate(&err));

    Job empty_id = valid_job;
    empty_id.id = "";
    assert(!empty_id.validate(&err));

    Job zero_cpu = valid_job;
    zero_cpu.resources.cpu_slots = 0;
    assert(!zero_cpu.validate(&err));

    Job neg_pri = valid_job;
    neg_pri.priority = -1;
    assert(!neg_pri.validate(&err));

    // JSON round-trip
    nlohmann::json j = valid_job;
    Job parsed = j.get<Job>();
    assert(parsed.id == valid_job.id);
    assert(parsed.name == valid_job.name);
    assert(parsed.executable_path == valid_job.executable_path);
    assert(parsed.argv == valid_job.argv);
    assert(parsed.resources.cpu_slots == 2);

    std::cout << "[PASS] Job model validation and JSON serialization\n";
}

void test_config_loader() {
    using namespace astra;

    AstraConfig cfg;
    std::string err;
    assert(cfg.validate(&err));

    std::string config_path = std::filesystem::exists("configs/astra.json") ? 
        "configs/astra.json" : "../configs/astra.json";
    AstraConfig loaded = AstraConfig::load_from_file(config_path);
    assert(loaded.max_workers == 6);
    assert(loaded.max_cpu_slots == 6);
    assert(loaded.default_policy == "priority_aging");

    // Invalid config checks
    AstraConfig bad_policy = cfg;
    bad_policy.default_policy = "random_shuffle";
    assert(!bad_policy.validate(&err));

    AstraConfig bad_hb = cfg;
    bad_hb.heartbeat_timeout_ms = 500;
    bad_hb.heartbeat_interval_ms = 1000;
    assert(!bad_hb.validate(&err));

    std::cout << "[PASS] AstraConfig validation and JSON loader\n";
}

void test_job_store_crud_and_persistence() {
    using namespace astra;

    const std::string test_db = "test_astra_phase1.db";
    std::filesystem::remove(test_db);

    {
        JobStore store;
        std::string err;
        assert(store.open(test_db, &err));
        assert(store.is_open());
        assert(store.get_schema_version() == 1);

        // 1. Create and Save Job
        Job job1;
        job1.id = "job-alpha";
        job1.name = "preprocess";
        job1.executable_path = "./workloads/cpu_burn";
        job1.argv = {"100"};
        job1.priority = 8;
        job1.resources.cpu_slots = 2;
        job1.resources.ram_mb = 256;
        job1.dependencies = {"dep-1", "dep-2"};
        job1.state = JobState::Queued;
        assert(store.save_job(job1, &err));

        // 2. Query Job
        auto fetched = store.get_job("job-alpha", &err);
        assert(fetched.has_value());
        assert(fetched->id == "job-alpha");
        assert(fetched->name == "preprocess");
        assert(fetched->priority == 8);
        assert(fetched->resources.cpu_slots == 2);
        assert(fetched->dependencies.size() == 2);
        assert(fetched->dependencies[0] == "dep-1");

        // 3. Update State
        assert(store.update_job_state("job-alpha", JobState::Running, 0, "", &err));
        auto running_job = store.get_job("job-alpha");
        assert(running_job->state == JobState::Running);

        // 4. Update Worker
        assert(store.update_job_worker("job-alpha", 3, 12345, &err));
        auto assigned = store.get_job("job-alpha");
        assert(assigned->assigned_worker_id == 3);
        assert(assigned->worker_pid == 12345);

        // 5. Record Attempt
        JobAttempt attempt;
        attempt.job_id = "job-alpha";
        attempt.attempt_number = 1;
        attempt.worker_id = 3;
        attempt.pid = 12345;
        attempt.start_time = std::chrono::system_clock::now();
        attempt.end_time = attempt.start_time + std::chrono::seconds(2);
        attempt.exit_code = 0;
        assert(store.record_attempt(attempt, &err));

        auto attempts = store.get_attempts_for_job("job-alpha");
        assert(attempts.size() == 1);
        assert(attempts[0].pid == 12345);
        assert(attempts[0].worker_id == 3);

        // 6. Save and Query Worker
        WorkerInfo worker;
        worker.id = 3;
        worker.pid = 12345;
        worker.state = WorkerState::Busy;
        worker.current_job_id = "job-alpha";
        assert(store.save_worker(worker, &err));

        auto fetched_worker = store.get_worker(3);
        assert(fetched_worker.has_value());
        assert(fetched_worker->state == WorkerState::Busy);

        // 7. Record Event
        Event ev;
        ev.type = EventType::JOB_STARTED;
        ev.job_id = "job-alpha";
        ev.worker_id = 3;
        ev.payload_json = "{\"cpu\": 2}";
        uint64_t ev_id = 0;
        assert(store.record_event(ev, &ev_id, &err));
        assert(ev_id > 0);

        auto events = store.get_events("job-alpha");
        assert(!events.empty());
        assert(events[0].type == EventType::JOB_STARTED);

        store.close();
    }

    // 8. Re-open and verify persistence across restart
    {
        JobStore store;
        std::string err;
        assert(store.open(test_db, &err));
        assert(store.get_schema_version() == 1);

        auto job = store.get_job("job-alpha");
        assert(job.has_value());
        assert(job->state == JobState::Running);
        assert(job->assigned_worker_id == 3);
        assert(job->dependencies.size() == 2);

        auto workers = store.get_all_workers();
        assert(workers.size() == 1);
        assert(workers[0].id == 3);
        assert(workers[0].state == WorkerState::Busy);

        auto events = store.get_events("job-alpha");
        assert(events.size() == 1);
        assert(events[0].job_id == "job-alpha");

        store.close();
    }

    std::filesystem::remove(test_db);
    std::cout << "[PASS] JobStore SQLite CRUD, migrations, and restart persistence\n";
}

int main() {
    std::cout << "Running ASTRA Phase 1 (Domain & Storage) Unit Tests...\n";
    test_job_state_transitions();
    test_job_model_validation();
    test_config_loader();
    test_job_store_crud_and_persistence();
    std::cout << "All Phase 1 tests passed successfully!\n";
    return 0;
}
