#include "cotamer.hh"
#include "examples/message_stream.hh"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <optional>
#include <print>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace cot = cotamer;
using namespace std::chrono_literals;


// TEST: Pipe read/write: basic test of readable() and async I/O
cot::task<> test_pipe_readwrite() {
    int piperaw[2];
    assert(pipe(piperaw) == 0);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd.fileno());
    cot::set_nonblocking(wfd.fileno());

    // Writer: write to the pipe
    auto writer = [&]() -> cot::task<> {
        const char* msg = "hello from pipe";
        auto r = co_await cot::write(wfd, msg, strlen(msg));
        assert(r == static_cast<ssize_t>(strlen(msg)));
    };

    // Reader: read from the pipe
    auto reader = [&]() -> cot::task<> {
        char buf[64] = {};
        auto r = co_await cot::read_once(rfd, buf, sizeof(buf));
        assert(r > 0);
        assert(std::string(buf, r) == "hello from pipe");
        std::cerr << "pipe_readwrite: read \"" << std::string(buf, r) << "\"\n";
    };

    writer().detach();
    co_await reader();
}


// TEST: Readable event triggers correctly
cot::task<> test_readable_event() {
    int piperaw[2];
    assert(pipe(piperaw) == 0);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd.fileno());
    cot::set_nonblocking(wfd.fileno());

    // Write something to make the read end readable
    auto writer = [&]() -> cot::task<> {
        co_await cot::asap(); // let the reader set up first
        const char data[] = "test";
        ssize_t nw = ::write(wfd.fileno(), data, 4);
        assert(nw == 4);
    };
    writer().detach();

    // Wait for readable
    co_await cot::readable(rfd);
    char buf[16] = {};
    ssize_t r = ::read(rfd.fileno(), buf, sizeof(buf));
    assert(r == 4);
    assert(std::string(buf, 4) == "test");
    std::cerr << "readable_event: ok\n";
}


// TEST: Timer still works in real-time mode
cot::task<> test_realtime_timer() {
    auto start = std::chrono::system_clock::now();
    co_await cot::after(50ms);
    auto elapsed = std::chrono::system_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cerr << "realtime_timer: elapsed " << ms << "ms\n";
    // In realtime mode, 50ms should actually take roughly 50ms wall clock
    assert(ms >= 30 && ms < 500);
}


// TEST: TCP echo server
cot::task<> test_tcp_echo() {
    uint16_t port = 19876;

    // Echo server: accept one connection, echo back one frame, then close
    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen("127.0.0.1:" + std::to_string(port));
        auto cfd = co_await cot::tcp_accept(lfd);
        message_stream readbuf(cfd, message_stream::receiver);
        message_stream writebuf(cfd, message_stream::sender);
        auto frame = co_await readbuf.recv();
        writebuf.send(frame);
        co_await writebuf.drained();
    };
    server().detach();

    // Give the server a moment to start listening
    co_await cot::after(2ms); // give `tcp_listen` a chance

    // Client: connect, send a frame, receive the echo
    auto client = [&]() -> cot::task<> {
        auto cfd = co_await cot::tcp_connect("127.0.0.1:" + std::to_string(port));
        message_stream readbuf(cfd, message_stream::receiver);
        message_stream writebuf(cfd, message_stream::sender);
        std::string msg("echo test message");
        writebuf.send(msg);
        auto reply = co_await readbuf.recv();
        assert(reply == msg);
        std::cerr << "tcp_echo: ok\n";
    };
    co_await client();
}


