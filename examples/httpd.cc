#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#if COTAMER_HTTPD_HAS_WSLAY
# include "cotamer/websocket.hh"
#endif

namespace cot = cotamer;

#if COTAMER_HTTPD_HAS_WSLAY
cot::task<> run_ws_echo(cot::ws_stream ws, cot::fd cfd) {
    while (true) {
        auto m = co_await ws.receive(cfd);
        if (std::holds_alternative<cot::ws_close>(m)) {
            break;
        }
        auto& msg = std::get<cot::ws_message>(m);
        if (msg.opcode == cot::ws_opcode::text) {
            co_await ws.send_text(cfd, msg.payload);
        } else {
            co_await ws.send_binary(cfd, msg.payload);
        }
    }
    co_await ws.close(cfd);
}
#endif

cot::task<> run_one(cot::fd cfd, double delay) {
    cot::http_parser hp(cot::http_parser::server);
    cot::http_message req, res;

    do {
        auto req = co_await hp.receive(cfd);
        if (!hp.ok()) {
            break;
        }
#if COTAMER_HTTPD_HAS_WSLAY
        if (cot::is_ws_upgrade_request(req)) {
            std::cerr << req.url() << " [ws upgrade]\n";
            try {
                auto ws = co_await cot::ws_upgrade(hp, cfd, req);
                co_await run_ws_echo(std::move(ws), std::move(cfd));
            } catch (const cot::ws_error& e) {
                std::cerr << "ws_upgrade error: " << e.what() << '\n';
            }
            co_return;
        }
#endif
        std::cerr << req.url() << '\n';
        co_await cot::after(std::chrono::duration<double>(delay));
        res.clear();
        std::string s = std::format("URL: {}\n", req.url());
        for (auto it = req.header_begin(); it != req.header_end(); ++it) {
            s += std::format("Header: {}: {}\n", it.name(), it.value());
        }
        for (auto it = req.search_param_begin(); it != req.search_param_end(); ++it) {
            s += std::format("Param: {}: {}\n", it.name(), it.value());
        }
        res.status_code(200)
            .date_header("Date", time(NULL))
            .header("Content-Type", "text/plain")
            .body(s);
        co_await hp.send(cfd, std::move(res));
    } while (hp.should_keep_alive());
}

cot::task<> start(std::string address, double delay) {
    auto lfd = co_await cot::tcp_listen(address);
    while (true) {
        run_one(co_await cot::tcp_accept(lfd), delay).detach();
    }
}

static void usage() {
    fprintf(stderr, "Usage: tamer-httpd [-p PORT] [-d DELAY]\n");
}

int main(int argc, char* argv[]) {
    int opt;
    int port = 11111;
    double delay = 0;
    while ((opt = getopt(argc, argv, "hp:t:")) != -1) {
        switch (opt) {
        case 'h':
            usage();
            exit(0);
        case 'p':
            port = strtol(optarg, 0, 0);
            break;
        case 't':
            delay = strtod(optarg, 0);
            break;
        case '?':
        usage:
            usage();
            exit(1);
        }
    }

    if (optind != argc) {
        goto usage;
    }

    cot::set_clock(cot::clock::real_time);
    cot::task<> t = start(std::format("localhost:{}", port), delay);
    cot::loop();
}
