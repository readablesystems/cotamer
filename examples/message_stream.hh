#pragma once
#include "cotamer.hh"
#include <cstring>

class message_stream {
public:
    enum operation { sender, receiver };
    enum class statuscode { running, eof, error };

    inline message_stream(cotamer::fd, operation);
    inline ~message_stream();
    message_stream(const message_stream&) = delete;
    message_stream(message_stream&&) = delete;
    message_stream& operator=(const message_stream&) = delete;
    message_stream& operator=(message_stream&&) = delete;

    inline bool running() const noexcept { return status_ == statuscode::running; }
    inline bool eof() const noexcept     { return status_ == statuscode::eof; }
    inline bool error() const noexcept   { return status_ == statuscode::error; }

    inline cotamer::event drained();

    inline cotamer::task<> send(const void* buf, size_t data);
    inline cotamer::task<> send(const std::string_view& s);

    inline cotamer::task<std::string> recv();

    inline size_t size() const noexcept  { return len_; }

private:
    static constexpr size_t backlog = 1 << 20;

    size_t head_ = 0;
    size_t pos_ = 0;
    size_t len_ = 0;
    size_t capacity_ = 1 << 12;
    char* buf_;
    operation op_;
    statuscode status_;
    int errno_ = 0;
    cotamer::event ev_{nullptr};
    cotamer::event rev_{nullptr};
    cotamer::event eev_{nullptr};
    cotamer::task<> task_;
    bool send_delay_ = false;

    inline cotamer::task<> make_writer(cotamer::fd);
    inline size_t first_message_length() const noexcept;
    inline cotamer::task<> make_reader(cotamer::fd);
};


inline message_stream::message_stream(cotamer::fd f, operation op)
    : buf_(new char[capacity_]), op_(op),
      status_(f.valid() ? statuscode::running : statuscode::error),
      task_(op == receiver ? make_reader(std::move(f)) : make_writer(std::move(f))) {
}

inline message_stream::~message_stream() {
    delete[] buf_;
}

inline cotamer::event message_stream::drained() {
    if (eev_.triggered() && len_ != 0) {
        eev_.arm();
    }
    return eev_;
}

inline cotamer::task<> message_stream::send(const void* buf, size_t len) {
    assert(len <= 0xFFFFFFFF && op_ != receiver);
    while (len_ > 0 && len_ + len > backlog && status_ == statuscode::running) {
        co_await ev_.arm();
    }
    if (status_ != statuscode::running) {
        throw std::system_error(EPIPE, std::generic_category());
    }
    if (len_ == 0) {
        head_ = pos_;
    }
    while (pos_ + len_ + len + 4 > head_ + capacity_) {
        size_t ncapacity = capacity_ * 2;
        char* nbuf = new char[ncapacity];
        memcpy(nbuf, buf_ + (pos_ - head_), len_);
        delete[] buf_;
        buf_ = nbuf;
        capacity_ = ncapacity;
        head_ = pos_;
    }
    uint32_t mlen = len;
    memcpy(buf_ + (pos_ + len_ - head_), &mlen, 4);
    memcpy(buf_ + (pos_ + len_ + 4 - head_), buf, len);
    len_ += len + 4;
    rev_.trigger();
}

inline cotamer::task<> message_stream::send(const std::string_view& s) {
    return send(s.data(), s.size());
}

inline cotamer::task<> message_stream::make_writer(cotamer::fd f) {
    while (true) {
        if (!f) {
            status_ = statuscode::error;
            break;
        }
        if (len_ == 0) {
            eev_.trigger();
            co_await rev_.arm();
            if (send_delay_) {
                co_await cotamer::asap();
            }
            continue;
        }
        ssize_t rv = ::write(f.fileno(), buf_ + (pos_ - head_), len_);
        if (rv > 0) {
            pos_ += rv;
            len_ -= rv;
            if (len_ < backlog) {
                ev_.trigger();
            }
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_await cotamer::writable(f);
        } else {
            status_ = statuscode::error;
            errno_ = errno;
            break;
        }
    }
    ev_.trigger();
}

inline size_t message_stream::first_message_length() const noexcept {
    if (len_ < 4) {
        return size_t(-1);
    }
    uint32_t dbuf;
    memcpy(&dbuf, buf_ + pos_ - head_, sizeof(dbuf));
    return dbuf + 4;
}

inline cotamer::task<std::string> message_stream::recv() {
    assert(op_ != sender);
    size_t fml;
    while ((fml = first_message_length()) > len_) {
        if (status_ != statuscode::running) {
            co_return std::string{};
        }
        rev_.trigger();
        co_await ev_.arm();
    }
    pos_ += fml;
    len_ -= fml;
    if (len_ == 0) {
        eev_.trigger();
    }
    co_return std::string(buf_ + (pos_ - head_) - (fml - 4), fml - 4);
}

inline cotamer::task<> message_stream::make_reader(cotamer::fd f) {
    while (true) {
        if (!f) {
            status_ = statuscode::error;
            break;
        }
        if (ev_.triggered()) {
            co_await rev_.arm();
            continue;
        }
        if (len_ == 0) {
            head_ = pos_;
        }
        if (pos_ + len_ + 512 > head_ + capacity_) {
            size_t ncapacity = capacity_ * 2;
            char* nbuf = new char[ncapacity];
            memcpy(nbuf, buf_ + (pos_ - head_), len_);
            delete[] buf_;
            buf_ = nbuf;
            capacity_ = ncapacity;
            head_ = pos_;
        }
        ssize_t rv = ::read(f.fileno(), buf_ + (pos_ + len_ - head_),
                            head_ + capacity_ - (pos_ + len_));
        if (rv > 0) {
            len_ += rv;
            ev_.trigger();
        } else if (rv == 0) {
            status_ = statuscode::eof;
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_await cotamer::readable(f);
        } else {
            status_ = statuscode::error;
            errno_ = errno;
            break;
        }
    }
    ev_.trigger();
}
