#include "core.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace pk {

static uint64_t seed64() {
    // Avoid std::random_device: GCC 6's libstdc++ throws if its device is unavailable.
    uint64_t s = 0;
    if (FILE* f = fopen("/dev/urandom", "rb")) {
        if (fread(&s, sizeof s, 1, f) != 1) s = 0;
        fclose(f);
    }
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return s ^ ((uint64_t)time(nullptr) << 32) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 16);
}

std::string uuid4() {
    static std::mt19937_64 g(seed64());
    uint64_t a = g(), b = g();
    a = (a & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
    b = (b & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", (unsigned)(a >> 32),
             (unsigned)((a >> 16) & 0xffff), (unsigned)(a & 0xffff), (unsigned)(b >> 48),
             (unsigned long long)(b & 0xffffffffffffULL));
    return buf;
}

Clock Clock::system() {
    Clock c;
    c.mono = [] {
        timespec ts{};
#ifdef CLOCK_BOOTTIME
        clock_gettime(CLOCK_BOOTTIME, &ts);  // continues through suspend on Linux
#else
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#endif
        return (int64_t)ts.tv_sec;
    };
    c.wall = [] { return (int64_t)time(nullptr); };
    c.boot_id = [] {
        std::ifstream f("/proc/sys/kernel/random/boot_id");
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) return s;
        return std::string("static");
    };
    c.local_date = [] {
        time_t t = time(nullptr);
        tm lt{};
        localtime_r(&t, &lt);
        char b[11];
        strftime(b, sizeof b, "%Y-%m-%d", &lt);
        return std::string(b);
    };
    return c;
}

static const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Idle: return "idle";
        case Phase::Focus: return "focus";
        case Phase::Paused: return "paused";
        case Phase::Ended: return "ended";
        case Phase::Break: return "break";
    }
    return "idle";
}
static Phase phase_from(const std::string& s) {
    if (s == "focus") return Phase::Focus;
    if (s == "paused") return Phase::Paused;
    if (s == "ended") return Phase::Ended;
    if (s == "break") return Phase::Break;
    return Phase::Idle;
}

App::App(std::string dir, Clock clock) : dir_(std::move(dir)), c_(std::move(clock)) {}

void App::load() {
    std::ifstream f(dir_ + "/state.json");
    if (!f) return;  // first run
    json j = json::parse(f);  // corrupt state throws: better than silently losing the log
    plan_version_ = j.value("plan_version", 0);
    focus_min_ = j.value("focus_min", 25);
    break_min_ = j.value("break_min", 5);
    next_seq_ = j.value("next_seq", 1);
    last_sync_ = j.value("last_sync", "never");
    today_date_ = j.value("today_date", "");
    today_sessions_ = j.value("today_sessions", 0);
    today_focus_ = j.value("today_focus", 0);
    tasks_.clear();
    for (auto& t : j.value("tasks", json::array()))
        tasks_.push_back({t.at("id"), t.at("title"), t.value("position", 0), t.value("review", false),
                          t.value("done_locally", false)});
    log_.clear();
    for (auto& e : j.value("log", json::array())) log_.push_back(e);
    if (j.contains("session")) {
        auto& s = j["session"];
        s_.phase = phase_from(s.value("phase", "idle"));
        s_.task_id = s.value("task_id", "");
        s_.title = s.value("title", "");
        s_.planned_min = s.value("planned_min", 25);
        s_.accumulated = s.value("accumulated", 0);
        s_.run_since = s.value("run_since", 0);
        s_.boot_id = s.value("boot_id", "");
        s_.interrupted = s.value("interrupted", false);
        s_.recovered = s.value("recovered", false);
        s_.break_min = s.value("break_min", 5);
    }
    // Reboot while running: the monotonic clock reset, so the gap is unknown.
    // Never count it as focus. Stop and let the user decide.
    if ((s_.phase == Phase::Focus || s_.phase == Phase::Break) && s_.boot_id != c_.boot_id()) {
        if (s_.phase == Phase::Focus) {
            s_.phase = Phase::Paused;
            s_.recovered = true;
        } else {
            s_.phase = Phase::Idle;  // break time is not tracked as focus; just drop it
        }
        save();
    }
}

