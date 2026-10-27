#include "astra/workflow.hpp"
#include "astra/job_store.hpp"
#include "astra/scheduler.hpp"
#include "astra/resource_manager.hpp"
#include "astra/worker_manager.hpp"
#include "astra/scheduling_policy.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <thread>
#include <chrono>

void test_dag_validation_and_topological_sort() {
    using namespace astra;

    // Test valid linear pipeline
    std::string json_str = R"({
        "name": "data_pipeline",
        "jobs": [
            { "key": "extract", "workload": "cpu_burn", "args": ["10000"], "depends_on": [] },
            { "key": "transform", "workload": "parallel_sum", "args": ["10000", "2"], "depends_on": ["extract"] },
            { "key": "load", "workload": "cpu_burn", "args": ["5000"], "depends_on": ["transform"] }
        ]
    })";

    std::string err;
    auto wf_opt = DagEngine::parse_json_string(json_str, &err);
    assert(wf_opt.has_value());
    assert(wf_opt->tasks.size() == 3);

    std::vector<std::string> topo_order;
    assert(DagEngine::validate_dag(*wf_opt, topo_order, &err));
    assert(topo_order.size() == 3);
    assert(topo_order[0] == "extract");
    assert(topo_order[1] == "transform");
    assert(topo_order[2] == "load");

    std::unordered_map<std::string, std::string> key_map;
    auto jobs = DagEngine::instantiate_jobs(*wf_opt, "./build/workloads", key_map, &err);
    assert(jobs.size() == 3);
    assert(key_map.count("extract") && key_map.count("transform") && key_map.count("load"));

    // Check states: extract should be Queued, others WaitingForDependencies
    for (const auto& j : jobs) {
        if (j.id == key_map["extract"]) {
            assert(j.state == JobState::Queued);
            assert(j.dependencies.empty());
        } else if (j.id == key_map["transform"]) {
            assert(j.state == JobState::WaitingForDependencies);
            assert(j.dependencies.size() == 1);
            assert(j.dependencies[0] == key_map["extract"]);
        } else if (j.id == key_map["load"]) {
            assert(j.state == JobState::WaitingForDependencies);
            assert(j.dependencies.size() == 1);
            assert(j.dependencies[0] == key_map["transform"]);
        }
    }

    std::cout << "[PASS] DAG JSON parsing, topological ordering, and job dependency translation\n";
}

void test_cycle_detection_and_error_diagnostics() {
    using namespace astra;

    // 1. Direct self cycle
    {
        WorkflowDefinition self_cycle;
        self_cycle.tasks.push_back(WorkflowTaskDefinition{"loop", "Self Loop", "cpu_burn", {}, 5, {}, 1, {"loop"}});
        std::vector<std::string> topo;
        std::string err;
        assert(!DagEngine::validate_dag(self_cycle, topo, &err));
        assert(err.find("depends on itself") != std::string::npos);
    }

    // 2. 3-node cycle (A -> B -> C -> A)
    {
        std::string cyclic_json = R"({
            "name": "cyclic_test",
            "jobs": [
                { "key": "A", "workload": "cpu_burn", "depends_on": ["C"] },
                { "key": "B", "workload": "cpu_burn", "depends_on": ["A"] },
                { "key": "C", "workload": "cpu_burn", "depends_on": ["B"] }
            ]
        })";
        std::string err;
        auto wf_opt = DagEngine::parse_json_string(cyclic_json, &err);
        assert(wf_opt.has_value());

        std::vector<std::string> topo;
        bool valid = DagEngine::validate_dag(*wf_opt, topo, &err);
        assert(!valid);
        assert(err.find("Cycle detected") != std::string::npos);
        assert(err.find("A -> B -> C -> A") != std::string::npos ||
               err.find("B -> C -> A -> B") != std::string::npos ||
               err.find("C -> A -> B -> C") != std::string::npos);
    }

    // 3. Unknown dependency reference
    {
        WorkflowDefinition missing_dep;
        missing_dep.tasks.push_back(WorkflowTaskDefinition{"foo", "Foo", "cpu_burn", {}, 5, {}, 1, {"ghost_job"}});
        std::vector<std::string> topo;
        std::string err;
        assert(!DagEngine::validate_dag(missing_dep, topo, &err));
        assert(err.find("unknown prerequisite dependency") != std::string::npos);
    }

    std::cout << "[PASS] DAG cycle detection with 3-color DFS path tracing and diagnostics\n";
}

