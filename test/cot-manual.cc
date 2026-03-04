#include "cotamer.hh"
#include <cstring>
#include <iostream>
#include <memory>
#include <print>
#include <stdexcept>
#include <thread>
#include <netdb.h>
namespace cot = cotamer;
using namespace std::chrono_literals;

cot::task<int> slow_add(int a, int b) {
    co_await cot::after(1h);          // pause for 1 hour in virtual time
    co_return a + b;
}

cot::task<> main_task() {
    std::print("{}: starting main_task\n", cot::now());
    int v = co_await slow_add(3, 4);  // suspend until slow_add finishes
    std::print("{}: slow_add returns {}\n", cot::now(), v);
}

// Outputs: (note that the initial time is fixed)
// 2021-10-12 20:21:09.000000: starting main_task
// 2021-10-12 21:21:09.000000: slow_add returns 7


// MANUAL: 
int test_event_copies() {
    cot::event e;                  // construct untriggered event
    cot::event e_copy = e;         // copy shares same underlying occurrence
    assert(!e.triggered() && !e_copy.triggered());
    e.trigger();                   // trigger underlying occurrence
    assert(e.triggered() && e_copy.triggered());  // also triggers copy
    return 0;
}


// MANUAL:
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


// MANUAL:
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


// MANUAL:
cot::task<> printer_10ms(int i) {
    std::print("printer({}) began\n", i);
    co_await cot::after(10ms);
    std::print("printer({}) completed\n", i);
}

int test_lifetime_2() {
    auto t0 = printer_10ms(0);   // `task` preserved → `printer` coroutine lives
    auto t1 = cot::attempt(printer_10ms(1), cot::after(1ms));
                             // `attempt` task preserved, but timeout fires first
                             // → `printer` coroutine destroyed
    cot::loop();
    return 1;
}

// Outputs:
// printer(0) began
// printer(1) began
// printer(0) completed



// MANUAL:
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



// MANUAL:
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


// MANUAL:
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


// MANUAL:
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


int main(int argc, char* argv[]) {
    unsigned ran = 0;

    auto run = [&](const char* name, auto fn) {
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

    if (ran == 0) {
        std::print(std::cerr, "No matching tests\n");
        exit(1);
    } else {
        std::print(std::cerr, "*** done ***\n");
        exit(0);
    }
}
