// pomo-kndl Kindle UI: GTK 2, black on white, two views (task list, session).
// Designed for e-ink: flat, high contrast, large targets, no animation, redraw only on change.
#include <gtk/gtk.h>
#include <gmodule.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core.hpp"
#include "net.hpp"

namespace {

struct Ui {
    std::unique_ptr<pk::App> app;
    std::string dir;
    GtkWidget* win = nullptr;
    GtkWidget* body = nullptr;
    std::string shown;  // signature of current screen, to redraw only on change
    std::atomic<bool> syncing{false};
    std::string sync_msg = "offline";
    int scale = 100;  // percent; type and spacing scale with screen width
    int content_w = 1124;  // window width minus margins, for text wrapping
};
Ui U;

void render(bool force = false);

// ---- sizing ---------------------------------------------------------------------------------
// Sizes are tuned for the Paperwhite 5 (1236 px wide) and scale with the actual screen.
int px(int v) { return v * U.scale / 100; }

GdkColor color(const char* spec) {
    GdkColor c;
    gdk_color_parse(spec, &c);
    return c;
}

void paint(GtkWidget* w, const char* bg) {
    GdkColor c = color(bg);
    for (GtkStateType s : {GTK_STATE_NORMAL, GTK_STATE_PRELIGHT, GTK_STATE_ACTIVE, GTK_STATE_SELECTED})
        gtk_widget_modify_bg(w, s, &c);
}

// ---- text -----------------------------------------------------------------------------------
// size is a type size in points as seen on paper; the Paperwhite 5 is 300 ppi, so 1 pt is about
// 4 device px. Pango's "px" unit keeps this independent of the X server's DPI setting, which
// differs between the Kindle and a desktop preview.
GtkWidget* label(const std::string& text, int size, const char* weight = "normal", const char* fg = "#000000",
                 bool strike = false, float xalign = 0.5f) {
    GtkWidget* l = gtk_label_new(nullptr);
    char* esc = g_markup_escape_text(text.c_str(), -1);
    int sz = px(size * 4) < 8 ? 8 : px(size * 4);
    char* m = g_strdup_printf("<span font_desc='Sans %dpx' weight='%s' foreground='%s'%s>%s</span>", sz, weight, fg,
                              strike ? " strikethrough='true'" : "", esc);
    gtk_label_set_markup(GTK_LABEL(l), m);
    // No wrapping by default: GTK 2 wraps at ~40 characters regardless of the space available.
    // Callers that need wrapping (task titles, centered()) enable it with a real width.
    gtk_misc_set_alignment(GTK_MISC(l), xalign, 0.5f);
    g_free(esc);
    g_free(m);
    return l;
}

// Wrapped text that stays centered. GTK 2 labels left-align a single line inside a wide size
// request and miscompute height when wrap width is guessed, so measure with Pango at the real
// content width and request exactly the wrapped size, centered in an alignment.
GtkWidget* centered(const std::string& text, int size, const char* weight = "normal", const char* fg = "#000000") {
    GtkWidget* l = label(text, size, weight, fg);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    PangoLayout* lay = gtk_label_get_layout(GTK_LABEL(l));
    pango_layout_set_width(lay, U.content_w * PANGO_SCALE);
    pango_layout_set_wrap(lay, PANGO_WRAP_WORD);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(lay, &w, &h);
    gtk_widget_set_size_request(l, w, h);
    GtkWidget* a = gtk_alignment_new(0.5f, 0.5f, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), l);
    return a;
}

// ---- tappable surfaces ----------------------------------------------------------------------
using Action = void (*)(const std::string&);
struct Btn { Action fn; std::string arg; };

// Preview/test hook: every tappable is registered by its visible label, so POMO_KNDL_SCRIPT can
// "tap" by name and snapshot the window without synthesizing pointer events.
struct Target { std::string name; Action fn; std::string arg; };
std::vector<Target> g_targets;

gboolean on_press(GtkWidget*, GdkEventButton* ev, gpointer p) {
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;  // ignore double/triple-click synthesis
    auto* b = static_cast<Btn*>(p);
    try {
        b->fn(b->arg);
    } catch (std::exception& e) {
        U.sync_msg = std::string("error: ") + e.what();
    }
    render();
    return TRUE;
}