void test_linear_dag_execution_with_scheduler() {
    using namespace astra;

    const std::string test_db = "test_dag_linear.db";
    std::filesystem::remove(test_db);

    AstraConfig config;
    config.max_workers = 2;
    config.max_cpu_slots = 4;
    config.workload_root = "./build/workloads";
    config.database_path = test_db;

    JobStore store;
    std::string err;
    assert(store.open(test_db, &err));

    ResourceManager rm(config.max_cpu_slots, 4096);
    WorkerManager wm(config.max_workers);
    assert(wm.init(config.max_workers));

    Scheduler scheduler(store, wm, rm, config);
    scheduler.set_policy(create_scheduling_policy("fcfs"));

    std::string json_str = R"({
        "id": "wf_linear_01",
        "name": "linear_workflow",
        "jobs": [
            { "key": "step1", "workload": "cpu_burn", "args": ["1"], "depends_on": [] },
            { "key": "step2", "workload": "cpu_burn", "args": ["1"], "depends_on": ["step1"] }
        ]
    })";

    auto wf_opt = DagEngine::parse_json_string(json_str, &err);
    assert(wf_opt.has_value());

    std::unordered_map<std::string, std::string> key_map;
    auto jobs = DagEngine::instantiate_jobs(*wf_opt, config.workload_root, key_map, &err);
    assert(jobs.size() == 2);

    WorkflowRecord rec;
    rec.id = wf_opt->id;
    rec.name = wf_opt->name;
    rec.state = "Running";
    assert(store.save_workflow(rec));

    for (const auto& j : jobs) {
        assert(store.save_job(j));
    }

    // Initial check: step1 is Queued, step2 is WaitingForDependencies
    auto j1 = store.get_job(key_map["step1"]);
    auto j2 = store.get_job(key_map["step2"]);
    assert(j1 && j1->state == JobState::Queued);
    assert(j2 && j2->state == JobState::WaitingForDependencies);

    // Run scheduler event loop until completion
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        scheduler.step();

        auto cur1 = store.get_job(key_map["step1"]);
        auto cur2 = store.get_job(key_map["step2"]);
        if (cur1 && cur1->state == JobState::Succeeded &&
            cur2 && cur2->state == JobState::Succeeded) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto final1 = store.get_job(key_map["step1"]);
    auto final2 = store.get_job(key_map["step2"]);
    assert(final1 && final1->state == JobState::Succeeded);
    assert(final2 && final2->state == JobState::Succeeded);

    // Verify ordering: step1 started before step2
    assert(final1->started_time <= final2->started_time);
    assert(final1->completed_time <= final2->started_time);

    store.close();
    std::filesystem::remove(test_db);

    std::cout << "[PASS] End-to-end linear DAG execution and automatic prerequisite unblocking\n";
}

void test_diamond_dag_execution() {
    using namespace astra;

    const std::string test_db = "test_dag_diamond.db";
    std::filesystem::remove(test_db);

    AstraConfig config;
    config.max_workers = 4;
    config.max_cpu_slots = 6;
    config.workload_root = "./build/workloads";
    config.database_path = test_db;

    JobStore store;
    std::string err;
    assert(store.open(test_db, &err));

    ResourceManager rm(config.max_cpu_slots, 4096);
    WorkerManager wm(config.max_workers);
    assert(wm.init(config.max_workers));

    Scheduler scheduler(store, wm, rm, config);
    scheduler.set_policy(create_scheduling_policy("fcfs"));

    std::string json_str = R"({
        "id": "wf_diamond_01",
        "name": "diamond_workflow",
        "jobs": [
            { "key": "root", "workload": "cpu_burn", "args": ["1"], "depends_on": [] },
            { "key": "branch_a", "workload": "cpu_burn", "args": ["1"], "depends_on": ["root"] },
            { "key": "branch_b", "workload": "cpu_burn", "args": ["1"], "depends_on": ["root"] },
            { "key": "join", "workload": "cpu_burn", "args": ["1"], "depends_on": ["branch_a", "branch_b"] }
        ]
    })";

    auto wf_opt = DagEngine::parse_json_string(json_str, &err);
    assert(wf_opt.has_value());

    std::unordered_map<std::string, std::string> key_map;
    auto jobs = DagEngine::instantiate_jobs(*wf_opt, config.workload_root, key_map, &err);
    assert(jobs.size() == 4);

    for (const auto& j : jobs) {
        assert(store.save_job(j));
    }

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(12)) {
        scheduler.step();

        auto j_join = store.get_job(key_map["join"]);
        if (j_join && j_join->state == JobState::Succeeded) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto f_root = store.get_job(key_map["root"]);
    auto f_a = store.get_job(key_map["branch_a"]);
    auto f_b = store.get_job(key_map["branch_b"]);
    auto f_join = store.get_job(key_map["join"]);

    assert(f_root && f_root->state == JobState::Succeeded);
    assert(f_a && f_a->state == JobState::Succeeded);
    assert(f_b && f_b->state == JobState::Succeeded);
    assert(f_join && f_join->state == JobState::Succeeded);

    // Verify join started only after both branch_a and branch_b completed
    assert(f_join->started_time >= f_a->completed_time);
    assert(f_join->started_time >= f_b->completed_time);

    store.close();
    std::filesystem::remove(test_db);

    std::cout << "[PASS] Diamond DAG multi-branch join coordination\n";
}

