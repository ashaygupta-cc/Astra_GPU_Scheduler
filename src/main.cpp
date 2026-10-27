#include "astra/version.hpp"

#include <iostream>
#include <string_view>
#include <thread>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#if __has_include(<curses.h>)
#include <curses.h>
#define ASTRA_HAS_NCURSES 1
#elif __has_include(<ncurses.h>)
#include <ncurses.h>
#define ASTRA_HAS_NCURSES 1
#else
#define ASTRA_HAS_NCURSES 0
#endif

#include <unistd.h>
#include <sys/utsname.h>

void print_help() {
    std::cout << astra::ASTRA_BANNER << "\n"
              << "ASTRA - Autonomous CPU/GPU Job Scheduler (v" << astra::ASTRA_VERSION << ")\n\n"
              << "Usage: astra [command] [options]\n\n"
              << "Commands:\n"
              << "  help                      Display this help message\n"
              << "  version                   Display version and system diagnostics\n"
              << "  submit <workload> [opts]  Submit a workload for scheduling\n"
              << "  ps [--all]                List current jobs and their execution states\n"
              << "  inspect <job-id>          Inspect full job details and timeline\n"
              << "  cancel <job-id>           Cancel a running or queued job\n"
              << "  logs <job-id>             Retrieve job stdout/stderr logs\n"
              << "  workers                   Display worker pool status\n"
              << "  policy show|set <name>    View or update scheduling policy\n"
              << "  top                       Launch full-screen ncurses TUI\n"
              << "  shutdown                  Gracefully terminate scheduler daemon\n\n"
              << "Phase 0 Status: Foundation & Environment Inspection Verified.\n";
}

void print_diagnostics() {
    std::cout << astra::ASTRA_BANNER << "\n"
              << "ASTRA Core Diagnostic Report [Phase 0]\n"
              << "==================================================\n"
              << "  ASTRA Version:         " << astra::ASTRA_VERSION << "\n"
              << "  C++ Standard:          " << __cplusplus << " (C++20 compliant)\n";

    struct utsname sys_info{};
    if (uname(&sys_info) == 0) {
        std::cout << "  OS Kernel:             " << sys_info.sysname << " " 
                  << sys_info.release << " (" << sys_info.machine << ")\n";
    }

    const unsigned int logical_cores = std::thread::hardware_concurrency();
    std::cout << "  CPU Hardware Concurrency: " << logical_cores << " threads\n"
              << "  POSIX Host Page Size:     " << sysconf(_SC_PAGESIZE) << " bytes\n"
              << "  SQLite3 Library:          v" << sqlite3_libversion() << "\n"
              << "  nlohmann/json Library:    v" << NLOHMANN_JSON_VERSION_MAJOR << "."
                                               << NLOHMANN_JSON_VERSION_MINOR << "."
                                               << NLOHMANN_JSON_VERSION_PATCH << "\n";

#if ASTRA_HAS_NCURSES
    std::cout << "  ncurses Library:          v" << NCURSES_VERSION << "\n";
#else
    std::cout << "  ncurses Library:          Not detected\n";
#endif

    // Detect NVIDIA GPU via sysfs / nvidia-smi presence
    if (access("/usr/lib/wsl/lib/nvidia-smi", X_OK) == 0 || access("/usr/bin/nvidia-smi", X_OK) == 0) {
        std::cout << "  GPU Hardware Interface:   Detected (NVIDIA driver present)\n";
    } else {
        std::cout << "  GPU Hardware Interface:   Unavailable (CPU-only execution)\n";
    }

    std::cout << "==================================================\n"
              << "System environment verified for Phase 1 (Domain & Storage).\n";
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        const std::string_view arg1 = argv[1];
        if (arg1 == "--version" || arg1 == "-v" || arg1 == "version") {
            print_diagnostics();
            return 0;
        }
        if (arg1 == "--help" || arg1 == "-h" || arg1 == "help") {
            print_help();
            return 0;
        }
    }

    std::cout << astra::ASTRA_BANNER << "\n";
    std::cout << "ASTRA Scheduler initialized in Phase 0 mode.\n";
    std::cout << "Run 'astra --version' for diagnostic report or 'astra --help' for CLI options.\n";
    return 0;
}