// Wrap any widget in a tappable box. Event boxes avoid the GTK theme's bevels and hover
// states, which look muddy on e-ink.
GtkWidget* tappable(GtkWidget* child, Action fn, const std::string& arg, const char* bg, const std::string& name = "") {
    if (!name.empty()) g_targets.push_back({name, fn, arg});
    GtkWidget* eb = gtk_event_box_new();
    paint(eb, bg);
    gtk_container_add(GTK_CONTAINER(eb), child);
    auto* data = new Btn{fn, arg};
    gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK);
    // Catch presses anywhere inside, even over nested event boxes (labels, fills).
    gtk_event_box_set_above_child(GTK_EVENT_BOX(eb), TRUE);
    g_signal_connect_data(eb, "button-press-event", G_CALLBACK(on_press), data,
                          [](gpointer d, GClosure*) { delete static_cast<Btn*>(d); }, (GConnectFlags)0);
    return eb;
}

// Bordered button: a black frame around a padded fill. The tap handler sits on the outermost
// box so the whole button, border included, is one hit target (nested event boxes would let
// the outer window swallow presses).
// primary = filled black with white text; secondary = white with black text.
GtkWidget* button(const std::string& text, Action fn, bool primary = false, const std::string& arg = "") {
    GtkWidget* inner = gtk_alignment_new(0.5f, 0.5f, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(inner), px(34), px(34), px(16), px(16));
    gtk_container_add(GTK_CONTAINER(inner), label(text, 15, "bold", primary ? "#ffffff" : "#000000"));
    GtkWidget* fill = gtk_event_box_new();
    paint(fill, primary ? "#000000" : "#ffffff");
    gtk_container_add(GTK_CONTAINER(fill), inner);
    GtkWidget* border = gtk_alignment_new(0.5f, 0.5f, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(border), px(3), px(3), px(3), px(3));
    gtk_container_add(GTK_CONTAINER(border), fill);
    GtkWidget* frame = tappable(border, fn, arg, "#000000", text);
    return frame;
}

// Small text-only action for the footer (Sync, Exit): still a big hit area, quiet visually.
GtkWidget* link(const std::string& text, Action fn) {
    GtkWidget* pad = gtk_alignment_new(0.5f, 0.5f, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(pad), px(22), px(22), px(28), px(28));
    gtk_container_add(GTK_CONTAINER(pad), label(text, 12, "bold"));
    return tappable(pad, fn, "", "#ffffff", text);
}

GtkWidget* rule(int thickness = 2, const char* c = "#000000") {
    GtkWidget* eb = gtk_event_box_new();
    paint(eb, c);
    gtk_widget_set_size_request(eb, -1, px(thickness));
    return eb;
}

GtkWidget* spacer(int h) {
    GtkWidget* s = gtk_label_new(nullptr);
    gtk_widget_set_size_request(s, -1, px(h));
    return s;
}

GtkWidget* row(std::initializer_list<GtkWidget*> ws, int gap = 28) {
    GtkWidget* h = gtk_hbox_new(TRUE, px(gap));
    for (auto* w : ws) gtk_box_pack_start(GTK_BOX(h), w, TRUE, TRUE, 0);
    return h;
}

// Progress bar: 20 flat segments inside a black outline. Discrete steps mean the bar only
// changes (and the e-ink only refreshes) a few times per session.
GtkWidget* progress(double frac) {
    const int n = 20;
    int filled = (int)(frac * n + 0.5);
    if (filled < 0) filled = 0;
    if (filled > n) filled = n;
    GtkWidget* outer = gtk_event_box_new();
    paint(outer, "#000000");
    GtkWidget* pad = gtk_alignment_new(0, 0.5f, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(pad), px(3), px(3), px(3), px(3));
    GtkWidget* track = gtk_hbox_new(TRUE, 0);
    for (int i = 0; i < n; i++) {
        GtkWidget* seg = gtk_event_box_new();
        paint(seg, i < filled ? "#000000" : "#ffffff");
        gtk_widget_set_size_request(seg, -1, px(36));
        gtk_box_pack_start(GTK_BOX(track), seg, TRUE, TRUE, 0);
    }
    gtk_container_add(GTK_CONTAINER(pad), track);
    gtk_container_add(GTK_CONTAINER(outer), pad);
    return outer;
}

