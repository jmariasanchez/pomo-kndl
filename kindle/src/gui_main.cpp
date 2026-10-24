// pomo-kndl Kindle UI: GTK 2, black on white, two views (task list, session).
// Designed for e-ink: flat, high contrast, large targets, no animation, redraw only on change.
#include <gtk/gtk.h>
#include <gmodule.h>
#include <gdk/gdkkeysyms.h>

#include <atomic>
#include <algorithm>
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
    std::string sync_msg = "Not synced yet";
    std::string sync_detail;
    std::string endpoint;
    bool connection_open = false;
    size_t page = 0;
    int scale = 100;  // percent; type and spacing scale with screen width
    int content_w = 1124;  // window width minus margins, for text wrapping
    int width = 1236, height = 1648;
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
    gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    PangoLayout* lay = gtk_label_get_layout(GTK_LABEL(l));
    pango_layout_set_width(lay, U.content_w * PANGO_SCALE);
    pango_layout_set_wrap(lay, PANGO_WRAP_WORD_CHAR);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(lay, &w, &h);
    gtk_widget_set_size_request(l, w, h);
    GtkWidget* a = gtk_alignment_new(0.5f, 0.5f, 0, 0);
    gtk_container_add(GTK_CONTAINER(a), l);
    return a;
}

GtkWidget* session_title(const std::string& text) {
    // Reserve space for the complete action area even with a maximum-length task title.
    PangoLayout* layout = gtk_widget_create_pango_layout(U.win, text.c_str());
    pango_layout_set_width(layout, U.content_w * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    int size = 18;
    for (; size > 8; --size) {
        const std::string font = "Sans Bold " + std::to_string(px(size * 4)) + "px";
        PangoFontDescription* desc = pango_font_description_from_string(font.c_str());
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
        int height = 0;
        pango_layout_get_pixel_size(layout, nullptr, &height);
        if (height <= px(220)) break;
    }
    g_object_unref(layout);
    return centered(text, size, "bold");
}

// ---- tappable surfaces ----------------------------------------------------------------------
using Action = void (*)(const std::string&);
struct Btn { Action fn; std::string arg; };

// Preview/test hook: register targets by label and dispatch GTK events without moving the pointer.
struct Target { std::string name; GtkWidget* widget; };
std::vector<Target> g_targets;

void activate(gpointer p) {
    const Btn b = *static_cast<Btn*>(p);
    try {
        b.fn(b.arg);
    } catch (std::exception& e) {
        U.sync_msg = "Action failed";
        U.sync_detail = e.what();
    }
    render();
}

gboolean on_press(GtkWidget*, GdkEventButton* ev, gpointer p) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1) return FALSE;
    activate(p);
    return TRUE;
}

gboolean on_key(GtkWidget*, GdkEventKey* ev, gpointer p) {
    if (ev->keyval != GDK_Return && ev->keyval != GDK_KP_Enter && ev->keyval != GDK_space) return FALSE;
    activate(p);
    return TRUE;
}

gboolean draw_focus(GtkWidget* w, GdkEventExpose*, gpointer) {
    if (gtk_widget_has_focus(w)) {
        GtkAllocation a;
        gtk_widget_get_allocation(w, &a);
        gtk_paint_focus(gtk_widget_get_style(w), gtk_widget_get_window(w), GTK_STATE_NORMAL,
                        nullptr, w, "button", 2, 2, a.width - 4, a.height - 4);
    }
    return FALSE;
}

// Wrap any widget in a tappable box. Event boxes avoid the GTK theme's bevels and hover
// states, which look muddy on e-ink.
GtkWidget* tappable(GtkWidget* child, Action fn, const std::string& arg, const char* bg, const std::string& name = "") {
    GtkWidget* eb = gtk_event_box_new();
    if (!name.empty()) g_targets.push_back({name, eb});
    gtk_widget_set_can_focus(eb, TRUE);
    paint(eb, bg);
    gtk_container_add(GTK_CONTAINER(eb), child);
    auto* data = new Btn{fn, arg};
    gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK);
    // Catch presses anywhere inside, even over nested event boxes (labels, fills).
    gtk_event_box_set_above_child(GTK_EVENT_BOX(eb), TRUE);
    g_signal_connect_data(eb, "button-press-event", G_CALLBACK(on_press), data,
                          [](gpointer d, GClosure*) { delete static_cast<Btn*>(d); }, (GConnectFlags)0);
    g_signal_connect(eb, "key-press-event", G_CALLBACK(on_key), data);
    g_signal_connect_after(eb, "expose-event", G_CALLBACK(draw_focus), nullptr);
    return eb;
}

