#pragma once
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// cotamer_serial.hh
//    Binary serialization for network messages.
//    Multi-byte integers are stored in network byte order (big-endian).

namespace cotamer {

// serial_writer: builds a serialized byte buffer.

class serial_writer {
public:
    void write_raw(const void* data, size_t n) {
        auto* p = static_cast<const char*>(data);
        buf_.insert(buf_.end(), p, p + n);
    }

    void write_u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
    void write_i8(int8_t v) { write_u8(static_cast<uint8_t>(v)); }

    void write_u16(uint16_t v) {
        uint16_t net = htons(v);
        write_raw(&net, sizeof(net));
    }
    void write_i16(int16_t v) { write_u16(static_cast<uint16_t>(v)); }

    void write_u32(uint32_t v) {
        uint32_t net = htonl(v);
        write_raw(&net, sizeof(net));
    }
    void write_i32(int32_t v) { write_u32(static_cast<uint32_t>(v)); }

    void write_u64(uint64_t v) {
        // Big-endian: high 32 bits first
        write_u32(static_cast<uint32_t>(v >> 32));
        write_u32(static_cast<uint32_t>(v));
    }
    void write_i64(int64_t v) { write_u64(static_cast<uint64_t>(v)); }

    void write_float(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        write_u32(bits);
    }

    void write_double(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        write_u64(bits);
    }

    void write_string(std::string_view s) {
        write_u32(static_cast<uint32_t>(s.size()));
        write_raw(s.data(), s.size());
    }

    void write_bool(bool v) { write_u8(v ? 1 : 0); }

    const char* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }
    const std::vector<char>& buffer() const { return buf_; }

private:
    std::vector<char> buf_;
};


// serial_reader: reads from a serialized byte buffer.

class serial_reader {
public:
    serial_reader(const char* data, size_t len)
        : data_(data), len_(len) {}

    explicit serial_reader(const std::vector<char>& buf)
        : data_(buf.data()), len_(buf.size()) {}

    bool read_raw(void* out, size_t n) {
        if (pos_ + n > len_) { error_ = true; return false; }
        std::memcpy(out, data_ + pos_, n);
        pos_ += n;
        return true;
    }

    uint8_t read_u8() {
        uint8_t v = 0;
        read_raw(&v, 1);
        return v;
    }
    int8_t read_i8() { return static_cast<int8_t>(read_u8()); }

    uint16_t read_u16() {
        uint16_t net = 0;
        read_raw(&net, sizeof(net));
        return ntohs(net);
    }
    int16_t read_i16() { return static_cast<int16_t>(read_u16()); }

    uint32_t read_u32() {
        uint32_t net = 0;
        read_raw(&net, sizeof(net));
        return ntohl(net);
    }
    int32_t read_i32() { return static_cast<int32_t>(read_u32()); }

    uint64_t read_u64() {
        uint64_t hi = read_u32();
        uint64_t lo = read_u32();
        return (hi << 32) | lo;
    }
    int64_t read_i64() { return static_cast<int64_t>(read_u64()); }

    float read_float() {
        uint32_t bits = read_u32();
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    double read_double() {
        uint64_t bits = read_u64();
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    std::string read_string() {
        uint32_t len = read_u32();
        if (pos_ + len > len_ || error_) {
            error_ = true;
            return {};
        }
        std::string s(data_ + pos_, len);
        pos_ += len;
        return s;
    }

    bool read_bool() { return read_u8() != 0; }

    bool error() const { return error_; }
    size_t remaining() const { return error_ ? 0 : len_ - pos_; }

private:
    const char* data_;
    size_t len_;
    size_t pos_ = 0;
    bool error_ = false;
};


// serializer<T>: trait for user types.
// Default: static_assert for unrecognized types.
// Specializations provided for arithmetic types and std::string.
// Trivially-copyable types with standard layout get an automatic specialization.

template <typename T, typename Enable = void>
struct serializer {
    static_assert(!std::is_same_v<T, T>,
                  "No serializer specialization for this type. "
                  "Please specialize cotamer::serializer<T>.");
};


// Specializations for fixed-width integer types

template <> struct serializer<uint8_t> {
    static void serialize(serial_writer& w, uint8_t v) { w.write_u8(v); }
    static uint8_t deserialize(serial_reader& r) { return r.read_u8(); }
};
template <> struct serializer<int8_t> {
    static void serialize(serial_writer& w, int8_t v) { w.write_i8(v); }
    static int8_t deserialize(serial_reader& r) { return r.read_i8(); }
};
template <> struct serializer<uint16_t> {
    static void serialize(serial_writer& w, uint16_t v) { w.write_u16(v); }
    static uint16_t deserialize(serial_reader& r) { return r.read_u16(); }
};
template <> struct serializer<int16_t> {
    static void serialize(serial_writer& w, int16_t v) { w.write_i16(v); }
    static int16_t deserialize(serial_reader& r) { return r.read_i16(); }
};
template <> struct serializer<uint32_t> {
    static void serialize(serial_writer& w, uint32_t v) { w.write_u32(v); }
    static uint32_t deserialize(serial_reader& r) { return r.read_u32(); }
};
template <> struct serializer<int32_t> {
    static void serialize(serial_writer& w, int32_t v) { w.write_i32(v); }
    static int32_t deserialize(serial_reader& r) { return r.read_i32(); }
};
template <> struct serializer<uint64_t> {
    static void serialize(serial_writer& w, uint64_t v) { w.write_u64(v); }
    static uint64_t deserialize(serial_reader& r) { return r.read_u64(); }
};
template <> struct serializer<int64_t> {
    static void serialize(serial_writer& w, int64_t v) { w.write_i64(v); }
    static int64_t deserialize(serial_reader& r) { return r.read_i64(); }
};

template <> struct serializer<bool> {
    static void serialize(serial_writer& w, bool v) { w.write_bool(v); }
    static bool deserialize(serial_reader& r) { return r.read_bool(); }
};

template <> struct serializer<float> {
    static void serialize(serial_writer& w, float v) { w.write_float(v); }
    static float deserialize(serial_reader& r) { return r.read_float(); }
};
template <> struct serializer<double> {
    static void serialize(serial_writer& w, double v) { w.write_double(v); }
    static double deserialize(serial_reader& r) { return r.read_double(); }
};

template <> struct serializer<std::string> {
    static void serialize(serial_writer& w, const std::string& v) { w.write_string(v); }
    static std::string deserialize(serial_reader& r) { return r.read_string(); }
};


// Convenience: serialize to/from vector<char>

template <typename T>
std::vector<char> serialize(const T& val) {
    serial_writer w;
    serializer<T>::serialize(w, val);
    return w.buffer();
}

template <typename T>
T deserialize(const char* data, size_t len) {
    serial_reader r(data, len);
    return serializer<T>::deserialize(r);
}

template <typename T>
T deserialize(const std::vector<char>& buf) {
    return deserialize<T>(buf.data(), buf.size());
}

} // namespace cotamer
