#include "astra/command_parser.hpp"

#include <sstream>
#include <charconv>
#include <algorithm>
#include <filesystem>

namespace astra {

namespace {

std::optional<int> parse_int(std::string_view s) {
    int val = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec == std::errc() && ptr == s.data() + s.size()) {
        return val;
    }
    return std::nullopt;
}

std::optional<uint64_t> parse_uint64(std::string_view s) {
    uint64_t val = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec == std::errc() && ptr == s.data() + s.size()) {
        return val;
    }
    return std::nullopt;
}

std::vector<std::string> split_csv(std::string_view s) {
    std::vector<std::string> items;
    size_t start = 0;
    while (start < s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string_view::npos) {
            std::string item(s.substr(start));
            if (!item.empty()) items.push_back(std::move(item));
            break;
        }
        std::string item(s.substr(start, comma - start));
        if (!item.empty()) items.push_back(std::move(item));
        start = comma + 1;
    }
    return items;
}

} // namespace

std::vector<std::string> CommandParser::tokenize(std::string_view input) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_double_quote = false;
    bool in_single_quote = false;
    bool escaped = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (escaped) {
            current.push_back(c);
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }

        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }

        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }

        if ((c == ' ' || c == '\t' || c == '\r' || c == '\n') && !in_double_quote && !in_single_quote) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(c);
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}

bool CommandParser::is_safe_workload_path(std::string_view path, 
                                          const std::string& /*workload_root*/, 
                                          std::string* error_out) {
    if (path.empty()) {
        if (error_out) *error_out = "Workload path cannot be empty";
        return false;
    }

    // Disallow dangerous shell metacharacters
    const std::string_view dangerous = ";|&><$`\\!\n\r\t";
    if (path.find_first_of(dangerous) != std::string_view::npos) {
        if (error_out) *error_out = "Workload path contains prohibited shell metacharacters";
        return false;
    }

    // Disallow directory traversal
    if (path.find("..") != std::string_view::npos) {
        if (error_out) *error_out = "Directory traversal ('..') is not permitted in workload path";
        return false;
    }

    return true;
}

Command CommandParser::parse_argc_argv(int argc, char* argv[], const std::string& workload_root) {
    std::vector<std::string> tokens;
    // Skip binary name (argv[0]) if present
    for (int i = 1; i < argc; ++i) {
        tokens.emplace_back(argv[i]);
    }
    return parse(tokens, workload_root);
}