// ---- sync -----------------------------------------------------------------------------------
void sync_done(const std::string& msg) {
    U.sync_msg = msg;
    U.syncing = false;
    render(true);
}

void do_sync(const std::string&) {
    if (U.syncing.exchange(true)) return;
    std::ifstream f(U.dir + "/pairing.json");
    if (!f) {
        U.syncing = false;
        U.sync_msg = "not paired";
        return;
    }
    pk::json pairing = pk::json::parse(f);
    pk::json req = U.app->landing_request(pairing);
    U.sync_msg = "syncing";
    std::thread([pairing, req] {
        std::string* res = new std::string;
        pk::json reply;
        try {
            reply = pk::tls_exchange(pairing, req);
        } catch (std::exception& e) {
            *res = std::string("offline: ") + e.what();
        }
        struct R { pk::json reply; std::string* err; };
        g_idle_add([](gpointer p) -> gboolean {
            std::unique_ptr<R> r(static_cast<R*>(p));
            std::unique_ptr<std::string> err(r->err);
            if (!err->empty()) {
                sync_done(*err);
                return FALSE;
            }
            try {
                U.app->apply_landing(r->reply);  // on the main thread: no data race
                U.app->set_last_sync("synced");
                sync_done("synced");
            } catch (std::exception& e) {
                sync_done(std::string("sync refused: ") + e.what());
            }
            return FALSE;
        }, new R{reply, res});
    }).detach();
}

// ---- actions --------------------------------------------------------------------------------
void a_start(const std::string& id) { U.app->start(id); }
void a_pause(const std::string&) { U.app->pause(); }
void a_resume(const std::string&) { U.app->resume(); }
void a_end(const std::string&) { U.app->end(); }
void a_done(const std::string&) { U.app->finish_done(); }
void a_later(const std::string&) { U.app->finish_later(); }
void a_break(const std::string&) { U.app->finish_break(); }
void a_end_break(const std::string&) { U.app->end_break(); }
void a_reopen(const std::string& id) { U.app->reopen(id); }
// State is saved after every action, so exiting never loses work. A running timer keeps
// counting (it is stored as a start time) and is reconciled when the app is reopened.
void a_exit(const std::string&) { gtk_main_quit(); }

std::string mins_left(int64_t sec) { return std::to_string((sec + 59) / 60); }  // countdown: round up
std::string mins_done(int64_t sec) { return std::to_string(sec / 60); }         // focused: never overstate

// Short, human status for the header. Errors are trimmed to their first clause.
std::string status_text() {
    if (U.syncing) return "syncing";
    std::string s = U.sync_msg;
    size_t n = U.app->pending_entries();
    if (s.rfind("offline", 0) == 0) return n ? "offline · " + std::to_string(n) + " to sync" : "offline";
    if (s.rfind("sync refused", 0) == 0) return "sync refused";
    if (s.rfind("error", 0) == 0) return "error";
    if (s == "not paired") return "not paired";
    if (n) return std::to_string(n) + " to sync";
    if (s == "synced") return "synced";
    return s == "never" ? "not synced yet" : s;
}

// ---- views ----------------------------------------------------------------------------------
const int kMaxTasks = 7;  // keeps every row a big target on a 6.8" screen

