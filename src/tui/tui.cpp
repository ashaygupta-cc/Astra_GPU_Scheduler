#include "astra/tui.hpp"
#include "astra/version.hpp"

#if __has_include(<curses.h>)
#include <curses.h>
#elif __has_include(<ncurses.h>)
#include <ncurses.h>
#endif

#include <unistd.h>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace astra {

namespace {

std::string format_time_hms(uint64_t total_sec) {
    uint64_t m = total_sec / 60;
    uint64_t s = total_sec % 60;
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(2) << m << ":"
       << std::setfill('0') << std::setw(2) << s;
    return ss.str();
}

std::string make_progress_bar(unsigned int current, unsigned int total, int width = 10) {
    if (total == 0) return std::string(static_cast<size_t>(width), ' ');
    int filled = static_cast<int>((static_cast<double>(current) / total) * width);
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;
    std::string bar = "[";
    bar.append(static_cast<size_t>(filled), '=');
    bar.append(static_cast<size_t>(width - filled), ' ');
    bar += "]";
    return bar;
}

} // namespace

Tui::Tui() = default;

Tui::~Tui() {
    shutdown();
}

bool Tui::init() {
    if (initialized_) return true;

    // Check if running in a real interactive terminal
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        initialized_ = true;
        return true;
    }

#if defined(NCURSES_VERSION) || defined(CURSES)
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100); // 100ms timeout for 10Hz non-blocking refresh

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_CYAN, COLOR_BLACK);    // Headers & Borders
        init_pair(2, COLOR_GREEN, COLOR_BLACK);   // Succeeded / Running
        init_pair(3, COLOR_YELLOW, COLOR_BLACK);  // Queued / Warning
        init_pair(4, COLOR_RED, COLOR_BLACK);     // Failed / Cancelled
        init_pair(5, COLOR_BLACK, COLOR_CYAN);    // Highlight row
        init_pair(6, COLOR_MAGENTA, COLOR_BLACK); // GPU / Accents
        init_pair(7, COLOR_WHITE, COLOR_BLUE);    // Title bar
    }

    initialized_ = true;
    return true;
#else
    initialized_ = true;
    return true;
#endif
}

void Tui::shutdown() {
    if (!initialized_) return;
#if defined(NCURSES_VERSION) || defined(CURSES)
    if (isatty(STDIN_FILENO)) {
        endwin();
    }
#endif
    initialized_ = false;
}

void Tui::render_header(int /*max_y*/, int max_x, const TuiSnapshot& snap) {
#if defined(NCURSES_VERSION) || defined(CURSES)
    attron(COLOR_PAIR(7) | A_BOLD);
    std::string title = " ASTRA — Autonomous Compute Scheduler (v" + std::string(astra::ASTRA_VERSION) + ") ";
    mvaddstr(0, 0, title.c_str());
    for (int x = static_cast<int>(title.size()); x < max_x; ++x) addch(' ');
    attroff(COLOR_PAIR(7) | A_BOLD);

    attron(COLOR_PAIR(1));
    std::string view_name;
    switch (current_view_) {
        case TuiView::Jobs:       view_name = "JOBS DASHBOARD"; break;
        case TuiView::Workers:    view_name = "WORKER POOL"; break;
        case TuiView::Resources:  view_name = "HARDWARE TELEMETRY"; break;
        case TuiView::Events:     view_name = "EVENT AUDIT LOG"; break;
        case TuiView::JobDetails: view_name = "JOB INSPECTION"; break;
        case TuiView::Logs:       view_name = "JOB OUTPUT LOGS"; break;
    }

    std::ostringstream ss;
    ss << " Policy: " << snap.policy_name
       << " | Workers: " << snap.active_workers << "/" << snap.total_workers
       << " | Queue: " << snap.queued_jobs_count
       << " | Running: " << snap.running_jobs_count
       << " | Uptime: " << format_time_hms(snap.uptime_seconds)
       << " | View: [" << view_name << "]";
    std::string bar = ss.str();
    mvaddstr(1, 0, bar.c_str());
    for (int x = static_cast<int>(bar.size()); x < max_x; ++x) addch(' ');

    mvhline(2, 0, ACS_HLINE, max_x);
    attroff(COLOR_PAIR(1));
#else
    (void)max_x;
    (void)snap;
#endif
}

