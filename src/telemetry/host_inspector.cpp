#include "astra/host_inspector.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>

namespace astra {

namespace {

std::string format_cpu_time(uint64_t ms) {
    uint64_t total_sec = ms / 1000;
    uint64_t m = total_sec / 60;
    uint64_t s = total_sec % 60;
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(2) << m << ":"
       << std::setfill('0') << std::setw(2) << s;
    return ss.str();
}

std::string get_username(uid_t uid) {
    struct passwd* pw = getpwuid(uid);
    if (pw && pw->pw_name) {
        return std::string(pw->pw_name);
    }
    return std::to_string(uid);
}

} // namespace

std::vector<HostProcessInfo> HostInspector::scan_processes(size_t limit) {
    std::vector<HostProcessInfo> procs;
    const std::filesystem::path proc_dir{"/proc"};

    if (!std::filesystem::exists(proc_dir) || !std::filesystem::is_directory(proc_dir)) {
        return procs;
    }

    long page_kb = sysconf(_SC_PAGESIZE);
    if (page_kb <= 0) page_kb = 4096;
    page_kb /= 1024;

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    double sys_uptime = 0.0;
    std::ifstream up_file("/proc/uptime");
    if (up_file.is_open()) {
        up_file >> sys_uptime;
    }

    for (const auto& entry : std::filesystem::directory_iterator(proc_dir)) {
        if (!entry.is_directory()) continue;
        std::string filename = entry.path().filename().string();
        if (!std::all_of(filename.begin(), filename.end(), ::isdigit)) continue;

        HostProcessInfo info;
        info.pid = static_cast<pid_t>(std::stol(filename));

        // 1. Get user name via file owner stat
        struct stat st;
        if (stat(entry.path().c_str(), &st) == 0) {
            info.user = get_username(st.st_uid);
        }

        // 2. Read /proc/<pid>/stat
        std::string stat_path = (entry.path() / "stat").string();
        std::ifstream stat_file(stat_path);
        if (stat_file.is_open()) {
            std::string line;
            if (std::getline(stat_file, line)) {
                auto lparen = line.find('(');
                auto rparen = line.rfind(')');
                if (lparen != std::string::npos && rparen != std::string::npos && rparen > lparen) {
                    info.comm = line.substr(lparen + 1, rparen - lparen - 1);
                    std::string rest = line.substr(rparen + 2);
                    std::istringstream iss(rest);

                    char state = 'S';
                    pid_t ppid = 0;
                    int pgrp = 0, session = 0, tty_nr = 0, tpgid = 0;
                    unsigned int flags = 0;
                    unsigned long minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0;
                    unsigned long utime = 0, stime = 0;
                    long cutime = 0, cstime = 0, priority = 0, nice_val = 0, threads = 1;
                    long itrealvalue = 0;
                    unsigned long long starttime = 0;
                    unsigned long vsize = 0;
                    long rss = 0;

                    if (iss >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid >> flags
                            >> minflt >> cminflt >> majflt >> cmajflt
                            >> utime >> stime >> cutime >> cstime
                            >> priority >> nice_val >> threads >> itrealvalue
                            >> starttime >> vsize >> rss) {
                        info.state = state;
                        info.ppid = ppid;
                        info.num_threads = threads;
                        info.priority = priority;
                        info.nice_val = nice_val;
                        info.minflt = minflt;
                        info.majflt = majflt;
                        info.vsz_kb = vsize / 1024;
                        info.rss_kb = (rss > 0) ? (static_cast<uint64_t>(rss) * static_cast<uint64_t>(page_kb)) : 0;
                        info.utime_ms = (utime * 1000) / static_cast<uint64_t>(clk_tck);
                        info.stime_ms = (stime * 1000) / static_cast<uint64_t>(clk_tck);
                        info.cpu_time_ms = ((utime + stime) * 1000) / static_cast<uint64_t>(clk_tck);
                        double start_sec = static_cast<double>(starttime) / static_cast<double>(clk_tck);
                        if (sys_uptime > start_sec) {
                            info.elapsed_sec = static_cast<uint64_t>(sys_uptime - start_sec);
                        }
                    }
                }
            }
        }

        // 3. Read /proc/<pid>/cmdline
        std::string cmd_path = (entry.path() / "cmdline").string();
        std::ifstream cmd_file(cmd_path);
        if (cmd_file.is_open()) {
            std::string buf((std::istreambuf_iterator<char>(cmd_file)), std::istreambuf_iterator<char>());
            for (char& c : buf) {
                if (c == '\0') c = ' ';
            }
            while (!buf.empty() && (buf.back() == ' ' || buf.back() == '\n' || buf.back() == '\r')) {
                buf.pop_back();
            }
            info.cmdline = buf;
        }

        if (info.cmdline.empty()) {
            info.cmdline = "[" + info.comm + "]";
        }

        procs.push_back(std::move(info));
    }

    // Sort: Running ('R') first, then Newest / Most Recent first (smallest elapsed_sec, then highest PID)
    // This ensures any newly created process (e.g. nano, python, worker tasks) appears immediately at the top!
    std::sort(procs.begin(), procs.end(), [](const HostProcessInfo& a, const HostProcessInfo& b) {
        if ((a.state == 'R') != (b.state == 'R')) {
            return a.state == 'R';
        }
        if (a.elapsed_sec != b.elapsed_sec) {
            return a.elapsed_sec < b.elapsed_sec;
        }
        return a.pid > b.pid;
    });

    if (procs.size() > limit && limit > 0) {
        procs.resize(limit);
    }

    return procs;
}

HostProcessStats HostInspector::compute_stats(const std::vector<HostProcessInfo>& procs) {
    HostProcessStats stats;
    stats.total_processes = procs.size();

    for (const auto& p : procs) {
        if (p.state == 'R') stats.running++;
        else if (p.state == 'S' || p.state == 'I') stats.sleeping++;
        else if (p.state == 'D') stats.disk_io++;
        else if (p.state == 'Z') stats.zombie++;
        else if (p.state == 'T') stats.stopped++;
        else stats.sleeping++;

        stats.total_rss_mb += (p.rss_kb / 1024);
    }

    return stats;
}

std::string HostInspector::render_host_table(const std::vector<HostProcessInfo>& procs, size_t limit) {
    if (procs.empty()) {
        return "No host processes detected. (Host /proc filesystem may not be accessible in this sandbox).\n";
    }

    auto stats = compute_stats(procs);
    size_t display_count = (limit > 0 && limit < procs.size()) ? limit : procs.size();

    std::ostringstream ss;
    ss << "========================================================================================================\n"
       << "                          HOST OS LINUX KERNEL PROCESS TABLE (/proc INSPECTOR)                          \n"
       << "========================================================================================================\n"
       << std::left
       << std::setw(8)  << "PID"
       << std::setw(8)  << "PPID"
       << std::setw(12) << "USER"
       << std::setw(6)  << "STAT"
       << std::setw(6)  << "THRD"
       << std::setw(11) << "CPU TIME"
       << std::setw(11) << "ELAPSED"
       << std::setw(12) << "RSS MEM"
       << "COMMAND / PROCESS NAME\n";
    ss << std::string(104, '-') << "\n";

    for (size_t i = 0; i < display_count; ++i) {
        const auto& p = procs[i];
        double mem_mb = static_cast<double>(p.rss_kb) / 1024.0;
        std::ostringstream mem_ss;
        mem_ss << std::fixed << std::setprecision(1) << mem_mb << " MB";

        std::string cmd = p.cmdline;
        if (cmd.size() > 36) {
            cmd = cmd.substr(0, 33) + "...";
        }

        ss << std::left
           << std::setw(8)  << p.pid
           << std::setw(8)  << p.ppid
           << std::setw(12) << (p.user.size() > 11 ? p.user.substr(0, 11) : p.user)
           << std::setw(6)  << p.state
           << std::setw(6)  << p.num_threads
           << std::setw(11) << format_cpu_time(p.cpu_time_ms)
           << std::setw(11) << format_cpu_time(p.elapsed_sec * 1000)
           << std::setw(12) << mem_ss.str()
           << cmd << "\n";
    }

    ss << std::string(104, '-') << "\n"
       << "Host Telemetry: " << stats.total_processes << " Total Host Processes ("
       << stats.running << " Running, " << stats.sleeping << " Sleeping, "
       << stats.disk_io << " Disk/IO Wait, " << stats.zombie << " Zombie) | RSS Active: "
       << stats.total_rss_mb << " MB\n"
       << "Kernel Subsystem: Linux CFS (Completely Fair Scheduler) & /proc Virtual Filesystem Active\n"
       << "========================================================================================================\n";

    return ss.str();
}

std::string HostInspector::state_description(char state) {
    switch (state) {
        case 'R': return "Running (Active on CPU / Ready on Runqueue)";
        case 'S': return "Sleeping (Interruptible Wait / Waiting for I/O or event)";
        case 'D': return "Disk Sleep (Uninterruptible Hardware/Disk I/O Wait)";
        case 'Z': return "Zombie (Terminated, pending parent reap)";
        case 'T': return "Stopped (SIGSTOP / SIGTSTP / Trace Interrupted)";
        case 't': return "Tracing Stop (Debugger attached)";
        case 'X': return "Dead (Completely terminated)";
        case 'I': return "Idle (Kernel background worker thread)";
        default:  return std::string("State '") + state + "'";
    }
}

std::optional<HostProcessInfo> HostInspector::inspect_process(pid_t pid) {
    std::filesystem::path proc_dir{"/proc/" + std::to_string(pid)};
    if (!std::filesystem::exists(proc_dir) || !std::filesystem::is_directory(proc_dir)) {
        return std::nullopt;
    }

    HostProcessInfo info;
    info.pid = pid;

    long page_kb = sysconf(_SC_PAGESIZE);
    if (page_kb <= 0) page_kb = 4096;
    page_kb /= 1024;

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    double sys_uptime = 0.0;
    std::ifstream up_file("/proc/uptime");
    if (up_file.is_open()) {
        up_file >> sys_uptime;
    }

    // 1. User from inode owner
    struct stat st;
    if (stat(proc_dir.c_str(), &st) == 0) {
        info.user = get_username(st.st_uid);
    }

    // 2. Read /proc/<pid>/stat
    std::string stat_path = (proc_dir / "stat").string();
    std::ifstream stat_file(stat_path);
    if (stat_file.is_open()) {
        std::string line;
        if (std::getline(stat_file, line)) {
            auto lparen = line.find('(');
            auto rparen = line.rfind(')');
            if (lparen != std::string::npos && rparen != std::string::npos && rparen > lparen) {
                info.comm = line.substr(lparen + 1, rparen - lparen - 1);
                std::string rest = line.substr(rparen + 2);
                std::istringstream iss(rest);

                char state = 'S';
                pid_t ppid = 0;
                int pgrp = 0, session = 0, tty_nr = 0, tpgid = 0;
                unsigned int flags = 0;
                unsigned long minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0;
                unsigned long utime = 0, stime = 0;
                long cutime = 0, cstime = 0, priority = 0, nice_val = 0, threads = 1;
                long itrealvalue = 0;
                unsigned long long starttime = 0;
                unsigned long vsize = 0;
                long rss = 0;

                if (iss >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid >> flags
                        >> minflt >> cminflt >> majflt >> cmajflt
                        >> utime >> stime >> cutime >> cstime
                        >> priority >> nice_val >> threads >> itrealvalue
                        >> starttime >> vsize >> rss) {
                    info.state = state;
                    info.ppid = ppid;
                    info.num_threads = threads;
                    info.priority = priority;
                    info.nice_val = nice_val;
                    info.minflt = minflt;
                    info.majflt = majflt;
                    info.vsz_kb = vsize / 1024;
                    info.rss_kb = (rss > 0) ? (static_cast<uint64_t>(rss) * static_cast<uint64_t>(page_kb)) : 0;
                    info.utime_ms = (utime * 1000) / static_cast<uint64_t>(clk_tck);
                    info.stime_ms = (stime * 1000) / static_cast<uint64_t>(clk_tck);
                    info.cpu_time_ms = ((utime + stime) * 1000) / static_cast<uint64_t>(clk_tck);
                    double start_sec = static_cast<double>(starttime) / static_cast<double>(clk_tck);
                    if (sys_uptime > start_sec) {
                        info.elapsed_sec = static_cast<uint64_t>(sys_uptime - start_sec);
                    }
                }
            }
        }
    }

    // 3. Read /proc/<pid>/cmdline
    std::string cmd_path = (proc_dir / "cmdline").string();
    std::ifstream cmd_file(cmd_path);
    if (cmd_file.is_open()) {
        std::string buf((std::istreambuf_iterator<char>(cmd_file)), std::istreambuf_iterator<char>());
        for (char& c : buf) {
            if (c == '\0') c = ' ';
        }
        while (!buf.empty() && (buf.back() == ' ' || buf.back() == '\n' || buf.back() == '\r')) {
            buf.pop_back();
        }
        info.cmdline = buf;
    }
    if (info.cmdline.empty()) {
        info.cmdline = "[" + info.comm + "]";
    }

    // 4. Read /proc/<pid>/status for context switches
    std::string status_path = (proc_dir / "status").string();
    std::ifstream status_file(status_path);
    if (status_file.is_open()) {
        std::string sline;
        while (std::getline(status_file, sline)) {
            if (sline.rfind("voluntary_ctxt_switches:", 0) == 0) {
                try {
                    info.voluntary_ctxt_switches = std::stoull(sline.substr(24));
                } catch (...) {}
            } else if (sline.rfind("nonvoluntary_ctxt_switches:", 0) == 0) {
                try {
                    info.nonvoluntary_ctxt_switches = std::stoull(sline.substr(27));
                } catch (...) {}
            }
        }
    }

    // 5. Read /proc/<pid>/wchan for kernel wait channel
    std::string wchan_path = (proc_dir / "wchan").string();
    std::ifstream wchan_file(wchan_path);
    if (wchan_file.is_open()) {
        std::string wch;
        if (std::getline(wchan_file, wch)) {
            while (!wch.empty() && (wch.back() == '\n' || wch.back() == '\r' || wch.back() == ' ')) {
                wch.pop_back();
            }
            if (!wch.empty() && wch != "0") {
                info.wchan = wch;
            } else if (info.state == 'R') {
                info.wchan = "0 (CPU Active Running)";
            }
        }
    }

    return info;
}

} // namespace astra
