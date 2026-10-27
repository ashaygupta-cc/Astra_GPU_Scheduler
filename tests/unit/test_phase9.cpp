#include "astra/replay_engine.hpp"
#include "astra/benchmark.hpp"
#include "astra/job_store.hpp"
#include "astra/command_parser.hpp"
#include "astra/cli_handler.hpp"

#include <iostream>
#include <cassert>
#include <fstream>
#include <filesystem>
#include <sstream>

void test_replay_from_store() {
    using namespace astra;

    const std::string test_db = "test_phase9_replay.db";
    std::filesystem::remove(test_db);

    JobStore store;
    std::string err;
    assert(store.open(test_db, &err));

    const std::string job_id = "replay-job-1";
    auto base_tp = std::chrono::system_clock::now();

    auto make_ev = [&](EventType type, int worker_id, const std::string& payload, int offset_ms) {
        Event ev;
        ev.job_id = job_id;
        ev.type = type;
        ev.worker_id = worker_id;
        ev.payload_json = payload;
        ev.timestamp = base_tp + std::chrono::milliseconds(offset_ms);
        return ev;
    };

    // Insert chronological events
    assert(store.record_event(make_ev(EventType::JOB_SUBMITTED, -1, "{\"workload\": \"cpu_burn\"}", 0)));
    assert(store.record_event(make_ev(EventType::JOB_QUEUED, -1, "{\"queue\": \"priority\"}", 50)));
    assert(store.record_event(make_ev(EventType::JOB_STARTED, 0, "{\"worker_pid\": 1234}", 120)));
    assert(store.record_event(make_ev(EventType::JOB_PROGRESS, 0, "{\"cpu_slots\": 2}", 250)));
    assert(store.record_event(make_ev(EventType::JOB_COMPLETED, 0, "{\"exit_code\": 0, \"duration_ms\": 450}", 500)));

    auto steps = ReplayEngine::load_from_store(store, job_id);
    assert(steps.size() == 5);
    assert(steps[0].step_index == 1);
    assert(steps[0].event_type == EventType::JOB_SUBMITTED);
    assert(steps[0].job_id == job_id);
    assert(steps[4].step_index == 5);
    assert(steps[4].event_type == EventType::JOB_COMPLETED);
    assert(steps[4].worker_id == 0);

    std::string table = ReplayEngine::render_timeline_table(steps);
    assert(table.find("DETERMINISTIC EVENT REPLAY TIMELINE") != std::string::npos);
    assert(table.find("replay-job-1") != std::string::npos);
    assert(table.find("JOB_COMPLETED") != std::string::npos);

    store.close();
    std::filesystem::remove(test_db);

    std::cout << "[PASS] Deterministic event replay from JobStore SQLite database\n";
}

void test_replay_from_jsonl() {
    using namespace astra;

    const std::string test_jsonl = "test_phase9_events.jsonl";
    std::filesystem::remove(test_jsonl);

    {
        std::ofstream out(test_jsonl);
        out << "{\"type\": \"JOB_SUBMITTED\", \"job_id\": \"trace-job-9\", \"worker_id\": -1, \"timestamp_ms\": 1000, \"payload\": {\"p\": 5}}\n";
        out << "{\"type\": \"JOB_STARTED\", \"job_id\": \"trace-job-9\", \"worker_id\": 2, \"timestamp_ms\": 1250, \"payload\": {\"worker\": 2}}\n";
        out << "{\"type\": \"JOB_COMPLETED\", \"job_id\": \"trace-job-9\", \"worker_id\": 2, \"timestamp_ms\": 1800, \"payload\": {\"exit\": 0}}\n";
    }

    auto steps = ReplayEngine::load_from_jsonl(test_jsonl);
    assert(steps.size() == 3);
    assert(steps[0].event_type == EventType::JOB_SUBMITTED);
    assert(steps[1].event_type == EventType::JOB_STARTED);
    assert(steps[1].worker_id == 2);
    assert(steps[2].event_type == EventType::JOB_COMPLETED);
    assert(steps[2].delta_seconds >= 0.79 && steps[2].delta_seconds <= 0.81);

    std::string table = ReplayEngine::render_timeline_table(steps);
    assert(table.find("trace-job-9") != std::string::npos);
    assert(table.find("Total Replay Events: 3") != std::string::npos);

    std::filesystem::remove(test_jsonl);

    std::cout << "[PASS] Deterministic event replay from JSONL event log\n";
}