void Tui::render_jobs_view(int max_y, int max_x, const TuiSnapshot& snap) {
#if defined(NCURSES_VERSION) || defined(CURSES)
    int split_x = std::max(52, max_x - 36);

    // Left Panel: Jobs Table
    attron(COLOR_PAIR(1) | A_BOLD);
    mvaddstr(3, 1, "JOBS QUEUE");
    mvaddstr(4, 1, "ID              NAME            STATE          DEVICE   PRI");
    attroff(COLOR_PAIR(1) | A_BOLD);
    mvhline(5, 1, ACS_HLINE, split_x - 2);

    int row_start = 6;
    int max_rows = max_y - 10;
    int job_count = static_cast<int>(snap.jobs.size());

    if (snap.jobs.empty()) {
        mvaddstr(row_start, 2, "(No active or queued jobs)");
    } else {
        if (selected_row_ >= job_count) selected_row_ = job_count - 1;
        if (selected_row_ < 0) selected_row_ = 0;

        for (int i = 0; i < max_rows && i < job_count; ++i) {
            const auto& j = snap.jobs[static_cast<size_t>(i)];
            bool selected = (i == selected_row_);

            if (selected) attron(COLOR_PAIR(5) | A_BOLD);
            else if (j.state == JobState::Running) attron(COLOR_PAIR(2));
            else if (j.state == JobState::Queued) attron(COLOR_PAIR(3));
            else if (is_terminal_state(j.state)) attron(COLOR_PAIR(4));

            std::string line_id = (j.id.size() > 14) ? j.id.substr(0, 14) : j.id;
            std::string line_name = (j.name.size() > 14) ? j.name.substr(0, 14) : j.name;
            std::string line_state = std::string(to_string(j.state));
            if (line_state.size() > 13) line_state = line_state.substr(0, 13);
            std::string line_dev = (j.resources.device == DeviceRequirement::GpuRequired) ? "GPU" : "CPU";

            char buf[128];
            snprintf(buf, sizeof(buf), "%-15s %-15s %-14s %-8s %-3d",
                     line_id.c_str(), line_name.c_str(), line_state.c_str(), 
                     line_dev.c_str(), j.priority);

            mvaddstr(row_start + i, 1, buf);

            if (selected) attroff(COLOR_PAIR(5) | A_BOLD);
            else if (j.state == JobState::Running) attroff(COLOR_PAIR(2));
            else if (j.state == JobState::Queued) attroff(COLOR_PAIR(3));
            else if (is_terminal_state(j.state)) attroff(COLOR_PAIR(4));
        }
    }

    // Vertical Divider
    for (int y = 3; y < max_y - 4; ++y) {
        mvaddch(y, split_x, ACS_VLINE);
    }

    // Right Panel: Resources Telemetry
    attron(COLOR_PAIR(1) | A_BOLD);
    mvaddstr(3, split_x + 2, "RESOURCES & TELEMETRY");
    attroff(COLOR_PAIR(1) | A_BOLD);
    mvhline(5, split_x + 2, ACS_HLINE, max_x - split_x - 3);

    int ry = 6;
    // CPU Slots Bar
    mvaddstr(ry++, split_x + 2, "Logical CPU Slots:");
    std::string cpu_bar = make_progress_bar(snap.resources.reserved_cpu_slots, snap.resources.total_cpu_slots, 12);
    char cpu_txt[64];
    snprintf(cpu_txt, sizeof(cpu_txt), "%s %u / %u", cpu_bar.c_str(), 
             snap.resources.reserved_cpu_slots, snap.resources.total_cpu_slots);
    attron(COLOR_PAIR(2));
    mvaddstr(ry++, split_x + 2, cpu_txt);
    attroff(COLOR_PAIR(2));

    ry++;
    // RAM Allocation
    mvaddstr(ry++, split_x + 2, "RAM Reservations:");
    std::string ram_bar = make_progress_bar(static_cast<unsigned int>(snap.resources.reserved_ram_mb), 
                                            static_cast<unsigned int>(snap.resources.total_ram_mb), 12);
    char ram_txt[64];
    snprintf(ram_txt, sizeof(ram_txt), "%s %lu / %lu MB", ram_bar.c_str(), 
             snap.resources.reserved_ram_mb, snap.resources.total_ram_mb);
    attron(COLOR_PAIR(3));
    mvaddstr(ry++, split_x + 2, ram_txt);
    attroff(COLOR_PAIR(3));

    ry++;
    // Host Linux Memory
    mvaddstr(ry++, split_x + 2, "Host /proc/meminfo:");
    char host_ram[64];
    snprintf(host_ram, sizeof(host_ram), "Avail: %lu MB / %lu MB", 
             snap.resources.measured_available_ram_mb, snap.resources.measured_total_ram_mb);
    mvaddstr(ry++, split_x + 2, host_ram);

    ry++;
    // GPU Status & Device
    mvaddstr(ry++, split_x + 2, "GPU Hardware:");
    if (snap.resources.gpu_available) {
        attron(COLOR_PAIR(6) | A_BOLD);
        mvaddstr(ry++, split_x + 2, "NVIDIA RTX 3050 (4 GB)");
        attroff(COLOR_PAIR(6) | A_BOLD);
        std::string gpu_st = snap.resources.gpu_reserved ? "Status: RESERVED" : "Status: AVAILABLE";
        mvaddstr(ry++, split_x + 2, gpu_st.c_str());
    } else {
        attron(COLOR_PAIR(4));
        mvaddstr(ry++, split_x + 2, "GPU: Unavailable (CPU Only)");
        attroff(COLOR_PAIR(4));
    }

    ry++;
    mvaddstr(ry++, split_x + 2, "Telemetry Source:");
    std::string src_label = snap.resources.telemetry_source_label;
    if (src_label.size() > 28) src_label = src_label.substr(0, 27) + "…";
    mvaddstr(ry++, split_x + 2, src_label.c_str());

    // Bottom Event Ticker
    int event_y = max_y - 4;
    mvhline(event_y, 0, ACS_HLINE, max_x);
    attron(COLOR_PAIR(1));
    mvaddstr(event_y + 1, 1, "Latest Event: ");
    attroff(COLOR_PAIR(1));

    if (!snap.recent_events.empty()) {
        const auto& ev = snap.recent_events.back();
        std::string ev_msg = "[" + std::string(to_string(ev.type)) + "] Job: " + 
            (ev.job_id.empty() ? "-" : ev.job_id) + " " + ev.payload_json;
        if (ev_msg.size() > static_cast<size_t>(max_x - 18)) {
            ev_msg = ev_msg.substr(0, static_cast<size_t>(max_x - 20)) + "…";
        }
        mvaddstr(event_y + 1, 15, ev_msg.c_str());
    } else {
        mvaddstr(event_y + 1, 15, "(No events recorded yet)");
    }
#else
    (void)max_y;
    (void)max_x;
    (void)snap;
#endif
}

