#include "astra/command_parser.hpp"
#include "astra/cli_handler.hpp"
#include "astra/job_store.hpp"
#include "astra/config.hpp"

#include <iostream>
#include <cassert>
#include <sstream>
#include <filesystem>

void test_tokenizer() {
    using namespace astra;

    auto t1 = CommandParser::tokenize("submit ./workloads/cpu_burn --cpu-slots 2");
    assert(t1.size() == 4);
    assert(t1[0] == "submit");
    assert(t1[1] == "./workloads/cpu_burn");
    assert(t1[2] == "--cpu-slots");
    assert(t1[3] == "2");

    auto t2 = CommandParser::tokenize("submit bin --args \"arg with spaces\" 'single quoted'");
    assert(t2.size() == 5);
    assert(t2[2] == "--args");
    assert(t2[3] == "arg with spaces");
    assert(t2[4] == "single quoted");

    auto t3 = CommandParser::tokenize("   ps    --all   ");
    assert(t3.size() == 2);
    assert(t3[0] == "ps");
    assert(t3[1] == "--all");

    std::cout << "[PASS] CommandParser tokenizer\n";
}

void test_path_safety() {
    using namespace astra;

    std::string err;
    assert(CommandParser::is_safe_workload_path("./workloads/cpu_burn", "./workloads", &err));
    assert(CommandParser::is_safe_workload_path("workloads/matrix_mul", "./workloads", &err));

    assert(!CommandParser::is_safe_workload_path("", "./workloads", &err));
    assert(!CommandParser::is_safe_workload_path("./workloads/test; rm -rf /", "./workloads", &err));
    assert(!CommandParser::is_safe_workload_path("../../../bin/sh", "./workloads", &err));
    assert(!CommandParser::is_safe_workload_path("test | grep foo", "./workloads", &err));

    std::cout << "[PASS] CommandParser path safety and allowlist\n";
}

void test_command_parser_variants() {
    using namespace astra;

    // 1. Help
    auto c_help = CommandParser::parse({"help"});
    assert(std::holds_alternative<HelpCommand>(c_help));

    // 2. Submit with options
    auto c_sub = CommandParser::parse({
        "submit", "./workloads/matrix_mul_cpu",
        "--cpu-slots", "4",
        "--ram-mb", "512",
        "--gpu",
        "--priority", "8",
        "--max-retries", "2",
        "--timeout", "60",
        "--depends-on", "job-1,job-2",
        "--checkpointable",
        "--args", "1024", "2048"
    });
    assert(std::holds_alternative<SubmitCommand>(c_sub));
    const auto& sc = std::get<SubmitCommand>(c_sub);
    assert(sc.workload == "./workloads/matrix_mul_cpu");
    assert(sc.cpu_slots == 4);
    assert(sc.ram_mb == 512);
    assert(sc.device == DeviceRequirement::GpuRequired);
    assert(sc.priority == 8);
    assert(sc.max_retries == 2);
    assert(sc.timeout_seconds == 60);
    assert(sc.depends_on.size() == 2);
    assert(sc.depends_on[0] == "job-1");
    assert(sc.depends_on[1] == "job-2");
    assert(sc.checkpointable == true);
    assert(sc.args.size() == 2);
    assert(sc.args[0] == "1024");
    assert(sc.args[1] == "2048");

    // 3. Ps
    auto c_ps = CommandParser::parse({"ps", "--all"});
    assert(std::holds_alternative<PsCommand>(c_ps));
    assert(std::get<PsCommand>(c_ps).all == true);

    // 4. Inspect
    auto c_ins = CommandParser::parse({"inspect", "job-999"});
    assert(std::holds_alternative<InspectCommand>(c_ins));
    assert(std::get<InspectCommand>(c_ins).job_id == "job-999");

    // 5. Cancel
    auto c_can = CommandParser::parse({"cancel", "job-999"});
    assert(std::holds_alternative<CancelCommand>(c_can));
    assert(std::get<CancelCommand>(c_can).job_id == "job-999");

    // 6. Policy
    auto c_pol_set = CommandParser::parse({"policy", "set", "sjf"});
    assert(std::holds_alternative<PolicyCommand>(c_pol_set));
    assert(std::get<PolicyCommand>(c_pol_set).policy_name == "sjf");

    // 7. Invalid commands
    auto c_inv1 = CommandParser::parse({"inspect"}); // Missing job ID
    assert(std::holds_alternative<InvalidCommand>(c_inv1));

    auto c_inv2 = CommandParser::parse({"unknown_subcommand_foo"});
    assert(std::holds_alternative<InvalidCommand>(c_inv2));

    auto c_inv3 = CommandParser::parse({"submit", "./bin", "--cpu-slots", "-5"});
    assert(std::holds_alternative<InvalidCommand>(c_inv3));

    std::cout << "[PASS] CommandParser variant mapping and validation\n";
}

