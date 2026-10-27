#include "astra/resource_manager.hpp"

#include <fstream>
#include <sstream>
#include <unistd.h>
#include <iostream>

namespace astra {

ResourceManager::ResourceManager(unsigned int total_cpu_slots, uint64_t total_ram_mb)
    : total_cpu_slots_(total_cpu_slots), total_ram_mb_(total_ram_mb) {
    refresh_hardware_telemetry();
}

void ResourceManager::refresh_hardware_telemetry() {
    std::lock_guard<std::mutex> lock(mtx_);

    // 1. Read host system memory from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        uint64_t total_kb = 0;
        uint64_t avail_kb = 0;

        while (std::getline(meminfo, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::istringstream ss(line.substr(9));
                ss >> total_kb;
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                std::istringstream ss(line.substr(13));
                ss >> avail_kb;
            }
        }

        if (total_kb > 0) {
            measured_total_ram_mb_ = total_kb / 1024;
            measured_available_ram_mb_ = avail_kb / 1024;
        }
    }

    // 2. Dynamically check and query GPU Hardware interface across diverse Linux/WSL distros
    std::string smi_path;
    if (access("/usr/bin/nvidia-smi", X_OK) == 0) {
        smi_path = "/usr/bin/nvidia-smi";
    } else if (access("/usr/lib/wsl/lib/nvidia-smi", X_OK) == 0) {
        smi_path = "/usr/lib/wsl/lib/nvidia-smi";
    } else if (access("/usr/local/cuda/bin/nvidia-smi", X_OK) == 0) {
        smi_path = "/usr/local/cuda/bin/nvidia-smi";
    } else {
        FILE* p = popen("command -v nvidia-smi 2>/dev/null", "r");
        if (p) {
            char pbuf[256];
            if (fgets(pbuf, sizeof(pbuf), p) != nullptr) {
                std::string res(pbuf);
                while (!res.empty() && (res.back() == '\n' || res.back() == '\r' || res.back() == ' ')) {
                    res.pop_back();
                }
                if (!res.empty() && access(res.c_str(), X_OK) == 0) {
                    smi_path = res;
                }
            }
            pclose(p);
        }
    }

    gpu_available_ = false;
    total_gpu_memory_mb_ = 0;
    gpu_device_name_ = "Host Emulated Device (CPU-Only)";

    if (!smi_path.empty()) {
        std::string cmd = smi_path + " --query-gpu=name,memory.total --format=csv,noheader,nounits 2>/dev/null";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            char buffer[256];
            if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                std::string line(buffer);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                    line.pop_back();
                }
                auto comma = line.find(',');
                if (comma != std::string::npos) {
                    gpu_device_name_ = line.substr(0, comma);
                    while (!gpu_device_name_.empty() && gpu_device_name_.back() == ' ') {
                        gpu_device_name_.pop_back();
                    }
                    try {
                        total_gpu_memory_mb_ = std::stoull(line.substr(comma + 1));
                    } catch (...) {
                        total_gpu_memory_mb_ = 4096;
                    }
                } else {
                    gpu_device_name_ = line;
                    total_gpu_memory_mb_ = 4096;
                }
                gpu_available_ = true;
                telemetry_source_ = "hardware (" + gpu_device_name_ + " & /proc/meminfo)";
            }
            pclose(pipe);
        }
    }

    if (!gpu_available_) {
        telemetry_source_ = "hardware (/proc/meminfo; CPU-Only)";
    }
}

