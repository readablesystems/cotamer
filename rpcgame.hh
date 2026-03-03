#ifndef CS2620_PSET1_RPCGAME_HH
#define CS2620_PSET1_RPCGAME_HH
#include <charconv>
#include <bit>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>
#include "xxhash.h"

// Implemented in `clientstub.cc`, called by `client.cc`:
// - open a connection
void client_connect(std::string address);

// - send a pair to the server
void client_send_try(const char* name, size_t name_len, uint64_t count);

// - send a finish message to the server and wait for the response
void client_finish();


// Implemented in `serverstub.cc`, called by `server.cc`:
// - start the server listening on `address`; does not return
void server_start(std::string address);


// Implemented in `client.cc`:
// - account for a received response
void client_recv_try_response(uint64_t value);


// Implemented in `server.cc`:
// - process a pair sent by the client
uint64_t server_process_try(uint64_t serial, const char* name, size_t name_len,
                            uint64_t count);

// - account for termination
void server_done();


// Implemented in both `client.cc` and `server.cc`:
// - return the checksum of client requests
std::string client_checksum();

// - return the checksum of server responses
std::string server_checksum();


// Helper functions
// - update an XXH3 hash with `value` in little-endian order
inline void XXH3_64bits_update_uint64(XXH3_state_t* ctx, uint64_t value) {
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    XXH3_64bits_update(ctx, &value, sizeof(value));
}

// - return an XX3 hash as a hex string
inline std::string XXH3_64bits_hexdigest(XXH3_state_t* ctx) {
    uint64_t digest = XXH3_64bits_digest(ctx);
    return std::format("{:016x}", digest);
}

// - perform std::from_chars on `s`; all of `s` must be parsed
template <typename T>
inline std::errc from_str_chars(const std::string& s, T& value, int base = 10) {
    auto [next, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (next != s.data() + s.size()) {
        ec = std::errc::invalid_argument;
    }
    return ec;
}

// - perform std::from_chars on `s`; all of `s` must be parsed. return parsed
//   value, or throw exception if parsing fails
template <typename T>
inline T from_str_chars(const std::string& s, int base = 10) {
    T value;
    auto ec = from_str_chars(s, value, base);
    if (ec != std::errc()) {
        throw std::invalid_argument(std::make_error_code(ec).message());
    }
    return value;
}

#define NONCOPYABLE(class_name) \
    class_name(const class_name&) = delete; \
    class_name(class_name&&) = delete; \
    class_name& operator=(const class_name&) = delete; \
    class_name& operator=(class_name&&) = delete

#endif