void test_dag_failure_cascade_and_blocking() {
    using namespace astra;

    const std::string test_db = "test_dag_failure.db";
    std::filesystem::remove(test_db);

    AstraConfig config;
    config.max_workers = 2;
    config.max_cpu_slots = 4;
    config.workload_root = "./build/workloads";
    config.database_path = test_db;

    JobStore store;
    std::string err;
    assert(store.open(test_db, &err));

    ResourceManager rm(config.max_cpu_slots, 4096);
    WorkerManager wm(config.max_workers);
    assert(wm.init(config.max_workers));

    Scheduler scheduler(store, wm, rm, config);
    scheduler.set_policy(create_scheduling_policy("fcfs"));

    // step1 fails deliberately, step2 depends on step1, step3 depends on step2
    std::string json_str = R"({
        "id": "wf_fail_cascade",
        "name": "failure_cascade_workflow",
        "jobs": [
            { "key": "step1", "workload": "deliberate_failure", "args": ["1"], "max_retries": 0, "depends_on": [] },
            { "key": "step2", "workload": "cpu_burn", "args": ["1"], "depends_on": ["step1"] },
            { "key": "step3", "workload": "cpu_burn", "args": ["1"], "depends_on": ["step2"] }
        ]
    })";

    auto wf_opt = DagEngine::parse_json_string(json_str, &err);
    assert(wf_opt.has_value());

    std::unordered_map<std::string, std::string> key_map;
    auto jobs = DagEngine::instantiate_jobs(*wf_opt, config.workload_root, key_map, &err);
    assert(jobs.size() == 3);

    for (const auto& j : jobs) {
        assert(store.save_job(j));
    }

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8)) {
        scheduler.step();

        auto j1 = store.get_job(key_map["step1"]);
        auto j2 = store.get_job(key_map["step2"]);
        auto j3 = store.get_job(key_map["step3"]);

        if (j1 && j1->state == JobState::Failed &&
            j2 && j2->state == JobState::Blocked &&
            j3 && j3->state == JobState::Blocked) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto f1 = store.get_job(key_map["step1"]);
    auto f2 = store.get_job(key_map["step2"]);
    auto f3 = store.get_job(key_map["step3"]);

    assert(f1 && f1->state == JobState::Failed);
    assert(f2 && f2->state == JobState::Blocked);
    assert(f2->failure_reason.find("Prerequisite dependency") != std::string::npos);

    assert(f3 && f3->state == JobState::Blocked);
    assert(f3->failure_reason.find("Prerequisite dependency") != std::string::npos);

    store.close();
    std::filesystem::remove(test_db);

    std::cout << "[PASS] Downstream failure cascade and cascade-blocking propagation\n";
}

int main() {
    std::cout << "Running ASTRA Phase 6 (DAG Workflows) Unit & Integration Tests...\n";
    test_dag_validation_and_topological_sort();
    test_cycle_detection_and_error_diagnostics();
    test_linear_dag_execution_with_scheduler();
    test_diamond_dag_execution();
    test_dag_failure_cascade_and_blocking();
    std::cout << "All Phase 6 tests passed successfully!\n";
    return 0;
}