GtkWidget* task_row(const pk::Task& t) {
    GtkWidget* h = gtk_hbox_new(FALSE, px(24));
    const char* fg = t.done_locally ? "#777777" : "#000000";
    GtkWidget* mark = label(t.done_locally ? "✓" : "▸", 14, "bold", fg);
    gtk_widget_set_size_request(mark, px(48), -1);
    gtk_box_pack_start(GTK_BOX(h), mark, FALSE, FALSE, 0);
    GtkWidget* title = label(t.title, 14, t.done_locally ? "normal" : "bold", fg, t.done_locally, 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    // Wrap to the row width, not GTK's default ~40 characters.
    gtk_widget_set_size_request(title, U.content_w - px(48 + 24 + 16), -1);
    gtk_box_pack_start(GTK_BOX(h), title, TRUE, TRUE, 0);
    if (t.review) gtk_box_pack_end(GTK_BOX(h), label("REVIEW", 10, "bold", "#000000"), FALSE, FALSE, 0);
    GtkWidget* pad = gtk_alignment_new(0, 0.5f, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(pad), px(40), px(40), px(8), px(8));
    gtk_container_add(GTK_CONTAINER(pad), h);
    // Tap an open task to start it; tap a crossed-off task to reopen it.
    return tappable(pad, t.done_locally ? a_reopen : a_start, t.id, "#ffffff", t.title);
}

void build_list(GtkWidget* box) {
    auto& ts = U.app->tasks();
    gtk_box_pack_start(GTK_BOX(box), label("TASKS", 9, "bold", "#555555", false, 0.0f), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(10), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), rule(3), FALSE, FALSE, 0);
    if (ts.empty()) {
        gtk_box_pack_start(GTK_BOX(box), spacer(120), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), label("No tasks yet", 20, "bold"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), spacer(24), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), centered("Add them on the computer with\npomo-kndl add \"…\", then tap Sync.", 12,
                                                  "normal", "#444444"),
                           FALSE, FALSE, 0);
        return;
    }
    int shown = 0;
    for (auto& t : ts) {
        if (shown == kMaxTasks) break;
        gtk_box_pack_start(GTK_BOX(box), task_row(t), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), rule(1, "#999999"), FALSE, FALSE, 0);
        shown++;
    }
    if ((int)ts.size() > kMaxTasks) {
        std::string more = "+" + std::to_string(ts.size() - kMaxTasks) + " more on the computer";
        gtk_box_pack_start(GTK_BOX(box), label(more, 11, "normal", "#555555"), FALSE, FALSE, px(20));
    }
    gtk_box_pack_start(GTK_BOX(box), spacer(28), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label("Tap a task to start focusing", 10, "normal", "#555555"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), button(U.syncing ? "Syncing" : "Sync with computer", do_sync), FALSE, FALSE, px(24));
}

