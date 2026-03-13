#include "rpcgame.hh"
#include "cotamer/cotamer.hh"
#include "message_stream.hh"

namespace cot = cotamer;
using namespace std::chrono_literals;

class RPCGameServer {
    message_stream sender_;
    message_stream recver_;
public:
    RPCGameServer(cot::fd f)
        : sender_(f, message_stream::sender),
          recver_(f, message_stream::receiver) {
    }

    cot::task<> handle_try(const std::string& m) {
        assert(m.size() > 16);
        uint64_t serial, count;
        memcpy(&serial, m.data(), 8);
        memcpy(&count, m.data() + 8, 8);
        uint64_t value = server_process_try(serial, m.data() + 16, m.size() - 16, count);
        co_await sender_.send(&value, 8);
    }

    cot::task<> loop() {
        while (true) {
            auto m = co_await recver_.recv();
            if (m == std::string()) {
                break;
            } else {
                co_await handle_try(m);
            }
        }
        co_await sender_.send(client_checksum() + " " + server_checksum());
        co_await cot::after(100ms);
        cot::clear();
    }

    static cot::task<> loop(cot::fd f) {
        RPCGameServer self(std::move(f));
        co_await self.loop();
    }
};

static cot::task<> server_listen_coroutine(std::string address) {
    auto lfd = co_await cot::tcp_listen(address);
    while (true) {
        auto cfd = co_await cot::accept(lfd);
        RPCGameServer::loop(std::move(cfd)).detach();
    }
}

void server_start(std::string address) {
    cot::set_clock(cot::clock::real_time);
    cotamer::fd lfd;
    server_listen_coroutine(address).detach();
    cot::loop();
    std::cout << "Server exiting\n";
}
