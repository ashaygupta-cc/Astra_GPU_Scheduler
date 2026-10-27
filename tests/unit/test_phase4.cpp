#include "astra/scheduling_policy.hpp"
#include "astra/resource_manager.hpp"
#include "astra/scheduler.hpp"
#include "astra/job_store.hpp"
#include "astra/worker_manager.hpp"
#include "astra/config.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <thread>
#include <chrono>

void test_scheduling_policies() {
    using namespace astra;

    auto now = std::chrono::system_clock::now();

    Job job_a;
    job_a.id = "job-a";
    job_a.priority = 3;
    job_a.enqueued_time = now - std::chrono::seconds(10); // Enqueued 10s ago

    Job job_b;
    job_b.id = "job-b";
    job_b.priority = 8;
    job_b.enqueued_time = now; // Enqueued just now

    std::vector<Job> jobs = {job_a, job_b};

    // 1. FCFS Policy: job_a should be chosen (earlier enqueue timestamp)
    FcfsPolicy fcfs;
    auto dec_fcfs = fcfs.choose_next(jobs, now);
    assert(dec_fcfs.has_value());
    assert(dec_fcfs->chosen_job->id == "job-a");
    std::cout << "[PASS] FCFS policy ordering\n";

    // 2. Strict Priority Policy: job_b should be chosen (priority 8 > 3)
    PriorityPolicy pri;
    auto dec_pri = pri.choose_next(jobs, now);
    assert(dec_pri.has_value());
    assert(dec_pri->chosen_job->id == "job-b");
    std::cout << "[PASS] Priority policy ordering\n";

    // 3. Priority Aging Policy:
    // job_a has base priority 3, waited 10s with 2s interval -> boost of +5 -> effective priority 8
    // If we make job_a wait 12s, boost is +6 -> effective priority 9 > 8!
    job_a.enqueued_time = now - std::chrono::seconds(12);
    PriorityAgingPolicy aging(2000, 10); // 2000ms interval, max 10
    auto dec_aging = aging.choose_next(jobs, now);
    assert(dec_aging.has_value());
    assert(dec_aging->chosen_job->id == "job-a"); // job_a aged past job_b!
    std::cout << "[PASS] Priority Aging starvation prevention\n";
}

void test_resource_manager_admission_and_exact_release() {
    using namespace astra;

    ResourceManager rm(4, 4096); // 4 CPU slots, 4GB RAM
    assert(rm.total_cpu_slots() == 4);
    assert(rm.reserved_cpu_slots() == 0);
    assert(rm.available_cpu_slots() == 4);

    ResourceRequest req1;
    req1.cpu_slots = 3;
    req1.ram_mb = 1024;
    req1.device = DeviceRequirement::CpuOnly;

    std::string reason;
    assert(rm.can_admit(req1, &reason));
    assert(rm.reserve("job-1", req1));
    assert(rm.reserved_cpu_slots() == 3);
    assert(rm.available_cpu_slots() == 1);

    // Overcommitment test: requesting 2 slots when only 1 is available
    ResourceRequest req2;
    req2.cpu_slots = 2;
    assert(!rm.can_admit(req2, &reason));
    assert(reason.find("CPU slot limit reached") != std::string::npos);

    // Release job-1
    assert(rm.release("job-1"));
    assert(rm.reserved_cpu_slots() == 0);
    assert(rm.available_cpu_slots() == 4);

    // Double-release test (Exact-once semantics)
    std::string rel_err;
    assert(!rm.release("job-1", &rel_err));
    assert(rel_err.find("No active resource reservation found") != std::string::npos);

    std::cout << "[PASS] ResourceManager admission and exact-once release semantics\n";
}

void test_scheduler_end_to_end() {
    using namespace astra;

    const std::string test_db = "test_scheduler_phase4.db";
    std::filesystem::remove(test_db);

    JobStore store;
    std::string err;
    assert(store.open(test_db, &err));

    AstraConfig config;
    config.max_workers = 2;
    config.max_cpu_slots = 4;
    config.default_policy = "priority";

    ResourceManager rm(config.max_cpu_slots, 4096);
    WorkerManager wm(config.max_workers);
    assert(wm.init(config.max_workers));

    Scheduler scheduler(store, wm, rm, config);
    scheduler.set_policy(std::make_unique<PriorityPolicy>());

    // Submit two jobs with different priorities
    std::string burn_bin = std::filesystem::exists("build/workloads/cpu_burn") ? 
        "build/workloads/cpu_burn" : "./workloads/cpu_burn";

    Job j1;
    j1.id = "sched-job-low";
    j1.name = "low_pri";
    j1.executable_path = burn_bin;
    j1.argv = {"1"};
    j1.priority = 2;
    j1.state = JobState::Queued;
    j1.enqueued_time = std::chrono::system_clock::now();
    assert(store.save_job(j1));

    Job j2;
    j2.id = "sched-job-high";
    j2.name = "high_pri";
    j2.executable_path = burn_bin;
    j2.argv = {"1"};
    j2.priority = 9;
    j2.state = JobState::Queued;
    j2.enqueued_time = std::chrono::system_clock::now();
    assert(store.save_job(j2));

    // Step scheduler loop until both complete
    auto start_time = std::chrono::steady_clock::now();
    bool all_done = false;

    while (!all_done && 
           std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count() < 10) {
        scheduler.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto low = store.get_job("sched-job-low");
        auto high = store.get_job("sched-job-high");
        if (low && high && low->state == JobState::Succeeded && high->state == JobState::Succeeded) {
            all_done = true;
        }
    }

    assert(all_done);
    assert(rm.reserved_cpu_slots() == 0); // Resources completely released

    // Check events
    auto events = store.get_events();
    assert(!events.empty());

    bool saw_decision = false;
    for (const auto& ev : events) {
        if (ev.type == EventType::SCHEDULER_DECISION) saw_decision = true;
    }
    assert(saw_decision);

    wm.shutdown_all(false);
    store.close();
    std::filesystem::remove(test_db);

    std::cout << "[PASS] Scheduler end-to-end execution, policy dispatch, and resource reclamation\n";
}

int main() {
    std::cout << "Running ASTRA Phase 4 (Scheduler Core) Unit & Integration Tests...\n";
    test_scheduling_policies();
    test_resource_manager_admission_and_exact_release();
    test_scheduler_end_to_end();
    std::cout << "All Phase 4 tests passed successfully!\n";
    return 0;
}
