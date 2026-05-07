#include "cotamer/cotamer.hh"
#include "cotamer/websocket.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

namespace cot = cotamer;

cot::task<> run_client(std::string addr, std::string host, std::string path,
                       std::string message) {
    auto cfd = co_await cot::tcp_connect(addr);
    auto ws = cot::ws_stream::make_client(host, path);
    co_await ws.handshake(cfd);
    std::cerr << "ws-echo: handshake ok\n";

    co_await ws.send_text(cfd, message);
    std::cerr << "ws-echo: sent " << message.size() << " bytes\n";

    auto m = co_await ws.receive(cfd);
    if (auto* msg = std::get_if<cot::ws_message>(&m)) {
        std::cout << msg->payload << '\n';
    } else {
        auto& c = std::get<cot::ws_close>(m);
        std::cerr << "ws-echo: peer closed (" << c.code << "): "
                  << c.reason << '\n';
    }

    co_await ws.close(cfd);
}

static void usage() {
    fprintf(stderr,
        "Usage: ws-echo [-h HOST] [-p PORT] [-u PATH] [-m MESSAGE]\n");
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 11111;
    std::string path = "/";
    std::string message = "hello, websocket";

    int opt;
    while ((opt = getopt(argc, argv, "h:p:u:m:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'p': port = strtol(optarg, 0, 0); break;
        case 'u': path = optarg; break;
        case 'm': message = optarg; break;
        default:
            usage();
            return 1;
        }
    }

    cot::set_clock(cot::clock::real_time);
    auto addr = std::format("{}:{}", host, port);
    auto t = run_client(addr, host, path, message);
    cot::loop();
    return 0;
}
