// pomo-kndl Kindle core: durable state, flight log, timer. No GTK or network here.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../third_party/json.hpp"

namespace pk {
using json = nlohmann::json;

struct Clock {
    // Seconds on a clock that keeps running through suspend (CLOCK_BOOTTIME on Linux).
    std::function<int64_t()> mono;
    // Unix seconds (informational only).
    std::function<int64_t()> wall;
    // Identifier that changes on reboot (mono resets).
    std::function<std::string()> boot_id;
    // Local date YYYY-MM-DD for display grouping.
    std::function<std::string()> local_date;
    static Clock system();
};

struct Task {
    std::string id, title;
    int64_t position = 0;
    bool review = false;
    bool done_locally = false;  // crossed off until next landing
};

enum class Phase { Idle, Focus, Paused, Ended, Break };

struct Session {
    Phase phase = Phase::Idle;
    std::string task_id, title;
    int64_t planned_min = 25;
    int64_t accumulated = 0;    // focused seconds in finished intervals
    int64_t run_since = 0;      // mono at start of current interval (Focus/Break)
    std::string boot_id;
    bool interrupted = false;   // ended before expiry
    bool recovered = false;     // came back after reboot: gap not counted
    int64_t break_min = 5;
};

class App {
public:
    App(std::string dir, Clock clock);

    // Persistence
    void load();
    void save();  // atomic: write tmp, fsync, rename, fsync dir

    // Queries
    const std::vector<Task>& tasks() const { return tasks_; }
    const Session& session() const { return s_; }
    int64_t focus_seconds() const;      // focused so far in current session
    int64_t remaining_seconds() const;  // Focus/Paused: until expiry. Break: until break end.
    int64_t plan_version() const { return plan_version_; }
    int64_t focus_min() const { return focus_min_; }
    int64_t break_min() const { return break_min_; }
    size_t pending_entries() const { return log_.size(); }
    std::pair<int, int64_t> today() const;  // sessions, focused minutes (local log + landed today)
    std::string last_sync() const { return last_sync_; }
    void set_last_sync(const std::string& s) { last_sync_ = s; save(); }

    // Actions (each saves before returning)
    void start(const std::string& task_id);
    void pause();
    void resume();
    void end();                 // to end screen, marks interrupted if early
    void finish_done();         // end screen: Done
    void finish_later();        // end screen: Continue later
    void finish_break();        // end screen: Break
    void end_break();
    void mark_done(const std::string& task_id);
    void reopen(const std::string& task_id);
    // Called periodically and on resume. Returns true if the screen should change.
    bool tick();

    // Sync
    json landing_request(const json& pairing) const;
    // Apply a "landed" reply. Throws on malformed reply. Never loses unacked entries.
    void apply_landing(const json& reply);

private:
    void append(json entry);
    void close_session(const char* status);
    const Task* find(const std::string& id) const;

    std::string dir_;
    Clock c_;
    std::vector<Task> tasks_;
    int64_t plan_version_ = 0, focus_min_ = 25, break_min_ = 5;
    std::vector<json> log_;
    int64_t next_seq_ = 1;
    Session s_;
    std::string last_sync_ = "never";
    std::string today_date_;
    int today_sessions_ = 0;
    int64_t today_focus_ = 0;
};

std::string uuid4();
}  // namespace pk
