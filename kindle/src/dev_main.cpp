// Headless driver for the Kindle core: used for Mac testing and as a fallback on device.
//   pomo-kndl-dev <dir> tasks|start <n>|pause|resume|end|done|later|break|endbreak|sync|state
#include <cstdio>
#include <fstream>
#include <iostream>

#include "core.hpp"
#include "net.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dir> <cmd> [arg]\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1], cmd = argv[2];
    pk::App app(dir, pk::Clock::system());
    try {
        app.load();
        if (cmd == "tasks") {
            int i = 1;
            for (auto& t : app.tasks())
                printf("%d [%c] %s%s\n", i++, t.done_locally ? 'x' : ' ', t.title.c_str(),
                       t.review ? " (review)" : "");
        } else if (cmd == "start" && argc > 3) {
            size_t n = std::stoul(argv[3]);
            if (n < 1 || n > app.tasks().size()) throw std::runtime_error("no such task");
            app.start(app.tasks()[n - 1].id);
        } else if (cmd == "pause") app.pause();
        else if (cmd == "resume") app.resume();
        else if (cmd == "end") app.end();
        else if (cmd == "done") app.finish_done();
        else if (cmd == "later") app.finish_later();
        else if (cmd == "break") app.finish_break();
        else if (cmd == "endbreak") app.end_break();
        else if (cmd == "sync") {
            std::ifstream f(dir + "/pairing.json");
            if (!f) throw std::runtime_error("not paired: pairing.json missing");
            auto p = pk::json::parse(f);
            app.apply_landing(pk::tls_exchange(p, app.landing_request(p)));
            app.set_last_sync("ok");
            printf("landed: plan %lld, %zu pending\n", (long long)app.plan_version(), app.pending_entries());
        } else if (cmd == "state") {
            app.tick();
            printf("plan %lld, pending %zu, remaining %llds, focus %llds\n", (long long)app.plan_version(),
                   app.pending_entries(), (long long)app.remaining_seconds(), (long long)app.focus_seconds());
        } else {
            fprintf(stderr, "unknown command\n");
            return 2;
        }
    } catch (std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