void App::save() {
    json j;
    j["version"] = 1;
    j["plan_version"] = plan_version_;
    j["focus_min"] = focus_min_;
    j["break_min"] = break_min_;
    j["next_seq"] = next_seq_;
    j["last_sync"] = last_sync_;
    j["today_date"] = today_date_;
    j["today_sessions"] = today_sessions_;
    j["today_focus"] = today_focus_;
    j["tasks"] = json::array();
    for (auto& t : tasks_)
        j["tasks"].push_back({{"id", t.id}, {"title", t.title}, {"position", t.position},
                              {"review", t.review}, {"done_locally", t.done_locally}});
    j["log"] = log_;
    j["session"] = {{"phase", phase_name(s_.phase)}, {"task_id", s_.task_id}, {"title", s_.title},
                    {"planned_min", s_.planned_min}, {"accumulated", s_.accumulated},
                    {"run_since", s_.run_since}, {"boot_id", s_.boot_id},
                    {"interrupted", s_.interrupted}, {"recovered", s_.recovered},
                    {"break_min", s_.break_min}};
    std::string data = j.dump();
    std::string tmp = dir_ + "/state.json.tmp", dst = dir_ + "/state.json";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("cannot write state");
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n <= 0) {
            ::close(fd);
            throw std::runtime_error("write failed");
        }
        off += (size_t)n;
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        throw std::runtime_error("fsync failed");
    }
    ::close(fd);
    if (::rename(tmp.c_str(), dst.c_str()) != 0) throw std::runtime_error("rename failed");
    int dfd = ::open(dir_.c_str(), O_RDONLY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
}

const Task* App::find(const std::string& id) const {
    for (auto& t : tasks_)
        if (t.id == id) return &t;
    return nullptr;
}

int64_t App::focus_seconds() const {
    int64_t f = s_.accumulated;
    if (s_.phase == Phase::Focus) f += std::max<int64_t>(0, c_.mono() - s_.run_since);
    return f;
}

int64_t App::remaining_seconds() const {
    if (s_.phase == Phase::Break)
        return std::max<int64_t>(0, s_.break_min * 60 - (c_.mono() - s_.run_since));
    return std::max<int64_t>(0, s_.planned_min * 60 - focus_seconds());
}

std::pair<int, int64_t> App::today() const {
    if (today_date_ != c_.local_date()) return {0, 0};
    return {today_sessions_, today_focus_ / 60};
}

void App::append(json e) {
    e["id"] = uuid4();
    e["seq"] = next_seq_++;
    e["plan_version"] = plan_version_;
    e["wall"] = c_.wall();
    log_.push_back(std::move(e));
}

void App::start(const std::string& task_id) {
    const Task* t = find(task_id);
    if (!t || s_.phase != Phase::Idle) return;
    s_ = Session{};
    s_.phase = Phase::Focus;
    s_.task_id = t->id;
    s_.title = t->title;
    s_.planned_min = focus_min_;  // settings apply from the next session
    s_.break_min = break_min_;
    s_.run_since = c_.mono();
    s_.boot_id = c_.boot_id();
    save();
}

void App::pause() {
    if (s_.phase != Phase::Focus) return;
    s_.accumulated = focus_seconds();
    s_.phase = Phase::Paused;
    save();
}

void App::resume() {
    if (s_.phase != Phase::Paused) return;
    s_.phase = Phase::Focus;
    s_.run_since = c_.mono();
    s_.boot_id = c_.boot_id();
    s_.recovered = false;
    save();
}

void App::close_session(const char* status) {
    int64_t f = std::min(focus_seconds(), s_.planned_min * 60);
    std::string d = c_.local_date();
    append({{"kind", "session"}, {"task_id", s_.task_id}, {"title", s_.title},
            {"planned_min", s_.planned_min}, {"focus_sec", f}, {"status", status},
            {"local_date", d}});
    if (today_date_ != d) {
        today_date_ = d;
        today_sessions_ = 0;
        today_focus_ = 0;
    }
    today_sessions_++;
    today_focus_ += f;
    s_.accumulated = f;
}

