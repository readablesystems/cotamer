#include "cotamer/cotamer.hh"
#include "examples/message_stream.hh"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <optional>
#include <print>
#include <thread>
#include <sys/resource.h>
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
        assert(r == strlen(msg));
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


// TEST: Smallest possible cross-thread wake. Specifically guards against
// regressions in the kqueue/epoll wakefd plumbing: a worker thread does
// nothing but trigger an event, the main coroutine awaits it with a
// generous bound, and the elapsed time must be small. With a
// misconfigured wakefd_ (e.g., epoll's `wakefd_` set to the epoll fd
// instead of the eventfd), `migrate_wake` writes to a wrong fd, the
// driver stays blocked in epoll_wait, and the bound assertion fires.
void test_cross_thread_wake_simple() {
    constexpr int ROUNDS = 20;
    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> primed{false};
        cot::event ev;

        std::thread worker([&] {
            while (!primed.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // Brief delay so the main thread is reliably blocked in the
            // notifier (epoll_wait/kevent) when we trigger.
            std::this_thread::sleep_for(5ms);
            ev.trigger();
        });

        auto fn = [&]() -> cot::task<> {
            primed.store(true, std::memory_order_release);
            auto start = std::chrono::steady_clock::now();
            // Long timer is the safety net — if the wake reaches us, we
            // resume in milliseconds; if not, we hit this and fail loud.
            co_await cot::any(ev, cot::after(2s));
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            assert(ev.triggered() && "event was never triggered");
            assert(ms < 500
                   && "cross-thread wake did not reach the driver in time "
                      "(notifier wakefd misconfigured?)");
        };
        auto t = fn();
        cot::loop();
        assert(t.done());
        worker.join();
    }
    std::cerr << "cross_thread_wake_simple: ok\n";
}


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