// Bordered button: a black frame around a padded fill. The tap handler sits on the outermost
// box so the whole button, border included, is one hit target (nested event boxes would let
// the outer window swallow presses).
// primary = filled black with white text; secondary = white with black text.
GtkWidget* button(const std::string& text, Action fn, bool primary = false, const std::string& arg = "",
                  bool enabled = true) {
    GtkWidget* inner = gtk_alignment_new(0.5f, 0.5f, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(inner), px(30), px(30), px(16), px(16));
    gtk_container_add(GTK_CONTAINER(inner), label(text, 13, "bold", primary ? "#ffffff" : "#000000"));
    GtkWidget* fill = gtk_event_box_new();
    paint(fill, primary ? "#000000" : "#ffffff");
    gtk_container_add(GTK_CONTAINER(fill), inner);
    GtkWidget* border = gtk_alignment_new(0.5f, 0.5f, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(border), px(3), px(3), px(3), px(3));
    gtk_container_add(GTK_CONTAINER(border), fill);
    GtkWidget* frame = tappable(border, fn, arg, "#000000", enabled ? text : "");
    gtk_widget_set_sensitive(frame, enabled);
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
    pk::json pairing, req;
    try {
        std::ifstream f(U.dir + "/pairing.json");
        if (!f) {
            U.syncing = false;
            U.sync_msg = "Not paired";
            U.sync_detail = "On the computer, run pomo-kndl pair --host <computer LAN IP>, then copy pairing.json to this app's folder over USB.";
            return;
        }
        pairing = pk::json::parse(f);
        const auto host = pairing.at("host").get<std::string>();
        const auto port = pairing.value("port", 7419);
        if (host.empty() || port < 1 || port > 65535 || pairing.value("proto", 0) != 1 ||
            pairing.at("server_sha256").get<std::string>().empty() ||
            pairing.at("token").get<std::string>().empty() ||
            pairing.at("device_id").get<std::string>().empty())
            throw std::runtime_error("incomplete pairing");
        U.endpoint = host + ":" + std::to_string(port);
        req = U.app->landing_request(pairing);
    } catch (const std::exception&) {
        U.syncing = false;
        U.sync_msg = "Check pairing";
        U.sync_detail = "pairing.json is invalid or incomplete. Copy a valid pairing file from the computer, then try again. Your saved work is unchanged.";
        return;
    }
    U.sync_msg = "Syncing";
    U.sync_detail.clear();
    try {
        std::thread([pairing, req] {
            struct Result { pk::json reply; std::string error; };
            auto result = std::make_unique<Result>();
            try {
                result->reply = pk::tls_exchange(pairing, req);
            } catch (const std::exception& e) {
                result->error = e.what();
            }
            g_idle_add([](gpointer p) -> gboolean {
                std::unique_ptr<Result> r(static_cast<Result*>(p));
                if (!r->error.empty()) {
                    U.sync_detail = r->error;
                    sync_done("Sync failed");
                    return FALSE;
                }
                try {
                    U.app->apply_landing(r->reply);  // main thread owns the app state
                    U.app->set_last_sync("synced");
                    U.sync_detail.clear();
                    sync_done("Last sync OK");
                } catch (const std::exception& e) {
                    U.sync_detail = e.what();
                    sync_done("Sync failed");
                }
                return FALSE;
            }, result.release());
        }).detach();
    } catch (const std::exception& e) {
        U.syncing = false;
        U.sync_msg = "Sync failed";
        U.sync_detail = e.what();
    }
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
void a_connection(const std::string&) { U.connection_open = true; }
void a_back(const std::string&) { U.connection_open = false; }
void a_prev(const std::string&) { if (U.page) --U.page; }
void a_next(const std::string&) { ++U.page; }
// State is saved after every action, so exiting never loses work. A running timer keeps
// counting (it is stored as a start time) and is reconciled when the app is reopened.
void a_exit(const std::string&) { gtk_main_quit(); }

std::string mins_left(int64_t sec) { return std::to_string((sec + 59) / 60); }  // countdown: round up
std::string mins_done(int64_t sec) { return std::to_string(sec / 60); }         // focused: never overstate

// A successful landing is history, not proof of an ongoing connection.
std::string status_text() {
    if (U.syncing) return "Syncing";
    if (!U.sync_detail.empty()) return U.sync_msg;
    if (U.app->pending_entries()) return std::to_string(U.app->pending_entries()) + " to sync";
    return U.sync_msg;
}

// ---- views ----------------------------------------------------------------------------------
const size_t kMaxTasks = 5;  // fixed-height rows leave room for paging and sync guidance

GtkWidget* task_row(const pk::Task& t) {
    GtkWidget* h = gtk_hbox_new(FALSE, px(24));
    const char* fg = t.done_locally ? "#555555" : "#000000";
    GtkWidget* mark = label(t.done_locally ? "✓" : "·", 14, "normal", fg);
    gtk_widget_set_size_request(mark, px(48), -1);
    gtk_box_pack_start(GTK_BOX(h), mark, FALSE, FALSE, 0);
    GtkWidget* title = label(t.title, 14, "normal", fg, t.done_locally, 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    // Let the box allocate the title after reserving the marker and review badge.
    gtk_widget_set_size_request(title, 0, -1);
    gtk_box_pack_start(GTK_BOX(h), title, TRUE, TRUE, 0);
    if (t.review) gtk_box_pack_end(GTK_BOX(h), label("Review", 10, "bold"), FALSE, FALSE, 0);
    GtkWidget* pad = gtk_alignment_new(0, 0.5f, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(pad), px(44), px(44), px(8), px(8));
    gtk_container_add(GTK_CONTAINER(pad), h);
    // Tap an open task to start it; tap a crossed-off task to reopen it.
    return tappable(pad, t.done_locally ? a_reopen : a_start, t.id, "#ffffff", t.title);
}

void build_list(GtkWidget* box) {
    auto& ts = U.app->tasks();
    gtk_box_pack_start(GTK_BOX(box), label("Your tasks", 20, "bold", "#000000", false, 0.0f), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(14), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label("Choose one for " + std::to_string(U.app->focus_min()) + " minutes of focus", 11,
                                        "normal", "#444444", false, 0.0f), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(36), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), rule(2), FALSE, FALSE, 0);
    if (ts.empty()) {
        gtk_box_pack_start(GTK_BOX(box), spacer(120), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), label("No tasks yet", 20, "bold"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), spacer(24), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), centered("Add a task on your computer with\npomo-kndl add \"Write an outline\"\nthen sync it here.", 12,
                                                  "normal", "#444444"),
                           FALSE, FALSE, 0);
    } else {
        const size_t pages = (ts.size() + kMaxTasks - 1) / kMaxTasks;
        U.page = std::min(U.page, pages - 1);
        for (size_t i = U.page * kMaxTasks; i < std::min(ts.size(), (U.page + 1) * kMaxTasks); ++i) {
            gtk_box_pack_start(GTK_BOX(box), task_row(ts[i]), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), rule(1, "#999999"), FALSE, FALSE, 0);
        }
        if (pages > 1) {
            GtkWidget* nav = gtk_hbox_new(FALSE, px(20));
            if (U.page) gtk_box_pack_start(GTK_BOX(nav), link("Previous", a_prev), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(nav), label(std::to_string(U.page + 1) + " / " + std::to_string(pages), 11), TRUE, TRUE, 0);
            if (U.page + 1 < pages) gtk_box_pack_end(GTK_BOX(nav), link("Next", a_next), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), nav, FALSE, FALSE, px(12));
        }
    }
    GtkWidget* sync = gtk_vbox_new(FALSE, px(18));
    gtk_box_pack_start(GTK_BOX(sync), centered("Work offline. Sync when you're ready.", 10, "normal", "#444444"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sync), button(U.syncing ? "Syncing…" : "Sync with computer", do_sync, false, "", !U.syncing), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), sync, FALSE, FALSE, px(24));
}