void App::end() {
    if (s_.phase != Phase::Focus && s_.phase != Phase::Paused) return;
    s_.interrupted = remaining_seconds() > 0;
    close_session(s_.interrupted ? "interrupted" : "completed");
    s_.phase = Phase::Ended;
    save();
}

void App::finish_done() {
    if (s_.phase != Phase::Ended) return;
    std::string id = s_.task_id;
    s_ = Session{};
    mark_done(id);  // saves
}

void App::finish_later() {
    if (s_.phase != Phase::Ended) return;
    s_ = Session{};
    save();
}

void App::finish_break() {
    if (s_.phase != Phase::Ended) return;
    int64_t b = s_.break_min;
    s_ = Session{};
    s_.phase = Phase::Break;
    s_.break_min = b > 0 ? b : 5;
    s_.run_since = c_.mono();
    s_.boot_id = c_.boot_id();
    save();
}

void App::end_break() {
    if (s_.phase != Phase::Break) return;
    int64_t sec = std::min(c_.mono() - s_.run_since, s_.break_min * 60);
    append({{"kind", "break"}, {"break_sec", std::max<int64_t>(0, sec)}, {"local_date", c_.local_date()}});
    s_ = Session{};
    save();
}

void App::mark_done(const std::string& task_id) {
    for (auto& t : tasks_)
        if (t.id == task_id && !t.done_locally) {
            t.done_locally = true;
            append({{"kind", "done"}, {"task_id", task_id}});
        }
    save();
}

void App::reopen(const std::string& task_id) {
    for (auto& t : tasks_)
        if (t.id == task_id && t.done_locally) {
            t.done_locally = false;
            append({{"kind", "reopen"}, {"task_id", task_id}});
        }
    save();
}

bool App::tick() {
    if (s_.phase == Phase::Focus && remaining_seconds() == 0) {
        // Due timer reconciled even if the device slept through expiry.
        s_.interrupted = false;
        close_session("completed");
        s_.phase = Phase::Ended;
        save();
        return true;
    }
    if (s_.phase == Phase::Break && remaining_seconds() == 0) {
        end_break();
        return true;
    }
    return false;
}

json App::landing_request(const json& p) const {
    return {{"type", "land"},          {"proto", 1},
            {"device_id", p.at("device_id")}, {"token", p.at("token")},
            {"plan_version", plan_version_}, {"clock", c_.wall()},
            {"entries", log_}};
}

void App::apply_landing(const json& r) {
    if (r.value("type", "") != "landed") throw std::runtime_error("landing refused: " + r.dump());
    int64_t acked = r.at("acked_seq").get<int64_t>();
    const json& plan = r.at("plan");
    std::vector<Task> nt;
    for (auto& t : plan.at("tasks"))
        nt.push_back({t.at("id"), t.at("title"), t.value("position", 0), t.value("review", false), false});
    // Parse fully before mutating anything.
    int64_t pv = plan.at("version").get<int64_t>();
    int64_t fm = plan.at("settings").at("focus_min").get<int64_t>();
    int64_t bm = plan.at("settings").at("break_min").get<int64_t>();

    log_.erase(std::remove_if(log_.begin(), log_.end(),
                              [&](const json& e) { return e.at("seq").get<int64_t>() <= acked; }),
               log_.end());
    // Entries made during the flight back are still pending: keep their local effect visible.
    for (auto& e : log_) {
        std::string k = e.value("kind", "");
        for (auto& t : nt)
            if (t.id == e.value("task_id", "")) {
                if (k == "done") t.done_locally = true;
                if (k == "reopen") t.done_locally = false;
            }
    }
    std::sort(nt.begin(), nt.end(), [](const Task& a, const Task& b) { return a.position < b.position; });
    tasks_ = std::move(nt);
    plan_version_ = pv;
    focus_min_ = fm;
    break_min_ = bm;
    save();
}

}  // namespace pk
