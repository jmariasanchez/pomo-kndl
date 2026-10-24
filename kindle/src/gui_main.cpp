// pomo-kndl Kindle UI: GTK 2, black on white, two views (task list, session).
#include <gtk/gtk.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

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
};
Ui U;

void render(bool force = false);

GtkWidget* label(const std::string& text, int size_pt, bool bold = false, bool strike = false) {
    GtkWidget* l = gtk_label_new(nullptr);
    char* esc = g_markup_escape_text(text.c_str(), -1);
    char* m = g_strdup_printf("<span font_desc='Sans %s%d'%s>%s</span>", bold ? "Bold " : "", size_pt,
                              strike ? " strikethrough='true'" : "", esc);
    gtk_label_set_markup(GTK_LABEL(l), m);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    g_free(esc);
    g_free(m);
    return l;
}

using Action = void (*)(const std::string&);
struct Btn { Action fn; std::string arg; };

void on_click(GtkWidget*, gpointer p) {
    auto* b = static_cast<Btn*>(p);
    try {
        b->fn(b->arg);
    } catch (std::exception& e) {
        U.sync_msg = std::string("error: ") + e.what();
    }
    render();
}

GtkWidget* button(const std::string& text, Action fn, const std::string& arg = "") {
    GtkWidget* b = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(b), label(text, 14, true));
    gtk_widget_set_size_request(b, -1, 110);  // generous touch target
    auto* data = new Btn{fn, arg};
    g_signal_connect_data(b, "clicked", G_CALLBACK(on_click), data,
                          [](gpointer d, GClosure*) { delete static_cast<Btn*>(d); }, (GConnectFlags)0);
    return b;
}

GtkWidget* row(std::initializer_list<GtkWidget*> ws) {
    GtkWidget* h = gtk_hbox_new(TRUE, 24);
    for (auto* w : ws) gtk_box_pack_start(GTK_BOX(h), w, TRUE, TRUE, 0);
    return h;
}

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
    U.sync_msg = "syncing...";
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

std::string mins(int64_t sec) { return std::to_string((sec + 59) / 60) + " min"; }  // countdown: round up
std::string mins_done(int64_t sec) { return std::to_string(sec / 60) + " min"; }      // focused: never overstate

void build_list(GtkWidget* box) {
    auto& ts = U.app->tasks();
    if (ts.empty()) gtk_box_pack_start(GTK_BOX(box), label("No tasks. Add them with pomo-kndl on the computer, then Sync.", 12), FALSE, FALSE, 20);
    int shown = 0;
    for (auto& t : ts) {
        if (shown++ >= 8) break;  // short list keeps targets big
        std::string text = t.title + (t.review ? "  (review)" : "");
        GtkWidget* b = gtk_button_new();
        gtk_container_add(GTK_CONTAINER(b), label(text, 13, false, t.done_locally));
        gtk_widget_set_size_request(b, -1, 100);
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        auto* data = new Btn{t.done_locally ? a_reopen : a_start, t.id};
        g_signal_connect_data(b, "clicked", G_CALLBACK(on_click), data,
                              [](gpointer d, GClosure*) { delete static_cast<Btn*>(d); }, (GConnectFlags)0);
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 4);
    }
    GtkWidget* bottom = row({button(U.syncing ? "Syncing" : "Sync", do_sync), button("Exit", a_exit)});
    gtk_box_pack_end(GTK_BOX(box), bottom, FALSE, FALSE, 10);
}