// TEST: TCP framed message exchange, multiple frames
cot::task<> test_tcp_multi_frame() {
    uint16_t port = 19877;

    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen("127.0.0.1:" + std::to_string(port));
        auto cfd = co_await cot::tcp_accept(lfd);
        // Receive 3 messages and send them back reversed
        std::vector<std::string> frames;
        message_stream readbuf(cfd, message_stream::receiver);
        for (int i = 0; i < 3; ++i) {
            frames.emplace_back(co_await readbuf.recv());
        }
        message_stream writebuf(cfd, message_stream::sender);
        for (int i = 2; i >= 0; --i) {
            writebuf.send(frames[i]);
        }
        co_await writebuf.drained();
    };
    server().detach();

    co_await cot::after(2ms); // give `tcp_listen` a chance

    auto client = [&]() -> cot::task<> {
        auto cfd = co_await cot::tcp_connect("127.0.0.1:" + std::to_string(port));
        message_stream writebuf(cfd, message_stream::sender);
        const char* msgs[] = {"first", "second", "third"};
        for (int i = 0; i < 3; ++i) {
            writebuf.send(msgs[i]);
        }
        // Should receive in reverse order
        message_stream readbuf(cfd, message_stream::receiver);
        for (int i = 2; i >= 0; --i) {
            auto frame = co_await readbuf.recv();
            assert(frame == msgs[i]);
        }
        std::cerr << "tcp_multi_frame: ok\n";
    };
    co_await client();
}


// TEST: Dup-then-close: demonstrates the file descriptor vs. file description
// issue. kqueue tracks file descriptors, so closing pipefd[0] fires events
// even though dupfd still references the same file description. epoll tracks
// file descriptions, so closing pipefd[0] does nothing — the file description
// is still alive via dupfd and the events never fire.
//
// With cotamer::fd, closing the fd object triggers all events and cleans up
// poller state automatically.
cot::task<> test_dup_close_fd() {
    int piperaw[2];
    assert(pipe(piperaw) == 0);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd.fileno());
    cot::set_nonblocking(wfd.fileno());

    // Register readable and closed events on the read end
    cot::event readable_ev = cot::readable(rfd);
    cot::event closed_ev = cot::closed(rfd);

    // Let the loop register the fd with the poller
    co_await cot::after(10ms);

    assert(!readable_ev.triggered() && !closed_ev.triggered());
    ssize_t nw = ::write(wfd.fileno(), "!", 1);
    assert(nw == 1);

    // This should not block
    auto time = cot::now();
    co_await cot::any(readable_ev, cot::after(1s));
    assert(cot::now() - time < 10ms);
    assert(readable_ev.triggered());
    assert(!closed_ev.triggered());
    char buf[100];
    nw = ::read(rfd.fileno(), buf, sizeof(buf));
    assert(nw == 1);

    // Re-register interest in readable_ev
    readable_ev = cot::readable(rfd);

    // Dup the read end, then close the original via cot::fd::close()
    int dupraw = dup(rfd.fileno());
    assert(dupraw >= 0);
    rfd.close();   // triggers events and cleans up poller state
    nw = ::write(wfd.fileno(), "!", 1);
    assert(nw == 1);

    // The close should have fired the events.
    co_await cot::any(readable_ev, closed_ev, cot::after(100ms));

    bool triggered = readable_ev.triggered() || closed_ev.triggered();
    std::cerr << "dup_close_fd: triggered=" << triggered << "\n";

    ::close(dupraw);
}


// TEST: fd close cleanup: verify that a coroutine suspended on a closed fd
// is properly cleaned up when its owning task is destroyed.
cot::task<> test_forget_fd_cleanup() {
    int piperaw[2];
    assert(pipe(piperaw) == 0);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd.fileno());
    cot::set_nonblocking(wfd.fileno());

    // Lifecycle tracker: 0=init, 1=suspended, 2=resumed, 3=frame destroyed
    int state = 0;

    auto waiter = [&]() -> cot::task<> {
        struct guard {
            int& s;
            ~guard() { s = 3; }
        };
        guard g{state};
        state = 1;
        co_await cot::readable(rfd);
        state = 2;
    };

    // Close the fd — this triggers fd events, which will resume the coroutine
    {
        auto t = waiter();
        assert(state == 1);              // suspended at co_await readable

        co_await cot::after(5ms);        // let loop register the fd
        rfd.close();                     // triggers events and cleans up
        co_await cot::after(5ms);        // let triggered coroutine run

        // The close triggers the readable event, so the coroutine resumes
        assert(state == 2 || state == 3);
    }
    // The task is now out of scope, so the coroutine frame is destroyed
    assert(state == 3 && "coroutine frame should be destroyed by ~task");

    std::cerr << "forget_fd_cleanup: ok\n";
}