bool ResourceManager::can_admit(const ResourceRequest& req, std::string* reason_out) const {
    std::lock_guard<std::mutex> lock(mtx_);

    // Check CPU slots limit
    if (reserved_cpu_slots_ + req.cpu_slots > total_cpu_slots_) {
        if (reason_out) {
            *reason_out = "CPU slot limit reached (requested: " + std::to_string(req.cpu_slots) + 
                          ", available: " + std::to_string(total_cpu_slots_ - reserved_cpu_slots_) + 
                          "/" + std::to_string(total_cpu_slots_) + ")";
        }
        return false;
    }

    // Check RAM limit
    if (reserved_ram_mb_ + req.ram_mb > total_ram_mb_) {
        if (reason_out) {
            *reason_out = "RAM capacity exceeded (requested: " + std::to_string(req.ram_mb) + 
                          " MB, available: " + std::to_string(total_ram_mb_ - reserved_ram_mb_) + " MB)";
        }
        return false;
    }

    // Check GPU device requirements
    if (req.device == DeviceRequirement::GpuRequired) {
        if (!gpu_available_) {
            if (reason_out) {
                *reason_out = "GPU hardware required, but no physical GPU interface is available";
            }
            return false;
        }
        if (gpu_reserved_) {
            if (reason_out) {
                *reason_out = "GPU hardware is currently reserved by another workload";
            }
            return false;
        }
    }

    return true;
}

bool ResourceManager::reserve(const std::string& job_id, const ResourceRequest& req, std::string* error_out) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (active_reservations_.find(job_id) != active_reservations_.end()) {
        if (error_out) *error_out = "Job '" + job_id + "' already holds an active resource reservation";
        return false;
    }

    if (reserved_cpu_slots_ + req.cpu_slots > total_cpu_slots_) {
        if (error_out) *error_out = "Cannot reserve: CPU slot overcommitment";
        return false;
    }

    reserved_cpu_slots_ += req.cpu_slots;
    reserved_ram_mb_ += req.ram_mb;

    if (req.device == DeviceRequirement::GpuRequired || 
       (req.device == DeviceRequirement::GpuPreferred && gpu_available_ && !gpu_reserved_)) {
        gpu_reserved_ = true;
        reserved_gpu_memory_mb_ += req.gpu_memory_mb;
    }

    active_reservations_[job_id] = req;
    return true;
}

bool ResourceManager::release(const std::string& job_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = active_reservations_.find(job_id);
    if (it == active_reservations_.end()) {
        if (error_out) *error_out = "Cannot release: No active resource reservation found for '" + job_id + "'";
        return false;
    }

    const ResourceRequest& req = it->second;

    if (reserved_cpu_slots_ >= req.cpu_slots) {
        reserved_cpu_slots_ -= req.cpu_slots;
    } else {
        reserved_cpu_slots_ = 0;
    }

    if (reserved_ram_mb_ >= req.ram_mb) {
        reserved_ram_mb_ -= req.ram_mb;
    } else {
        reserved_ram_mb_ = 0;
    }

    if (req.device == DeviceRequirement::GpuRequired || req.device == DeviceRequirement::GpuPreferred) {
        if (gpu_reserved_) {
            gpu_reserved_ = false;
            if (reserved_gpu_memory_mb_ >= req.gpu_memory_mb) {
                reserved_gpu_memory_mb_ -= req.gpu_memory_mb;
            } else {
                reserved_gpu_memory_mb_ = 0;
            }
        }
    }

    active_reservations_.erase(it);
    return true;
}

bool ResourceManager::has_reservation(const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return active_reservations_.find(job_id) != active_reservations_.end();
}

ResourceSnapshot ResourceManager::get_snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    ResourceSnapshot snap;
    snap.total_cpu_slots = total_cpu_slots_;
    snap.reserved_cpu_slots = reserved_cpu_slots_;
    snap.total_ram_mb = total_ram_mb_;
    snap.reserved_ram_mb = reserved_ram_mb_;
    snap.measured_total_ram_mb = measured_total_ram_mb_;
    snap.measured_available_ram_mb = measured_available_ram_mb_;
    snap.gpu_available = gpu_available_;
    snap.gpu_reserved = gpu_reserved_;
    snap.total_gpu_memory_mb = total_gpu_memory_mb_;
    snap.reserved_gpu_memory_mb = reserved_gpu_memory_mb_;
    snap.gpu_name = gpu_device_name_;
    snap.telemetry_source_label = telemetry_source_;
    snap.last_updated = std::chrono::system_clock::now();
    return snap;
}

} // namespace astra
