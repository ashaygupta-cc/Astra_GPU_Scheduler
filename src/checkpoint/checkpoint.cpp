#include "astra/checkpoint.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>

namespace astra {

void to_json(nlohmann::json& j, const CheckpointData& cp) {
    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        cp.timestamp.time_since_epoch()).count();
    j = nlohmann::json{
        {"job_id", cp.job_id},
        {"current_step", cp.current_step},
        {"total_steps", cp.total_steps},
        {"running_value", cp.running_value},
        {"custom_payload", cp.custom_payload},
        {"timestamp_ms", ts_ms}
    };
}

void from_json(const nlohmann::json& j, CheckpointData& cp) {
    if (j.contains("job_id") && j["job_id"].is_string()) cp.job_id = j["job_id"].get<std::string>();
    if (j.contains("current_step") && j["current_step"].is_number()) cp.current_step = j["current_step"].get<uint64_t>();
    if (j.contains("total_steps") && j["total_steps"].is_number()) cp.total_steps = j["total_steps"].get<uint64_t>();
    if (j.contains("running_value") && j["running_value"].is_number()) cp.running_value = j["running_value"].get<uint64_t>();
    if (j.contains("custom_payload") && j["custom_payload"].is_string()) cp.custom_payload = j["custom_payload"].get<std::string>();
    if (j.contains("timestamp_ms") && j["timestamp_ms"].is_number()) {
        int64_t ms = j["timestamp_ms"].get<int64_t>();
        cp.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
    }
}

bool CheckpointManager::save_atomic(const std::string& path, const std::string& raw_content, std::string* error_out) {
    std::string tmp_path = path + ".tmp";

    // 1. Open temporary file with O_CREAT | O_WRONLY | O_TRUNC
    int fd = ::open(tmp_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        if (error_out) *error_out = "Failed to create temporary checkpoint file: " + std::string(strerror(errno));
        return false;
    }

    // 2. Write content
    size_t written = 0;
    while (written < raw_content.size()) {
        ssize_t res = ::write(fd, raw_content.data() + written, raw_content.size() - written);
        if (res < 0) {
            if (errno == EINTR) continue;
            if (error_out) *error_out = "Write failed during checkpoint creation: " + std::string(strerror(errno));
            ::close(fd);
            ::unlink(tmp_path.c_str());
            return false;
        }
        written += static_cast<size_t>(res);
    }

    // 3. Force sync to disk
    if (::fsync(fd) != 0) {
        if (error_out) *error_out = "fsync failed: " + std::string(strerror(errno));
        ::close(fd);
        ::unlink(tmp_path.c_str());
        return false;
    }

    ::close(fd);

    // 4. Atomic POSIX rename
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        if (error_out) *error_out = "Atomic rename failed: " + ec.message();
        ::unlink(tmp_path.c_str());
        return false;
    }

    return true;
}

bool CheckpointManager::save_checkpoint(const std::string& path, const CheckpointData& data, std::string* error_out) {
    nlohmann::json j = data;
    return save_atomic(path, j.dump(2), error_out);
}

std::optional<std::string> CheckpointManager::load_raw(const std::string& path, std::string* error_out) {
    if (!std::filesystem::exists(path)) {
        if (error_out) *error_out = "Checkpoint file does not exist: " + path;
        return std::nullopt;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        if (error_out) *error_out = "Failed to open checkpoint file: " + path;
        return std::nullopt;
    }

    std::stringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

std::optional<CheckpointData> CheckpointManager::load_checkpoint(const std::string& path, std::string* error_out) {
    auto raw = load_raw(path, error_out);
    if (!raw) return std::nullopt;

    auto parsed = nlohmann::json::parse(*raw, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error_out) *error_out = "Invalid checkpoint JSON structure in " + path;
        return std::nullopt;
    }

    try {
        CheckpointData cp = parsed.get<CheckpointData>();
        return cp;
    } catch (const std::exception& e) {
        if (error_out) *error_out = "Deserialization error: " + std::string(e.what());
        return std::nullopt;
    }
}

bool CheckpointManager::exists(const std::string& path) {
    return std::filesystem::exists(path);
}

bool CheckpointManager::remove(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".tmp", ec);
    return !ec;
}

} // namespace astra
