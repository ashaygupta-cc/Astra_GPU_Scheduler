#include "astra/checkpoint.hpp"
#include "astra/resource_manager.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <array>
#include <cstdio>
#include <memory>

std::pair<int, std::string> execute_command(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return {-1, "popen failed"};
    }
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    int rc = ::pclose(pipe);
    int exit_code = WEXITSTATUS(rc);
    return {exit_code, result};
}

void test_atomic_checkpoint_lifecycle() {
    using namespace astra;

    const std::string test_ckpt = "test_atomic.ckpt";
    CheckpointManager::remove(test_ckpt);

    // 1. Save checkpoint
    CheckpointData cp;
    cp.job_id = "test_job_1";
    cp.current_step = 2500;
    cp.total_steps = 10000;
    cp.running_value = 3126250;
    cp.custom_payload = "{\"tensor_shape\": [64, 128]}";

    std::string err;
    assert(CheckpointManager::save_checkpoint(test_ckpt, cp, &err));
    assert(CheckpointManager::exists(test_ckpt));

    // Verify .tmp was cleaned up by atomic rename
    assert(!std::filesystem::exists(test_ckpt + ".tmp"));

    // 2. Load checkpoint
    auto loaded = CheckpointManager::load_checkpoint(test_ckpt, &err);
    assert(loaded.has_value());
    assert(loaded->job_id == "test_job_1");
    assert(loaded->current_step == 2500);
    assert(loaded->total_steps == 10000);
    assert(loaded->running_value == 3126250);
    assert(loaded->custom_payload == "{\"tensor_shape\": [64, 128]}");

    // 3. Clean up
    assert(CheckpointManager::remove(test_ckpt));
    assert(!CheckpointManager::exists(test_ckpt));

    std::cout << "[PASS] Atomic checkpoint serialization, fsync, and atomic POSIX rename\n";
}

void test_checkpointable_workload_simulation_and_recovery() {
    using namespace astra;

    const std::string ckpt_file = "test_sum_recovery.json";
    CheckpointManager::remove(ckpt_file);

    std::string binary = std::filesystem::exists("build/workloads/checkpointable_sum") ?
        "build/workloads/checkpointable_sum" : "./workloads/checkpointable_sum";

    // Step 1: Run with failure simulated at step 50,000 out of 100,000
    std::string cmd1 = binary + " --target 100000 --fail-at 50000 --checkpoint-file " + ckpt_file;
    auto [exit1, out1] = execute_command(cmd1);

    // Expect exit code 42
    assert(exit1 == 42);
    assert(out1.find("SIMULATED CRASH triggered at step 50000") != std::string::npos);
    assert(CheckpointManager::exists(ckpt_file));

    // Verify checkpoint content
    auto saved_state = CheckpointManager::load_checkpoint(ckpt_file);
    assert(saved_state.has_value());
    assert(saved_state->current_step == 50000);
    assert(saved_state->running_value == (50000ULL * 50001ULL) / 2); // 1250025000

    // Step 2: Resume from checkpoint
    std::string cmd2 = binary + " --target 100000 --resume --checkpoint-file " + ckpt_file;
    auto [exit2, out2] = execute_command(cmd2);

    assert(exit2 == 0);
    assert(out2.find("RESUMING from checkpoint at step 50000") != std::string::npos);
    assert(out2.find("final_sum=5000050000") != std::string::npos); // 100000 * 100001 / 2

    // Checkpoint cleaned up after successful completion
    assert(!CheckpointManager::exists(ckpt_file));

    std::cout << "[PASS] Checkpointable workload failure simulation and state recovery\n";
}

void test_gpu_awareness_and_admission_telemetry() {
    using namespace astra;

    ResourceManager rm(4, 8192);
    auto snap = rm.get_snapshot();

    // Check GPU detection logic
    std::cout << "  Telemetry Source: " << snap.telemetry_source_label << "\n";
    std::cout << "  NVIDIA GPU Detected: " << (snap.gpu_available ? "YES (Hardware)" : "Simulated/Absent") << "\n";
    if (snap.gpu_available) {
        std::cout << "  Total GPU VRAM: " << snap.total_gpu_memory_mb << " MB\n";
    }

    ResourceRequest gpu_req;
    gpu_req.device = DeviceRequirement::GpuRequired;
    gpu_req.cpu_slots = 1;
    gpu_req.ram_mb = 1024;
    gpu_req.gpu_memory_mb = 512;

    std::string reason;
    if (snap.gpu_available) {
        assert(rm.can_admit(gpu_req, &reason));
        assert(rm.reserve("gpu-job-1", gpu_req));
        assert(rm.get_snapshot().gpu_reserved);

        // Second GPU request cannot be admitted simultaneously (capacity 1)
        ResourceRequest second_gpu = gpu_req;
        assert(!rm.can_admit(second_gpu, &reason));

        // Release first job
        assert(rm.release("gpu-job-1"));
        assert(!rm.get_snapshot().gpu_reserved);
        assert(rm.can_admit(second_gpu));
    } else {
        assert(!rm.can_admit(gpu_req, &reason));
        assert(reason.find("GPU") != std::string::npos);
    }

    std::cout << "[PASS] GPU hardware telemetry awareness and admission control\n";
}

int main() {
    std::cout << "Running ASTRA Phase 8 (GPU & Checkpointing) Unit & Integration Tests...\n";
    test_atomic_checkpoint_lifecycle();
    test_checkpointable_workload_simulation_and_recovery();
    test_gpu_awareness_and_admission_telemetry();
    std::cout << "All Phase 8 tests passed successfully!\n";
    return 0;
}