// TEST: Abandoned fd events do not keep kqueue/epoll registrations alive,
// and are eventually reclaimed.
//
// "Abandoned" = a `cot::event` returned by `readable()`/`writable()` is
// dropped without anyone awaiting it (or after the awaiter is destroyed).
// The event becomes empty (no listeners + only the fderec holds a ref).
// Two things must hold:
//   1. The kqueue/epoll registration must drop on the next `pop_update`
//      (lazy mask recompute filters out empty events). Otherwise the
//      kernel keeps waking us for fds nobody cares about.
//   2. The erec slots must be reclaimed eventually. With the lazy model
//      they linger until a sweep point: a real kqueue fire on the fd,
//      `process_clearing`, or `fd_close`. This test exercises both the
//      kqueue-fire and the close paths.
cot::task<> test_abandoned_event_cleanup() {
    int piperaw[2];
    assert(pipe(piperaw) == 0);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd.fileno());
    cot::set_nonblocking(wfd.fileno());

    auto& drv = *cot::driver::current;
    int fd = rfd.fileno();
    size_t baseline_watches = drv.fds().active_watch_count();
    assert(drv.fds().fd_mask(fd) == cot::fdevent::none);

    // --- Part 1: a single abandoned watch deregisters from kqueue/epoll.
    {
        cot::event re = cot::readable(rfd);
        // re is the only external holder; on scope exit refcount goes to 1
        // (only the fderec) and listeners is empty -> event becomes empty.
    }
    // Allow pop_update to recompute the fd mask.
    co_await cot::after(0ms);
    assert(drv.fds().fd_mask(fd) == cot::fdevent::none
           && "abandoned watch must report mask 0");
    assert(drv.nfdctl() == 0
           && "abandoned watch must not keep a kqueue registration");
    // Lazy: the empty erec is still parked.
    assert(drv.fds().active_watch_count() >= baseline_watches + 1);

    // --- Part 2: many abandoned watches do not blow up nfdctl_, and the
    // pool stays bounded by the number of abandoned events (no hidden
    // duplicates). Then a real kqueue fire reclaims the chain.
    constexpr int N = 500;
    for (int i = 0; i < N; ++i) {
        cot::event re = cot::readable(rfd);
    }
    co_await cot::after(0ms);
    assert(drv.fds().fd_mask(fd) == cot::fdevent::none);
    assert(drv.nfdctl() == 0);
    // Bounded: at most one entry per abandoned readable() plus the carry-over
    // from Part 1 (no spurious extras).
    assert(drv.fds().active_watch_count() <= baseline_watches + N + 1);

    // Now install a live read watch BEFORE there's any data, so the
    // coroutine actually suspends on kqueue. Then write a byte: the
    // kqueue notification fires, take_watches drains the entire chain
    // — the live entry AND every abandoned one piggy-backed on the same
    // notification — and the live coroutine wakes up to read.
    size_t live_nr = 0;
    auto reader = [&]() -> cot::task<> {
        char buf[16];
        live_nr = co_await cot::read_once(rfd, buf, sizeof(buf));
        assert(live_nr == 1 && buf[0] == 'x');
    };
    auto rt = reader();
    co_await cot::after(5ms);   // give read_once time to install its watch
    assert(drv.fds().fd_mask(fd) == cot::fdevent::read
           && "live watch should make fd readable-mask non-zero");
    char ch = 'x';
    assert(::write(wfd.fileno(), &ch, 1) == 1);
    co_await rt;
    // take_watches added fd to update_link inside watch_fds; we need a full
    // loop iteration through pop_update to reconcile the kqueue mask down.
    co_await cot::after(5ms);

    // After the fire, take_watches has emptied the chain of every entry
    // whose mask intersected the kqueue-reported bits. Empty + live alike.
    assert(drv.nfdctl() == 0);
    assert(drv.fds().active_watch_count() == baseline_watches
           && "kqueue fire must reclaim entire matching chain");

    // --- Part 3: abandon some more, then close the fd. fd_close
    // must walk the chain and reclaim everything — no dependence on a
    // kqueue fire, since the fd is going away.
    for (int i = 0; i < 10; ++i) {
        cot::event re = cot::readable(rfd);
    }
    assert(drv.fds().active_watch_count() >= baseline_watches + 10);
    rfd.close();
    co_await cot::after(0ms);
    assert(drv.fds().active_watch_count() == baseline_watches);
    assert(drv.nfdctl() == 0);

    std::cerr << "abandoned_event_cleanup: ok\n";
}


// Helpers for the many-fd / chain-stress / churn tests below. These are
// free functions (not lambdas) to sidestep the coroutine-lambda lifetime
// trap: a `[&]` (or `[&, x]`) lambda that's destroyed before its
// coroutine completes leaves the coroutine with a dangling `this`.
// Parameters of a coroutine, by contrast, are copied (or aliased) into
// the coroutine frame at call time and outlive the call site.
namespace {
cot::task<> many_fds_reader(cot::fd rfd, int expected, int& woken) {
    char buf[1];
    auto nr = co_await cot::read_once(rfd, buf, 1);
    assert(nr == 1);
    assert(buf[0] == char(expected & 0xFF));
    ++woken;
}

cot::task<> wait_readable_helper(cot::fd rfd, int& woken) {
    co_await cot::readable(rfd);
    ++woken;
}

cot::task<> churn_writer(cot::fd wfd) {
    co_await cot::asap();
    char ch = 'x';
    ssize_t nw = ::write(wfd.fileno(), &ch, 1);
    assert(nw == 1);
}

cot::task<> wait_either_helper(cot::fd rfd, int& woken) {
    co_await cot::any(cot::readable(rfd), cot::closed(rfd));
    ++woken;
}

cot::task<> read_until_eof(cot::fd rfd, int& result) {
    char buf[16];
    result = co_await cot::read_once(rfd, buf, sizeof(buf));
}

cot::task<> write_then_close_helper(int raw_wfd, std::string data) {
    co_await cot::asap();
    ssize_t nw = ::write(raw_wfd, data.data(), data.size());
    assert(nw == ssize_t(data.size()));
    ::close(raw_wfd);
}

// Ensure RLIMIT_NOFILE allows at least `target` fds, raising the soft
// limit up to the hard limit if needed. Returns the effective soft
// limit; tests that need many fds should consult this rather than
// blindly opening up to a constant.
size_t ensure_fd_limit(size_t target) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        return 0;
    }
    if (rl.rlim_cur < target) {
        auto bumped = std::min(rl.rlim_max, rlim_t(target));
        rl.rlim_cur = bumped;
        setrlimit(RLIMIT_NOFILE, &rl);   // best effort
        getrlimit(RLIMIT_NOFILE, &rl);
    }
    return size_t(rl.rlim_cur);
}
}


