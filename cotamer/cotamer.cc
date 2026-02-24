#include "cotamer.hh"
#include <iterator>
#include <memory>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#if !COTAMER_USE_KQUEUE && !COTAMER_USE_EPOLL && !COTAMER_USE_POLL
# if defined(__APPLE__) || defined(__FreeBSD__)
#  define COTAMER_USE_KQUEUE 1
# elif defined(__linux__)
#  define COTAMER_USE_EPOLL 1
# else
#  define COTAMER_USE_POLL 1
# endif
#endif
#if COTAMER_USE_KQUEUE
# include <sys/event.h>
#elif COTAMER_USE_EPOLL
# include <sys/epoll.h>
# include <sys/eventfd.h>
#else
# include <poll.h>
#endif

using namespace std::chrono_literals;

namespace cotamer {

struct timespec duration_timespec(clock::duration d) {
    if (d <= clock::duration::zero()) {
        return {0, 0};
    }
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(d);
    auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(d - sec);
    return {sec.count(), nsec.count()};
}

int duration_milliseconds(clock::duration d) {
    if (d <= clock::duration::zero()) {
        return 0;
    }
    auto msec = std::chrono::duration_cast<std::chrono::milliseconds>(d);
    return msec.count();
}


// file descriptor functions

namespace detail {

void fd_event_set::hard_ensure(unsigned ufd) {
    assert(ufd >= capacity_);
    // choose new capacity to fit; double up to `block_capacity`
    unsigned ncapacity;
    if (ufd >= block_capacity) {
        ncapacity = ((ufd / block_capacity) + 1) * block_capacity;
    } else {
        ncapacity = std::max(first_capacity, capacity_ * 2);
        while (ufd >= ncapacity) {
            ncapacity *= 2;
        }
    }
    // allocate, copy, initialize
    fdrec* nfdrs = std::allocator<fdrec>().allocate(ncapacity);
    std::uninitialized_move(fdrs_, fdrs_ + capacity_, nfdrs);
    std::uninitialized_default_construct(nfdrs + capacity_, nfdrs + ncapacity);
    if (capacity_) {
        std::destroy(fdrs_, fdrs_ + capacity_);
        std::allocator<fdrec>().deallocate(fdrs_, capacity_);
    }
    // assign
    fdrs_ = nfdrs;
    capacity_ = ncapacity;
}

void fd_event_set::clear() {
    if (capacity_ != 0) {
        std::destroy(fdrs_, fdrs_ + capacity_);
        std::allocator<fdrec>().deallocate(fdrs_, capacity_);
    }
    fdrs_ = nullptr;
    capacity_ = 0;
    update_link_ = -1;
}


struct fd_batch {
#if COTAMER_USE_KQUEUE
    using system_event_type = struct kevent;
    static constexpr size_t capacity = 512;
    struct kevent ev[capacity];
    int changes = 0;
#elif COTAMER_USE_EPOLL
    using system_event_type = struct epoll_event;
    static constexpr size_t capacity = 512;
    struct epoll_event ev[capacity];
#else
    using system_event_type = struct pollfd;
    small_vector<struct pollfd, 16> ev;
#endif
    int size = 0;
    int index = 0;

    static inline int mask_out(int mask) noexcept;
    static inline int mask_in(const system_event_type&) noexcept;

    inline void add(int pollfd, const fd_update& fdu, int old_mask);
    inline void clear(int pollfd);
    inline std::optional<fd_update> pop() noexcept;
};

inline int fd_batch::mask_out(int mask) noexcept {
#if COTAMER_USE_KQUEUE
    // should never be called
    (void) mask;
    return 0;
#elif COTAMER_USE_EPOLL
    return (mask & 1 ? int(EPOLLIN | EPOLLRDHUP) : 0)
        | (mask & 2 ? int(EPOLLOUT) : 0)
        | (mask & 4 ? int(EPOLLRDHUP) : 0);
#else
    return (mask & 1 ? int(POLLIN) : 0)
        | (mask & 2 ? int(POLLOUT) : 0);
#endif
}

inline int fd_batch::mask_in(const system_event_type& se) noexcept {
#if COTAMER_USE_KQUEUE
    return (se.filter == EVFILT_READ ? 1 : 0)
        | (se.filter == EVFILT_WRITE ? 2 : 0)
        | (se.flags & EV_EOF ? 7 : 0);
#elif COTAMER_USE_EPOLL
    return (se.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR) ? 1 : 0)
        | (se.events & (EPOLLOUT | EPOLLHUP | EPOLLERR) ? 2 : 0)
        | (se.events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR) ? 4 : 0);
#else
    return (se.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL) ? 1 : 0)
        | (se.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL) ? 2 : 0)
        | (se.revents & (POLLERR | POLLHUP | POLLNVAL) ? 4 : 0);
#endif
}

