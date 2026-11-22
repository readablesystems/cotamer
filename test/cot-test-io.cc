#include "cotamer/cotamer.hh"
#include "examples/message_stream.hh"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <sys/resource.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <netdb.h>
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
        assert(*r == strlen(msg));
    };

    // Reader: read from the pipe
    auto reader = [&]() -> cot::task<> {
        char buf[64] = {};
        auto r = co_await cot::read_once(rfd, buf, sizeof(buf));
        assert(*r > 0);
        assert(std::string(buf, *r) == "hello from pipe");
        std::cerr << "pipe_readwrite: read \"" << std::string(buf, *r) << "\"\n";
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
                result = *(co_await cot::read_once(sfd, buf, sizeof(buf)));
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
                result = *(co_await cot::read_once(sfd, buf, sizeof(buf)));
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
//      `process_clearing`, or closing the fd. This test exercises both the
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
        live_nr = *(co_await cot::read_once(rfd, buf, sizeof(buf)));
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

    // --- Part 3: abandon some more, then close the fd. Closing
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
    result = *(co_await cot::read_once(rfd, buf, sizeof(buf)));
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
        assert(*nr == 1);
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
        assert(*nr == 1 && buf[0] == 'x');
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
        assert(*nr == 5 && std::string(buf, 5) == "hello");

        // Second read: pipe is closed, EOF.
        auto nr2 = co_await cot::read_once(rfd, buf, sizeof(buf));
        assert(*nr2 == 0 && "expected EOF after writer close");
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


// TEST: virtual time advances while an idle fd is watched
void test_virtual_time_idle_fd() {
    cot::set_clock(cot::clock::virtual_time);
    cot::reset();

    int p[2];
    assert(pipe(p) == 0);
    cot::set_nonblocking(p[0]);
    cot::fd rfd(p[0]), wfd(p[1]);

    auto start = cot::steady_now();
    bool read_fired = false;
    auto reader = [&]() -> cot::task<> {
        co_await cot::readable(rfd);
        read_fired = true;
    };
    auto writer = [&]() -> cot::task<> {
        co_await cot::after(1h);
        assert(!read_fired);
        ssize_t nw = ::write(wfd.fileno(), "x", 1);
        assert(nw == 1);
    };
    auto rt = reader();
    auto wt = writer();
    cot::loop();

    assert(rt.done() && wt.done() && read_fired);
    assert(cot::steady_now() - start >= 1h);
    cot::set_clock(cot::clock::real_time);
    std::cerr << "virtual_time_idle_fd: ok\n";
}

// TEST: waiting on `closed(f)` doesn't busy-poll
static double cpu_seconds() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6
        + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

cot::task<> test_closed_no_spin() {
    int sockraw[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockraw) == 0);
    cot::set_nonblocking(sockraw[0]);
    cot::set_nonblocking(sockraw[1]);
    cot::fd a(sockraw[0]), b(sockraw[1]);

    // Pending unread data must not make a close-only watch spin either.
    assert(::write(b.fileno(), "x", 1) == 1);

    auto closer = [&]() -> cot::task<> {
        co_await cot::after(300ms);
        b.close();
    };
    auto ct = closer();
    double cpu0 = cpu_seconds();
    co_await cot::closed(a);
    double cpu = cpu_seconds() - cpu0;
    assert(ct.done());
    assert(cpu < 0.1 && "closed() wait burned CPU");

    // any(readable, closed) on a socket with no data
    int sockraw2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockraw2) == 0);
    cot::set_nonblocking(sockraw2[0]);
    cot::set_nonblocking(sockraw2[1]);
    cot::fd c(sockraw2[0]), d(sockraw2[1]);
    auto closer2 = [&]() -> cot::task<> {
        co_await cot::after(300ms);
        d.close();
    };
    auto ct2 = closer2();
    cpu0 = cpu_seconds();
    co_await cot::any(cot::readable(c), cot::closed(c));
    cpu = cpu_seconds() - cpu0;
    assert(ct2.done());
    assert(cpu < 0.1 && "any(readable, closed) wait burned CPU");
    std::cerr << "closed_no_spin: ok\n";
}

