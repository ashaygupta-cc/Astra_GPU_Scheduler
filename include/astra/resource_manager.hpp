#pragma once

#include "astra/job.hpp"
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>

namespace astra {

struct ResourceSnapshot {
    unsigned int total_cpu_slots = 4;
    unsigned int reserved_cpu_slots = 0;
    uint64_t total_ram_mb = 4096;
    uint64_t reserved_ram_mb = 0;
    uint64_t measured_total_ram_mb = 0;
    uint64_t measured_available_ram_mb = 0;
    bool gpu_available = false;
    bool gpu_reserved = false;
    uint64_t total_gpu_memory_mb = 0;
    uint64_t reserved_gpu_memory_mb = 0;
    std::string gpu_name = "N/A";
    std::string telemetry_source_label = "simulated";
    std::chrono::system_clock::time_point last_updated = std::chrono::system_clock::now();
};

class ResourceManager {
public:
    explicit ResourceManager(unsigned int total_cpu_slots = 4, uint64_t total_ram_mb = 4096);
    ~ResourceManager() = default;

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    void refresh_hardware_telemetry();

    bool can_admit(const ResourceRequest& req, std::string* reason_out = nullptr) const;

    bool reserve(const std::string& job_id, const ResourceRequest& req, std::string* error_out = nullptr);

    bool release(const std::string& job_id, std::string* error_out = nullptr);

    bool has_reservation(const std::string& job_id) const;

    ResourceSnapshot get_snapshot() const;

    unsigned int total_cpu_slots() const { return total_cpu_slots_; }
    unsigned int reserved_cpu_slots() const { return reserved_cpu_slots_; }
    unsigned int available_cpu_slots() const { 
        return (total_cpu_slots_ > reserved_cpu_slots_) ? (total_cpu_slots_ - reserved_cpu_slots_) : 0; 
    }
    std::string gpu_device_name() const { 
        std::lock_guard<std::mutex> lock(mtx_); 
        return gpu_device_name_; 
    }

private:
    mutable std::mutex mtx_;
    unsigned int total_cpu_slots_;
    unsigned int reserved_cpu_slots_ = 0;
    uint64_t total_ram_mb_;
    uint64_t reserved_ram_mb_ = 0;

    bool gpu_available_ = false;
    bool gpu_reserved_ = false;
    uint64_t total_gpu_memory_mb_ = 0;
    uint64_t reserved_gpu_memory_mb_ = 0;
    std::string gpu_device_name_ = "N/A";

    uint64_t measured_total_ram_mb_ = 0;
    uint64_t measured_available_ram_mb_ = 0;
    std::string telemetry_source_ = "uninitialized";

    std::unordered_map<std::string, ResourceRequest> active_reservations_;
};

} // namespace astra
