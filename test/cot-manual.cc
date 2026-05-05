#include "cotamer/cotamer.hh"
#include "cotamer/config.hh"
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <print>
#include <stdexcept>
#include <thread>
#include <netdb.h>
#include <sys/stat.h>
#include <unistd.h>
#if COTAMER_HAVE_LLHTTP
# include "cotamer/http.hh"
#endif
namespace cot = cotamer;
using namespace std::chrono_literals;


// COTAMER.md: Tasks

cot::task<int> slow_add(int a, int b) {
    co_await cot::after(1h);          // pause for 1 hour in virtual time
    co_return a + b;
}

cot::task<> main_task() {
    std::print("{}: starting main_task\n", cot::now());
    int v = co_await slow_add(3, 4);  // suspend until slow_add finishes
    std::print("{}: slow_add returns {}\n", cot::now(), v);
}

// Outputs:
// 2021-10-12 20:21:09.000000: starting main_task
// 2021-10-12 21:21:09.000000: slow_add returns 7


// COTAMER.md: Events

int test_event_copies() {
    cot::event e;                  // construct untriggered event
    cot::event e_copy = e;         // copy shares same underlying occurrence
    assert(!e.triggered() && !e_copy.triggered());
    e.trigger();                   // trigger underlying occurrence
    assert(e.triggered() && e_copy.triggered());  // also triggers copy
    return 0;
}


cot::task<> awaiter(int n, cot::event e) {
    co_await e;
    std::print("{} ", n);
}

int test_source_order() {
    auto t0 = awaiter(0, cot::asap());
    auto t1 = awaiter(1, cot::asap());
    auto t2 = awaiter(2, cot::after(5ms));
    auto t3 = awaiter(3, cot::after(10ms));
    auto t4 = awaiter(4, cot::after(10ms));
    auto t5 = awaiter(5, cot::after(5ms));     // earlier expiration time
    cot::loop();
    std::print("\n");
    return 0;
}


// COTAMER.md: Task lifetime

cot::task<> printer_asap(int i) {
    std::print("printer({}) began\n", i);
    co_await cot::asap();
    std::print("printer({}) completed\n", i);
}

int test_lifetime_1() {
    printer_asap(0);             // `task` destroyed → `printer` coroutine destroyed
    auto t = printer_asap(1);    // `task` preserved → `printer` coroutine lives
    cot::loop();
    return 1;
}

// Outputs:
// printer(0) began
// printer(1) began
// printer(1) completed


cot::task<> printer_10ms(int i) {
    std::print("printer({}) began\n", i);
    co_await cot::after(10ms);
    std::print("printer({}) completed\n", i);
}

int test_lifetime_2() {
    auto t0 = printer_10ms(0);
    auto t1 = cot::attempt(printer_10ms(1), cot::after(1ms));
    cot::loop();
    return 1;
}

// Outputs:
// printer(0) began
// printer(1) began
// printer(0) completed


int test_lifetime_3() {
    printer_asap(0).detach();    // `task` detached → `printer` coroutine lives
    auto t = printer_asap(1);    // `task` preserved → `printer` coroutine lives
    cot::loop();
    return 1;
}

// Outputs:
// printer(0) began
// printer(1) began
// printer(0) completed
// printer(1) completed


cot::task<> print_after(cot::duration time, int i) {
    co_await cot::after(time);
    std::print("print_after({})\n", i);
}

cot::task<> print_by_value() {
    co_await cot::first(print_after(10s, 10), print_after(20s, 20));
    co_await cot::after(40s);
}

// Outputs:
// print_after(10)


cot::task<> print_by_reference() {
    auto p10 = print_after(10s, 10), p20 = print_after(20s, 20);
    co_await cot::first(p10, p20);
    co_await cot::after(40s);
}

// Outputs:
// print_after(10)
// print_after(20)


cot::task<> print_by_move() {
    auto p10 = print_after(10s, 10), p20 = print_after(20s, 20);
    co_await cot::first(std::move(p10), std::move(p20));
    co_await cot::after(40s);
}

// Outputs:
// print_after(10)


// COTAMER.md: Event loop

cot::task<struct addrinfo*> nonblocking_getaddrinfo(
    const char* host, const char* port, const struct addrinfo* hints
) {
    struct addrinfo* result;       // collect result from `getaddrinfo`
    int status;
    cot::event notifier;           // wake coroutine when `getaddrinfo` thread completes
    cot::driver_guard guard;       // prevent early exit from event loop

    std::thread([&] () {
        status = getaddrinfo(host, port, hints, &result);
        notifier.trigger();
    }).detach();

    co_await notifier;             // coroutine waits for thread
    if (status != 0) {
        throw std::runtime_error(gai_strerror(status));
    }
    co_return result;
}