// TEST: switching an fd between close-only and read interest. On kqueue
// these use different EVFILT_READ modes (edge vs. level).
cot::task<> test_closed_then_readable() {
    int sockraw[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockraw) == 0);
    cot::set_nonblocking(sockraw[0]);
    cot::set_nonblocking(sockraw[1]);
    cot::fd a(sockraw[0]), b(sockraw[1]);

    cot::event cev = cot::closed(a);
    co_await cot::after(10ms);
    assert(!cev.triggered());

    // add read interest; data arrives; readable fires, closed doesn't
    cot::event rev = cot::readable(a);
    assert(::write(b.fileno(), "x", 1) == 1);
    co_await rev;
    assert(!cev.triggered());

    // level-triggered: still readable while the byte is unread
    co_await cot::readable(a);
    char ch;
    assert(::read(a.fileno(), &ch, 1) == 1);

    // back to close-only; EOF fires it
    co_await cot::after(10ms);
    assert(!cev.triggered());
    b.close();
    co_await cev;
    std::cerr << "closed_then_readable: ok\n";
}

// TEST: `closed(f)` in virtual time doesn't stop time from advancing
void test_virtual_time_closed() {
    cot::set_clock(cot::clock::virtual_time);
    cot::reset();

    int sockraw[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockraw) == 0);
    cot::set_nonblocking(sockraw[0]);
    cot::set_nonblocking(sockraw[1]);
    cot::fd a(sockraw[0]), b(sockraw[1]);

    auto start = cot::steady_now();
    bool closed_fired = false;
    auto waiter = [&]() -> cot::task<> {
        co_await cot::closed(a);
        closed_fired = true;
    };
    auto closer = [&]() -> cot::task<> {
        co_await cot::after(1h);
        assert(!closed_fired);
        b.close();
    };
    auto wt = waiter();
    auto ct = closer();
    cot::loop();

    assert(wt.done() && ct.done() && closed_fired);
    assert(cot::steady_now() - start >= 1h);
    cot::set_clock(cot::clock::real_time);
    std::cerr << "virtual_time_closed: ok\n";
}

// TEST: readable() and writable() on a regular file fire promptly. (epoll
// rejects regular files with EPERM; those watches used to throw from the
// driver loop.)
cot::task<> test_regular_file_ready() {
    const char* tmpdir = getenv("TMPDIR");
    std::string path = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp")
        + "/cot-test-io-XXXXXX";
    int fileno = mkstemp(path.data());
    assert(fileno >= 0);
    unlink(path.c_str());
    assert(::write(fileno, "hello", 5) == 5);
    assert(lseek(fileno, 0, SEEK_SET) == 0);
    cot::fd f(fileno);

    auto start = cot::now();
    auto r = co_await cot::attempt(
        [](cot::fd f) -> cot::task<> { co_await cot::readable(f); }(f),
        cot::after(1s));
    assert(r.has_value());
    auto w = co_await cot::attempt(
        [](cot::fd f) -> cot::task<> { co_await cot::writable(f); }(f),
        cot::after(1s));
    assert(w.has_value());
    // a second watch after the first fired also works
    auto r2 = co_await cot::attempt(
        [](cot::fd f) -> cot::task<> { co_await cot::readable(f); }(f),
        cot::after(1s));
    assert(r2.has_value());
    assert(cot::now() - start < 500ms);
    std::cerr << "regular_file_ready: ok\n";
}

// TEST: waiting on sub-millisecond timers doesn't busy-poll (epoll and poll
// take millisecond timeouts, which used to be truncated to 0)
cot::task<> test_submillisecond_timer_no_spin() {
    double cpu0 = cpu_seconds();
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i != 200; ++i) {
        co_await cot::after(900us);
    }
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    double cpu = cpu_seconds() - cpu0;
    assert(cpu < 0.1 * wall && "sub-millisecond timer waits burned CPU");
    std::cerr << "submillisecond_timer_no_spin: ok (cpu " << cpu << "s, wall " << wall << "s)\n";
}

