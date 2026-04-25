#include "cotamer/config.hh"
#include "cotamer/curl.hh"
#include "cotamer/io.hh"

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <curl/curl.h>

namespace cotamer {

namespace {

using milliseconds = std::chrono::milliseconds;

// Per-transfer state, kept alive by the curl_fetch coroutine and reachable
// from CURLOPT_PRIVATE.
struct transfer_state {
    std::string body;
    std::multimap<std::string, std::string, std::less<>> headers;
    event done;           // triggered when curl reports CURLMSG_DONE
    CURLcode result = CURLE_OK;
};

struct watch_state;

class curl_driver {
public:
    curl_driver();
    ~curl_driver();
    curl_driver(const curl_driver&) = delete;
    curl_driver& operator=(const curl_driver&) = delete;
    static curl_driver& current();

    CURLM* multi() const noexcept { return multi_; }

    void add(CURL* easy);
    void remove(CURL* easy);

private:
    CURLM* multi_;

    // timer management
    event expiry_;
    task<> timer_task_;
    long next_timeout_ = -1;

    // curl-owned sockets; the fd destructor does the actual ::close on erase
    std::unordered_map<curl_socket_t, fd> fds_;
    std::unordered_map<curl_socket_t, watch_state> watches_;

    // Socket whose watch_task is currently inside curl_multi_socket_action;
    // CURL_SOCKET_BAD otherwise. Lets socket_cb / closesocket_cb detect a
    // recursive callback for that socket and avoid destroying the coroutine
    // on its own stack.
    curl_socket_t current_socket_ = CURL_SOCKET_BAD;

    // libcurl callbacks.
    static curl_socket_t opensocket_cb(void* clientp,
                                       curlsocktype purpose,
                                       struct curl_sockaddr* addr);
    static int closesocket_cb(void* clientp, curl_socket_t s);
    static int socket_cb(CURL* easy, curl_socket_t s, int what,
                         void* userp, void* socketp);
    static int timer_cb(CURLM* multi, long timeout_ms, void* userp);

    task<> watch_task(curl_socket_t s, fd f, watch_state& w);
    task<> timer_task();
    void on_action();     // drain CURLMSG_DONE completions
};

struct watch_state {
    int mask;      // CURL_POLL_IN/OUT bits, or CURL_POLL_REMOVE to exit
    event ev;      // file_event watch_task is awaiting; outsiders trigger to wake it
    task<> task_;  // owns the watch_task coroutine

    watch_state(int m) : mask(m) { }
};

// Per-thread driver.
thread_local std::unique_ptr<curl_driver> tls_curl_driver;

curl_driver& curl_driver::current() {
    if (!tls_curl_driver) {
        tls_curl_driver.reset(new curl_driver);
    }
    return *tls_curl_driver;
}

static std::once_flag curl_global_once;

curl_driver::curl_driver() {
    std::call_once(curl_global_once, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    multi_ = curl_multi_init();
    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, &socket_cb);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, &timer_cb);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);
    timer_task_ = timer_task();
}

curl_driver::~curl_driver() {
    // curl_multi_cleanup tears down remaining transfers, calling socket_cb
    // (POLL_REMOVE) and closesocket_cb for each socket.
    curl_multi_cleanup(multi_);
}

void curl_driver::add(CURL* easy) {
    curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION, &opensocket_cb);
    curl_easy_setopt(easy, CURLOPT_OPENSOCKETDATA, this);
    curl_easy_setopt(easy, CURLOPT_CLOSESOCKETFUNCTION, &closesocket_cb);
    curl_easy_setopt(easy, CURLOPT_CLOSESOCKETDATA, this);
    curl_multi_add_handle(multi_, easy);
}

void curl_driver::remove(CURL* easy) {
    curl_multi_remove_handle(multi_, easy);
}

curl_socket_t curl_driver::opensocket_cb(void* clientp,
                                         curlsocktype purpose,
                                         struct curl_sockaddr* addr) {
    (void) purpose;
    auto* drv = static_cast<curl_driver*>(clientp);
    int s = ::socket(addr->family, addr->socktype, addr->protocol);
    if (s < 0) {
        return CURL_SOCKET_BAD;
    }
    try {
        set_nonblocking(s);
    } catch (...) {
        ::close(s);
        return CURL_SOCKET_BAD;
    }
    drv->fds_.emplace(s, fd(s));
    return s;
}

int curl_driver::closesocket_cb(void* clientp, curl_socket_t s) {
    auto* drv = static_cast<curl_driver*>(clientp);
    // libcurl always calls socket_cb(POLL_REMOVE) before closesocket_cb, so
    // any prior watches_[s] is already gone or marked to self-exit.
    drv->fds_.erase(s);
    return 0;
}

