#include "astra/checkpoint.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <cstdint>

int main(int argc, char* argv[]) {
    uint64_t target = 100000;
    std::string checkpoint_file = "sum_checkpoint.json";
    uint64_t checkpoint_freq = 10000;
    uint64_t fail_at = 0;
    bool resume = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--target" && i + 1 < argc) {
            target = std::stoull(argv[++i]);
        } else if (arg == "--checkpoint-file" && i + 1 < argc) {
            checkpoint_file = argv[++i];
        } else if (arg == "--checkpoint-freq" && i + 1 < argc) {
            checkpoint_freq = std::stoull(argv[++i]);
        } else if (arg == "--fail-at" && i + 1 < argc) {
            fail_at = std::stoull(argv[++i]);
        } else if (arg == "--resume") {
            resume = true;
        }
    }

    uint64_t current_step = 1;
    uint64_t running_sum = 0;

    if (resume && astra::CheckpointManager::exists(checkpoint_file)) {
        auto cp = astra::CheckpointManager::load_checkpoint(checkpoint_file);
        if (cp.has_value()) {
            current_step = cp->current_step + 1;
            running_sum = cp->running_value;
            std::cout << "[WORKLOAD:checkpointable_sum] RESUMING from checkpoint at step " 
                      << cp->current_step << " with running sum " << running_sum << "\n";
        }
    } else {
        std::cout << "[WORKLOAD:checkpointable_sum] STARTING fresh computation to target " << target << "\n";
    }

    for (uint64_t i = current_step; i <= target; ++i) {
        running_sum += i;

        if (fail_at > 0 && i == fail_at) {
            // Save state right before simulated crash
            astra::CheckpointData cp;
            cp.job_id = "checkpointable_sum";
            cp.current_step = i;
            cp.total_steps = target;
            cp.running_value = running_sum;
            astra::CheckpointManager::save_checkpoint(checkpoint_file, cp);

            std::cout << "[WORKLOAD:checkpointable_sum] SIMULATED CRASH triggered at step " << i 
                      << " (checkpoint saved to " << checkpoint_file << ")\n";
            return 42; // Simulated fault exit code
        }

        if (i % checkpoint_freq == 0 || i == target) {
            astra::CheckpointData cp;
            cp.job_id = "checkpointable_sum";
            cp.current_step = i;
            cp.total_steps = target;
            cp.running_value = running_sum;
            astra::CheckpointManager::save_checkpoint(checkpoint_file, cp);
        }
    }

    std::cout << "[WORKLOAD:checkpointable_sum] FINISHED: total_steps=" << target 
              << ", final_sum=" << running_sum << "\n";

    // Clean up checkpoint on normal clean completion
    astra::CheckpointManager::remove(checkpoint_file);
    return 0;
}
