#pragma once
#include "cotamer/cotamer.hh"
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

// cotamer/curl.hh
//    Coroutine-driven libcurl transfers.
//
//    libcurl's multi interface is plumbed into the cotamer event loop. Each
//    thread has its own `curl_driver::current()` holding a CURLM* and the
//    set of cotamer fds that curl has handed us via its open-socket callback.
//
//    Transfers are issued via `curl_fetch(url)` which returns a coroutine
//    producing a `curl_response`. Defaults: follow redirects, reasonable
//    user-agent, body streamed into a `std::string`. The configure overload
//    takes a lambda that receives the raw `CURL*` handle for any other
//    libcurl option.

typedef void CURL;

namespace cotamer {

struct curl_response {
    long status = 0;
    std::string body;
    std::multimap<std::string, std::string, std::less<>> headers;
};

// Thrown by curl_fetch on transport failure. `code()` is the CURLcode,
// `what()` a detailed error message.
class curl_error : public std::runtime_error {
public:
    curl_error(int code, const char* msg)
        : std::runtime_error(msg), code_(code) { }
    int code() const noexcept { return code_; }
private:
    int code_;
};

// Fetch `url` with defaults. Throws `curl_error` on transport failure;
// non-2xx responses do NOT throw — inspect `status`.
task<curl_response> curl_fetch(std::string url);

// Fetch with caller access to the raw CURL easy handle. `configure` runs
// after defaults are installed, so it can override any option.
task<curl_response> curl_fetch(std::string url,
                               std::function<void(CURL*)> configure);

// Drop this thread's curl_driver (and any connections in its pool). Primarily
// for tests that reset the cotamer driver and need the curl_driver to be
// rebuilt against the fresh driver.
void curl_reset();

} // namespace cotamer