Command CommandParser::parse(const std::vector<std::string>& raw_tokens, const std::string& workload_root) {
    if (raw_tokens.empty()) {
        return HelpCommand{};
    }

    // Strip leading "astra" token if user typed "astra submit ..." in REPL
    size_t start_idx = 0;
    if (raw_tokens[0] == "astra") {
        start_idx = 1;
        if (start_idx >= raw_tokens.size()) {
            return HelpCommand{};
        }
    }

    const std::string& subcmd = raw_tokens[start_idx];

    if (subcmd == "help" || subcmd == "--help" || subcmd == "-h") {
        HelpCommand cmd;
        if (start_idx + 1 < raw_tokens.size()) {
            cmd.topic = raw_tokens[start_idx + 1];
        }
        return cmd;
    }

    if (subcmd == "version" || subcmd == "--version" || subcmd == "-v") {
        return VersionCommand{};
    }

    if (subcmd == "ps") {
        PsCommand cmd;
        for (size_t i = start_idx + 1; i < raw_tokens.size(); ++i) {
            if (raw_tokens[i] == "--all" || raw_tokens[i] == "-a") {
                cmd.all = true;
            } else {
                return InvalidCommand{"Unknown option for 'ps': " + raw_tokens[i]};
            }
        }
        return cmd;
    }

    if (subcmd == "inspect") {
        if (start_idx + 1 >= raw_tokens.size()) {
            return InvalidCommand{"Usage: astra inspect <job-id>"};
        }
        return InspectCommand{raw_tokens[start_idx + 1]};
    }

    if (subcmd == "cancel") {
        if (start_idx + 1 >= raw_tokens.size()) {
            return InvalidCommand{"Usage: astra cancel <job-id>"};
        }
        return CancelCommand{raw_tokens[start_idx + 1]};
    }

    if (subcmd == "logs") {
        if (start_idx + 1 >= raw_tokens.size()) {
            return InvalidCommand{"Usage: astra logs <job-id> [-f|--follow]"};
        }
        LogsCommand cmd{raw_tokens[start_idx + 1], false};
        for (size_t i = start_idx + 2; i < raw_tokens.size(); ++i) {
            if (raw_tokens[i] == "-f" || raw_tokens[i] == "--follow") {
                cmd.follow = true;
            } else {
                return InvalidCommand{"Unknown option for 'logs': " + raw_tokens[i]};
            }
        }
        return cmd;
    }

    if (subcmd == "workers") {
        return WorkersCommand{};
    }

    if (subcmd == "top") {
        return TopCommand{};
    }

    if (subcmd == "demo") {
        return DemoCommand{};
    }

    if (subcmd == "deadlock") {
        std::string action = "demo";
        if (start_idx + 1 < raw_tokens.size()) {
            action = raw_tokens[start_idx + 1];
        }
        return DeadlockCommand{action};
    }

    if (subcmd == "replay") {
        if (start_idx + 1 >= raw_tokens.size()) {
            return InvalidCommand{"Usage: astra replay <job-id|file.jsonl>"};
        }
        return ReplayCommand{raw_tokens[start_idx + 1]};
    }

    if (subcmd == "bench" || subcmd == "benchmark") {
        BenchmarkCommand cmd;
        for (size_t i = start_idx + 1; i < raw_tokens.size(); ++i) {
            if (raw_tokens[i] == "--jobs" && i + 1 < raw_tokens.size()) {
                cmd.jobs = static_cast<size_t>(std::stoul(raw_tokens[++i]));
            } else if (raw_tokens[i] == "--slots" && i + 1 < raw_tokens.size()) {
                cmd.slots = std::stoi(raw_tokens[++i]);
            }
        }
        return cmd;
    }

    if (subcmd == "shutdown") {
        ShutdownCommand cmd;
        for (size_t i = start_idx + 1; i < raw_tokens.size(); ++i) {
            if (raw_tokens[i] == "--immediate" || raw_tokens[i] == "-i") {
                cmd.immediate = true;
            } else {
                return InvalidCommand{"Unknown option for 'shutdown': " + raw_tokens[i]};
            }
        }
        return cmd;
    }

    if (subcmd == "config") {
        if (start_idx + 1 >= raw_tokens.size() || raw_tokens[start_idx + 1] == "show") {
            return ConfigCommand{ConfigCommand::Action::Show};
        }
        return InvalidCommand{"Usage: astra config show"};
    }

    if (subcmd == "policy") {
        if (start_idx + 1 >= raw_tokens.size()) {
            return InvalidCommand{"Usage: astra policy <show|set <fcfs|priority|sjf>>"};
        }
        const std::string& act = raw_tokens[start_idx + 1];
        if (act == "show") {
            return PolicyCommand{PolicyCommand::Action::Show, ""};
        }
        if (act == "set") {
            if (start_idx + 2 >= raw_tokens.size()) {
                return InvalidCommand{"Usage: astra policy set <fcfs|priority|priority_aging|sjf>"};
            }
            std::string pol = raw_tokens[start_idx + 2];
            if (pol != "fcfs" && pol != "priority" && pol != "priority_aging" && pol != "sjf") {
                return InvalidCommand{"Invalid scheduling policy: " + pol + " (expected: fcfs, priority, priority_aging, sjf)"};
            }
            return PolicyCommand{PolicyCommand::Action::Set, pol};
        }
        return InvalidCommand{"Unknown policy action: " + act + " (expected 'show' or 'set')"};
    }

    if (subcmd == "workflow") {
        if (start_idx + 1 >= raw_tokens.size()) {
            return InvalidCommand{"Usage: astra workflow <submit <file.json>|status <workflow_id>|list>"};
        }
        const std::string& act = raw_tokens[start_idx + 1];
        if (act == "submit") {
            if (start_idx + 2 >= raw_tokens.size()) {
                return InvalidCommand{"Missing workflow JSON file. Usage: astra workflow submit <file.json>"};
            }
            return WorkflowCommand{WorkflowCommand::Action::Submit, raw_tokens[start_idx + 2]};
        } else if (act == "status") {
            if (start_idx + 2 >= raw_tokens.size()) {
                return InvalidCommand{"Missing workflow ID. Usage: astra workflow status <workflow_id>"};
            }
            return WorkflowCommand{WorkflowCommand::Action::Status, raw_tokens[start_idx + 2]};
        } else if (act == "list") {
            return WorkflowCommand{WorkflowCommand::Action::List, ""};
        } else {
            return InvalidCommand{"Unknown workflow action: " + act + " (expected 'submit', 'status', or 'list')"};
        }
    }

    if (subcmd == "events") {
        EventsCommand cmd;
        for (size_t i = start_idx + 1; i < raw_tokens.size(); ++i) {
            if (raw_tokens[i] == "--job" && i + 1 < raw_tokens.size()) {
                cmd.job_id = raw_tokens[++i];
            } else if (raw_tokens[i] == "--limit" && i + 1 < raw_tokens.size()) {
                auto l = parse_int(raw_tokens[++i]);
                if (l && *l > 0) cmd.limit = static_cast<size_t>(*l);
            } else {
                return InvalidCommand{"Unknown option for 'events': " + raw_tokens[i]};
            }
        }
        return cmd;
    }

    if (subcmd == "submit") {
        if (start_idx + 1 >= raw_tokens.size()) {
            return InvalidCommand{"Usage: astra submit <workload> [options]"};
        }

        SubmitCommand cmd;
        cmd.workload = raw_tokens[start_idx + 1];

        std::string path_err;
        if (!is_safe_workload_path(cmd.workload, workload_root, &path_err)) {
            return InvalidCommand{"Invalid workload: " + path_err};
        }

        for (size_t i = start_idx + 2; i < raw_tokens.size(); ++i) {
            const std::string& opt = raw_tokens[i];
            if (opt == "--cpu-slots") {
                if (i + 1 >= raw_tokens.size()) return InvalidCommand{"Missing value for --cpu-slots"};
                auto val = parse_int(raw_tokens[++i]);
                if (!val || *val <= 0) return InvalidCommand{"--cpu-slots must be a positive integer"};
                cmd.cpu_slots = static_cast<unsigned int>(*val);
            } else if (opt == "--ram-mb") {
                if (i + 1 >= raw_tokens.size()) return InvalidCommand{"Missing value for --ram-mb"};
                auto val = parse_uint64(raw_tokens[++i]);
                if (!val || *val == 0) return InvalidCommand{"--ram-mb must be a positive integer"};
                cmd.ram_mb = *val;
            } else if (opt == "--gpu") {
                cmd.device = DeviceRequirement::GpuRequired;
            } else if (opt == "--gpu-preferred") {
                cmd.device = DeviceRequirement::GpuPreferred;
            } else if (opt == "--gpu-memory-mb") {
                if (i + 1 >= raw_tokens.size()) return InvalidCommand{"Missing value for --gpu-memory-mb"};
                auto val = parse_uint64(raw_tokens[++i]);
                if (!val) return InvalidCommand{"--gpu-memory-mb must be a valid integer"};
                cmd.gpu_memory_mb = *val;
            } else if (opt == "--priority") {
                if (i + 1 >= raw_tokens.size()) return InvalidCommand{"Missing value for --priority"};
                auto val = parse_int(raw_tokens[++i]);
                if (!val || *val < 0) return InvalidCommand{"--priority must be a non-negative integer"};
                cmd.priority = *val;
            } else if (opt == "--max-retries") {
                if (i + 1 >= raw_tokens.size()) return InvalidCommand{"Missing value for --max-retries"};
                auto val = parse_int(raw_tokens[++i]);
                if (!val || *val < 0) return InvalidCommand{"--max-retries must be a non-negative integer"};
                cmd.max_retries = *val;
            } else if (opt == "--timeout") {
                if (i + 1 >= raw_tokens.size()) return InvalidCommand{"Missing value for --timeout"};
                auto val = parse_int(raw_tokens[++i]);
                if (!val || *val <= 0) return InvalidCommand{"--timeout must be a positive integer (seconds)"};
                cmd.timeout_seconds = *val;
            } else if (opt == "--depends-on") {
                if (i + 1 >= raw_tokens.size()) return InvalidCommand{"Missing value for --depends-on"};
                cmd.depends_on = split_csv(raw_tokens[++i]);
            } else if (opt == "--checkpointable") {
                cmd.checkpointable = true;
            } else if (opt == "--args") {
                // Collect remainder of tokens as workload arguments
                for (size_t a = i + 1; a < raw_tokens.size(); ++a) {
                    cmd.args.push_back(raw_tokens[a]);
                }
                break;
            } else {
                // Treat positional parameter after workload as workload arg
                cmd.args.push_back(opt);
            }
        }

        return cmd;
    }

    return InvalidCommand{"Unknown command: '" + subcmd + "'. Type 'astra help' for available commands."};
}

} // namespace astra