void build_connection(GtkWidget* box) {
    gtk_box_pack_start(GTK_BOX(box), label("Computer sync", 20, "bold", "#000000", false, 0), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(40), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), centered(U.sync_msg, 16, "bold"), FALSE, FALSE, 0);
    if (!U.endpoint.empty()) gtk_box_pack_start(GTK_BOX(box), centered(U.endpoint, 11, "normal", "#444444"), FALSE, FALSE, px(18));
    const size_t n = U.app->pending_entries();
    gtk_box_pack_start(GTK_BOX(box), centered(std::to_string(n) + (n == 1 ? " saved change waiting to sync" : " saved changes waiting to sync"), 12), FALSE, FALSE, px(24));
    if (!U.sync_detail.empty()) {
        // Keep raw backend diagnostics from forcing a window wider than the device.
        std::string detail = U.sync_detail.substr(0, 360);
        gtk_box_pack_start(GTK_BOX(box), centered(detail, 11, "normal", "#333333"), FALSE, FALSE, px(24));
    }
    const std::string help = U.sync_msg == "Check pairing" || U.sync_msg == "Not paired"
        ? "Pairing is done over USB. Tasks and timer changes stay saved on this Kindle."
        : "Keep both devices on the same Wi-Fi and run pomo-kndl serve on the computer. Sync is a one-time exchange, not a live connection.";
    gtk_box_pack_start(GTK_BOX(box), centered(help, 12, "normal", "#333333"), FALSE, FALSE, px(24));
    gtk_box_pack_end(GTK_BOX(box), row({button("Back", a_back), button(U.syncing ? "Syncing…" : "Sync now", do_sync, true, "", !U.syncing)}), FALSE, FALSE, px(32));
}