void build_session(GtkWidget* box) {
    auto& s = U.app->session();
    using P = pk::Phase;
    const bool in_break = s.phase == P::Break;

    std::string kicker = in_break ? "BREAK" : s.phase == P::Ended ? "SESSION COMPLETE" : "FOCUS";
    if (s.phase == P::Paused) kicker = s.recovered ? "PAUSED AFTER RESTART" : "PAUSED";
    if (s.phase == P::Ended && s.interrupted) kicker = "ENDED EARLY";

    gtk_box_pack_start(GTK_BOX(box), spacer(40), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label(kicker, 12, "bold", "#000000"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(18), FALSE, FALSE, 0);
    if (!in_break) gtk_box_pack_start(GTK_BOX(box), centered(s.title, 18, "bold"), FALSE, FALSE, 0);
    else gtk_box_pack_start(GTK_BOX(box), centered("Step away from the screen", 18, "normal", "#333333"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(60), FALSE, FALSE, 0);

    std::string big, unit;
    double frac = 0;
    if (s.phase == P::Focus || s.phase == P::Paused) {
        big = mins_left(U.app->remaining_seconds());
        unit = "min left";
        frac = s.planned_min > 0 ? (double)U.app->focus_seconds() / (s.planned_min * 60) : 0;
    } else if (in_break) {
        big = mins_left(U.app->remaining_seconds());
        unit = "min break";
        frac = s.break_min > 0 ? 1.0 - (double)U.app->remaining_seconds() / (s.break_min * 60) : 0;
    } else {
        big = s.accumulated > 0 && s.accumulated < 60 ? "<1" : mins_done(s.accumulated);
        unit = "min focused";
        frac = s.planned_min > 0 ? (double)s.accumulated / (s.planned_min * 60) : 0;
    }
    gtk_box_pack_start(GTK_BOX(box), label(big, 96, "bold"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label(unit, 16, "normal", "#333333"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(50), FALSE, FALSE, 0);

    GtkWidget* bar = gtk_alignment_new(0.5f, 0.5f, 0.8f, 1);
    gtk_container_add(GTK_CONTAINER(bar), progress(frac));
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(90), FALSE, FALSE, 0);

    GtkWidget* r = nullptr;
    switch (s.phase) {
        case P::Focus: r = row({button("Pause", a_pause, true), button("End", a_end)}); break;
        case P::Paused: r = row({button("Resume", a_resume, true), button("End", a_end)}); break;
        case P::Ended: r = row({button("Done", a_done, true), button("Later", a_later), button("Break", a_break)}); break;
        case P::Break: {
            // Single action: half width, centered, like the paired buttons above it.
            GtkWidget* a = gtk_alignment_new(0.5f, 0.5f, 0.5f, 1);
            gtk_container_add(GTK_CONTAINER(a), button("End break", a_end_break, true));
            r = a;
            break;
        }
        default: break;
    }
    if (r) gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 0);
    if (s.phase == P::Ended) {
        gtk_box_pack_start(GTK_BOX(box), spacer(24), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box),
                           centered("Done marks the task complete. Later keeps it open.", 10, "normal", "#555555"),
                           FALSE, FALSE, 0);
    }
}

// ---- frame ----------------------------------------------------------------------------------
std::string signature() {
    auto& s = U.app->session();
    std::string sig = std::to_string((int)s.phase) + "|" + status_text();
    if (s.phase == pk::Phase::Idle) {
        for (auto& t : U.app->tasks()) sig += t.id + t.title + (t.done_locally ? "x" : "") + (t.review ? "r" : "");
    } else {
        // Minute-level countdown; the 20-step bar changes at most a little faster on short sessions.
        sig += std::to_string((U.app->remaining_seconds() + 59) / 60) + "/" +
               std::to_string(U.app->focus_seconds() * 20 / (s.planned_min > 0 ? s.planned_min * 60 : 1));
    }
    auto td = U.app->today();
    return sig + "|" + std::to_string(td.first) + "," + std::to_string(td.second);
}

GtkWidget* header() {
    GtkWidget* h = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), label("pomo-kndl", 12, "heavy", "#000000", false, 0.0f), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(h), label(status_text(), 10, "normal", "#333333", false, 1.0f), FALSE, FALSE, 0);
    return h;
}

GtkWidget* footer() {
    GtkWidget* v = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), rule(1, "#999999"), FALSE, FALSE, 0);
    GtkWidget* h = gtk_hbox_new(FALSE, 0);
    auto td = U.app->today();
    std::string today = "Today  " + std::to_string(td.first) + (td.first == 1 ? " session" : " sessions") + " · " +
                        std::to_string(td.second) + " min";
    gtk_box_pack_start(GTK_BOX(h), label(today, 11, "normal", "#000000", false, 0.0f), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(h), link("Exit", a_exit), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
    return v;
}

void render(bool force) {
    std::string sig = signature();
    if (!force && sig == U.shown) return;  // nothing visible changed: no e-ink refresh
    U.shown = sig;
    g_targets.clear();
    if (U.body) gtk_widget_destroy(U.body);
    U.body = gtk_vbox_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(U.body), px(56));

    gtk_box_pack_start(GTK_BOX(U.body), header(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(U.body), spacer(56), FALSE, FALSE, 0);

    GtkWidget* mid = gtk_vbox_new(FALSE, 0);
    if (U.app->session().phase == pk::Phase::Idle) build_list(mid); else build_session(mid);
    gtk_box_pack_start(GTK_BOX(U.body), mid, TRUE, TRUE, 0);

    gtk_box_pack_end(GTK_BOX(U.body), footer(), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(U.win), U.body);
    gtk_widget_show_all(U.win);
}

gboolean on_tick(gpointer) {
    // Runs every 10 s. After suspend, CLOCK_BOOTTIME has advanced, so a due timer is reconciled here.
    try {
        U.app->tick();
    } catch (std::exception& e) {
        U.sync_msg = std::string("error: ") + e.what();
    }
    render();
    return TRUE;
}

}  // namespace

// POMO_KNDL_SCRIPT="tap:Pause;shot:a.png;tap:End;shot:b.png" drives the UI by button label and
// writes PNG snapshots, then exits. Used for README screenshots and visual checks; not on device.
static std::vector<std::string> g_script;
static size_t g_step = 0;