// TEST: Watches across many file descriptors with high fd numbers.
//
// Specifically guards against regressions in `apply_fd_update`'s fdctl_
// shift: `(int) << fdcs` is undefined when fdcs >= 32 (i.e. fd >= 8) and
// on x86 it wraps mod 32, so a watch on fd 8 toggles bit 0 of the wrong
// uint64_t lane. The bug silently corrupts nfdctl_ accounting but only
// when high fd numbers are involved; existing tests with a handful of
// fds never trip it.
//
// We open many pipes, install a read watch on each read end, then write
// one byte to each write end and verify exactly one wake per pipe. After
// everything completes, nfdctl_ must be zero — which is the assertion
// that fails under the shift bug, since the unaccounted bits leak.
cot::task<> test_many_fds() {
    constexpr int N = 64;
    std::vector<cot::fd> rfds, wfds;
    rfds.reserve(N);
    wfds.reserve(N);
    for (int i = 0; i < N; ++i) {
        int piperaw[2];
        assert(pipe(piperaw) == 0);
        cot::set_nonblocking(piperaw[0]);
        cot::set_nonblocking(piperaw[1]);
        rfds.emplace_back(piperaw[0]);
        wfds.emplace_back(piperaw[1]);
    }

    // High-fd-number sanity: at least some of these pipes had to land in
    // the regime that exercises fdcs >= 32 and fdci > 0.
    int max_fd = 0;
    for (auto& f : rfds) {
        if (f.fileno() > max_fd) max_fd = f.fileno();
    }
    for (auto& f : wfds) {
        if (f.fileno() > max_fd) max_fd = f.fileno();
    }
    assert(max_fd >= 16
           && "test environment didn't yield high enough fd numbers");

    int woken = 0;
    std::vector<cot::task<>> tasks;
    tasks.reserve(N);
    for (int i = 0; i < N; ++i) {
        tasks.emplace_back(many_fds_reader(rfds[i], i, woken));
    }

    co_await cot::after(20ms);   // let every reader install its watch

    // Write to wfds in reverse order so the kernel-side notifications
    // arrive interleaved across many fdci/fdcs slots.
    for (int i = N - 1; i >= 0; --i) {
        char ch = char(i & 0xFF);
        assert(::write(wfds[i].fileno(), &ch, 1) == 1);
    }

    for (auto& t : tasks) {
        co_await t;
    }
    assert(woken == N);

    // Drain the update list so apply_fd_update reconciles the kernel
    // side. With the shift bug, fdctl_ tracking drifts and nfdctl_
    // never returns to 0.
    co_await cot::after(20ms);
    auto& drv = *cot::driver::current;
    assert(drv.nfdctl() == 0
           && "nfdctl_ leaked after many-fd activity (shift bug?)");

    std::cerr << "many_fds: ok (N=" << N << ", max_fd=" << max_fd << ")\n";
}