// TEST: Peer close wakes a blocked reader.
// Thread A awaits async_read on one end of a socketpair. Thread B closes the
// other end. The kernel reports EOF (kqueue EV_EOF / epoll EPOLLHUP), which
// fires the readable event, resumes the coroutine (read returns 0), and lets
// loop() exit cleanly.
void test_close_wakes_reader() {
    constexpr int ROUNDS = 100;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<int> phase{0};

        int sockraw[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockraw) == 0);
        cot::set_nonblocking(sockraw[0]);

        // sockfd[0] is owned by the reader thread's cot::fd
        // sockfd[1] is closed by the closer thread directly
        int raw1 = sockraw[1];

        std::thread reader([&, raw0 = sockraw[0]] {
            cot::set_clock(cot::clock::virtual_time);

            cot::fd sfd(raw0);
            ssize_t result = -99;
            auto fn = [&]() -> cot::task<> {
                char buf[64];
                result = co_await cot::read_once(sfd, buf, sizeof(buf));
            };
            auto t = fn();

            phase.store(1, std::memory_order_release);

            auto start = std::chrono::steady_clock::now();
            cot::loop();
            auto elapsed = std::chrono::steady_clock::now() - start;

            assert(t.done());
            assert(result == 0 && "expected EOF from peer close");
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            assert(ms < 2000 && "peer close wake too slow");
        });

        std::thread closer([&, raw1] {
            while (phase.load(std::memory_order_acquire) < 1) {
                std::this_thread::yield();
            }
            // Give the reader time to enter watch_fds
            std::this_thread::sleep_for(5ms);
            ::close(raw1);
        });

        reader.join();
        closer.join();
    }
    std::cerr << "close_wakes_reader: ok\n";
}


// TEST: Cross-thread shutdown wakes a blocked reader.
// Thread A awaits async_read on sockfd[0]. Thread B calls
// shutdown(sockfd[0], SHUT_RDWR) from another thread. Unlike ::close(),
// shutdown() does not remove the fd from kqueue/epoll — it signals EOF/error
// on the socket, which the kernel reports as a readable event. The reader
// wakes up, read() returns 0 (EOF), and loop() exits cleanly.
// This is the correct cross-thread teardown mechanism for sockets.
void test_shutdown_wakes_reader() {
    constexpr int ROUNDS = 100;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<int> phase{0};

        int sockraw[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockraw) == 0);
        cot::set_nonblocking(sockraw[0]);

        int raw0 = sockraw[0], raw1 = sockraw[1];

        std::thread reader([&, raw0] {
            cot::set_clock(cot::clock::real_time);

            cot::fd sfd(raw0);
            ssize_t result = -99;
            auto fn = [&]() -> cot::task<> {
                char buf[64];
                result = co_await cot::read_once(sfd, buf, sizeof(buf));
            };
            auto t = fn();

            phase.store(1, std::memory_order_release);

            auto start = std::chrono::steady_clock::now();
            cot::loop();
            auto elapsed = std::chrono::steady_clock::now() - start;

            assert(t.done());
            assert(result == 0 && "expected EOF from shutdown");
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            assert(ms < 2000 && "shutdown wake too slow");
        });

        std::thread closer([&, raw0] {
            while (phase.load(std::memory_order_acquire) < 1) {
                std::this_thread::yield();
            }
            // Give the reader time to enter watch_fds
            std::this_thread::sleep_for(5ms);
            // shutdown() signals EOF without closing the fd — the poller
            // still has the fd registered, so it reports the state change.
            ::shutdown(raw0, SHUT_RDWR);
        });

        reader.join();
        closer.join();
        ::close(raw1);
    }
    std::cerr << "shutdown_wakes_reader: ok\n";
}