void build_session(GtkWidget* box) {
    auto& s = U.app->session();
    using P = pk::Phase;
    const bool in_break = s.phase == P::Break;

    std::string kicker = in_break ? "Take a break" : s.phase == P::Ended ? "Session complete" : "Focusing";
    if (s.phase == P::Paused) kicker = s.recovered ? "Paused after restart" : "Paused";
    if (s.phase == P::Ended && s.interrupted) kicker = "Session ended early";

    gtk_box_pack_start(GTK_BOX(box), spacer(40), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label(kicker, 12, "normal", "#444444"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(18), FALSE, FALSE, 0);
    if (!in_break) gtk_box_pack_start(GTK_BOX(box), session_title(s.title), FALSE, FALSE, 0);
    else gtk_box_pack_start(GTK_BOX(box), centered("Step away from the screen", 18, "normal", "#333333"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(60), FALSE, FALSE, 0);

    std::string big, unit;
    double frac = 0;
    if (s.phase == P::Focus || s.phase == P::Paused) {
        big = mins_left(U.app->remaining_seconds());
        unit = s.phase == P::Paused ? "minutes remaining" : "minutes left";
        frac = s.planned_min > 0 ? (double)U.app->focus_seconds() / (s.planned_min * 60) : 0;
    } else if (in_break) {
        big = mins_left(U.app->remaining_seconds());
        unit = "minutes to recharge";
        frac = s.break_min > 0 ? 1.0 - (double)U.app->remaining_seconds() / (s.break_min * 60) : 0;
    } else {
        big = s.accumulated > 0 && s.accumulated < 60 ? "<1" : mins_done(s.accumulated);
        unit = "minutes focused";
        frac = s.planned_min > 0 ? (double)s.accumulated / (s.planned_min * 60) : 0;
    }
    gtk_box_pack_start(GTK_BOX(box), label(big, 90, "bold"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label(unit, 16, "normal", "#333333"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(50), FALSE, FALSE, 0);

    GtkWidget* bar = gtk_alignment_new(0.5f, 0.5f, 0.8f, 1);
    gtk_container_add(GTK_CONTAINER(bar), progress(frac));
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spacer(60), FALSE, FALSE, 0);

    GtkWidget* r = nullptr;
    switch (s.phase) {
        case P::Focus: r = row({button("Pause", a_pause, true), button("End session", a_end)}); break;
        case P::Paused: r = row({button("Resume", a_resume, true), button("End session", a_end)}); break;
        case P::Ended:
            r = gtk_vbox_new(FALSE, px(24));
            gtk_box_pack_start(GTK_BOX(r), button("Complete task", a_done, true), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(r), row({button("Keep open", a_later), button("Take a break", a_break)}), FALSE, FALSE, 0);
            break;
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
                           centered("A finished session doesn't have to mean a finished task.", 10, "normal", "#444444"),
                           FALSE, FALSE, 0);
    }
}

// ---- frame ----------------------------------------------------------------------------------
std::string signature() {
    auto& s = U.app->session();
    std::string sig = std::to_string((int)s.phase) + "|" + status_text() + "|" + U.sync_detail + "|" +
                      (U.connection_open ? "connection" : "main") + std::to_string(U.page);
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
    GtkWidget* status = label(status_text(), 10, "normal", "#333333", false, 1.0f);
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    gtk_label_set_attributes(GTK_LABEL(status), attrs);
    pango_attr_list_unref(attrs);
    GtkWidget* pad = gtk_alignment_new(1, 0.5, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(pad), px(18), px(18), px(20), 0);
    gtk_container_add(GTK_CONTAINER(pad), status);
    gtk_box_pack_end(GTK_BOX(h), tappable(pad, a_connection, "", "#ffffff", "Sync status"), FALSE, FALSE, 0);
    return h;
}

GtkWidget* footer() {
    GtkWidget* v = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), rule(1, "#999999"), FALSE, FALSE, 0);
    GtkWidget* h = gtk_hbox_new(FALSE, 0);
    auto td = U.app->today();
    std::string today = "Today  " + std::to_string(td.first) + (td.first == 1 ? " session" : " sessions") + " · " +
                        std::to_string(td.second) + " min";
    gtk_box_pack_start(GTK_BOX(h), label(today, 10, "normal", "#333333", false, 0.0f), TRUE, TRUE, 0);
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
    gtk_box_pack_start(GTK_BOX(U.body), spacer(40), FALSE, FALSE, 0);

    GtkWidget* mid = gtk_vbox_new(FALSE, 0);
    if (U.connection_open) build_connection(mid);
    else if (U.app->session().phase == pk::Phase::Idle) build_list(mid);
    else build_session(mid);
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
        U.sync_msg = "Save failed";
        U.sync_detail = e.what();
    }
    render();
    return TRUE;
}

}  // namespace