// TEST: Many waiters on the same fd. Stresses the per-fd watch chain in
// `fd_event_set`: take_watch_list partition, freelist, listener
// accounting. After a single kqueue/epoll fire, every waiter must wake
// exactly once and the chain must drain.
cot::task<> test_many_waiters_one_fd() {
    int piperaw[2];
    assert(pipe(piperaw) == 0);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd.fileno());
    cot::set_nonblocking(wfd.fileno());

    constexpr int N = 100;
    int woken = 0;
    std::vector<cot::task<>> waiters;
    waiters.reserve(N);
    for (int i = 0; i < N; ++i) {
        waiters.emplace_back(wait_readable_helper(rfd, woken));
    }

    co_await cot::after(20ms);  // let installs settle

    auto& drv = *cot::driver::current;
    // All N waiters installed independent watches: chain length = N.
    assert(drv.fds().active_watch_count() >= size_t(N));
    assert(drv.fds().fd_mask(rfd.fileno()) == cot::fdevent::read);

    // One kernel notification should fire all N waiters via take_watch_list.
    char ch = 'x';
    assert(::write(wfd.fileno(), &ch, 1) == 1);
    for (auto& t : waiters) {
        co_await t;
    }
    assert(woken == N);

    // Pop_update reconciles afterwards.
    co_await cot::after(20ms);
    assert(drv.fds().fd_mask(rfd.fileno()) == cot::fdevent::none);
    assert(drv.nfdctl() == 0);

    std::cerr << "many_waiters_one_fd: ok (N=" << N << ")\n";
}


// TEST: Mixed-mask watches on a single fd. A read watch and a write
// watch coexist on the same fd. Only the write watch should fire
// initially (an empty socketpair half is always writable); after we
// write to the peer, the read watch fires. This exercises
// take_watch_list's mask-based partition: the write-fire must NOT
// consume the read watch, and vice versa.
cot::task<> test_mixed_mask_one_fd() {
    int sockraw[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockraw) == 0);
    cot::set_nonblocking(sockraw[0]);
    cot::set_nonblocking(sockraw[1]);
    cot::fd a(sockraw[0]), b(sockraw[1]);

    int reads = 0, writes = 0;
    auto reader = [&]() -> cot::task<> {
        co_await cot::readable(a);
        ++reads;
    };
    auto writer = [&]() -> cot::task<> {
        co_await cot::writable(a);
        ++writes;
    };
    auto rt = reader();
    auto wt = writer();

    co_await cot::after(20ms);
    // Empty socket: writable must have fired; readable must not have.
    assert(wt.done() && writes == 1);
    assert(!rt.done() && reads == 0);

    // Now write to peer; readable on `a` should fire.
    char ch = 'x';
    assert(::write(b.fileno(), &ch, 1) == 1);
    co_await rt;
    assert(reads == 1);

    co_await cot::after(20ms);
    auto& drv = *cot::driver::current;
    assert(drv.fds().fd_mask(a.fileno()) == cot::fdevent::none);
    assert(drv.nfdctl() == 0);

    std::cerr << "mixed_mask_one_fd: ok\n";
}


// TEST: Rapid fd churn. Repeatedly open a pipe, install a read watch
// (via read_once on an empty pipe), satisfy it, close. Stresses the
// watchrec freelist, fdrec re-allocation under high turnover, and the
// update_link bookkeeping across many install/teardown cycles.
cot::task<> test_fd_churn() {
    constexpr int ROUNDS = 200;
    auto& drv = *cot::driver::current;
    size_t baseline_watches = drv.fds().active_watch_count();

    for (int round = 0; round < ROUNDS; ++round) {
        int piperaw[2];
        assert(pipe(piperaw) == 0);
        cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
        cot::set_nonblocking(rfd.fileno());
        cot::set_nonblocking(wfd.fileno());

        churn_writer(wfd).detach();

        char buf[1];
        auto nr = co_await cot::read_once(rfd, buf, 1);
        assert(nr == 1);
        // rfd/wfd close at scope end; watchrecs return to freelist.
    }

    co_await cot::after(20ms);
    assert(drv.fds().active_watch_count() == baseline_watches
           && "watchrec leak across churn rounds");
    assert(drv.nfdctl() == 0
           && "nfdctl_ leak across churn rounds");

    std::cerr << "fd_churn: ok (ROUNDS=" << ROUNDS << ")\n";
}


