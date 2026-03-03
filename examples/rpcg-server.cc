#include <cassert>
#include <condition_variable>
#include <getopt.h>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <getopt.h>
#include "rpcgame.hh"

namespace {

class rpc_server {
public:
    rpc_server();
    ~rpc_server();

    inline uint64_t process_try(uint64_t serial,
                                const char* name, size_t name_len,
                                uint64_t value);

    enum endpoint {
        client_type = 0, server_type = 1
    };
    inline std::string checksum(endpoint);

private:
    XXH3_state_t* _ctx[2];
    uint64_t _count;
    std::string _hash[2];
    uint64_t _want_serial = 1;
    bool _done = false;

    std::mutex _mutex;
    std::condition_variable _cv;

    NONCOPYABLE(rpc_server);
};

rpc_server::rpc_server() {
    _ctx[0] = XXH3_createState();
    XXH3_64bits_reset(_ctx[0]);
    _ctx[1] = XXH3_createState();
    XXH3_64bits_reset(_ctx[1]);
}

rpc_server::~rpc_server() {
    XXH3_freeState(_ctx[0]);
    XXH3_freeState(_ctx[1]);
}

uint64_t rpc_server::process_try(uint64_t serial,
                                 const char* name, size_t name_len,
                                 uint64_t value) {
    std::unique_lock<std::mutex> guard(_mutex);
    _cv.wait(guard, [this, serial] () { return serial == _want_serial; });
    ++_want_serial;
    assert(!_done);

    XXH3_64bits_update(_ctx[client_type], name, name_len);
    XXH3_64bits_update_uint64(_ctx[client_type], value);

    // compute response
    uint64_t response = XXH3_64bits(name, name_len) + value + _count;
    ++_count;

    XXH3_64bits_update_uint64(_ctx[server_type], response);

    guard.unlock();
    _cv.notify_all();
    return response;
}

inline std::string rpc_server::checksum(endpoint ep) {
    _done = true;
    return XXH3_64bits_hexdigest(_ctx[ep]);
}

rpc_server rpcc;

}


// connectors required by `serverstub.cc`

uint64_t server_process_try(uint64_t serial, const char* name, size_t name_len,
                            uint64_t value) {
    return rpcc.process_try(serial, name, name_len, value);
}

std::string client_checksum() {
    return rpcc.checksum(rpc_server::client_type);
}

std::string server_checksum() {
    return rpcc.checksum(rpc_server::server_type);
}


// main

int main(int argc, char* const argv[]) {
    bool all = false;
    int port = 29381;
    int ch;
    while ((ch = getopt(argc, argv, "ap:")) != -1) {
        if (ch == 'p') {
            port = from_str_chars<uint16_t>(std::string(optarg));
        } else if (ch == 'a') {
            all = true;
        }
    }

    if (all) {
        server_start(std::format("0.0.0.0:{}", port));
    } else {
        server_start(std::format("localhost:{}", port));
    }
}
