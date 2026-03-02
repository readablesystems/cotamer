#pragma once
#include "cotamer_io.hh"
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <vector>

// cotamer_tcp.hh
//    TCP stream with length-prefixed framing for message-based protocols.

namespace cotamer {

// Set a file descriptor to non-blocking mode.
inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("set_nonblocking failed");
    }
}


// tcp_stream
//    Owns a TCP file descriptor and provides length-prefixed message framing.
//    Messages are sent as [4-byte big-endian length][payload].

class tcp_stream {
public:
    explicit tcp_stream(cotamer::fd f) : fd_(std::move(f)) {
        set_nonblocking(fd_.fileno());
        int flag = 1;
        setsockopt(fd_.fileno(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    }

    tcp_stream(tcp_stream&& x) noexcept = default;
    tcp_stream& operator=(tcp_stream&& x) noexcept = default;

    tcp_stream(const tcp_stream&) = delete;
    tcp_stream& operator=(const tcp_stream&) = delete;

    ~tcp_stream() = default;

    // Send a length-prefixed message.
    task<> send_frame(const void* data, size_t len) {
        uint32_t net_len = htonl(static_cast<uint32_t>(len));
        auto r = co_await write(fd_, &net_len, sizeof(net_len));
        if (r != sizeof(net_len)) {
            throw std::runtime_error("tcp_stream: send_frame header failed");
        }
        if (len > 0) {
            r = co_await write(fd_, data, len);
            if (r != static_cast<ssize_t>(len)) {
                throw std::runtime_error("tcp_stream: send_frame payload failed");
            }
        }
    }

    // Receive a length-prefixed message. Returns empty vector on EOF/error.
    task<std::vector<char>> recv_frame() {
        // Read 4-byte length header
        uint32_t net_len;
        size_t hdr_read = 0;
        char* hdr_buf = reinterpret_cast<char*>(&net_len);
        while (hdr_read < sizeof(net_len)) {
            auto r = co_await read_once(fd_, hdr_buf + hdr_read,
                                         sizeof(net_len) - hdr_read);
            if (r <= 0) {
                co_return std::vector<char>{};
            }
            hdr_read += r;
        }
        uint32_t len = ntohl(net_len);
        if (len > 64 * 1024 * 1024) {
            // Reject absurdly large frames
            co_return std::vector<char>{};
        }
        // Read payload
        std::vector<char> buf(len);
        size_t payload_read = 0;
        while (payload_read < len) {
            auto r = co_await read_once(fd_, buf.data() + payload_read,
                                         len - payload_read);
            if (r <= 0) {
                co_return std::vector<char>{};
            }
            payload_read += r;
        }
        co_return buf;
    }

    const cotamer::fd& fd() const { return fd_; }

    void close() {
        fd_.close();
    }

private:
    cotamer::fd fd_;
};


// Create a non-blocking listening socket bound to host:port.
inline cotamer::fd tcp_listen(const char* host, uint16_t port, int backlog = 128) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    auto port_str = std::to_string(port);
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, port_str.c_str(), &hints, &res) != 0 || !res) {
        throw std::runtime_error("tcp_listen: getaddrinfo failed");
    }

    int raw = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (raw < 0) {
        freeaddrinfo(res);
        throw std::runtime_error("tcp_listen: socket failed");
    }
    int opt = 1;
    setsockopt(raw, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(raw, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        ::close(raw);
        throw std::runtime_error(std::string("tcp_listen: bind failed: ") + strerror(errno));
    }
    freeaddrinfo(res);

    if (listen(raw, backlog) < 0) {
        ::close(raw);
        throw std::runtime_error("tcp_listen: listen failed");
    }
    set_nonblocking(raw);
    return cotamer::fd(raw);
}


// Accept a connection and return a tcp_stream.
inline task<tcp_stream> tcp_accept(const cotamer::fd& listen_fd) {
    auto f = co_await accept(listen_fd);
    if (!f) {
        throw std::runtime_error("tcp_accept failed");
    }
    co_return tcp_stream(std::move(f));
}


// Connect to host:port and return a tcp_stream.
inline task<tcp_stream> tcp_connect(const char* host, uint16_t port) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    auto port_str = std::to_string(port);
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, port_str.c_str(), &hints, &res) != 0 || !res) {
        throw std::runtime_error("tcp_connect: getaddrinfo failed");
    }

    int raw = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (raw < 0) {
        freeaddrinfo(res);
        throw std::runtime_error("tcp_connect: socket failed");
    }
    set_nonblocking(raw);

    cotamer::fd f(raw);
    int r = co_await connect(f, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0) {
        throw std::runtime_error("tcp_connect: connect failed");
    }
    co_return tcp_stream(std::move(f));
}

} // namespace cotamer