// TEST: fd-number reuse across rounds. Open a pipe, install a watch
// that never fires, close. Open a new pipe (kernel typically returns
// the same fd numbers under "smallest unused" allocation), install a
// fresh watch, drive a real read. The new round's read must succeed
// with no influence from the previous round's stale registration —
// epoch invalidation in `take_watch_list` is the guard.
cot::task<> test_fd_reuse_race() {
    constexpr int ROUNDS = 50;
    auto& drv = *cot::driver::current;
    size_t baseline_watches = drv.fds().active_watch_count();

    for (int round = 0; round < ROUNDS; ++round) {
        // Round A: install + abandon + close.
        {
            int p[2];
            assert(pipe(p) == 0);
            cot::fd rfd(p[0]), wfd(p[1]);
            cot::set_nonblocking(rfd.fileno());
            cot::set_nonblocking(wfd.fileno());
            cot::event re = cot::readable(rfd);   // never satisfied
            co_await cot::after(2ms);             // let pop_update register
            // re drops, fds destroy at scope end → close, deregister, epoch++
        }
        co_await cot::after(2ms);

        // Round B: new pipe (same fd numbers very likely).
        int p[2];
        assert(pipe(p) == 0);
        cot::fd rfd(p[0]), wfd(p[1]);
        cot::set_nonblocking(rfd.fileno());
        cot::set_nonblocking(wfd.fileno());
        churn_writer(wfd).detach();
        char buf[1];
        auto nr = co_await cot::read_once(rfd, buf, 1);
        assert(nr == 1 && buf[0] == 'x');
    }

    co_await cot::after(20ms);
    assert(drv.fds().active_watch_count() == baseline_watches);
    assert(drv.nfdctl() == 0);

    std::cerr << "fd_reuse_race: ok (ROUNDS=" << ROUNDS << ")\n";
}


// TEST: Writer sends data, immediately closes the write end. The reader
// must see the data first, then EOF on a subsequent read. Specifically
// exercises the EPOLLRDHUP / EV_EOF path where the kernel surfaces both
// a read-ready signal AND a hangup; cotamer must not lose the data.
cot::task<> test_write_close_read() {
    constexpr int ROUNDS = 100;
    auto& drv = *cot::driver::current;
    size_t baseline_watches = drv.fds().active_watch_count();

    for (int round = 0; round < ROUNDS; ++round) {
        int p[2];
        assert(pipe(p) == 0);
        cot::fd rfd(p[0]);
        cot::set_nonblocking(rfd.fileno());

        // Writer writes from a separate coroutine (after asap), so the
        // reader has actually installed a watch by the time the data
        // arrives. write_then_close_helper takes ownership of the raw
        // wfd and ::close()'s it after writing.
        write_then_close_helper(p[1], "hello").detach();

        char buf[16];
        auto nr = co_await cot::read_once(rfd, buf, sizeof(buf));
        assert(nr == 5 && std::string(buf, 5) == "hello");

        // Second read: pipe is closed, EOF.
        auto nr2 = co_await cot::read_once(rfd, buf, sizeof(buf));
        assert(nr2 == 0 && "expected EOF after writer close");
    }

    co_await cot::after(20ms);
    assert(drv.fds().active_watch_count() == baseline_watches);
    assert(drv.nfdctl() == 0);

    std::cerr << "write_close_read: ok (ROUNDS=" << ROUNDS << ")\n";
}


