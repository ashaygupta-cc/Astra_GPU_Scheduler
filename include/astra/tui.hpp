#pragma once

#include "astra/tui_snapshot.hpp"
#include <functional>
#include <string>

namespace astra {

enum class TuiView {
    Jobs,
    Workers,
    Resources,
    Events,
    JobDetails,
    Logs
};

class Tui {
public:
    Tui();
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    bool init();
    void shutdown();
    bool is_initialized() const { return initialized_; }

    // Main interactive loop
    int run(std::function<TuiSnapshot()> snapshot_fn,
            std::function<bool(const std::string& job_id)> cancel_fn = nullptr,
            std::function<void()> submit_demo_fn = nullptr);

    // Render a single snapshot (can be used for headless test verification)
    void render_frame(const TuiSnapshot& snap);

    // Change current active view
    void set_view(TuiView view) { current_view_ = view; }
    TuiView current_view() const { return current_view_; }

private:
    bool initialized_ = false;
    TuiView current_view_ = TuiView::Jobs;
    int selected_row_ = 0;
    std::string inspect_job_id_;
    std::string status_banner_msg_;

    void render_header(int max_y, int max_x, const TuiSnapshot& snap);
    void render_jobs_view(int max_y, int max_x, const TuiSnapshot& snap);
    void render_workers_view(int max_y, int max_x, const TuiSnapshot& snap);
    void render_resources_view(int max_y, int max_x, const TuiSnapshot& snap);
    void render_events_view(int max_y, int max_x, const TuiSnapshot& snap);
    void render_job_details_view(int max_y, int max_x, const TuiSnapshot& snap);
    void render_logs_view(int max_y, int max_x, const TuiSnapshot& snap);
    void render_footer(int max_y, int max_x);
};

} // namespace astra
