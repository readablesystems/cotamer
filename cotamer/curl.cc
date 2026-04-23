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

// Per-transfer state. Lives in CURLOPT_PRIVATE; shared_ptr kept by the
// initiating curl_fetch coroutine until the transfer completes.
struct transfer_state {
    std::string body;
    std::multimap<std::string, std::string, std::less<>> headers;
    event done;           // triggered by the reaper after curl reports DONE
    CURLcode result = CURLE_OK;
};

struct watch_state;

class curl_driver {
public:
    static curl_driver& current();

    CURLM* multi() const noexcept { return multi_; }

    // Called by curl_fetch after it has attached an easy handle. Registers
    // the transfer_state for CURLOPT_PRIVATE retrieval on completion.
    void add(CURL* easy);
    void remove(CURL* easy);

    // OPENSOCKET/CLOSESOCKET handlers called by libcurl. They own the fd
    // lifetime: cotamer `fd` objects close the underlying fileno when
    // erased. libcurl will only call closesocket_cb on sockets it owns.
    static curl_socket_t opensocket_cb(void* clientp,
                                       curlsocktype purpose,
                                       struct curl_sockaddr* addr);
    static int closesocket_cb(void* clientp, curl_socket_t s);

    curl_driver();
    ~curl_driver();
    curl_driver(const curl_driver&) = delete;
    curl_driver& operator=(const curl_driver&) = delete;

private:

    static int socket_cb(CURL* easy, curl_socket_t s, int what,
                         void* userp, void* socketp);
    static int timer_cb(CURLM* multi, long timeout_ms, void* userp);

    void start_or_update_watch(curl_socket_t s, int mask);
    void stop_watch(curl_socket_t s);
    void schedule_timer(long timeout_ms);
    task<> run_watch(std::shared_ptr<watch_state> w);
    task<> run_timer(milliseconds ms, event cancel);
    void on_action();                     // drain completions; reap done xfers

    CURLM* multi_ = nullptr;

    // curl-owned sockets, keyed by curl_socket_t. fd destructor does the
    // actual ::close when erased.
    std::unordered_map<curl_socket_t, fd> fds_;
    std::unordered_map<curl_socket_t, std::shared_ptr<watch_state>> watches_;

    event timer_cancel_;                  // triggered to supersede a pending timer
};

struct watch_state {
    curl_driver* drv;
    curl_socket_t sock;
    fd f;                 // shared with driver's fds_ map
    int mask = 0;         // CURL_POLL_IN / OUT bits
    bool armed = true;
    event rearm;          // triggered when mask changes so watch reloops
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
}

curl_driver::~curl_driver() {
    // Mark all watches disarmed and wake them so they exit cleanly.
    for (auto& [sock, w] : watches_) {
        w->armed = false;
        w->rearm.trigger();
    }
    watches_.clear();
    timer_cancel_.trigger();
    if (multi_) {
        curl_multi_cleanup(multi_);
    }
}

void curl_driver::add(CURL* easy) {
    // Install our open/close socket hooks so the sockets curl creates come
    // through our fd map.
    curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION, &opensocket_cb);
    curl_easy_setopt(easy, CURLOPT_OPENSOCKETDATA, this);
    curl_easy_setopt(easy, CURLOPT_CLOSESOCKETFUNCTION, &closesocket_cb);
    curl_easy_setopt(easy, CURLOPT_CLOSESOCKETDATA, this);
    curl_multi_add_handle(multi_, easy);
    // Adding a handle will typically fire timer_cb with a near-zero timeout;
    // schedule_timer arms a coroutine that will pump the multi.
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
    if (s < 0) return CURL_SOCKET_BAD;
    try {
        set_nonblocking(s);
    } catch (...) {
        ::close(s);
        return CURL_SOCKET_BAD;
    }
    // Hand ownership to a cotamer fd in the driver's map.
    drv->fds_.emplace(s, fd(s));
    return s;
}