// TEST: Storm — many pipes × many waiters per pipe, each waiting on
// `any(readable, closed)`. We then fire each pipe via either a write
// or a close. Combines high chain count (M waiters per fd, each
// installing two watches) with high fd count (NPIPES) to exercise
// take_watch_list partitioning, mask propagation, and the per-fd
// chain bookkeeping all at once.
cot::task<> test_storm() {
    constexpr int NPIPES = 32;
    constexpr int M_PER_PIPE = 8;
    constexpr int total = NPIPES * M_PER_PIPE;

    std::vector<cot::fd> rfds, wfds;
    rfds.reserve(NPIPES);
    wfds.reserve(NPIPES);
    for (int i = 0; i < NPIPES; ++i) {
        int p[2];
        assert(pipe(p) == 0);
        cot::set_nonblocking(p[0]);
        cot::set_nonblocking(p[1]);
        rfds.emplace_back(p[0]);
        wfds.emplace_back(p[1]);
    }

    int woken = 0;
    std::vector<cot::task<>> tasks;
    tasks.reserve(total);
    for (int p = 0; p < NPIPES; ++p) {
        for (int j = 0; j < M_PER_PIPE; ++j) {
            tasks.emplace_back(wait_either_helper(rfds[p], woken));
        }
    }

    co_await cot::after(20ms);

    // Fire each pipe in some order. Mix writes and closes so we hit both
    // EPOLLIN-only and EPOLLRDHUP/EV_EOF paths.
    for (int p = 0; p < NPIPES; ++p) {
        if (p % 3 == 0) {
            wfds[p].close();   // OS fd closes once last driver deregisters
        } else {
            char ch = 'x';
            assert(::write(wfds[p].fileno(), &ch, 1) == 1);
        }
    }

    for (auto& t : tasks) {
        co_await t;
    }
    assert(woken == total);

    co_await cot::after(20ms);
    auto& drv = *cot::driver::current;
    assert(drv.nfdctl() == 0);

    std::cerr << "storm: ok (NPIPES=" << NPIPES << ", M=" << M_PER_PIPE
              << ", total=" << total << ")\n";
}


// TEST: Many file descriptors (>100). Like `many_fds` but stresses fd
// numbers that span more fdci lanes (fd / 16) and higher fdcs offsets.
// Bumps RLIMIT_NOFILE if needed; skips with an explanatory failure if
// the environment can't accommodate.
cot::task<> test_many_fds_huge() {
    constexpr size_t TARGET = 150;
    size_t limit = ensure_fd_limit(TARGET * 2 + 64);
    if (limit < TARGET * 2 + 16) {
        std::cerr << "many_fds_huge: SKIP (RLIMIT_NOFILE too low: "
                  << limit << ")\n";
        co_return;
    }

    std::vector<cot::fd> rfds, wfds;
    rfds.reserve(TARGET);
    wfds.reserve(TARGET);
    size_t N = 0;
    for (size_t i = 0; i < TARGET; ++i) {
        int p[2];
        if (pipe(p) != 0) {
            break;
        }
        cot::set_nonblocking(p[0]);
        cot::set_nonblocking(p[1]);
        rfds.emplace_back(p[0]);
        wfds.emplace_back(p[1]);
        ++N;
    }
    assert(N >= 100 && "couldn't allocate at least 100 pipes");

    int max_fd = 0;
    for (auto& f : rfds) max_fd = std::max(max_fd, f.fileno());
    for (auto& f : wfds) max_fd = std::max(max_fd, f.fileno());

    int woken = 0;
    std::vector<cot::task<>> tasks;
    tasks.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        tasks.emplace_back(many_fds_reader(rfds[i], int(i), woken));
    }

    co_await cot::after(50ms);

    // Write in interleaved order to scatter notifications across many
    // fdci/fdcs lanes.
    for (size_t step = 0; step < N; ++step) {
        size_t i = (step * 17 + 3) % N;   // pseudo-shuffle
        char ch = char(i & 0xFF);
        assert(::write(wfds[i].fileno(), &ch, 1) == 1);
    }

    for (auto& t : tasks) {
        co_await t;
    }
    assert(size_t(woken) == N);

    co_await cot::after(50ms);
    auto& drv = *cot::driver::current;
    assert(drv.nfdctl() == 0);

    std::cerr << "many_fds_huge: ok (N=" << N << ", max_fd=" << max_fd << ")\n";
}