void test_policy_benchmarks_and_starvation_analysis() {
    using namespace astra;

    // Run comparison with 20 jobs and 2 execution slots
    auto results = PolicyBenchmarkSuite::run_comparison(20, 2);
    assert(results.size() == 4);

    const PolicyBenchmarkResult* fcfs_res = nullptr;
    const PolicyBenchmarkResult* prio_res = nullptr;
    const PolicyBenchmarkResult* aging_res = nullptr;
    const PolicyBenchmarkResult* sjf_res = nullptr;

    for (const auto& r : results) {
        if (r.policy_name == "fcfs") fcfs_res = &r;
        else if (r.policy_name == "priority") prio_res = &r;
        else if (r.policy_name == "aging") aging_res = &r;
        else if (r.policy_name == "sjf") sjf_res = &r;
    }

    assert(fcfs_res != nullptr);
    assert(prio_res != nullptr);
    assert(aging_res != nullptr);
    assert(sjf_res != nullptr);

    assert(fcfs_res->total_jobs == 20);
    assert(prio_res->total_jobs == 20);
    assert(aging_res->total_jobs == 20);
    assert(sjf_res->total_jobs == 20);

    // Theory check 1: SJF achieves lowest or equal average turnaround time compared to FCFS
    assert(sjf_res->avg_turnaround_time <= fcfs_res->avg_turnaround_time + 0.1);

    // Theory check 2: Priority Aging prevents starvation (starved jobs <= strict priority)
    assert(aging_res->starved_jobs <= prio_res->starved_jobs);

    std::string report = PolicyBenchmarkSuite::format_benchmark_report(results);
    assert(report.find("ASTRA SCHEDULING POLICY BENCHMARK REPORT") != std::string::npos);
    assert(report.find("FCFS (Baseline)") != std::string::npos);
    assert(report.find("Priority Aging") != std::string::npos);
    assert(report.find("Shortest Job First") != std::string::npos);

    std::cout << "[PASS] Scheduling policy benchmarks, makespan accounting, and starvation verification\n";
}

void test_cli_replay_and_bench_dispatch() {
    using namespace astra;

    const std::string test_db = "test_phase9_cli.db";
    std::filesystem::remove(test_db);

    AstraConfig cfg;
    JobStore store;
    assert(store.open(test_db));

    CliHandler handler(store, cfg);

    // 1. Replay dispatch for existing job
    Event ev1;
    ev1.job_id = "job-test-cli";
    ev1.type = EventType::JOB_SUBMITTED;
    ev1.worker_id = -1;
    ev1.payload_json = "{}";
    ev1.timestamp = std::chrono::system_clock::now();
    store.record_event(ev1);

    Event ev2;
    ev2.job_id = "job-test-cli";
    ev2.type = EventType::JOB_COMPLETED;
    ev2.worker_id = 1;
    ev2.payload_json = "{\"code\": 0}";
    ev2.timestamp = ev1.timestamp + std::chrono::milliseconds(500);
    store.record_event(ev2);

    auto cmd_replay = CommandParser::parse({"replay", "job-test-cli"}, "");
    std::ostringstream out1, err1;
    int rc1 = handler.execute(cmd_replay, out1, err1);
    assert(rc1 == 0);
    assert(out1.str().find("job-test-cli") != std::string::npos);

    // 2. Bench dispatch
    auto cmd_bench = CommandParser::parse({"bench", "--jobs", "15", "--slots", "2"}, "");
    std::ostringstream out2, err2;
    int rc2 = handler.execute(cmd_bench, out2, err2);
    assert(rc2 == 0);
    assert(out2.str().find("POLICY BENCHMARK REPORT") != std::string::npos);

    store.close();
    std::filesystem::remove(test_db);

    std::cout << "[PASS] CLI command dispatch for 'replay' and 'bench'\n";
}

int main() {
    std::cout << "Running ASTRA Phase 9 (Replay & Benchmarks) Unit & Integration Tests...\n";
    test_replay_from_store();
    test_replay_from_jsonl();
    test_policy_benchmarks_and_starvation_analysis();
    test_cli_replay_and_bench_dispatch();
    std::cout << "All Phase 9 tests passed successfully!\n";
    return 0;
}