// --- Cross-thread wake torture tests ---
//
// These tests exercise the wakefd_ mechanism: when thread B triggers an event
// whose listener coroutine lives on thread A's driver, and thread A is blocked
// in watch_fds() (kevent/epoll_wait), thread B must interrupt the blocking
// call via EVFILT_USER (kqueue) or eventfd (Linux). Without the wake, thread A
// would sleep for the 1-hour fallback timeout.
//
// Each test registers a pipe fd on thread A's driver to force it through the
// watch_fds() path (rather than the nfdctl_==0 sleep_for path). A "keeper"
// coroutine watches the pipe's read end; the last event coroutine writes a
// byte to the pipe to let the keeper fire, nfdctl_ drop to 0, and loop() exit.


// TEST: Cross-thread wake: exercises the wakefd_ path.
// Thread A blocks in watch_fds(); thread B triggers after a delay that is
// long enough for thread A to always be blocked when the trigger arrives.
void test_cross_thread_wake() {
    constexpr int ROUNDS = 200;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<int> phase{0};
        cot::event* ev_ptr = nullptr;

        int piperaw[2];
        assert(pipe(piperaw) == 0);
        cot::set_nonblocking(piperaw[0]);
        cot::set_nonblocking(piperaw[1]);

        std::thread thread_a([&, r = piperaw[0], w = piperaw[1]] {
            cot::set_clock(cot::clock::real_time);

            cot::fd rfd(r), wfd(w);
            cot::event ev;
            ev_ptr = &ev;

            auto fn = [&]() -> cot::task<> {
                // Register the pipe read end so the driver enters watch_fds()
                auto readable_ev = cot::readable(rfd);
                // Block until thread B triggers ev
                co_await ev;
                // Consume the fd registration so loop() can exit cleanly
                ssize_t nw = ::write(wfd.fileno(), "x", 1);
                assert(nw == 1);
                co_await readable_ev;
            };
            auto t = fn();

            phase.store(1, std::memory_order_release);

            auto start = std::chrono::steady_clock::now();
            cot::loop();
            auto elapsed = std::chrono::steady_clock::now() - start;

            assert(t.done());
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            assert(ms < 2000 && "cross-thread wake too slow");
        });

        std::thread thread_b([&] {
            while (phase.load(std::memory_order_acquire) < 1) {
                std::this_thread::yield();
            }
            // Give thread A time to enter watch_fds() and block
            std::this_thread::sleep_for(5ms);
            ev_ptr->trigger();
        });

        thread_a.join();
        thread_b.join();
    }
    std::cerr << "cross_thread_wake: ok\n";
}