// TEST: Close storm. Many pipes; one driver thread awaits read_once on
// every read end (each will see EOF). Multiple closer threads run in
// parallel and each closes its slice of write ends. Exercises
// `migrate_fd_close` under concurrent producers — every close routes
// through `fd_body::close` on the closer thread, which posts to
// the reader thread's `migrate_fd_close_` list. Reader must observe
// EOF on every fd and exit cleanly.
void test_close_storm() {
    constexpr int NTHREADS = 4;
    constexpr int NPIPES_PER_THREAD = 25;
    constexpr int N = NTHREADS * NPIPES_PER_THREAD;

    std::atomic<int> phase{0};
    std::vector<int> raw_w(N);
    std::vector<int> results(N, -99);

    std::thread reader([&]() {
        cot::set_clock(cot::clock::virtual_time);

        std::vector<cot::fd> rfds;
        rfds.reserve(N);
        std::vector<cot::task<>> tasks;
        tasks.reserve(N);

        for (int i = 0; i < N; ++i) {
            int p[2];
            assert(pipe(p) == 0);
            cot::set_nonblocking(p[0]);
            rfds.emplace_back(p[0]);
            raw_w[i] = p[1];
            tasks.emplace_back(read_until_eof(rfds[i], results[i]));
        }

        phase.store(1, std::memory_order_release);
        cot::loop();

        for (auto& t : tasks) {
            assert(t.done());
        }
    });

    std::vector<std::thread> closers;
    closers.reserve(NTHREADS);
    for (int t = 0; t < NTHREADS; ++t) {
        closers.emplace_back([&phase, &raw_w, t]() {
            while (phase.load(std::memory_order_acquire) < 1) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(5ms);
            for (int i = t * NPIPES_PER_THREAD;
                 i < (t + 1) * NPIPES_PER_THREAD;
                 ++i) {
                ::close(raw_w[i]);
            }
        });
    }

    reader.join();
    for (auto& c : closers) {
        c.join();
    }

    for (int i = 0; i < N; ++i) {
        assert(results[i] == 0
               && "reader did not see EOF on every fd");
    }

    std::cerr << "close_storm: ok (N=" << N
              << ", threads=" << NTHREADS << ")\n";
}


// TEST: clear() exits cleanly when fd events are outstanding.
// A coroutine awaits readable() on a pipe (no data will arrive), then
// clear() is called. The clearing mechanism must force-trigger the fd
// event so the coroutine wakes up, throws clearing_exception, and
// loop() exits.
void test_clear_with_fd_events() {
    cot::reset();

    int piperaw[2];
    assert(pipe(piperaw) == 0);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd.fileno());
    cot::set_nonblocking(wfd.fileno());

    bool reader_cleared = false;

    auto reader = [&]() -> cot::task<> {
        try {
            co_await cot::readable(rfd);
            assert(false && "readable should have thrown clearing_exception");
        } catch (cot::detail::clearing_exception&) {
            reader_cleared = true;
        }
    };
    auto t = reader();

    cot::clear();
    cot::loop();

    assert(reader_cleared);
    assert(t.done());
    std::cerr << "clear_with_fd_events: ok\n";
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
    run("abandoned_event_cleanup", test_abandoned_event_cleanup);
    run("many_fds", test_many_fds);
    run("many_fds_huge", test_many_fds_huge);
    run("many_waiters_one_fd", test_many_waiters_one_fd);
    run("mixed_mask_one_fd", test_mixed_mask_one_fd);
    run("fd_churn", test_fd_churn);
    run("fd_reuse_race", test_fd_reuse_race);
    run("write_close_read", test_write_close_read);
    run("storm", test_storm);

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

    run_threaded("clear_with_fd_events", test_clear_with_fd_events);
    run_threaded("close_wakes_reader", test_close_wakes_reader);
    run_threaded("shutdown_wakes_reader", test_shutdown_wakes_reader);
    run_threaded("close_storm", test_close_storm);
    run_threaded("cross_thread_wake_simple", test_cross_thread_wake_simple);
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