inline void fd_batch::add(int pollfd, const fd_update& fdu, int old_mask) {
#if COTAMER_USE_KQUEUE
    if (changes == capacity) {
        clear(pollfd);
    }
    void* udata = reinterpret_cast<void*>(uintptr_t(fdu.epoch));
    if ((fdu.mask & 5) && !(old_mask & 5)) {
        EV_SET(&ev[changes], fdu.fd, EVFILT_READ, EV_ADD, 0, 0, udata);
        ++changes;
    } else if (!(fdu.mask & 5) && (old_mask & 5)) {
        EV_SET(&ev[changes], fdu.fd, EVFILT_READ, EV_DELETE, 0, 0, udata);
        ++changes;
    }
    if (!(fdu.mask & 6) == !(old_mask & 6)) {
        return;
    }
    if (changes == capacity) {
        clear(pollfd);
    }
    if ((fdu.mask & 6) && !(old_mask & 6)) {
        EV_SET(&ev[changes], fdu.fd, EVFILT_WRITE, EV_ADD, 0, 0, udata);
        ++changes;
    } else if (!(fdu.mask & 6) && (old_mask & 6)) {
        EV_SET(&ev[changes], fdu.fd, EVFILT_WRITE, EV_DELETE, 0, 0, udata);
        ++changes;
    }
#elif COTAMER_USE_EPOLL
    epoll_event epev;
    epev.events = mask_out(fdu.mask);
    epev.data.u64 = fdu.fd | (uint64_t(fdu.epoch) << 32);
    if (!fdu.mask) {
        epoll_ctl(pollfd, EPOLL_CTL_DEL, fdu.fd, &epev);
    } else if (!old_mask) {
        epoll_ctl(pollfd, EPOLL_CTL_ADD, fdu.fd, &epev);
    } else {
        epoll_ctl(pollfd, EPOLL_CTL_MOD, fdu.fd, &epev);
    }
#else
    (void) pollfd, (void) fdu, (void) old_mask;
#endif
}

inline void fd_batch::clear(int pollfd) {
#if COTAMER_USE_KQUEUE
    if (changes) {
        kevent(pollfd, ev, changes, nullptr, 0, nullptr);
        changes = 0;
    }
#else
    (void) pollfd;
#endif
}

inline std::optional<fd_update> fd_batch::pop() noexcept {
    while (index < size) {
        auto& e = ev[index];
        ++index;
#if COTAMER_USE_KQUEUE
        int fd = e.ident;
        int mask = mask_in(e);
        unsigned epoch = reinterpret_cast<uintptr_t>(e.udata);
#elif COTAMER_USE_EPOLL
        int fd = e.data.u64 & 0xFFFF'FFFF;
        int mask = mask_in(e);
        unsigned epoch = e.data.u64 >> 32;
#else
        int fd = e.fd;
        int mask = mask_in(e);
        unsigned epoch = fd_event_set::empty_epoch;
#endif
        if (mask) {
            return {{fd, mask, epoch}};
        }
    }
    return std::nullopt;
}

}

