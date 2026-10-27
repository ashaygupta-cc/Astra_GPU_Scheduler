#pragma once

#include "astra/job.hpp"
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <cstdint>

namespace astra {

struct HelpCommand {
    std::string topic;
};

struct VersionCommand {};

struct SubmitCommand {
    std::string workload;
    std::vector<std::string> args;
    unsigned int cpu_slots = 1;
    uint64_t ram_mb = 128;
    uint64_t gpu_memory_mb = 0;
    DeviceRequirement device = DeviceRequirement::CpuOnly;
    int priority = 5;
    int max_retries = 1;
    int timeout_seconds = 0;
    std::vector<std::string> depends_on;
    bool checkpointable = false;
};

struct PsCommand {
    bool all = false;
};

struct InspectCommand {
    std::string job_id;
};

struct CancelCommand {
    std::string job_id;
};

struct LogsCommand {
    std::string job_id;
    bool follow = false;
};

struct WorkersCommand {};

struct PolicyCommand {
    enum class Action { Show, Set };
    Action action = Action::Show;
    std::string policy_name;
};

struct ConfigCommand {
    enum class Action { Show };
    Action action = Action::Show;
};

struct WorkflowCommand {
    std::string file_path;
};

struct EventsCommand {
    std::string job_id;
    size_t limit = 50;
};

struct ReplayCommand {
    std::string run_id;
};

struct DeadlockCommand {
    std::string action = "demo";
};

struct DemoCommand {};

struct TopCommand {};

struct ShutdownCommand {
    bool immediate = false;
};

struct ReplCommand {};

struct InvalidCommand {
    std::string error_message;
};

using Command = std::variant<
    HelpCommand,
    VersionCommand,
    SubmitCommand,
    PsCommand,
    InspectCommand,
    CancelCommand,
    LogsCommand,
    WorkersCommand,
    PolicyCommand,
    ConfigCommand,
    WorkflowCommand,
    EventsCommand,
    ReplayCommand,
    DeadlockCommand,
    DemoCommand,
    TopCommand,
    ShutdownCommand,
    ReplCommand,
    InvalidCommand
>;

} // namespace astra
