#include <iostream>
#include <chrono>
#include <thread>
#include <string>

int main(int argc, char* argv[]) {
    int duration_seconds = 1;
    if (argc > 1) {
        try {
            duration_seconds = std::stoi(argv[1]);
        } catch (...) {
            duration_seconds = 1;
        }
    }

    std::cout << "[WORKLOAD:cpu_burn] Starting CPU burn for " << duration_seconds << "s...\n";
    auto start = std::chrono::steady_clock::now();

    uint64_t operations = 0;
    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed >= duration_seconds) {
            break;
        }

        // Do compute work
        for (int i = 0; i < 50000; ++i) {
            operations += (i * 31) ^ (operations >> 3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "[WORKLOAD:cpu_burn] Completed successfully. Total ops: " << operations << "\n";
    return 0;
}