// POMO_KNDL_SCRIPT="tap:Pause;shot:a.png;tap:End;shot:b.png" drives the UI by button label and
// writes PNG snapshots, then exits. Used for README screenshots and visual checks; not on device.
static std::vector<std::string> g_script;
static size_t g_step = 0;
static int g_script_result = 0;
static int g_sync_waits = 0;

static bool has_text(GtkWidget* w, const std::string& text) {
    if (GTK_IS_LABEL(w) && text == gtk_label_get_text(GTK_LABEL(w))) return true;
    if (!GTK_IS_CONTAINER(w)) return false;
    GList* children = gtk_container_get_children(GTK_CONTAINER(w));
    bool found = false;
    for (GList* p = children; p && !found; p = p->next) found = has_text(GTK_WIDGET(p->data), text);
    g_list_free(children);
    return found;
}

static gboolean script_fail(const std::string& message) {
    fprintf(stderr, "script: %s\n", message.c_str());
    g_script_result = 1;
    gtk_main_quit();
    return FALSE;
}

static bool layout_fits(GtkWidget* parent) {
    if (!GTK_IS_CONTAINER(parent)) return true;
    GtkAllocation bounds;
    gtk_widget_get_allocation(parent, &bounds);
    GList* children = gtk_container_get_children(GTK_CONTAINER(parent));
    bool fits = true;
    for (GList* p = children; p && fits; p = p->next) {
        GtkWidget* child = GTK_WIDGET(p->data);
        if (!gtk_widget_get_visible(child)) continue;
        GtkAllocation a;
        gtk_widget_get_allocation(child, &a);
        int x = 0, y = 0;
        gtk_widget_translate_coordinates(child, parent, 0, 0, &x, &y);
        fits = x >= 0 && y >= 0 && x + a.width <= bounds.width && y + a.height <= bounds.height && layout_fits(child);
        if (!fits) fprintf(stderr, "layout overflow: %s in %s\n", G_OBJECT_TYPE_NAME(child), G_OBJECT_TYPE_NAME(parent));
    }
    g_list_free(children);
    return fits;
}