void Tui::render_workers_view(int max_y, int max_x, const TuiSnapshot& snap) {
#if defined(NCURSES_VERSION) || defined(CURSES)
    attron(COLOR_PAIR(1) | A_BOLD);
    mvaddstr(3, 1, "WORKER PROCESS POOL");
    mvaddstr(4, 1, "WORKER ID   PID      STATE       CURRENT JOB        RESTARTS   LAST HEARTBEAT");
    attroff(COLOR_PAIR(1) | A_BOLD);
    mvhline(5, 1, ACS_HLINE, max_x - 2);

    int row = 6;
    for (size_t i = 0; i < snap.workers.size() && row < max_y - 4; ++i, ++row) {
        const auto& w = snap.workers[i];
        if (w.state == WorkerState::Busy) attron(COLOR_PAIR(2));
        else if (w.state == WorkerState::Idle) attron(COLOR_PAIR(3));
        else attron(COLOR_PAIR(4));

        char buf[128];
        snprintf(buf, sizeof(buf), "worker-%-4d %-8d %-11s %-18s %-10d %-20s",
                 w.id, w.pid, to_string(w.state).data(),
                 (w.current_job_id.empty() ? "-" : w.current_job_id.c_str()),
                 w.restart_count, "active");
        mvaddstr(row, 1, buf);

        if (w.state == WorkerState::Busy) attroff(COLOR_PAIR(2));
        else if (w.state == WorkerState::Idle) attroff(COLOR_PAIR(3));
        else attroff(COLOR_PAIR(4));
    }
#else
    (void)max_y;
    (void)max_x;
    (void)snap;
#endif
}

