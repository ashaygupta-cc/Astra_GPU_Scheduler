#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace astra {

struct CheckpointData {
    std::string job_id;
    uint64_t current_step = 0;
    uint64_t total_steps = 0;
    uint64_t running_value = 0;
    std::string custom_payload;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

void to_json(nlohmann::json& j, const CheckpointData& cp);
void from_json(const nlohmann::json& j, CheckpointData& cp);

class CheckpointManager {
public:
    // Atomic checkpoint writer: writes to path.tmp, fsyncs, then atomic POSIX rename
    static bool save_atomic(const std::string& path, const std::string& raw_content, std::string* error_out = nullptr);
    static bool save_checkpoint(const std::string& path, const CheckpointData& data, std::string* error_out = nullptr);

    // Reads and validates checkpoint
    static std::optional<std::string> load_raw(const std::string& path, std::string* error_out = nullptr);
    static std::optional<CheckpointData> load_checkpoint(const std::string& path, std::string* error_out = nullptr);

    // Checks existence
    static bool exists(const std::string& path);

    // Removes checkpoint file and any leftover .tmp
    static bool remove(const std::string& path);
};

} // namespace astra