// TEST: an fd number closed and reused behind cotamer’s back. The old fd’s
// watches wake, and the new fd’s watches work (its kernel registration
// is set up fresh).
cot::task<> test_fd_reused_behind_back() {
    int p1[2], p2[2];
    assert(pipe(p1) == 0);
    cot::set_nonblocking(p1[0]);
    cot::fd a(p1[0]);
    cot::event aev = cot::readable(a);
    co_await cot::after(10ms);      // let the loop register `a`
    assert(!aev.triggered());

    int n = p1[0];
    ::close(n);                     // behind cotamer’s back
    assert(pipe(p2) == 0);
    if (p2[0] != n) {
        assert(dup2(p2[0], n) == n);
        ::close(p2[0]);
        p2[0] = n;
    }
    cot::set_nonblocking(p2[0]);
    cot::fd b(p2[0]);
    cot::event bev = cot::readable(b);
    assert(aev.triggered());
    co_await cot::after(10ms);
    assert(!bev.triggered());
    assert(::write(p2[1], "x", 1) == 1);
    co_await cot::any(bev, cot::after(1s));
    assert(bev.triggered());

    ::close(p1[1]);
    ::close(p2[1]);
    // `b` is destroyed before `a`; `a`’s destructor then closes `n` again,
    // which harmlessly fails.
    std::cerr << "fd_reused_behind_back: ok\n";
}

// TEST: readiness events on a closed or never-opened fd trigger immediately,
// with user flags reporting the requested mask; an empty mask never fires
cot::task<> test_closed_fd_ready() {
    int p[2];
    assert(pipe(p) == 0);
    cot::fd r(p[0]), w(p[1]);
    r.close();
    cot::fd none;
    for (cot::fd* f : {&r, &none}) {
        cot::event rev = cot::readable(*f), wev = cot::writable(*f),
            cev = cot::closed(*f);
        assert(rev.triggered() && rev.user_flags() == int(cot::fdevent::read));
        assert(wev.triggered() && wev.user_flags() == int(cot::fdevent::write));
        assert(cev.triggered() && cev.user_flags() == int(cot::fdevent::close));
        co_await cot::any(rev, cot::after(1s));
        assert(!cot::file_event(*f, cot::fdevent::none).triggered());
    }
    std::cerr << "closed_fd_ready: ok\n";
}