cot::task<struct stat> nonblocking_stat(std::string path) {
    // receive return value from thread
    // (The shared_ptr avoids undefined behavior if the coroutine is cancelled
    // while the thread is still running.)
    struct result_struct { struct stat st; int status; int err; };
    auto result = std::make_shared<result_struct>();

    cot::event notifier;           // communicate from thread to coroutine
    std::thread([=] () mutable {
        result->status = stat(path.c_str(), &result->st);
        result->err = errno;
        notifier.trigger();
    }).detach();

    cot::driver_guard guard;       // prevent early exit from event loop
    co_await notifier;             // coroutine waits for thread

    if (result->status != 0) {
        throw std::system_error(result->err, std::generic_category());
    }
    co_return result->st;
}


// COTAMER.md: Clocks

cot::task<> long_delay() {
    std::print("{}: good morning\n", cot::now());
    co_await cot::after(10000h);
    std::print("{}: good evening\n", cot::now());
}

int test_long_delay() {
    long_delay().detach();
    cot::loop();
    return 1;
}

// Outputs:
// 2021-10-12 20:21:09.000000: good morning
// 2022-12-03 12:21:09.000000: good evening


// COTAMER.md: Rearming events

struct Item {};
cot::event work_queue_wakeup;
std::deque<Item> work_queue;

cot::task<> background_worker() {
    while (true) {
        while (work_queue.empty()) {
            co_await work_queue_wakeup.arm();
        }
        [[maybe_unused]] auto item = std::move(work_queue.front());
        work_queue.pop_front();
        // ... perform work specified by `item` ...
    }
}

void enqueue_work(Item item) {
    work_queue.push_back(std::move(item));
    work_queue_wakeup.trigger();   // does nothing unless background_worker is waiting
}


int test_event_arm_copies() {
    cot::event e;                  // construct untriggered event
    cot::event e_copy = e;         // copy shares same underlying occurrence
    assert(e == e_copy);           // `operator==` tests underlying occurrence
    assert(!e.triggered() && !e_copy.triggered());
    e.trigger();                   // trigger underlying occurrence
    assert(e == e_copy);
    assert(e.triggered() && e_copy.triggered());  // also triggers copy
    e.arm();                       // rearm `e`
    assert(e != e_copy);           // but `e_copy` still refers to original
    assert(!e.triggered() && e_copy.triggered());
    return 0;
}


// COTAMER.md: Resolution

cot::task<Item> dequeue_work() {
    do {
        while (work_queue.empty()) {
            co_await work_queue_wakeup.arm();
        }
        co_await cot::resolve{};    // wait until return value wanted
        // If we get here in `cot::first` or `cot::race`, then this task has
        // won the race, so any value it returns will be used.
    } while (work_queue.empty());   // must re-check after resumption
    auto item = std::move(work_queue.front());
    work_queue.pop_front();
    co_return item;
}


// COTAMER.md: Forwarding

void log_work(const Item&) {}

cot::task<Item> dequeue_work_logged() {
    auto item = co_await cot::forward(dequeue_work());
    log_work(item);
    co_return item;
}


// COTAMER.md: Lazy tasks

cot::task<int> lazy() {
    co_await cot::interest{};      // pause here until someone wants the result
    co_await cot::after(1h);       // then do the real work
    co_return 42;
}


// COTAMERIO.md: Byte transfer

cot::task<> pipe_echo() {
    int piperaw[2];
    pipe(piperaw);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd);
    cot::set_nonblocking(wfd);

    // writer
    auto writer = [&]() -> cot::task<> {
        co_await cot::write(wfd, "hello", 5);
    };
    writer().detach();

    // reader
    char buf[64] = {};
    if (auto r = co_await cot::read_once(rfd, buf, sizeof(buf))) {
        std::print("read {} bytes: {}\n", *r, std::string_view(buf, *r));
    }
}


// COTAMERIO.md: TCP

cot::task<> handle_echo(cot::fd f) {
    char buf[4096];
    while (true) {
        auto n = co_await cot::recv_once(f, buf, sizeof(buf));
        if (!n || *n == 0) {
            break;
        }
        co_await cot::send(f, buf, *n);
    }
}

cot::task<> echo_server() {
    auto lfd = co_await cot::tcp_listen("127.0.0.1:9000");
    while (true) {
        auto cfd = co_await cot::tcp_accept(lfd);
        handle_echo(std::move(cfd)).detach();
    }
}


// COTAMERIO.md: Mutual exclusion