void Tui::render_resources_view(int max_y, int max_x, const TuiSnapshot& snap) {
#if defined(NCURSES_VERSION) || defined(CURSES)
    (void)max_y;
    attron(COLOR_PAIR(1) | A_BOLD);
    mvaddstr(3, 1, "SYSTEM HARDWARE & COMPUTATIONAL CAPACITY");
    attroff(COLOR_PAIR(1) | A_BOLD);
    mvhline(4, 1, ACS_HLINE, max_x - 2);

    int y = 6;
    mvaddstr(y++, 2, ("Logical CPU Core Slots:  " + std::to_string(snap.resources.total_cpu_slots)).c_str());
    mvaddstr(y++, 2, ("Reserved CPU Slots:      " + std::to_string(snap.resources.reserved_cpu_slots)).c_str());
    mvaddstr(y++, 2, ("Available CPU Slots:     " + std::to_string(snap.resources.total_cpu_slots - snap.resources.reserved_cpu_slots)).c_str());
    y++;

    mvaddstr(y++, 2, ("Logical Memory Limit:    " + std::to_string(snap.resources.total_ram_mb) + " MB").c_str());
    mvaddstr(y++, 2, ("Reserved Memory:         " + std::to_string(snap.resources.reserved_ram_mb) + " MB").c_str());
    mvaddstr(y++, 2, ("Host Total Physical RAM: " + std::to_string(snap.resources.measured_total_ram_mb) + " MB").c_str());
    mvaddstr(y++, 2, ("Host Available RAM:      " + std::to_string(snap.resources.measured_available_ram_mb) + " MB").c_str());
    y++;

    mvaddstr(y++, 2, ("GPU Acceleration:        " + std::string(snap.resources.gpu_available ? "Detected (NVIDIA)" : "Disabled (CPU-Only)")).c_str());
    mvaddstr(y++, 2, ("GPU Device Name:         " + std::string(snap.resources.gpu_available ? "NVIDIA GeForce RTX 3050 Laptop GPU" : "N/A")).c_str());
    mvaddstr(y++, 2, ("GPU Total VRAM:          " + std::to_string(snap.resources.total_gpu_memory_mb) + " MB").c_str());
    mvaddstr(y++, 2, ("GPU Reservation State:   " + std::string(snap.resources.gpu_reserved ? "BUSY / RESERVED" : "IDLE / AVAILABLE")).c_str());
    y++;

    mvaddstr(y++, 2, ("Telemetry Source:        " + snap.resources.telemetry_source_label).c_str());
#else
    (void)max_y;
    (void)max_x;
    (void)snap;
#endif
}

void Tui::render_events_view(int max_y, int max_x, const TuiSnapshot& snap) {
#if defined(NCURSES_VERSION) || defined(CURSES)
    attron(COLOR_PAIR(1) | A_BOLD);
    mvaddstr(3, 1, "AUDIT EVENT HISTORY");
    mvaddstr(4, 1, "EVENT#  TYPE                  JOB ID          WORKER   PAYLOAD");
    attroff(COLOR_PAIR(1) | A_BOLD);
    mvhline(5, 1, ACS_HLINE, max_x - 2);

    int row = 6;
    for (size_t i = 0; i < snap.recent_events.size() && row < max_y - 4; ++i, ++row) {
        const auto& ev = snap.recent_events[i];
        char buf[128];
        snprintf(buf, sizeof(buf), "%-7lu %-21s %-15s %-8d %s",
                 ev.id, to_string(ev.type).data(),
                 (ev.job_id.empty() ? "-" : ev.job_id.c_str()),
                 ev.worker_id, ev.payload_json.c_str());
        mvaddstr(row, 1, buf);
    }
#else
    (void)max_y;
    (void)max_x;
    (void)snap;
#endif
}

