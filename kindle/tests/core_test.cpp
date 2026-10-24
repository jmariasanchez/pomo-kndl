// Core tests with a fake clock. Build: make test
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "../src/core.hpp"

using pk::json;
static int64_t T = 1000;
static std::string BOOT = "b1";

static pk::Clock fake() {
    pk::Clock c;
    c.mono = [] { return T; };
    c.wall = [] { return (int64_t)1777000000 + T; };
    c.boot_id = [] { return BOOT; };
    c.local_date = [] { return std::string("2026-10-24"); };
    return c;
}

static std::string tmpdir() {
    char t[] = "/tmp/pkt.XXXXXX";
    return mkdtemp(t);
}

static json plan(int v, std::initializer_list<const char*> ids, int focus = 25) {
    json ts = json::array();
    int p = 1;
    for (auto i : ids) ts.push_back({{"id", i}, {"title", std::string("T") + i}, {"position", p++}, {"review", false}});
    return {{"type", "landed"}, {"acked_seq", 0},
            {"plan", {{"version", v}, {"settings", {{"focus_min", focus}, {"break_min", 5}}}, {"tasks", ts}}}};
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void full_cycle_offline_with_pause() {
    auto d = tmpdir();
    pk::App a(d, fake());
    a.apply_landing(plan(3, {"a", "b"}));
    a.start("a");
    T += 600;
    a.pause();
    T += 3000;  // paused time excluded
    a.resume();
    T += 900;
    CHECK(a.tick());  // 25 min focus reached
    CHECK(a.session().phase == pk::Phase::Ended);
    a.finish_break();
    T += 400;
    CHECK(a.tick());  // break due
    CHECK(a.session().phase == pk::Phase::Idle);
    CHECK(a.pending_entries() == 2);
    auto r = a.landing_request({{"device_id", "d"}, {"token", "t"}});
    CHECK(r["entries"][0]["kind"] == "session");
    CHECK(r["entries"][0]["focus_sec"] == 1500);
    CHECK(r["entries"][0]["status"] == "completed");
    CHECK(r["entries"][0]["plan_version"] == 3);
    CHECK(r["entries"][1]["break_sec"] == 300);
    CHECK(a.today().first == 1 && a.today().second == 25);
}

static void persists_and_survives_restart() {
    auto d = tmpdir();
    {
        pk::App a(d, fake());
        a.apply_landing(plan(1, {"a"}));
        a.start("a");
        T += 60;
    }
    pk::App b(d, fake());
    b.load();
    CHECK(b.session().phase == pk::Phase::Focus);
    CHECK(b.focus_seconds() == 60);  // same boot: clock continued
}

static void reboot_never_counts_gap() {
    auto d = tmpdir();
    {
        pk::App a(d, fake());
        a.apply_landing(plan(1, {"a"}));
        a.start("a");
        T += 120;
        a.pause();
        a.resume();
        T += 60;
    }
    BOOT = "b2";
    T = 5;  // boot clock reset
    pk::App b(d, fake());
    b.load();
    CHECK(b.session().phase == pk::Phase::Paused);
    CHECK(b.session().recovered);
    CHECK(b.focus_seconds() == 120);  // last persisted checkpoint only
    BOOT = "b1";
    T = 1000;
}

static void done_and_landing_keeps_unacked() {
    auto d = tmpdir();
    pk::App a(d, fake());
    a.apply_landing(plan(1, {"a", "b"}));
    a.mark_done("a");   // seq 1
    a.mark_done("a");   // no duplicate
    CHECK(a.pending_entries() == 1);
    a.mark_done("b");   // seq 2, pretend made after request was sent
    json r = plan(2, {"a", "b"});
    r["acked_seq"] = 1;  // computer acked only seq 1 (still lists a? no: a is done)
    r["plan"]["tasks"].erase(0);
    a.apply_landing(r);
    CHECK(a.pending_entries() == 1);
    CHECK(a.tasks().size() == 1 && a.tasks()[0].id == "b" && a.tasks()[0].done_locally);
    CHECK(a.plan_version() == 2);
}

static void settings_apply_next_session() {
    auto d = tmpdir();
    pk::App a(d, fake());
    a.apply_landing(plan(1, {"a"}));
    a.start("a");
    a.apply_landing(plan(2, {"a"}, 50));
    CHECK(a.session().planned_min == 25);
    a.end();
    a.finish_later();
    a.start("a");
    CHECK(a.session().planned_min == 50);
}

static void early_end_is_interrupted() {
    auto d = tmpdir();
    pk::App a(d, fake());
    a.apply_landing(plan(1, {"a"}));
    a.start("a");
    T += 300;
    a.end();
    auto r = a.landing_request({{"device_id", "d"}, {"token", "t"}});
    CHECK(r["entries"][0]["status"] == "interrupted");
    CHECK(r["entries"][0]["focus_sec"] == 300);
}

static void malformed_reply_changes_nothing() {
    auto d = tmpdir();
    pk::App a(d, fake());
    a.apply_landing(plan(1, {"a"}));
    a.mark_done("a");
    json bad = {{"type", "landed"}, {"acked_seq", 1}, {"plan", {{"version", 2}}}};
    bool threw = false;
    try { a.apply_landing(bad); } catch (...) { threw = true; }
    CHECK(threw);
    CHECK(a.pending_entries() == 1 && a.plan_version() == 1);
}

int main() {
    full_cycle_offline_with_pause();
    persists_and_survives_restart();
    reboot_never_counts_gap();
    done_and_landing_keeps_unacked();
    settings_apply_next_session();
    early_end_is_interrupted();
    malformed_reply_changes_nothing();
    puts("core tests: 7 passed");
}