int curl_driver::closesocket_cb(void* clientp, curl_socket_t s) {
    auto* drv = static_cast<curl_driver*>(clientp);
    drv->stop_watch(s);
    drv->fds_.erase(s);    // fd destructor does ::close
    return 0;
}

int curl_driver::socket_cb(CURL* /*easy*/, curl_socket_t s, int what,
                           void* userp, void* /*socketp*/) {
    auto* drv = static_cast<curl_driver*>(userp);
    if (what == CURL_POLL_REMOVE) {
        drv->stop_watch(s);
    } else {
        drv->start_or_update_watch(s, what);
    }
    return 0;
}

int curl_driver::timer_cb(CURLM* /*multi*/, long timeout_ms, void* userp) {
    auto* drv = static_cast<curl_driver*>(userp);
    drv->schedule_timer(timeout_ms);
    return 0;
}

void curl_driver::start_or_update_watch(curl_socket_t s, int what) {
    int mask = 0;
    if (what == CURL_POLL_IN || what == CURL_POLL_INOUT)   mask |= CURL_POLL_IN;
    if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT)  mask |= CURL_POLL_OUT;

    auto it = watches_.find(s);
    if (it != watches_.end()) {
        // Already watching; update mask and poke the coroutine.
        it->second->mask = mask;
        it->second->rearm.trigger();
        it->second->rearm = event{};
        return;
    }
    auto fd_it = fds_.find(s);
    if (fd_it == fds_.end()) {
        // Shouldn't happen: curl asked us to watch a socket we didn't open.
        return;
    }
    auto w = std::make_shared<watch_state>();
    w->drv = this;
    w->sock = s;
    w->f = fd_it->second;
    w->mask = mask;
    watches_.emplace(s, w);
    run_watch(w).detach();
}

void curl_driver::stop_watch(curl_socket_t s) {
    auto it = watches_.find(s);
    if (it == watches_.end()) return;
    it->second->armed = false;
    it->second->rearm.trigger();
    watches_.erase(it);
}

void curl_driver::schedule_timer(long timeout_ms) {
    // Cancel pending timer.
    timer_cancel_.trigger();
    timer_cancel_ = event{};
    if (timeout_ms < 0) {
        return;  // no new timer requested
    }
    run_timer(milliseconds(timeout_ms), timer_cancel_).detach();
}

task<> curl_driver::run_watch(std::shared_ptr<watch_state> w) {
    while (w->armed) {
        int mask = w->mask;
        event re = (mask & CURL_POLL_IN)  ? readable(w->f) : event{};
        event we = (mask & CURL_POLL_OUT) ? writable(w->f) : event{};
        event rearm = w->rearm;
        co_await any(re, we, rearm);
        if (!w->armed) {
            co_return;
        }
        if (rearm.triggered() && !re.triggered() && !we.triggered()) {
            // mask changed; reloop
            continue;
        }
        int ev_bits = 0;
        if (re.triggered()) ev_bits |= CURL_CSELECT_IN;
        if (we.triggered()) ev_bits |= CURL_CSELECT_OUT;
        int running = 0;
        curl_multi_socket_action(multi_, w->sock, ev_bits, &running);
        on_action();
    }
}

task<> curl_driver::run_timer(milliseconds ms, event cancel) {
    if (ms.count() <= 0) {
        co_await any(asap(), cancel);
    } else {
        co_await any(after(ms), cancel);
    }
    if (cancel.triggered()) {
        co_return;
    }
    int running = 0;
    curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &running);
    on_action();
}

void curl_driver::on_action() {
    // Drain completions.
    CURLMsg* msg;
    int remaining;
    while ((msg = curl_multi_info_read(multi_, &remaining)) != nullptr) {
        if (msg->msg != CURLMSG_DONE) continue;
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
    if (line.empty()) return n;
    // Skip status lines ("HTTP/x.y code reason").
    if (line.starts_with("HTTP/")) return n;
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return n;
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

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "cotamer-curl/0.1");
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
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
        throw std::system_error(
            std::error_code(result, std::generic_category()),
            curl_easy_strerror(result));
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