static gboolean script_step(gpointer) {
    while (gtk_events_pending()) gtk_main_iteration();
    if (g_step >= g_script.size()) {
        gtk_main_quit();
        return FALSE;
    }
    std::string cmd = g_script[g_step++];
    if (cmd.rfind("tap:", 0) == 0) {
        std::string want = cmd.substr(4);
        bool hit = false;
        for (auto& t : g_targets)
            if (t.name == want) {
                t.fn(t.arg);
                render();
                hit = true;
                break;
            }
        if (!hit) fprintf(stderr, "script: no target '%s'\n", want.c_str());
    } else if (cmd.rfind("shot:", 0) == 0) {
        // Offscreen window: readable on every backend (Quartz cannot read back on-screen windows).
        // Looked up at run time so the device binary never hard-depends on a newer GTK symbol.
        using GetPixbuf = GdkPixbuf* (*)(GtkWidget*);
        gpointer sym = nullptr;
        GModule* self = g_module_open(nullptr, (GModuleFlags)0);
        if (self) g_module_symbol(self, "gtk_offscreen_window_get_pixbuf", &sym);
        GdkPixbuf* pb = sym ? reinterpret_cast<GetPixbuf>(sym)(U.win) : nullptr;
        if (pb) {
            gdk_pixbuf_save(pb, cmd.substr(5).c_str(), "png", nullptr, nullptr);
            g_object_unref(pb);
        }
    }
    g_timeout_add(300, script_step, nullptr);
    return FALSE;
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    const char* d = getenv("POMO_KNDL_DIR");
    U.dir = d ? d : "/mnt/us/extensions/pomo-kndl";
    U.app = std::make_unique<pk::App>(U.dir, pk::Clock::system());
    try {
        U.app->load();
    } catch (std::exception& e) {
        fprintf(stderr, "pomo-kndl: cannot load state: %s\n", e.what());
        return 1;  // never overwrite a state file we could not read
    }
    U.sync_msg = U.app->pending_entries() ? "pending" : U.app->last_sync();

    // Snapshot mode renders offscreen (GTK >= 2.20, resolved at run time; preview only).
    U.win = nullptr;
    if (getenv("POMO_KNDL_SCRIPT")) {
        using NewWin = GtkWidget* (*)();
        gpointer sym = nullptr;
        GModule* self = g_module_open(nullptr, (GModuleFlags)0);
        if (self && g_module_symbol(self, "gtk_offscreen_window_new", &sym)) U.win = reinterpret_cast<NewWin>(sym)();
    }
    if (!U.win) U.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    // Kindle window manager (awesome) reads placement from the title: app layer, application role,
    // Kindle top bar (PC:T), id without underscores, portrait (O:U).
    gtk_window_set_title(GTK_WINDOW(U.win), "L:A_N:application_PC:T_ID:com.pomokndl.app_O:U");
    GdkScreen* scr = gdk_screen_get_default();
    int w = gdk_screen_get_width(scr), h = gdk_screen_get_height(scr);
    if (const char* g = getenv("POMO_KNDL_GEOMETRY")) sscanf(g, "%dx%d", &w, &h);  // desktop preview
    U.scale = w * 100 / 1236;
    if (U.scale < 40) U.scale = 40;
    U.content_w = w - 2 * px(56);
    gtk_window_set_default_size(GTK_WINDOW(U.win), w, h);
    gtk_widget_set_size_request(U.win, w, h);
    paint(U.win, "#ffffff");
    g_signal_connect(U.win, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    render(true);
    if (const char* sc = getenv("POMO_KNDL_SCRIPT")) {
        std::string s = sc, part;
        for (char c : s + ";") {
            if (c == ';') {
                if (!part.empty()) g_script.push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
        g_timeout_add(800, script_step, nullptr);
        gtk_main();
        return 0;
    }
    g_timeout_add_seconds(10, on_tick, nullptr);
    if (U.app->pending_entries() || U.app->last_sync() == "never") do_sync("");  // try once at launch
    gtk_main();
    return 0;
}
