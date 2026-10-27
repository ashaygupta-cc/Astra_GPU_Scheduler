#include <iostream>
#include <string>
#include <cstdlib>

int main(int argc, char* argv[]) {
    int exit_code = 42;
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--crash") {
            std::cerr << "[WORKLOAD:deliberate_failure] Inducing deliberate abort crash!\n";
            std::abort();
        }
        try {
            exit_code = std::stoi(arg);
        } catch (...) {
            exit_code = 42;
        }
    }

    std::cerr << "[WORKLOAD:deliberate_failure] Simulated controlled failure with exit code " 
              << exit_code << "\n";
    return exit_code;
}