inline int driver::pollfd() {
#if COTAMER_USE_KQUEUE
    if (pollfd_ >= 0) {
        return pollfd_;
    }
    if ((pollfd_ = kqueue()) < 0) {
        throw std::system_error(errno, std::generic_category());
    }
    fcntl(pollfd_, F_SETFD, FD_CLOEXEC);
    // prepare wake event for cross-thread wake
    struct kevent ev;
    EV_SET(&ev, -1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(pollfd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category());
    }
    return pollfd_;
#elif COTAMER_USE_EPOLL
    if (pollfd_ >= 0) {
        return pollfd_;
    }
    if ((pollfd_ = epoll_create1(EPOLL_CLOEXEC)) < 0
        || (epoll_wakefd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
        throw std::system_error(errno, std::generic_category());
    }
    epoll_event epev;
    epev.events = EPOLLIN;
    epev.data.u64 = epoll_wakefd_ | (uint64_t(fd_event_set::internal_epoch) << 32);
    if (epoll_ctl(pollfd_, EPOLL_CTL_ADD, epoll_wakefd_, &epev) < 0) {
        throw std::system_error(errno, std::generic_category());
    }
    return pollfd_;
#else
    return 0;
#endif
}

void driver::apply_fd_update(detail::fd_batch& batch, const detail::fd_update& fdu) {
    // look up events currently registered on our event notification fd
    // for file `fdu.fd`; we store them in `fdctl_` in bit-packed form
    unsigned fdci = unsigned(fdu.fd) / 16,
        fdcs = (unsigned(fdu.fd) % 16) * 4;
    if (fdci >= fdctl_.size()) {
        fdctl_.resize(fdci + 1, uint64_t(0));
    }
    // return if no change (e.g., firing a read event removed the readable
    // watch, but application code re-installed that watch)
    int old_mask = (fdctl_[fdci] >> fdcs) & 15;
    if (old_mask == fdu.mask) {
        return;
    }
    // add to the batch of event notification fd updates
    batch.add(pollfd(), fdu, old_mask);
    // record the new notification state in `fdctl_`
    fdctl_[fdci] ^= (old_mask ^ fdu.mask) << fdcs;
    if (old_mask == 0) {
        ++nfdctl_;
    } else if (fdu.mask == 0) {
        --nfdctl_;
    }
}

bool driver::watch_fds(detail::fd_batch& batch, clock::duration timeout) {
    // block in kernel
#if COTAMER_USE_KQUEUE
    wakefd_.store(pollfd_, std::memory_order_seq_cst);
    if (lock_.load(std::memory_order_seq_cst)) {
        timeout = clock::duration::zero();
    }

    struct timespec ts = duration_timespec(timeout);
    batch.size = kevent(pollfd_, batch.ev, batch.changes, batch.ev, batch.capacity, &ts);
    batch.changes = 0;

    wakefd_.store(-1, std::memory_order_relaxed);
#elif COTAMER_USE_EPOLL
    wakefd_.store(epoll_wakefd_, std::memory_order_seq_cst);
    if (lock_.load(std::memory_order_seq_cst)) {
        timeout = clock::duration::zero();
    }

    batch.size = epoll_wait(pollfd_, batch.ev, batch.capacity, duration_milliseconds(timeout));

    wakefd_.store(-1, std::memory_order_relaxed);
#else
    // The poll() fallback has no cross-thread wake mechanism (no equivalent
    // of EVFILT_USER or eventfd). Cross-thread triggers will be delayed
    // until the poll timeout expires. This is an acceptable tradeoff for a
    // portability fallback; kqueue/epoll should be used on production systems.
    batch.ev.clear();
    int fd = -1;
    while (auto fdu = fds_.next_nonempty(fd)) {
        batch.ev.emplace_back(fdu->fd, batch.mask_out(fdu->mask), 0);
        fd = fdu->fd;
    }
    poll(batch.ev.begin(), batch.ev.size(), duration_milliseconds(timeout));
    batch.size = batch.ev.size();
#endif
    batch.index = 0;

    // process returned events
    while (auto fdu = batch.pop()) {
        if (fdu->mask & 1) {
            if (auto eh = fds_.take(fdu->fd, 0, fdu->epoch)) {
                while (auto coh = eh->driver_trigger(this)) {
                    coh();
                    step_time();
                }
            }
#if COTAMER_USE_EPOLL
            else if (fdu->fd == epoll_wakefd_) {
                uint64_t v;
                ssize_t nr = ::read(epoll_wakefd_, &v, sizeof(v));
                (void) nr;
            }
#endif
        }
        if (fdu->mask & 2) {
            if (auto eh = fds_.take(fdu->fd, 1, fdu->epoch)) {
                while (auto coh = eh->driver_trigger(this)) {
                    coh();
                    step_time();
                }
            }
        }
        if (fdu->mask & 4) {
            if (auto eh = fds_.take(fdu->fd, 2, fdu->epoch)) {
                while (auto coh = eh->driver_trigger(this)) {
                    coh();
                    step_time();
                }
            }
        }
    }

    // return true if we saw any fd activity, possibly including internal
    // activity (like a notification from another thread)
    return batch.size > 0;
}

int driver::close_fd(int fd) {
    int r = ::close(fd);
    if (auto eh = fds_.take(fd, 0, 0)) {
        eh->trigger(); // NB: not driver_trigger -- delay unblocked coroutines
    }
    if (auto eh = fds_.take(fd, 1, 0)) {
        eh->trigger();
    }
    if (auto eh = fds_.take(fd, 2, 0)) {
        eh->trigger();
    }
    forget_fd(fd);
    return r;
}

void driver::forget_fd(int fd) {
    if (auto fdu = fds_.forget(fd)) {
        detail::fd_batch fdb;
        apply_fd_update(fdb, *fdu);
        fdb.clear(pollfd_);
    }
}

void driver::migrate_asap(detail::event_handle eh) {
    auto df = lock();
    migrate_.emplace_back(std::move(eh));
    unlock(df | driver::df_nonempty);

    int wakefd = wakefd_.load(std::memory_order_seq_cst);
    if (wakefd >= 0) {
#if COTAMER_USE_KQUEUE
        struct kevent ev;
        EV_SET(&ev, -1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        kevent(wakefd, &ev, 1, nullptr, 0, nullptr);
#elif COTAMER_USE_EPOLL
        uint64_t val = 1;
        ssize_t nw = ::write(wakefd, &val, sizeof(val));
        (void) nw;
#endif
    }
}


// driver methods

thread_local std::unique_ptr<driver> driver::main{new driver};
std::atomic<bool> driver::real_time;

driver::driver()
    : now_(clock::from_time_t(1634070069)),
      real_time_(real_time.load(std::memory_order_relaxed)) {
}

driver::~driver() {
    if (!asap_.empty()
        || !timed_.empty()
        || fds_.has_update()
        || nfdctl_ != 0
        || lock_.load(std::memory_order_relaxed)) {
        // Clear any remaining events and coroutines
        clear();
        loop();
    }
    if (epoll_wakefd_ >= 0) {
        ::close(epoll_wakefd_);
    }
    if (pollfd_ >= 0) {
        ::close(pollfd_);
    }
}

void driver::loop() {
    detail::fd_batch fdb;

    while (true) {
        // import migrated tasks
        if (lock_.load(std::memory_order_relaxed) != 0) {
            uint32_t flags = lock();
            std::deque<detail::event_handle> migrate;
            migrate_.swap(migrate);
            unlock(flags & ~df_nonempty);
            std::move(migrate.begin(), migrate.end(), std::back_inserter(asap_));
        }

        // process asap tasks
        while (!asap_.empty()) {
            auto eh = std::move(asap_.front());
            asap_.pop_front();
            while (auto coh = eh->driver_trigger(this)) {
                coh();
                step_time();
            }
        }

        // register changes in interested file descriptor set
        while (auto fdu = fds_.pop_update()) {
            apply_fd_update(fdb, *fdu);
        }

        // exit if nothing to do
        timed_.cull();
        if (timed_.empty()
            && nfdctl_ == 0
            && !lock_.load(std::memory_order_relaxed)) {
            break;
        }

        // compute timeout
        clock::duration timeout;
        if (!asap_.empty()
            || !real_time_
            || lock_.load(std::memory_order_relaxed)) {
            timeout = clock::duration::zero();
        } else if (!timed_.empty()) {
            timeout = timed_.top_time() - clock::now();
        } else {
            timeout = clock::duration(1h);
        }

        // block or poll for file descriptor events
        bool had_fd_event = false;
        if (nfdctl_ == 0 && timeout <= clock::duration::zero()) {
            fdb.clear(pollfd_);
        } else {
            // call kqueue/epoll/poll, process batch of returned events
            had_fd_event = watch_fds(fdb, timeout);
        }

        // update time
        if (real_time_) {
            now_ = now();
        } else if (!timed_.empty()
                   && asap_.empty()
                   && !had_fd_event) {
            now_ = timed_.top_time();
        }

        // process timers
        while (!timed_.empty()
               && timed_.top_time() <= now_) {
            auto eh = std::move(timed_.top());
            timed_.pop();
            while (auto coh = eh->driver_trigger(this)) {
                coh();
                step_time();
            }
        }
    }
    clearing_ = false;
}

void driver::clear() {
    clearing_ = true;
}

void reset() {
    driver::main.reset(new driver);
}



// event functions

std::string event::debug_info() const {
    return std::format("#<event {}{}>", static_cast<void*>(handle().get()),
                       triggered() ? " triggered" : "");
}


// mutex functions

inline bool mutex::allow(bool is_shared, latch_type l) const noexcept {
    return is_shared ? !(l & mf_lock_excl) : l < mf_lock_excl;
}

inline auto mutex::notify_locked(latch_type l) -> latch_type {
    while (!waiters_.empty()) {
        auto& fw = waiters_.front();
        if (!fw.empty()) {
            bool fws = waiter_shared(fw);
            if (!allow(fws, l)) {
                break;
            }
            if (fw->trigger()) {
                l += fws ? mf_lock_shared : mf_lock_excl;
            }
        }
        waiters_.pop_front();
    }
    return l;
}

void mutex::lock_impl(bool is_shared, detail::event_handle& e) {
    latch_type l = latch();
    l = notify_locked(l);
    if (waiters_.empty() && allow(is_shared, l)) {
        if (e) {
            e->trigger();
        }
        l += is_shared ? mf_lock_shared : mf_lock_excl;
    } else {
        if (!e) {
            e = detail::event_handle(new detail::event_body);
        }
        if (is_shared) {
            e->set_user_flags(detail::ef_user);
        }
        waiters_.push_back(e);
    }
    unlatch(l);
}

void mutex::unlock_impl(bool is_shared) {
    latch_type l = latch();
    l = notify_locked(l - (is_shared ? mf_lock_shared : mf_lock_excl));
    unlatch(l);
}


cotamer_error::cotamer_error(cotamer_errc ec)
    : std::logic_error(message(ec)), errc_(ec) {
}

constexpr const char* cotamer_error::message(cotamer_errc ec) noexcept {
    switch (ec) {
    case cotamer_errc::cross_driver_await:
        return "cannot co_await a task created on a different driver";
    default:
        return "unknown cotamer error";
    }
}

}
