#pragma once
#include "cotamer.hh"
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

// tcp_stream
//    Owns a TCP file descriptor and provides length-prefixed message framing.
//    Messages are sent as [4-byte big-endian length][payload].

class tcp_stream {
public:
    explicit tcp_stream(cotamer::fd f) : fd_(std::move(f)) {
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

} // namespace cotamer
