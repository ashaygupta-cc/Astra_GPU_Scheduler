#include "astra/version.hpp"
#include "astra/config.hpp"
#include "astra/job_store.hpp"
#include "astra/command_parser.hpp"
#include "astra/cli_handler.hpp"

#include <iostream>
#include <filesystem>

int main(int argc, char* argv[]) {
    // 1. Load configuration (fallback to defaults if file missing)
    astra::AstraConfig config;
    std::string config_path = std::filesystem::exists("configs/astra.json") ? 
        "configs/astra.json" : "../configs/astra.json";

    if (std::filesystem::exists(config_path)) {
        try {
            config = astra::AstraConfig::load_from_file(config_path);
        } catch (const std::exception& ex) {
            std::cerr << "[WARN] Failed to load config from " << config_path 
                      << ": " << ex.what() << ". Using defaults.\n";
        }
    }

    // 2. Open persistent JobStore
    astra::JobStore store;
    std::string db_err;
    if (!store.open(config.database_path, &db_err)) {
        std::cerr << "[ERROR] Failed to open SQLite job store: " << db_err << "\n";
        return 1;
    }

    // 3. Initialize CLI Dispatcher
    astra::CliHandler handler(store, config);

    // 4. Execution mode: One-shot CLI command vs Interactive REPL
    if (argc > 1) {
        astra::Command cmd = astra::CommandParser::parse_argc_argv(argc, argv, config.workload_root);
        return handler.execute(cmd, std::cout, std::cerr);
    }

    // No arguments provided: launch interactive REPL
    return handler.run_repl(std::cin, std::cout, std::cerr);
}