int curl_driver::socket_cb(CURL* /*easy*/, curl_socket_t s, int what,
                           void* userp, void* /*socketp*/) {
    auto* drv = static_cast<curl_driver*>(userp);
    if (what == CURL_POLL_REMOVE && s != drv->current_socket_) {
        drv->watches_.erase(s);
        return 0;
    }
    auto it = drv->watches_.find(s);
    if (it != drv->watches_.end()) {
        // update mask and poke watch_task to re-install its file_event
        it->second.mask = what;
        it->second.ev.trigger();
    } else if (what != CURL_POLL_REMOVE) {
        auto fit = drv->fds_.find(s);
        if (fit != drv->fds_.end()) {
            auto [nit, created] = drv->watches_.emplace(s, what);
            nit->second.task_ = drv->watch_task(s, fit->second, nit->second);
        }
    }
    return 0;
}

int curl_driver::timer_cb(CURLM* /*multi*/, long timeout_ms, void* userp) {
    auto* drv = static_cast<curl_driver*>(userp);
    if (!drv->expiry_.triggered()) {
        drv->expiry_.set_user_flags(1);
        drv->expiry_.trigger();
    }
    drv->next_timeout_ = timeout_ms;
    return 0;
}

task<> curl_driver::watch_task(curl_socket_t sock, fd f, watch_state& w) {
    while (w.mask != CURL_POLL_REMOVE) {
        fdevent mask = fdevent::none;
        if (w.mask & CURL_POLL_IN) {
            mask = mask | fdevent::read;
        }
        if (w.mask & CURL_POLL_OUT) {
            mask = mask | fdevent::write;
        }
        w.ev = file_event(f, mask);
        co_await w.ev;
        // Zero user_flags() = manual trigger from socket_cb (mask changed);
        // nonzero = the kernel-reported bits.
        fdevent fired = fdevent(w.ev.user_flags());
        if (fired == fdevent::none) {
            continue;
        }
        int ev_bits = 0;
        if (int(fired & fdevent::read)) {
            ev_bits |= CURL_CSELECT_IN;
        }
        if (int(fired & fdevent::write)) {
            ev_bits |= CURL_CSELECT_OUT;
        }
        int running = 0;
        current_socket_ = sock;
        curl_multi_socket_action(multi_, sock, ev_bits, &running);
        current_socket_ = CURL_SOCKET_BAD;
        on_action();
    }
    // A recursive POLL_REMOVE arrived during cma; detach before erasing so
    // the erase doesn't destroy us on our own stack.
    auto it = watches_.find(sock);
    if (it != watches_.end() && &w == &it->second) {
        it->second.task_.detach();
        watches_.erase(it);
    }
}

task<> curl_driver::timer_task() {
    while (true) {
        if (expiry_.triggered()) {
            expiry_ = next_timeout_ < 0 ? event() : after(milliseconds(next_timeout_));
            next_timeout_ = -1;
        }
        co_await expiry_;
        if (expiry_.user_flags()) {
            continue;
        }
        int running = 0;
        curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &running);
        on_action();
    }
}

void curl_driver::on_action() {
    CURLMsg* msg;
    int remaining;
    while ((msg = curl_multi_info_read(multi_, &remaining)) != nullptr) {
        if (msg->msg != CURLMSG_DONE) {
            continue;
        }
        CURL* easy = msg->easy_handle;
        transfer_state* ts = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &ts);
        if (ts) {
            ts->result = msg->data.result;
            ts->done.trigger();
        }
    }
}

// --- HTTP header/body write callbacks ---------------------------------------

size_t write_body_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ts = static_cast<transfer_state*>(userdata);
    size_t n = size * nmemb;
    ts->body.append(ptr, n);
    return n;
}

size_t write_header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ts = static_cast<transfer_state*>(userdata);
    size_t n = size * nmemb;
    std::string_view line(ptr, n);
    // Strip trailing CRLF.
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    if (line.empty()) {
        return n;
    }
    // Skip status lines ("HTTP/x.y code reason").
    if (line.starts_with("HTTP/")) {
        return n;
    }
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return n;
    }
    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);
    // Trim leading whitespace on value.
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    ts->headers.emplace(std::string(name), std::string(value));
    return n;
}

} // namespace

// --- public API -------------------------------------------------------------

task<curl_response> curl_fetch(std::string url) {
    return curl_fetch(std::move(url), [](CURL*) {});
}

task<curl_response> curl_fetch(std::string url,
                               std::function<void(CURL*)> configure) {
    auto& drv = curl_driver::current();
    CURL* easy = curl_easy_init();
    if (!easy) {
        throw std::runtime_error("curl_easy_init failed");
    }

    auto ts = std::make_shared<transfer_state>();
    char errbuf[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "cotamer-curl/0.1");
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &write_body_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, ts.get());
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &write_header_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, ts.get());
    curl_easy_setopt(easy, CURLOPT_PRIVATE, ts.get());

    configure(easy);

    drv.add(easy);

    co_await ts->done;

    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    CURLcode result = ts->result;

    drv.remove(easy);
    curl_easy_cleanup(easy);

    if (result != CURLE_OK) {
        throw curl_error(result, errbuf[0] ? errbuf : curl_easy_strerror(result));
    }

    curl_response resp;
    resp.status = status;
    resp.body = std::move(ts->body);
    resp.headers = std::move(ts->headers);
    co_return resp;
}

void curl_reset() {
    tls_curl_driver.reset();
}

} // namespace cotamer