void test_cli_handler_execution_and_repl() {
    using namespace astra;

    const std::string test_db = "test_cli_handler.db";
    std::filesystem::remove(test_db);

    JobStore store;
    std::string err;
    assert(store.open(test_db, &err));

    AstraConfig cfg;
    CliHandler handler(store, cfg);

    std::ostringstream out;
    std::ostringstream err_stream;

    // 1. Submit Job via CLI
    SubmitCommand sub_cmd;
    sub_cmd.workload = "./workloads/cpu_burn";
    sub_cmd.cpu_slots = 2;
    sub_cmd.ram_mb = 256;
    sub_cmd.priority = 7;
    int rc = handler.execute(sub_cmd, out, err_stream);
    assert(rc == 0);
    std::string out_str = out.str();
    assert(out_str.find("[SUBMITTED]") != std::string::npos);

    // Verify job in database
    auto jobs = store.get_all_jobs();
    assert(jobs.size() == 1);
    std::string job_id = jobs[0].id;
    assert(jobs[0].priority == 7);
    assert(jobs[0].state == JobState::Queued);

    // 2. Ps Command
    out.str("");
    rc = handler.execute(PsCommand{true}, out, err_stream);
    assert(rc == 0);
    assert(out.str().find(job_id) != std::string::npos);

    // 3. Inspect Command
    out.str("");
    rc = handler.execute(InspectCommand{job_id}, out, err_stream);
    assert(rc == 0);
    assert(out.str().find("Job Inspection Report:") != std::string::npos);

    // 4. Cancel Command
    out.str("");
    rc = handler.execute(CancelCommand{job_id}, out, err_stream);
    assert(rc == 0);
    assert(out.str().find("[CANCELLED]") != std::string::npos);

    auto cancelled_job = store.get_job(job_id);
    assert(cancelled_job->state == JobState::Cancelled);

    // 5. Policy Command
    out.str("");
    rc = handler.execute(PolicyCommand{PolicyCommand::Action::Set, "priority"}, out, err_stream);
    assert(rc == 0);
    assert(cfg.default_policy == "priority");

    // 6. Interactive REPL Simulation
    std::istringstream repl_input("ps --all\npolicy show\nexit\n");
    out.str("");
    rc = handler.run_repl(repl_input, out, err_stream);
    assert(rc == 0);
    assert(out.str().find("ASTRA Interactive Control REPL") != std::string::npos);
    assert(out.str().find("Exiting ASTRA REPL.") != std::string::npos);

    store.close();
    std::filesystem::remove(test_db);

    std::cout << "[PASS] CliHandler execution and simulated REPL loop\n";
}

int main() {
    std::cout << "Running ASTRA Phase 2 (CLI) Unit Tests...\n";
    test_tokenizer();
    test_path_safety();
    test_command_parser_variants();
    test_cli_handler_execution_and_repl();
    std::cout << "All Phase 2 tests passed successfully!\n";
    return 0;
}