static gboolean script_step(gpointer) {
    while (gtk_events_pending()) gtk_main_iteration();
    if (g_step >= g_script.size()) {
        gtk_main_quit();
        return FALSE;
    }
    std::string cmd = g_script[g_step++];
    if (cmd.rfind("tap:", 0) == 0 || cmd.rfind("key:", 0) == 0) {
        std::string want = cmd.substr(4);
        bool hit = false;
        for (const auto& t : g_targets)
            if (t.name == want) {
                // Dispatch the same GTK signal as a touch, including callback error handling.
                GtkWidget* widget = t.widget;
                const bool keyboard = cmd.rfind("key:", 0) == 0;
                GdkEvent* event = gdk_event_new(keyboard ? GDK_KEY_PRESS : GDK_BUTTON_PRESS);
                event->any.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(widget)));
                if (keyboard) {
                    gtk_widget_grab_focus(widget);
                    event->key.keyval = GDK_Return;
                } else event->button.button = 1;
                gtk_widget_event(widget, event);
                gdk_event_free(event);
                hit = true;
                break;
            }
        if (!hit) return script_fail("no target '" + want + "'");
    } else if (cmd == "wait-sync") {
        if (U.syncing) {
            if (++g_sync_waits > 100) return script_fail("sync did not finish within 30 seconds");
            --g_step;
        } else g_sync_waits = 0;
    } else if (cmd.rfind("expect:", 0) == 0) {
        if (!has_text(U.win, cmd.substr(7))) return script_fail("missing text '" + cmd.substr(7) + "'");
    } else if (cmd.rfind("absent:", 0) == 0) {
        if (has_text(U.win, cmd.substr(7))) return script_fail("unexpected text '" + cmd.substr(7) + "'");
    } else if (cmd == "fit") {
        GtkAllocation a;
        gtk_widget_get_allocation(U.win, &a);
        if (a.width != U.width || a.height != U.height)
            return script_fail("window grew beyond requested geometry: " + std::to_string(a.width) + "x" + std::to_string(a.height));
        if (!layout_fits(U.win)) return script_fail("content overlaps or extends beyond its container");
    } else if (cmd.rfind("shot:", 0) == 0) {
        // Offscreen window: readable on every backend (Quartz cannot read back on-screen windows).
        // Looked up at run time so the device binary never hard-depends on a newer GTK symbol.
        using GetPixbuf = GdkPixbuf* (*)(GtkWidget*);
        gpointer sym = nullptr;
        GModule* self = g_module_open(nullptr, (GModuleFlags)0);
        if (self) g_module_symbol(self, "gtk_offscreen_window_get_pixbuf", &sym);
        GdkPixbuf* pb = sym ? reinterpret_cast<GetPixbuf>(sym)(U.win) : nullptr;
        if (!pb) return script_fail("offscreen snapshot unavailable");
        GError* error = nullptr;
        const bool saved = gdk_pixbuf_save(pb, cmd.substr(5).c_str(), "png", &error, nullptr);
        g_object_unref(pb);
        if (!saved) {
            std::string message = error ? error->message : "snapshot failed";
            if (error) g_error_free(error);
            return script_fail(message);
        }
    } else return script_fail("unknown command '" + cmd + "'");
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
    U.sync_msg = U.app->last_sync() == "never" ? "Not synced yet" : "Last sync OK";
    if (!std::ifstream(U.dir + "/pairing.json")) {
        U.sync_msg = "Not paired";
        U.sync_detail = "Copy pairing.json from your computer to this app's folder over USB, then sync.";
    }

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
    U.width = w;
    U.height = h;
    U.scale = w * 100 / 1236;
    if (U.scale < 40) U.scale = 40;
    U.content_w = w - 2 * px(56);
    gtk_window_set_default_size(GTK_WINDOW(U.win), w, h);
    gtk_widget_set_size_request(U.win, w, h);
    paint(U.win, "#ffffff");
    g_signal_connect(U.win, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    render(true);
    const char* script = getenv("POMO_KNDL_SCRIPT");
    if (script) {
        std::string s = script, part;
        for (char c : s + ";") {
            if (c == ';') {
                if (!part.empty()) g_script.push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
        g_timeout_add(800, script_step, nullptr);
    } else g_timeout_add_seconds(10, on_tick, nullptr);
    // Preview scripts normally stay offline, but can exercise the same startup path explicitly.
    if (!script || getenv("POMO_KNDL_AUTO_SYNC")) {
        do_sync("");  // fetch new computer tasks even when the local log is empty
        render();
    }
    gtk_main();
    return g_script_result;
}