void Tui::render_job_details_view(int max_y, int max_x, const TuiSnapshot& snap) {
#if defined(NCURSES_VERSION) || defined(CURSES)
    (void)max_y;
    attron(COLOR_PAIR(1) | A_BOLD);
    mvaddstr(3, 1, "JOB INSPECTION DETAILS [Press Esc to return]");
    attroff(COLOR_PAIR(1) | A_BOLD);
    mvhline(4, 1, ACS_HLINE, max_x - 2);

    const Job* found = nullptr;
    for (const auto& j : snap.jobs) {
        if (j.id == inspect_job_id_) {
            found = &j;
            break;
        }
    }

    if (!found) {
        mvaddstr(6, 2, "Job not found or no job selected.");
        return;
    }

    int y = 6;
    mvaddstr(y++, 2, ("Job ID:          " + found->id).c_str());
    mvaddstr(y++, 2, ("Workload Name:   " + found->name).c_str());
    mvaddstr(y++, 2, ("Binary Path:     " + found->executable_path).c_str());
    mvaddstr(y++, 2, ("State:           " + std::string(to_string(found->state))).c_str());
    mvaddstr(y++, 2, ("Base Priority:   " + std::to_string(found->priority)).c_str());
    mvaddstr(y++, 2, ("CPU Slots:       " + std::to_string(found->resources.cpu_slots)).c_str());
    mvaddstr(y++, 2, ("RAM Estimate:    " + std::to_string(found->resources.ram_mb) + " MB").c_str());
    mvaddstr(y++, 2, ("Device Req:      " + std::string(to_string(found->resources.device))).c_str());
    mvaddstr(y++, 2, ("Worker Assigned: " + (found->assigned_worker_id >= 0 ? std::to_string(found->assigned_worker_id) : "-")).c_str());
    mvaddstr(y++, 2, ("Worker PID:      " + (found->worker_pid > 0 ? std::to_string(found->worker_pid) : "-")).c_str());
    mvaddstr(y++, 2, ("Exit Code:       " + std::to_string(found->exit_code)).c_str());
    if (!found->failure_reason.empty()) {
        mvaddstr(y++, 2, ("Diagnostics:     " + found->failure_reason).c_str());
    }
#else
    (void)max_y;
    (void)max_x;
    (void)snap;
#endif
}

void Tui::render_logs_view(int max_y, int max_x, const TuiSnapshot& snap) {
#if defined(NCURSES_VERSION) || defined(CURSES)
    (void)max_y;
    attron(COLOR_PAIR(1) | A_BOLD);
    mvaddstr(3, 1, "JOB STDOUT / STDERR LOG VIEWER [Press Esc to return]");
    attroff(COLOR_PAIR(1) | A_BOLD);
    mvhline(4, 1, ACS_HLINE, max_x - 2);

    const Job* found = nullptr;
    for (const auto& j : snap.jobs) {
        if (j.id == inspect_job_id_) {
            found = &j;
            break;
        }
    }

    if (!found) {
        mvaddstr(6, 2, "No job selected.");
        return;
    }

    mvaddstr(6, 2, ("Output logs for: " + found->id + " (" + found->name + ")").c_str());
    if (!found->failure_reason.empty()) {
        mvaddstr(8, 2, found->failure_reason.c_str());
    } else {
        mvaddstr(8, 2, "(No captured output for this job)");
    }
#else
    (void)max_y;
    (void)max_x;
    (void)snap;
#endif
}

