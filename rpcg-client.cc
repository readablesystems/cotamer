#include <sys/mman.h>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include "rpcgame.hh"

using steady_time_point = std::chrono::time_point<std::chrono::steady_clock>;

namespace {

class rpc_client {
public:
    rpc_client(const char* filename);
    ~rpc_client();

    void run(uint64_t n, steady_time_point timestamp);

    inline void process_response(uint64_t value);

    enum endpoint {
        client_type = 0, server_type = 1
    };
    inline std::string checksum(endpoint);

private:
    int _inputfd;
    size_t _inputlen;
    void* _inputdata;
    struct input_line {
        const char* name;
        size_t name_len;
        size_t count;
    };
    std::vector<input_line> _inputs;
    uint64_t _inputindex = 0;

    XXH3_state_t* _ctx[2];
    bool _done = false;

    NONCOPYABLE(rpc_client);
};

rpc_client::rpc_client(const char* filename) {
    _inputfd = open(filename, O_RDONLY);
    if (_inputfd < 0) {
        std::cerr << filename << ": " << strerror(errno) << "\n";
        exit(1);
    }

    off_t sz = lseek(_inputfd, 0, SEEK_END);
    if (sz == -1) {
        std::cerr << filename << ": " << strerror(errno) << "\n";
        exit(1);
    } else if (size_t(sz) > 0xFFFF'FFFFUL) {
        std::cerr << filename << ": File too large\n";
        exit(1);
    }
    _inputlen = sz;

    _inputdata = mmap(nullptr, _inputlen, PROT_READ, MAP_SHARED, _inputfd, 0);
    if (_inputdata == MAP_FAILED) {
        std::cerr << "mmap " << filename << ": " << strerror(errno) << "\n";
        exit(1);
    }

    const char* s = reinterpret_cast<char*>(_inputdata);
    const char* efile = s + sz;
    while (s != efile) {
        const char* line = s;
        while (s != efile && *s != '\n' && *s != ',') {
            ++s;
        }
        if (s == efile) {
            continue;
        } else if (*s == '\n') {
            ++s;
            continue;
        }
        const char* comma = s;
        uint64_t value;
        auto [next, ec] = std::from_chars(comma + 1, efile, value, 10);
        if (ec == std::errc()) {
            _inputs.emplace_back(line, size_t(comma - line), value);
        }
        s = next;
        while (s != efile && *s != '\n') {
            ++s;
        }
        if (s != efile) {
            ++s;
        }
    }

    _ctx[0] = XXH3_createState();
    XXH3_64bits_reset(_ctx[0]);
    _ctx[1] = XXH3_createState();
    XXH3_64bits_reset(_ctx[1]);
}

rpc_client::~rpc_client() {
    munmap(_inputdata, _inputlen);
    close(_inputfd);
    XXH3_freeState(_ctx[0]);
    XXH3_freeState(_ctx[1]);
}

void rpc_client::run(uint64_t n, steady_time_point timestamp) {
    assert(!_done);
    uint64_t i = 0;
    while (i != n) {
        const input_line& line = _inputs[_inputindex];
        ++_inputindex;
        if (_inputindex == _inputs.size()) {
            _inputindex = 0;
        }

        XXH3_64bits_update(_ctx[client_type], line.name, line.name_len);
        XXH3_64bits_update_uint64(_ctx[client_type], line.count);

        client_send_try(line.name, line.name_len, line.count);

        ++i;
        if (i % 10000 == 0) {
            auto next_timestamp = std::chrono::steady_clock::now();
            const std::chrono::duration<double> diff = next_timestamp - timestamp;
            std::cerr << std::format("sent {} RPCs, recently {:.0f} RPCs/sec...\n",
                                     i, 10000 / diff.count());
            timestamp = next_timestamp;
        }
    }
}

inline void rpc_client::process_response(uint64_t value) {
    assert(!_done);
    XXH3_64bits_update_uint64(_ctx[server_type], value);
}

inline std::string rpc_client::checksum(endpoint ep) {
    _done = true;
    return XXH3_64bits_hexdigest(_ctx[ep]);
}

std::unique_ptr<rpc_client> rpcc;

}


// connectors required by `clientstub.cc`

void client_recv_try_response(uint64_t value) {
    rpcc->process_response(value);
}

std::string client_checksum() {
    return rpcc->checksum(rpc_client::client_type);
}

std::string server_checksum() {
    return rpcc->checksum(rpc_client::server_type);
}


// main program

int main(int argc, char* const argv[]) {
    std::string address = "localhost:29381";
    uint64_t n = 100000;
    const char* filename = "lines.txt";
    int ch;
    while ((ch = getopt(argc, argv, "h:n:f:")) != -1) {
        if (ch == 'h') {
            address = optarg;
        } else if (ch == 'n') {
            n = from_str_chars<uint64_t>(optarg);
        } else if (ch == 'f') {
            filename = optarg;
        }
    }

    rpcc = std::make_unique<rpc_client>(filename);

    client_connect(address);

    const auto start_time = std::chrono::steady_clock::now();

    rpcc->run(n, start_time);

    client_finish();

    const auto end_time = std::chrono::steady_clock::now();
    const std::chrono::duration<double> diff = end_time - start_time;
    std::cerr << std::format("sent {} RPCs in {:.09f} sec\n", n, diff.count())
        << std::format("sent {:.0f} RPCs per sec\n", n / diff.count());
}