// TEST: Cross-thread wake hammer.
// 4 trigger threads fire 40 events at a single fd-blocked driver with
// staggered delays, so triggers arrive both while the driver is blocked
// and while it is processing previous wakes.
void test_cross_thread_wake_hammer() {
    constexpr int NTHREADS = 4;
    constexpr int EVENTS = 40;
    constexpr int ROUNDS = 50;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> go{false};
        cot::event* ev_ptrs[EVENTS];

        int piperaw[2];
        assert(pipe(piperaw) == 0);
        cot::set_nonblocking(piperaw[0]);
        cot::set_nonblocking(piperaw[1]);

        std::thread thread_a([&, r = piperaw[0], w = piperaw[1]] {
            cot::set_clock(cot::clock::real_time);
            int resume_count = 0;

            cot::fd rfd(r), wfd(w);
            cot::event events[EVENTS];
            std::optional<cot::task<>> tasks[EVENTS];

            auto fn = [&](int i) -> cot::task<> {
                co_await events[i];
                ++resume_count;
                if (resume_count == EVENTS) {
                    // All events processed — let the keeper coroutine fire
                    ssize_t nw = ::write(wfd.fileno(), "x", 1);
                    assert(nw == 1);
                }
            };

            for (int i = 0; i < EVENTS; ++i) {
                ev_ptrs[i] = &events[i];
                tasks[i] = fn(i);
            }

            // Keeper: holds the fd registration alive until all events fire
            auto keeper = [&]() -> cot::task<> {
                co_await cot::readable(rfd);
            };
            auto kt = keeper();

            go.store(true, std::memory_order_release);

            auto start = std::chrono::steady_clock::now();
            cot::loop();
            auto elapsed = std::chrono::steady_clock::now() - start;

            assert(kt.done());
            for (int i = 0; i < EVENTS; ++i) {
                assert(tasks[i]->done());
            }
            assert(resume_count == EVENTS);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            assert(ms < 5000 && "cross-thread wake hammer too slow");
        });

        std::vector<std::thread> triggers;
        for (int t = 0; t < NTHREADS; ++t) {
            triggers.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = t; i < EVENTS; i += NTHREADS) {
                    // Stagger: some triggers arrive while thread A is blocked,
                    // others while it processes the previous batch
                    if (i % 5 == 0) {
                        std::this_thread::sleep_for(1ms);
                    }
                    ev_ptrs[i]->trigger();
                }
            });
        }

        thread_a.join();
        for (auto& t : triggers) {
            t.join();
        }
    }
    std::cerr << "cross_thread_wake_hammer: ok\n";
}


// TEST: Cross-thread wake, repeated sequential.
// Thread A awaits events one at a time in a loop, blocking in watch_fds()
// between each. Thread B triggers each event after thread A signals readiness.
// This tests that the wake mechanism works correctly across many block/wake
// cycles on the same driver.
void test_cross_thread_wake_repeated() {
    constexpr int EVENTS = 20;
    constexpr int ROUNDS = 50;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<int> phase{0};
        cot::event* ev_ptrs[EVENTS];

        int piperaw[2];
        assert(pipe(piperaw) == 0);
        cot::set_nonblocking(piperaw[0]);
        cot::set_nonblocking(piperaw[1]);

        std::thread thread_a([&, r = piperaw[0], w = piperaw[1]] {
            cot::set_clock(cot::clock::real_time);

            cot::fd rfd(r), wfd(w);
            cot::event events[EVENTS];
            for (int i = 0; i < EVENTS; ++i) {
                ev_ptrs[i] = &events[i];
            }

            auto fn = [&]() -> cot::task<> {
                auto readable_ev = cot::readable(rfd);
                for (int i = 0; i < EVENTS; ++i) {
                    // Signal thread B: ready for event i
                    phase.store(i + 1, std::memory_order_release);
                    co_await events[i];
                }
                ssize_t nw = ::write(wfd.fileno(), "x", 1);
                assert(nw == 1);
                co_await readable_ev;
            };
            auto t = fn();

            auto start = std::chrono::steady_clock::now();
            cot::loop();
            auto elapsed = std::chrono::steady_clock::now() - start;

            assert(t.done());
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            assert(ms < 5000 && "cross-thread wake repeated too slow");
        });

        std::thread thread_b([&] {
            for (int i = 0; i < EVENTS; ++i) {
                while (phase.load(std::memory_order_acquire) < i + 1) {
                    std::this_thread::yield();
                }
                // Give thread A time to reach watch_fds()
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                ev_ptrs[i]->trigger();
            }
        });

        thread_a.join();
        thread_b.join();
    }
    std::cerr << "cross_thread_wake_repeated: ok\n";
}