void Tui::render_footer(int max_y, int max_x) {
#if defined(NCURSES_VERSION) || defined(CURSES)
    int y = max_y - 2;
    mvhline(y, 0, ACS_HLINE, max_x);

    attron(COLOR_PAIR(1) | A_BOLD);
    const char* controls = " [Tab] View  [arrows] Nav  [Enter] Inspect  [c] Cancel  [s] Submit  [l] Logs  [q] Quit ";
    mvaddstr(y + 1, 1, controls);
    attroff(COLOR_PAIR(1) | A_BOLD);
#else
    (void)max_y;
    (void)max_x;
#endif
}

void Tui::render_frame(const TuiSnapshot& snap) {
#if defined(NCURSES_VERSION) || defined(CURSES)
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return;
    }

    int max_y = 24, max_x = 80;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    render_header(max_y, max_x, snap);

    switch (current_view_) {
        case TuiView::Jobs:
            render_jobs_view(max_y, max_x, snap);
            break;
        case TuiView::Workers:
            render_workers_view(max_y, max_x, snap);
            break;
        case TuiView::Resources:
            render_resources_view(max_y, max_x, snap);
            break;
        case TuiView::Events:
            render_events_view(max_y, max_x, snap);
            break;
        case TuiView::JobDetails:
            render_job_details_view(max_y, max_x, snap);
            break;
        case TuiView::Logs:
            render_logs_view(max_y, max_x, snap);
            break;
    }

    render_footer(max_y, max_x);
    refresh();
#else
    (void)snap;
#endif
}

int Tui::run(std::function<TuiSnapshot()> snapshot_fn,
             std::function<bool(const std::string& job_id)> cancel_fn,
             std::function<void()> submit_demo_fn) {
    if (!init()) {
        return 1;
    }

#if defined(NCURSES_VERSION) || defined(CURSES)
    if (!isatty(STDIN_FILENO)) {
        return 0; // Clean exit if run in non-interactive environment
    }

    bool running = true;
    while (running) {
        TuiSnapshot snap = snapshot_fn();
        render_frame(snap);

        int ch = getch();
        if (ch == ERR) {
            continue; // 100ms timeout expired, re-render frame
        }

        switch (ch) {
            case 'q':
            case 'Q':
                running = false;
                break;

            case '\t': // Tab cycles views
                if (current_view_ == TuiView::Jobs) current_view_ = TuiView::Workers;
                else if (current_view_ == TuiView::Workers) current_view_ = TuiView::Resources;
                else if (current_view_ == TuiView::Resources) current_view_ = TuiView::Events;
                else current_view_ = TuiView::Jobs;
                break;

            case 27: // Escape key
                current_view_ = TuiView::Jobs;
                break;

            case KEY_UP:
            case 'k':
                if (selected_row_ > 0) --selected_row_;
                break;

            case KEY_DOWN:
            case 'j':
                if (!snap.jobs.empty() && selected_row_ < static_cast<int>(snap.jobs.size()) - 1) {
                    ++selected_row_;
                }
                break;

            case '\n':
            case '\r':
            case KEY_ENTER:
                if (!snap.jobs.empty() && selected_row_ < static_cast<int>(snap.jobs.size())) {
                    inspect_job_id_ = snap.jobs[static_cast<size_t>(selected_row_)].id;
                    current_view_ = TuiView::JobDetails;
                }
                break;

            case 'l':
            case 'L':
                if (!snap.jobs.empty() && selected_row_ < static_cast<int>(snap.jobs.size())) {
                    inspect_job_id_ = snap.jobs[static_cast<size_t>(selected_row_)].id;
                    current_view_ = TuiView::Logs;
                }
                break;

            case 'c':
            case 'C':
                if (!snap.jobs.empty() && selected_row_ < static_cast<int>(snap.jobs.size())) {
                    const std::string& target_id = snap.jobs[static_cast<size_t>(selected_row_)].id;
                    if (cancel_fn) {
                        cancel_fn(target_id);
                    }
                }
                break;

            case 's':
            case 'S':
                if (submit_demo_fn) {
                    submit_demo_fn();
                }
                break;

            case 'w':
            case 'W':
                current_view_ = TuiView::Workers;
                break;

            case 'e':
            case 'E':
                current_view_ = TuiView::Events;
                break;
        }
    }
#endif

    shutdown();
    return 0;
}

} // namespace astra
