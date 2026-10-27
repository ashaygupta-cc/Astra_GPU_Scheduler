#pragma once

#include "astra/job_store.hpp"
#include "astra/worker_manager.hpp"
#include "astra/resource_manager.hpp"
#include "astra/scheduling_policy.hpp"
#include "astra/config.hpp"

#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

namespace astra {

class Scheduler {
public:
    Scheduler(JobStore& store, 
              WorkerManager& worker_manager, 
              ResourceManager& resource_manager, 
              const AstraConfig& config);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void set_policy(std::unique_ptr<SchedulingPolicy> policy);
    const SchedulingPolicy& policy() const { return *policy_; }

    void start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Executes a single discrete scheduling iteration
    bool step();

private:
    JobStore& store_;
    WorkerManager& worker_manager_;
    ResourceManager& resource_manager_;
    AstraConfig config_;
    std::unique_ptr<SchedulingPolicy> policy_;

    std::atomic<bool> running_{false};
    std::thread loop_thread_;
    std::mutex step_mtx_;

    void setup_callbacks();
    void reconcile_dependencies();
};

} // namespace astra