// TEST: Cross-thread wake race: exercises the lock_ path.
// Thread B triggers with zero delay so the event lands while thread A is
// still in the loop body (processing fd updates, checking exit conditions,
// computing timeout) — before it stores wakefd_ and blocks in watch_fds().
// In that case, migrate_asap() sees wakefd_ == -1 and does NOT send a wake
// signal; thread A must catch the work via its lock_ check before blocking.
//
// The delay is varied across rounds:
//   - No delay: trigger almost certainly arrives before thread A blocks
//   - 1 yield: very short delay, right at the boundary
//   - ~50us: might land in either window depending on scheduling
// Over 2000 rounds this exercises the critical store-load ordering between
// wakefd_ and lock_ that seq_cst is supposed to guarantee. If the ordering
// is wrong, thread A will block with a 1-hour timeout despite pending work
// in migrate_, and the timing assertion will fire.
void test_cross_thread_wake_race() {
    constexpr int ROUNDS = 2000;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<int> phase{0};
        cot::event* ev_ptr = nullptr;

        int piperaw[2];
        assert(pipe(piperaw) == 0);
        cot::set_nonblocking(piperaw[0]);
        cot::set_nonblocking(piperaw[1]);

        std::thread thread_a([&, r = piperaw[0], w = piperaw[1]] {
            cot::set_clock(cot::clock::real_time);

            cot::fd rfd(r), wfd(w);
            cot::event ev;
            ev_ptr = &ev;

            auto fn = [&]() -> cot::task<> {
                auto readable_ev = cot::readable(rfd);
                co_await ev;
                ssize_t nw = ::write(wfd.fileno(), "x", 1);
                assert(nw == 1);
                co_await readable_ev;
            };
            auto t = fn();

            // Signal and immediately enter loop — no waiting
            phase.store(1, std::memory_order_release);
            cot::loop();
            assert(t.done());
        });

        std::thread thread_b([&, round] {
            while (phase.load(std::memory_order_acquire) < 1) {
                std::this_thread::yield();
            }
            // Vary the delay to probe different points in thread A's loop:
            switch (round % 3) {
            case 0:
                // No delay — trigger fires before thread A reaches watch_fds
                break;
            case 1:
                // Single yield — minimal delay, right at the boundary
                std::this_thread::yield();
                break;
            case 2:
                // ~50us — short enough to sometimes beat watch_fds
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                break;
            }
            ev_ptr->trigger();
        });

        thread_a.join();
        thread_b.join();
    }
    std::cerr << "cross_thread_wake_race: ok\n";
}


int main(int argc, char* argv[]) {
    unsigned ran = 0;
    cot::set_clock(cot::clock::real_time);

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
        assert(t.done() && "test did not complete");
    };

    run("pipe_readwrite", test_pipe_readwrite);
    run("readable_event", test_readable_event);
    run("realtime_timer", test_realtime_timer);
    run("tcp_echo", test_tcp_echo);
    run("tcp_multi_frame", test_tcp_multi_frame);
    run("dup_close_fd", test_dup_close_fd);
    run("forget_fd_cleanup", test_forget_fd_cleanup);

    // Cross-thread wake tests use their own threads and drivers
    auto run_threaded = [&](const char* name, auto fn) {
        bool found = argc == 1;
        for (int argi = 1; !found && argi < argc; ++argi) {
            found = strcmp(name, argv[argi]) == 0;
        }
        if (!found) {
            return;
        }
        ++ran;
        std::cerr << "=== " << name << " ===\n";
        fn();
    };

    run_threaded("close_wakes_reader", test_close_wakes_reader);
    run_threaded("shutdown_wakes_reader", test_shutdown_wakes_reader);
    run_threaded("cross_thread_wake", test_cross_thread_wake);
    run_threaded("cross_thread_wake_hammer", test_cross_thread_wake_hammer);
    run_threaded("cross_thread_wake_repeated", test_cross_thread_wake_repeated);
    run_threaded("cross_thread_wake_race", test_cross_thread_wake_race);

    if (ran == 0) {
        std::print(std::cerr, "No matching tests\n");
        exit(1);
    } else {
        std::print(std::cerr, "*** done ***\n");
        exit(0);
    }
}