struct lockable_fd {
    cot::fd f;
    cot::mutex mutex;
};

cot::task<> write_message(lockable_fd& lfd, std::string message) {
    cot::unique_lock guard(co_await lfd.mutex);
    // critical section — automatically unlocked when guard goes out of scope
    // or coroutine is destroyed. `cot::send` may suspend internally on
    // `writable(lfd.f)`; the guard keeps interleaved writes from other
    // coroutines from corrupting our message across those suspensions.
    co_await cot::send(lfd.f, message.data(), message.size());
}

cot::task<> mutex_with_timeout(cot::mutex& m) {
    auto result = co_await cot::attempt(m.lock(), cot::after(1s));
    if (result) {
        cot::unique_lock guard(*result);
        // acquired within 1 second
    } else {
        // timed out — lock was not acquired; this coroutine loses its place in line
    }
}

cot::task<> test_mutex_with_timeout() {
    cot::mutex m;
    co_await mutex_with_timeout(m);   // free lock: acquires immediately
    cot::unique_lock guard(co_await m.lock());
    co_await mutex_with_timeout(m);   // contended: must time out
}


// COTAMERIO.md: HTTP

#if COTAMER_HAVE_LLHTTP

cot::task<std::string> fetch_robots_txt(std::string host) {
    auto fd = co_await cot::tcp_connect(std::format("{}:80", host));
    cot::http_parser hp(std::move(fd), cot::http_parser::client, host);
    cot::http_message req(HTTP_GET, "/robots.txt");
    auto ticket = co_await hp.send_request(std::move(req));
    co_return (co_await hp.receive(std::move(ticket))).body();
}

cot::task<> test_fetch_robots() {
    auto body = co_await fetch_robots_txt("example.com");
    std::print("fetched {} bytes\n", body.size());
    std::print("--- first 200 bytes ---\n{}\n", std::string_view(body).substr(0, 200));
}

cot::task<> http_connection(cot::fd cfd) {
    cot::http_parser hp(std::move(cfd), cot::http_parser::server);
    do {
        auto req = co_await hp.receive();
        if (!hp.ok()) {
            break;                                 // peer closed or parse error
        }
        cot::http_message res;
        res.status_code(200)
            .header("Content-Type", "text/plain")
            .body(std::format("you asked for {}\n", req.url()));
        co_await hp.send(std::move(res));
    } while (hp.should_keep_alive());
}

#endif


int main(int argc, char* argv[]) {
    unsigned ran = 0;

    auto run = [&](const char* name, auto fn, cot::clock c = cot::clock::virtual_time) {
        bool found = argc == 1;
        for (int argi = 1; !found && argi < argc; ++argi) {
            found = strcmp(name, argv[argi]) == 0;
        }
        if (!found) {
            return;
        }
        ++ran;
        std::cerr << "=== " << name << " ===\n";
        cot::reset();
        cot::set_clock(c);
        auto t = fn();
        cot::loop();
        if constexpr (cot::is_task_value(t)) {
            assert(t.done() && "test did not complete");
        }
    };

    run("main_task", main_task);
    run("event_copies", test_event_copies);
    run("source_order", test_source_order);
    run("lifetime_1", test_lifetime_1);
    run("lifetime_2", test_lifetime_2);
    run("lifetime_3", test_lifetime_3);
    run("long_delay", test_long_delay);
    run("event_arm_copies", test_event_arm_copies);
    run("print_by_value", print_by_value);
    run("print_by_reference", print_by_reference);
    run("print_by_move", print_by_move);
    run("pipe_echo", pipe_echo);
    run("mutex_with_timeout", test_mutex_with_timeout);
#if COTAMER_HAVE_LLHTTP
    run("fetch_robots", test_fetch_robots, cot::clock::real_time);
#endif

    // Compile-only references to keep the rest of the manual examples linked
    // and exercised by -Wunused diagnostics. They are not run because they
    // need external resources (network, files, stdin) or run forever.
    [[maybe_unused]] auto compile_only = [] {
        (void) &nonblocking_getaddrinfo;
        (void) &nonblocking_stat;
        (void) &background_worker;
        (void) &enqueue_work;
        (void) &dequeue_work;
        (void) &dequeue_work_logged;
        (void) &lazy;
        (void) &echo_server;
        (void) &handle_echo;
        (void) &write_message;
        (void) &mutex_with_timeout;
#if COTAMER_HAVE_LLHTTP
        (void) &http_connection;
#endif
    };

    if (ran == 0) {
        std::print(std::cerr, "No matching tests\n");
        exit(1);
    } else {
        std::print(std::cerr, "*** done ***\n");
        exit(0);
    }
}
