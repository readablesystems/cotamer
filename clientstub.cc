#include "rpcgame.hh"
#include "cotamer.hh"
#include "message_stream.hh"

namespace cot = cotamer;

class RPCGameClient {
    message_stream sender_;
    message_stream recver_;

public:
    RPCGameClient(cot::fd fd)
        : sender_(fd, message_stream::sender),
          recver_(fd, message_stream::receiver) {
    }

    cot::task<> send_try(const char* name, size_t name_len, uint64_t count) {
        char buf[8 * BUFSIZ];
        assert(name_len + 16 <= 8 * BUFSIZ);
        memcpy(buf, &_serial, 8);
        memcpy(buf + 8, &count, 8);
        memcpy(buf + 16, name, name_len);
        ++_serial;
        co_await sender_.send({buf, buf + 16 + name_len});
        auto m = co_await recver_.recv();
        assert(m.size() == 8);
        uint64_t value;
        memcpy(&value, m.data(), 8);
        client_recv_try_response(value);
    }

    cot::task<> finish() {
        co_await sender_.send(std::string());
        auto m = co_await recver_.recv();
        auto space = m.find(' ');
        assert(space != std::string::npos);
        auto s_client_checksum = m.substr(0, space),
            s_server_checksum = m.substr(space + 1);

        // parse response
        std::string my_client_checksum = client_checksum(),
            my_server_checksum = server_checksum();
        bool ok = my_client_checksum == s_client_checksum
            && my_server_checksum == s_server_checksum;
        std::cout << "client checksums: "
            << my_client_checksum << "/" << s_client_checksum
            << "\nserver checksums: "
            << my_server_checksum << "/" << s_server_checksum
            << "\nmatch: " << (ok ? "true\n" : "false\n");
    }

    void maybe_poll() {
        if (sender_.size() > 8192) {
            cot::poll();
        }
    }

private:
    uint64_t _serial = 1;
};


static std::unique_ptr<RPCGameClient> client;

static cot::task<> client_connect_coroutine(std::string address, cot::fd& f) {
    f = co_await cot::tcp_connect(address);
}

void client_connect(std::string address) {
    cot::set_clock(cot::clock::real_time);
    cotamer::fd cfd;
    client_connect_coroutine(address, cfd).detach();
    cot::loop();
    client = std::make_unique<RPCGameClient>(std::move(cfd));
}

void client_send_try(const char* name, size_t name_len, uint64_t count) {
    client->send_try(name, name_len, count).detach();
    client->maybe_poll();
}

void client_finish() {
    client->finish().detach();
    cot::loop();
}