void build_session(GtkWidget* box) {
    auto& s = U.app->session();
    gtk_box_pack_start(GTK_BOX(box), label(s.title, 16, true), FALSE, FALSE, 30);
    using P = pk::Phase;
    std::string big, sub;
    if (s.phase == P::Focus || s.phase == P::Paused) {
        big = mins(U.app->remaining_seconds());
        sub = s.phase == P::Focus ? "focus" : (s.recovered ? "paused after restart" : "paused");
    } else if (s.phase == P::Break) {
        big = mins(U.app->remaining_seconds());
        sub = "break";
    } else {
        big = mins_done(s.accumulated);
        sub = s.interrupted ? "focused (ended early)" : "focused";
    }
    gtk_box_pack_start(GTK_BOX(box), label(big, 48, true), FALSE, FALSE, 10);
    gtk_box_pack_start(GTK_BOX(box), label(sub, 16), FALSE, FALSE, 0);
    GtkWidget* r = nullptr;
    switch (s.phase) {
        case P::Focus: r = row({button("Pause", a_pause), button("End", a_end)}); break;
        case P::Paused: r = row({button("Resume", a_resume), button("End", a_end)}); break;
        case P::Ended: r = row({button("Done", a_done), button("Continue later", a_later), button("Break", a_break)}); break;
        case P::Break: r = row({button("End break", a_end_break)}); break;
        default: break;
    }
    if (r) gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 60);
    // Leaving mid-session is safe: the timer is a saved start time and keeps running.
    gtk_box_pack_end(GTK_BOX(box), button("Exit (timer keeps running)", a_exit), FALSE, FALSE, 10);
}

std::string signature() {
    auto& s = U.app->session();
    std::string sig = std::to_string((int)s.phase) + "|" + U.sync_msg + "|" + std::to_string(U.syncing.load());
    if (s.phase == pk::Phase::Idle) {
        for (auto& t : U.app->tasks()) sig += t.id + t.title + (t.done_locally ? "x" : "") + (t.review ? "r" : "");
    } else {
        sig += std::to_string((U.app->remaining_seconds() + 59) / 60);
    }
    auto td = U.app->today();
    return sig + "|" + std::to_string(td.first) + "," + std::to_string(td.second);
}

void render(bool force) {
    std::string sig = signature();
    if (!force && sig == U.shown) return;  // nothing visible changed: no e-ink refresh
    U.shown = sig;
    if (U.body) gtk_widget_destroy(U.body);
    U.body = gtk_vbox_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(U.body), 40);

    GtkWidget* head = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), label("pomo-kndl", 11, true), FALSE, FALSE, 0);
    std::string st = U.sync_msg;
    if (U.app->pending_entries() && st == "synced") st = "pending";
    gtk_box_pack_end(GTK_BOX(head), label(st.substr(0, 40), 10), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(U.body), head, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(U.body), gtk_hseparator_new(), FALSE, FALSE, 16);

    GtkWidget* mid = gtk_vbox_new(FALSE, 0);
    if (U.app->session().phase == pk::Phase::Idle) build_list(mid); else build_session(mid);
    gtk_box_pack_start(GTK_BOX(U.body), mid, TRUE, TRUE, 0);

    auto td = U.app->today();
    gtk_box_pack_end(GTK_BOX(U.body),
                     label("Today: " + std::to_string(td.first) + " sessions · " + std::to_string(td.second) + " min", 11),
                     FALSE, FALSE, 0);
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

    U.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    // Kindle window manager (awesome) reads placement from the title: app layer, application role,
    // Kindle top bar (PC:T), id without underscores, portrait (O:U).
    gtk_window_set_title(GTK_WINDOW(U.win), "L:A_N:application_PC:T_ID:com.pomokndl.app_O:U");
    GdkScreen* scr = gdk_screen_get_default();
    gtk_window_set_default_size(GTK_WINDOW(U.win), gdk_screen_get_width(scr), gdk_screen_get_height(scr));
    GdkColor white;
    gdk_color_parse("white", &white);
    gtk_widget_modify_bg(U.win, GTK_STATE_NORMAL, &white);
    g_signal_connect(U.win, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    render(true);
    g_timeout_add_seconds(10, on_tick, nullptr);
    if (U.app->pending_entries() || U.app->last_sync() == "never") do_sync("");  // try once at launch
    gtk_main();
    return 0;
}