static uint16_t local_port(int fileno) {
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    assert(getsockname(fileno, reinterpret_cast<sockaddr*>(&ss), &len) == 0);
    return ntohs(ss.ss_family == AF_INET6
                 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port
                 : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
}

static uint16_t peer_port(int fileno) {
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(fileno, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return 0;
    }
    return ntohs(ss.ss_family == AF_INET6
                 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port
                 : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
}

// TEST: a connection reset before it is accepted doesn't make tcp_accept
// throw (macOS/BSD report ECONNABORTED for it)
cot::task<> test_accept_after_reset() {
    uint16_t port = 19881;
    auto lfd = co_await cot::tcp_listen("127.0.0.1:" + std::to_string(port));

    // client 1 connects, then resets the connection before it's accepted
    int c1 = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::connect(c1, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) == 0);
    linger lg{1, 0};
    setsockopt(c1, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    ::close(c1);
    co_await cot::after(20ms);

    // client 2 connects normally; accept until we get it (Linux may first
    // hand back client 1's dead connection)
    auto c2 = co_await cot::tcp_connect("127.0.0.1:" + std::to_string(port));
    uint16_t c2port = local_port(c2.fileno());
    bool found = false;
    for (int i = 0; i != 3 && !found; ++i) {
        auto afd = co_await cot::tcp_accept(lfd);
        found = peer_port(afd.fileno()) == c2port;
    }
    assert(found);
    std::cerr << "accept_after_reset: ok\n";
}

// TEST: only one UDP socket can bind a given address and port
cot::task<> test_udp_port_exclusive() {
    std::string address = "127.0.0.1:19882";
    auto u1 = co_await cot::udp_listen(address);
    bool in_use = false;
    try {
        auto u2 = co_await cot::udp_listen(address);
    } catch (const std::system_error& e) {
        in_use = e.code() == std::errc::address_in_use;
    }
    assert(in_use);
    std::cerr << "udp_port_exclusive: ok\n";
}

// TEST: address resolution errors throw `std::system_error` with
// getaddrinfo_error_category()
cot::task<> test_resolve_error() {
    for (int i = 0; i != 4; ++i) {
        std::error_code ec;
        try {
            if (i == 0) {
                co_await cot::tcp_connect("localhost");
            } else if (i == 1) {
                co_await cot::tcp_listen("localhost");
            } else if (i == 2) {
                co_await cot::udp_connect("localhost");
            } else {
                co_await cot::udp_listen("localhost");
            }
        } catch (const std::system_error& e) {
            ec = e.code();
        }
        assert(ec.category() == cot::getaddrinfo_error_category());
        assert(ec.value() == EAI_NONAME);
    }
    std::cerr << "resolve_error: ok\n";
}

// TEST: listening with an empty host (`:PORT`) accepts both IPv4 and IPv6
// connections when IPv6 is available, with tcp_listen_all and tcp_listen
static cot::task<cot::fd> accept_any(cot::fd_list& lfds) {
    while (true) {
        for (auto& lfd : lfds) {
            if (auto f = co_await cot::attempt(cot::tcp_accept(lfd), cot::after(20ms))) {
                co_return std::move(*f);
            }
        }
    }
}

cot::task<> test_listen_wildcard_dual_stack() {
    uint16_t port = 19883;
    int probe = socket(AF_INET6, SOCK_STREAM, 0);
    bool have_ipv6 = probe >= 0;
    if (probe >= 0) {
        ::close(probe);
    }
    // tcp_listen_all: one dual-stack socket, or one per family
    auto lfds = co_await cot::tcp_listen_all(":" + std::to_string(port));
    auto c4 = co_await cot::tcp_connect("127.0.0.1:" + std::to_string(port));
    auto a4 = co_await cot::attempt(accept_any(lfds), cot::after(1s));
    assert(a4);
    if (have_ipv6) {
        auto c6 = co_await cot::tcp_connect("[::1]:" + std::to_string(port));
        auto a6 = co_await cot::attempt(accept_any(lfds), cot::after(1s));
        assert(a6);
    }
    size_t nall = lfds.size();
    lfds.clear();

    // tcp_listen: a single socket covers both families
    auto lfd = co_await cot::tcp_listen(":" + std::to_string(port + 1));
    auto d4 = co_await cot::tcp_connect("127.0.0.1:" + std::to_string(port + 1));
    auto b4 = co_await cot::attempt(cot::tcp_accept(lfd), cot::after(1s));
    assert(b4);
    if (have_ipv6) {
        auto d6 = co_await cot::tcp_connect("[::1]:" + std::to_string(port + 1));
        auto b6 = co_await cot::attempt(cot::tcp_accept(lfd), cot::after(1s));
        assert(b6);
    }
    std::cerr << "listen_wildcard_dual_stack: ok (tcp_listen_all: " << nall
              << " sockets" << (have_ipv6 ? "" : "; no IPv6") << ")\n";
}

// TEST: sockets cotamer creates are close-on-exec; set_cloexec toggles it
static bool is_cloexec(const cot::fd& f) {
    int flags = fcntl(f.fileno(), F_GETFD, 0);
    assert(flags >= 0);
    return (flags & FD_CLOEXEC) != 0;
}

cot::task<> test_sockets_cloexec() {
    std::string taddr = "127.0.0.1:19885", uaddr = "127.0.0.1:19886";
    auto lfd = co_await cot::tcp_listen(taddr);
    auto cfd = co_await cot::tcp_connect(taddr);
    auto afd = co_await cot::tcp_accept(lfd);
    auto ufd = co_await cot::udp_listen(uaddr);
    auto ucfd = co_await cot::udp_connect(uaddr);
    for (auto* f : {&lfd, &cfd, &afd, &ufd, &ucfd}) {
        assert(is_cloexec(*f));
    }
    cot::set_cloexec(afd, false);
    assert(!is_cloexec(afd));
    cot::set_cloexec(afd);
    assert(is_cloexec(afd));
    std::cerr << "sockets_cloexec: ok\n";
}

// TEST: accept() survives file descriptor exhaustion: it backs off on EMFILE
// and accepts connections once descriptors are available again. (On Linux,
// connections pending during EMFILE stay queued; on macOS, they are dropped.)
cot::task<> test_accept_fd_exhaustion() {
    auto lfd = co_await cot::tcp_listen("127.0.0.1:0");
    sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);
    int r = getsockname(lfd.fileno(), reinterpret_cast<sockaddr*>(&ss), &sslen);
    assert(r == 0);

    std::vector<cot::fd> clients;
    auto add_client = [&] {
        int c = socket(ss.ss_family, SOCK_STREAM, 0);
        assert(c >= 0);
        int cr = ::connect(c, reinterpret_cast<sockaddr*>(&ss), sslen);
        assert(cr == 0);
        (void) cr;
        clients.emplace_back(c);
    };

    // Queue connections in the listen backlog
    constexpr size_t nclients = 6;
    for (size_t i = 0; i != nclients; ++i) {
        add_client();
    }
    // ensure the driver has allocated its own notification fds
    co_await cot::after(1ms);

    // Leave room for at most two more file descriptors
    struct rlimit old_rl;
    getrlimit(RLIMIT_NOFILE, &old_rl);
    int probe = dup(lfd.fileno());
    assert(probe >= 0);
    ::close(probe);
    struct rlimit rl = old_rl;
    rl.rlim_cur = probe + 2;
    r = setrlimit(RLIMIT_NOFILE, &rl);
    assert(r == 0);

    std::vector<cot::fd> accepted;
    auto acceptor = [](cot::fd lfd, std::vector<cot::fd>& accepted) -> cot::task<> {
        while (true) {
            accepted.push_back(co_await cot::accept(lfd));
        }
    }(lfd, accepted);

    co_await cot::after(100ms);
    assert(!acceptor.done());
    size_t naccepted = accepted.size();
    assert(naccepted < nclients);

    // After the limit is restored, new connections are accepted
    r = setrlimit(RLIMIT_NOFILE, &old_rl);
    assert(r == 0);
    add_client();
    add_client();
    for (int i = 0; i != 300 && accepted.size() < naccepted + 2; ++i) {
        co_await cot::after(10ms);
    }
    assert(accepted.size() >= naccepted + 2);
    assert(!acceptor.done());
    std::cerr << "accept_fd_exhaustion: ok\n";
}


using signal_handler_type = void (*)(int);

static signal_handler_type signal_action(int signo) {
    struct sigaction sa;
    assert(sigaction(signo, nullptr, &sa) == 0);
    return sa.sa_handler;
}

static cot::task<> kill_after(cot::duration d, int signo, int n = 1) {
    co_await cot::after(d);
    for (int i = 0; i != n; ++i) {
        kill(getpid(), signo);
    }
}

// TEST: at_signal waits keep loop() running, coalesce signals, miss earlier
// signals, and restore the previous action when done
cot::task<> test_at_signal() {
    signal(SIGUSR1, SIG_IGN);
    auto start = std::chrono::steady_clock::now();
    cot::event e = cot::at_signal(SIGUSR1);
    assert(signal_action(SIGUSR1) != SIG_IGN && signal_action(SIGUSR1) != SIG_DFL);
    kill_after(20ms, SIGUSR1, 3).detach();
    co_await e;
    assert(std::chrono::steady_clock::now() - start >= 20ms);

    cot::event e2 = cot::at_signal(SIGUSR1);
    co_await cot::any(e2, cot::after(30ms));
    assert(!e2.triggered());
    e2 = cot::event();

    // waiting again in a loop keeps our handler installed
    for (int i = 0; i != 3; ++i) {
        kill_after(5ms, SIGUSR1).detach();
        co_await cot::at_signal(SIGUSR1);
        assert(signal_action(SIGUSR1) != SIG_IGN);
    }
    co_await cot::after(1ms);
    assert(signal_action(SIGUSR1) == SIG_IGN);
    signal(SIGUSR1, SIG_DFL);
    std::cerr << "at_signal: ok\n";
}

// TEST: waits for different signals trigger independently
cot::task<> test_at_signal_independent() {
    cot::event e1 = cot::at_signal(SIGUSR1), e2 = cot::at_signal(SIGUSR2);
    kill_after(5ms, SIGUSR2).detach();
    co_await e2;
    assert(!e1.triggered());
    kill_after(5ms, SIGUSR1).detach();
    co_await e1;
    co_await cot::after(1ms);
    assert(signal_action(SIGUSR1) == SIG_DFL && signal_action(SIGUSR2) == SIG_DFL);
    std::cerr << "at_signal_independent: ok\n";
}

// TEST: bad signal numbers throw
cot::task<> test_at_signal_bad() {
    for (int signo : {0, -1, SIGKILL, SIGSTOP, NSIG}) {
        bool threw = false;
        try {
            (void) cot::at_signal(signo);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
    std::cerr << "at_signal_bad: ok\n";
    co_return;
}

// TEST: abandoned waits don't keep loop() or our handler; reset() cancels
// waits
void test_at_signal_lifetime() {
    cot::reset();
    {
        cot::event e = cot::at_signal(SIGUSR2);
        assert(signal_action(SIGUSR2) != SIG_DFL);
    }
    cot::loop();
    assert(signal_action(SIGUSR2) == SIG_DFL);

    bool woke = false;
    auto waiter = [](bool& woke) -> cot::task<> {
        co_await cot::at_signal(SIGUSR2);
        woke = true;
    };
    auto t = waiter(woke);
    assert(signal_action(SIGUSR2) != SIG_DFL);
    cot::reset();
    assert(!woke && signal_action(SIGUSR2) == SIG_DFL);
    std::cerr << "at_signal_lifetime: ok\n";
}

// TEST: a signal triggers waits in every driver
void test_at_signal_threads() {
    cot::reset();
    std::atomic<int> ready{0}, woke{0};
    auto waiter = [&]() -> cot::task<> {
        cot::event e = cot::at_signal(SIGUSR1);
        ++ready;
        co_await cot::any(e, cot::after(5s));
        if (e.triggered()) {
            ++woke;
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i != 3; ++i) {
        threads.emplace_back([&] {
            cot::reset();
            auto t = waiter();
            cot::loop();
            cot::reset();
        });
    }
    auto t = waiter();
    while (ready.load() != 4) {
        std::this_thread::yield();
    }
    kill(getpid(), SIGUSR1);
    cot::loop();
    for (auto& th : threads) {
        th.join();
    }
    assert(woke.load() == 4);
    assert(signal_action(SIGUSR1) == SIG_DFL);
    std::cerr << "at_signal_threads: ok\n";
}


// TEST: a signal handled on another thread triggers the driver's wait
void test_at_signal_other_thread() {
    cot::reset();
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    sigset_t old;
    pthread_sigmask(SIG_BLOCK, &usr1, &old);
    std::atomic<bool> done{false};
    // SIGUSR1 is delivered here
    std::thread receiver([&] {
        pthread_sigmask(SIG_UNBLOCK, &usr1, nullptr);
        while (!done.load()) {
            std::this_thread::sleep_for(1ms);
        }
    });
    auto fn = [&]() -> cot::task<> {
        cot::event e = cot::at_signal(SIGUSR1);
        kill_after(20ms, SIGUSR1).detach();
        co_await cot::any(e, cot::after(5s));
        assert(e.triggered());
    };
    auto start = std::chrono::steady_clock::now();
    auto t = fn();
    cot::loop();
    assert(t.done());
    assert(std::chrono::steady_clock::now() - start < 2s);
    done = true;
    receiver.join();
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    assert(signal_action(SIGUSR1) == SIG_DFL);
    std::cerr << "at_signal_other_thread: ok\n";
}


// TEST: weak waits don't keep loop() running, but keep our handler; a signal
// that arrives outside loop() triggers them on the next loop()
void test_at_signal_weak() {
    cot::reset();
    bool woke = false;
    auto waiter = [](bool& woke) -> cot::task<> {
        co_await cot::at_signal_weak(SIGUSR1);
        woke = true;
    };
    auto t = waiter(woke);
    auto start = std::chrono::steady_clock::now();
    cot::loop();                        // returns at once
    assert(std::chrono::steady_clock::now() - start < 1s);
    assert(!woke && !t.done());
    assert(signal_action(SIGUSR1) != SIG_DFL);
    kill(getpid(), SIGUSR1);            // caught; nothing runs yet
    assert(!woke);
    cot::loop();
    assert(woke && t.done());
    assert(signal_action(SIGUSR1) == SIG_DFL);

    // a weak wait triggers while other work keeps the loop running
    auto timed = []() -> cot::task<> {
        kill_after(10ms, SIGUSR2).detach();
        cot::event e = cot::at_signal_weak(SIGUSR2);
        co_await cot::any(e, cot::after(5s));
        assert(e.triggered());
    };
    auto t2 = timed();
    cot::loop();
    assert(t2.done());

    // an abandoned weak wait gives back the handler
    {
        cot::event e = cot::at_signal_weak(SIGUSR2);
        assert(signal_action(SIGUSR2) != SIG_DFL);
    }
    cot::loop();
    assert(signal_action(SIGUSR2) == SIG_DFL);

    // reset() destroys a coroutine in a weak wait and restores the action
    woke = false;
    auto t3 = waiter(woke);
    assert(signal_action(SIGUSR1) != SIG_DFL);
    cot::reset();
    assert(!woke && signal_action(SIGUSR1) == SIG_DFL);

    // clearing also wakes weak waits begun while clearing
    auto stubborn = [](bool& woke) -> cot::task<> {
        try {
            co_await cot::at_signal_weak(SIGUSR1);
        } catch (...) {
        }
        co_await cot::at_signal_weak(SIGUSR1);
        woke = true;
    };
    auto t4 = stubborn(woke);
    cot::clear();
    cot::loop();
    assert(t4.done() && !woke && signal_action(SIGUSR1) == SIG_DFL);
    std::cerr << "at_signal_weak: ok\n";
}


// TEST: reset() clears the old driver while it is current, so a coroutine
// that waits again after being cleared doesn't wait on the new driver
void test_reset_rewait() {
    cot::reset();
    auto stubborn = []() -> cot::task<> {
        try {
            co_await cot::after(1h);
        } catch (...) {
        }
        co_await cot::after(1h);
    };
    auto t = stubborn();
    cot::reset();
    assert(t.done());
    assert(cot::driver::current->empty());
    std::cerr << "reset_rewait: ok\n";
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
        t.promise_ptr()->result();      // rethrow if the test threw
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
    run("closed_no_spin", test_closed_no_spin);
    run("closed_then_readable", test_closed_then_readable);
    run("regular_file_ready", test_regular_file_ready);
    run("submillisecond_timer_no_spin", test_submillisecond_timer_no_spin);
    run("fd_reused_behind_back", test_fd_reused_behind_back);
    run("closed_fd_ready", test_closed_fd_ready);
    run("accept_after_reset", test_accept_after_reset);
    run("udp_port_exclusive", test_udp_port_exclusive);
    run("resolve_error", test_resolve_error);
    run("listen_wildcard_dual_stack", test_listen_wildcard_dual_stack);
    run("sockets_cloexec", test_sockets_cloexec);
    run("accept_fd_exhaustion", test_accept_fd_exhaustion);
    run("at_signal", test_at_signal);
    run("at_signal_independent", test_at_signal_independent);
    run("at_signal_bad", test_at_signal_bad);

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
    run_threaded("virtual_time_idle_fd", test_virtual_time_idle_fd);
    run_threaded("virtual_time_closed", test_virtual_time_closed);
    run_threaded("close_wakes_reader", test_close_wakes_reader);
    run_threaded("shutdown_wakes_reader", test_shutdown_wakes_reader);
    run_threaded("close_storm", test_close_storm);
    run_threaded("cross_thread_wake_simple", test_cross_thread_wake_simple);
    run_threaded("cross_thread_wake", test_cross_thread_wake);
    run_threaded("cross_thread_wake_hammer", test_cross_thread_wake_hammer);
    run_threaded("cross_thread_wake_repeated", test_cross_thread_wake_repeated);
    run_threaded("cross_thread_wake_race", test_cross_thread_wake_race);
    run_threaded("at_signal_lifetime", test_at_signal_lifetime);
    run_threaded("at_signal_threads", test_at_signal_threads);
    run_threaded("at_signal_other_thread", test_at_signal_other_thread);
    run_threaded("at_signal_weak", test_at_signal_weak);
    run_threaded("reset_rewait", test_reset_rewait);

    if (ran == 0) {
        std::print(std::cerr, "No matching tests\n");
        exit(1);
    } else {
        std::print(std::cerr, "*** done ***\n");
        exit(0);
    }
}
