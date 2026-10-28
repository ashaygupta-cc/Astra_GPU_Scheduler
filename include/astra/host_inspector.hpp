#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <sys/types.h>

namespace astra {

struct HostProcessInfo {
    pid_t pid = 0;
    pid_t ppid = 0;
    std::string user = "system";
    char state = 'S';
    long num_threads = 1;
    uint64_t cpu_time_ms = 0;
    uint64_t elapsed_sec = 0;
    uint64_t rss_kb = 0;
    uint64_t vsz_kb = 0;
    uint64_t voluntary_ctxt_switches = 0;
    uint64_t nonvoluntary_ctxt_switches = 0; // Preemptive interrupts
    std::string wchan = "-";
    long priority = 0;
    long nice_val = 0;
    uint64_t utime_ms = 0;
    uint64_t stime_ms = 0;
    uint64_t minflt = 0;
    uint64_t majflt = 0;
    std::string comm;
    std::string cmdline;
};

struct HostProcessStats {
    size_t total_processes = 0;
    size_t running = 0;
    size_t sleeping = 0;
    size_t disk_io = 0;
    size_t zombie = 0;
    size_t stopped = 0;
    uint64_t total_rss_mb = 0;
};

class HostInspector {
public:
    // Read and parse all processes currently alive in /proc
    static std::vector<HostProcessInfo> scan_processes(size_t limit = 100);

    // Deep inspection of a single process by PID
    static std::optional<HostProcessInfo> inspect_process(pid_t pid);

    // Human-readable process state description
    static std::string state_description(char state);

    // Compute aggregate host process statistics
    static HostProcessStats compute_stats(const std::vector<HostProcessInfo>& procs);

    // Format rich table output for CLI / inspection
    static std::string render_host_table(const std::vector<HostProcessInfo>& procs, size_t limit = 30);
};

} // namespace astra
