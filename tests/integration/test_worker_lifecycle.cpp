#include "astra/ipc_channel.hpp"
#include "astra/worker_manager.hpp"
#include "astra/job.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <chrono>
#include <thread>
#include <csignal>

void test_ipc_channel_standalone() {
    using namespace astra;

    auto [fd1, fd2] = IpcChannel::create_socketpair();
    assert(fd1.valid() && fd2.valid());

    IpcChannel ch1(std::move(fd1));
    IpcChannel ch2(std::move(fd2));

    // Send from ch1 to ch2
    std::string test_payload = "{\"status\": \"active\", \"slots\": 4}";
    assert(ch1.send(MessageType::RUN_JOB, 12345, 1, 0, test_payload));

    MessageHeader hdr;
    std::string received_payload;
    assert(ch2.receive(hdr, received_payload, 1000));
    assert(hdr.type == static_cast<uint32_t>(MessageType::RUN_JOB));
    assert(hdr.job_id_numeric == 12345);
    assert(hdr.request_id == 1);
    assert(received_payload == test_payload);

    // Timeout test
    MessageHeader timeout_hdr;
    std::string timeout_payload;
    assert(!ch2.receive(timeout_hdr, timeout_payload, 50)); // Should timeout

    std::cout << "[PASS] IpcChannel socketpair communication and framing\n";
}

void test_worker_pool_and_execution() {
    using namespace astra;

    // Resolve workloads binary path
    std::string cpu_burn_bin = std::filesystem::exists("build/workloads/cpu_burn") ? 
        "build/workloads/cpu_burn" : "./workloads/cpu_burn";
    std::string fail_bin = std::filesystem::exists("build/workloads/deliberate_failure") ? 
        "build/workloads/deliberate_failure" : "./workloads/deliberate_failure";

    WorkerManager manager(2);
    assert(manager.init(2));
    assert(manager.active_worker_count() == 2);

    int idle_worker = manager.find_idle_worker();
    assert(idle_worker >= 0);

    // 1. Test successful CPU workload
    bool job1_started = false;
    bool job1_completed = false;
    int job1_exit = -1;
    std::string job1_output;

    manager.on_job_started = [&](int, const std::string& j_id, pid_t pid) {
        if (j_id == "job-test-success") {
            job1_started = true;
            assert(pid > 0);
        }
    };

    manager.on_job_completed = [&](int, const std::string& j_id, int exit_code, const std::string& logs) {
        if (j_id == "job-test-success") {
            job1_completed = true;
            job1_exit = exit_code;
            job1_output = logs;
        }
    };

    Job job1;
    job1.id = "job-test-success";
    job1.executable_path = cpu_burn_bin;
    job1.argv = {"1"}; // 1 second run
    assert(manager.dispatch_job(idle_worker, job1));

    // Poll until both started and completed are received (up to 5 seconds)
    auto start_poll = std::chrono::steady_clock::now();
    while ((!job1_started || !job1_completed) && 
           std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_poll).count() < 5) {
        manager.poll_events(50);
        manager.reap_children();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    assert(job1_started);
    assert(job1_completed);
    assert(job1_exit == 0);
    assert(job1_output.find("[WORKLOAD:cpu_burn]") != std::string::npos);
    std::cout << "[PASS] Worker process execution: successful job (exit code 0)\n";

    // 2. Test failing workload
    bool job2_failed = false;
    int job2_exit = -1;
    std::string job2_output;

    manager.on_job_failed = [&](int, const std::string& j_id, int exit_code, const std::string& logs) {
        if (j_id == "job-test-fail") {
            job2_failed = true;
            job2_exit = exit_code;
            job2_output = logs;
        }
    };

    idle_worker = manager.find_idle_worker();
    assert(idle_worker >= 0);

    Job job2;
    job2.id = "job-test-fail";
    job2.executable_path = fail_bin;
    job2.argv = {"42"}; // Deliberate exit code 42
    assert(manager.dispatch_job(idle_worker, job2));

    start_poll = std::chrono::steady_clock::now();
    while (!job2_failed && 
           std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_poll).count() < 5) {
        manager.poll_events(50);
        manager.reap_children();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    assert(job2_failed);
    assert(job2_exit == 42);
    assert(job2_output.find("deliberate_failure") != std::string::npos);
    std::cout << "[PASS] Worker process execution: failing job (exit code 42)\n";

    // 3. Test job cancellation
    idle_worker = manager.find_idle_worker();
    assert(idle_worker >= 0);

    Job job3;
    job3.id = "job-test-cancel";
    job3.executable_path = cpu_burn_bin;
    job3.argv = {"30"}; // 30 seconds
    assert(manager.dispatch_job(idle_worker, job3));

    // Wait until started
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    manager.poll_events(20);

    // Cancel job
    assert(manager.cancel_job("job-test-cancel"));

    // Poll until terminated
    start_poll = std::chrono::steady_clock::now();
    while (manager.workers()[static_cast<size_t>(idle_worker)].state == WorkerState::Busy && 
           std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_poll).count() < 4) {
        manager.poll_events(50);
        manager.reap_children();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    assert(manager.workers()[static_cast<size_t>(idle_worker)].state == WorkerState::Idle);
    std::cout << "[PASS] Worker process execution: job cancellation and process termination\n";

    // 4. Test worker crash & automatic restart
    pid_t worker0_pid = manager.workers()[0].pid;
    assert(worker0_pid > 0);

    // Deliberately kill worker 0
    kill(worker0_pid, SIGKILL);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    manager.reap_children();
    assert(manager.workers()[0].state == WorkerState::Unhealthy);

    // Revive worker
    manager.check_health_and_restart();
    assert(manager.workers()[0].state == WorkerState::Idle);
    assert(manager.workers()[0].pid > 0);
    assert(manager.workers()[0].pid != worker0_pid);
    assert(manager.workers()[0].restart_count == 1);
    std::cout << "[PASS] Worker crash detection, reaping, and auto-restart\n";

    // 5. Clean shutdown
    manager.shutdown_all(false);
    assert(manager.active_worker_count() == 0);
    std::cout << "[PASS] Clean worker pool shutdown without zombies\n";
}

int main() {
    std::cout << "Running ASTRA Phase 3 (Workers and IPC) Integration Tests...\n";
    test_ipc_channel_standalone();
    test_worker_pool_and_execution();
    std::cout << "All Phase 3 tests passed successfully!\n";
    return 0;
}
