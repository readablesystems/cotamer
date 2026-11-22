#include "cotamer/io.hh"
#include "cotamer/config.hh"
#include <climits>
#include <ranges>
#include <thread>
#include <netdb.h>
#include <unistd.h>
#include <sys/uio.h>
#if !COTAMER_USE_KQUEUE && defined(__linux__)
// signal wake fds are eventfds
# define COTAMER_SIGNAL_EVENTFD 1
# include <sys/eventfd.h>
#endif

namespace cotamer {

struct timespec duration_timespec(duration d) {
    if (d <= duration::zero()) {
        return {0, 0};
    }
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(d);
    auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(d - sec);
    return {sec.count(), nsec.count()};
}

// Truncate, since wakeups are usually late anyway, but round a nonzero
// sub-millisecond timeout up to 1 so it waits rather than busy-polls.
int duration_milliseconds(duration d) {
    if (d <= duration::zero()) {
        return 0;
    }
    auto msec = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    return msec < 1 ? 1 : (msec < INT_MAX ? int(msec) : INT_MAX);
}

// Drain a signal wake fd
static void drain_signal_wakefd(int fd) {
#if COTAMER_SIGNAL_EVENTFD
    uint64_t v;
    ssize_t nr = ::read(fd, &v, sizeof(v));
    (void) nr;
#else
    char buf[64];
    while (::read(fd, buf, sizeof(buf)) > 0) {
    }
#endif
}

// per-driver signal state
namespace detail {
struct signal_waiter {
    int signo;
    unsigned count;             // signal count at start
    bool weak;                  // doesn't keep loop() running
    event_handle eh;
};

inline bool any_strong(const std::vector<signal_waiter>& waiters) {
    return std::ranges::any_of(waiters, [](const signal_waiter& w) { return !w.weak; });
}

struct signal_set {
    std::vector<signal_waiter> waiters;
    int slot = -1;                  // index in `sigpool`
    int readfd = -1;                // wake fd
    unsigned generation = 0;        // `sigpool.generation` last seen
};
}

#if COTAMER_USE_EPOLL
// Wait with nanosecond precision via epoll_pwait2 (Linux 5.11) if the C
// library declares it, otherwise with epoll_wait’s milliseconds.
int epoll_wait_duration(int epfd, epoll_event* ev, int maxev, duration timeout) {
# if COTAMER_HAVE_EPOLL_PWAIT2
    static std::atomic<bool> have_pwait2{true};
    if (have_pwait2.load(std::memory_order_relaxed)) {
        struct timespec ts = duration_timespec(timeout);
        int r = epoll_pwait2(epfd, ev, maxev, &ts, nullptr);
        if (r >= 0 || errno != ENOSYS) {
            return r;
        }
        // glibc has the wrapper but the kernel lacks the system call
        have_pwait2.store(false, std::memory_order_relaxed);
    }
# endif
    return epoll_wait(epfd, ev, maxev, duration_milliseconds(timeout));
}
#endif


// file descriptor functions

namespace detail {

void fd_event_set::deref_all(driver* drv) {
    // must be called before destroying `fd_event_set`
    if (!fdr_capacity_) {
        return;
    }
    for (fdrec* fdr = fdrs_; fdr != fdrs_ + fdr_capacity_; ++fdr) {
        if (fdr->body) {
            fdr->body->remove_listener(drv);
        }
    }
}

void fd_event_set::hard_ensure(unsigned ufd) {
    assert(ufd >= fdr_capacity_);
    // choose new capacity to fit; double up to `block_capacity`
    unsigned ncapacity;
    if (ufd >= block_capacity) {
        ncapacity = ((ufd / block_capacity) + 1) * block_capacity;
    } else {
        ncapacity = std::max(first_capacity, fdr_capacity_ * 2);
        while (ufd >= ncapacity) {
            ncapacity *= 2;
        }
    }
    // allocate, copy, initialize
    fdrec* nfdrs = std::allocator<fdrec>().allocate(ncapacity);
    std::uninitialized_move(fdrs_, fdrs_ + fdr_capacity_, nfdrs);
    std::uninitialized_default_construct(nfdrs + fdr_capacity_, nfdrs + ncapacity);
    if (fdr_capacity_) {
        std::destroy(fdrs_, fdrs_ + fdr_capacity_);
        std::allocator<fdrec>().deallocate(fdrs_, fdr_capacity_);
    }
    // assign
    fdrs_ = nfdrs;
    fdr_capacity_ = ncapacity;
}

void fd_body::close(bool because_deref) {
    lock();
    // If this is the first close() call (fd_>=0), mark closed (fd_=-1) and
    // notify drivers. (The OS fd must remain open until no driver has it
    // registered on a kernel notifier -- otherwise epoll's EPOLL_CTL_DEL will
    // fail. If there are no drivers, close base_fd now; otherwise, the last
    // remove_listener will close it. We need local copies of base_fd and
    // drivers in case of concurrent `delete this`.)
    int base_fd = -1;
    small_vector<driver*, 4> drivers;
    if (fd_.load(std::memory_order_relaxed) >= 0) {
        fd_.store(-1, std::memory_order_relaxed);
        for (auto dx : drivers_) {
            drivers.push_back(dx);
        }
        base_fd = base_fd_;
        if (base_fd >= 0 && drivers.empty()) {
            // will ::close now
            base_fd_ = -1;
        }
    }
    if (because_deref) {
        unreferenced_ = true;
    }
    bool deletable = unreferenced_ && drivers_.empty();
    unlock();

    if (base_fd < 0) {
        // do nothing
    } else if (drivers.empty()) {
        ::close(base_fd);
    } else {
        driver* my_driver = driver::current.get();
        for (auto dx : drivers) {
            if (dx == my_driver) {
                dx->notify_close(base_fd);
            } else {
                dx->migrate_fd_close(base_fd);
            }
        }
    }
    if (deletable) {
        delete this;
    }
}


event_handle fd_event_set::watch(int fd, fdevent imask, fd_body* body,
                                 driver* drv) {
    if (imask == fdevent::none) {
        // never fires (but can be triggered by hand)
        return event_handle{new event_body};
    } else if (fd < 0) {
        // A closed or unopened fd is always ready: I/O on it fails at once.
        event_handle eh{new event_body};
        eh->set_user_flags(int(imask) << efs_user);
        eh->trigger();
        return eh;
    }
    unsigned ufd = fd;
    if (ufd >= fdr_capacity_) {
        hard_ensure(ufd);
    }

    // ensure `fdrec`
    fdrec& fdi = fdrs_[ufd];
    if (fdi.body != body) {
        if (fdi.body) {
            // `fd`'s number now belongs to a different body: the file was
            // closed and reopened behind cotamer's back, or two `fd`s were
            // made for it. Forget the old body.
            drv->notify_close(fd, true);
        }
        fdi.body = body;
        body->add_listener(drv);
    }
    if (fdi.update_link == link_clean) {
        fdi.update_link = update_link_;
        update_link_ = ufd + 1;
    }

    // append `watchrec`
    auto wix = free_wlink_;
    watchrec* wr;
    if (wix) {
        wr = &ws_[wix - 1];
        free_wlink_ = wr->wlink;
        wr->mask = imask;
        wr->wlink = link_clean;
    } else {
        ws_.push_back({event_handle{}, imask, link_clean});
        wix = ws_.size();
        wr = &ws_[wix - 1];
    }
    if (fdi.whead) {
        ws_[fdi.wtail - 1].wlink = wix;
    } else {
        fdi.whead = wix;
    }
    fdi.wtail = wix;

    // construct and return event
    wr->ev = event_handle{new event_body};
    return wr->ev;
}

std::optional<std::pair<fd_body*, unsigned>> fd_event_set::detach(int fd, bool stale) {
    unsigned ufd = fd;
    if (ufd >= fdr_capacity_) {
        return std::nullopt;
    }
    fdrec& fdi = fdrs_[ufd];
    if (!fdi.body || (!stale && fdi.body->fileno() >= 0)) {
        return std::nullopt;
    }

    while (auto wix = fdi.whead) {
        auto& wr = ws_[wix - 1];
        fdi.whead = wr.wlink;
        std::exchange(wr.ev, nullptr)->trigger();
        wr.wlink = free_wlink_;
        free_wlink_ = wix;
    }
    fdi.wtail = link_clean;

    ++fdi.epoch;
    if (fdi.update_link == link_clean) {
        fdi.update_link = update_link_;
        update_link_ = ufd + 1;
    }
    return {{std::exchange(fdi.body, nullptr), fdi.epoch - 1}};
}

void fd_batch::print_changes() {
#if COTAMER_USE_KQUEUE
    for (int i = 0; i != changes; ++i) {
        const auto& e = ev[i];
        std::string t;
        switch (e.filter) {
        case EVFILT_READ:   t = std::format("READ {}", e.ident);    break;
        case EVFILT_EXCEPT: t = std::format("EXCEPT {}", e.ident);  break;
        case EVFILT_WRITE:  t = std::format("WRITE {}", e.ident);   break;
        case EVFILT_VNODE:  t = std::format("VNODE {}", e.ident);   break;
        case EVFILT_PROC:   t = std::format("PROC {}", e.ident);    break;
        case EVFILT_SIGNAL: t = std::format("SIGNAL {}", e.ident);  break;
        case EVFILT_TIMER:  t = std::format("TIMER {}", e.ident);   break;
        case EVFILT_USER:   t = "USER";                             break;
        default:            t = std::format("#{} {}", e.filter, e.ident); break;
        }
        if (e.flags & EV_ADD) {
            t += " ADD";
        }
        if (e.flags & EV_DELETE) {
            t += " DELETE";
        }
        std::print(std::cerr, "<{} U{}>\n", t, e.udata);
    }
#endif
}

#if COTAMER_USE_POLL && !defined(POLLRDHUP)
# define POLLRDHUP 0
#endif

inline int fd_batch::mask_out(fdevent mask) noexcept {
#if COTAMER_USE_KQUEUE
    // should never be called
    (void) mask;
    return 0;
#elif COTAMER_USE_EPOLL
    return (int(mask & fdevent::read) ? int(EPOLLIN | EPOLLRDHUP) : 0)
        | (int(mask & fdevent::write) ? int(EPOLLOUT) : 0)
        | (int(mask & fdevent::close) ? int(EPOLLRDHUP) : 0);
#else
    // POSIX says `POLLHUP` is ignored in `events`, but macOS, which lacks
    // `POLLRDHUP`, reports read-side EOF as `POLLHUP` only if it’s requested.
    return (int(mask & fdevent::read) ? int(POLLIN | POLLRDHUP) : 0)
        | (int(mask & fdevent::write) ? int(POLLOUT) : 0)
        | (int(mask & fdevent::close) ? int(POLLRDHUP | POLLHUP) : 0);
#endif
}

inline fdevent fd_batch::mask_in(const system_event_type& se) noexcept {
#if COTAMER_USE_KQUEUE
    return (se.filter == EVFILT_READ ? fdevent::read : fdevent::none)
        | (se.filter == EVFILT_WRITE ? fdevent::write : fdevent::none)
        | (se.flags & EV_EOF ? fdevent::all : fdevent::none);
#elif COTAMER_USE_EPOLL
    return (se.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR) ? fdevent::read : fdevent::none)
        | (se.events & (EPOLLOUT | EPOLLHUP | EPOLLERR) ? fdevent::write : fdevent::none)
        | (se.events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR) ? fdevent::close : fdevent::none);
#else
    return (se.revents & (POLLIN | POLLRDHUP | POLLERR | POLLHUP | POLLNVAL) ? fdevent::read : fdevent::none)
        | (se.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL) ? fdevent::write : fdevent::none)
        | (se.revents & (POLLRDHUP | POLLERR | POLLHUP | POLLNVAL) ? fdevent::close : fdevent::none);
#endif
}

// Returns false if the kernel can’t watch `fdu.fd` (epoll rejects regular
// files and directories); such fds are always ready.
inline bool fd_batch::add(int pollfd, const fd_update& fdu, fdevent old_mask) {
#if COTAMER_USE_KQUEUE
    // EVFILT_READ is level-triggered for read interest. For close-only
    // interest it is edge-triggered (EV_CLEAR), so pending data doesn't fire
    // repeatedly (but a change will fire). Since EV_ADD doesn't change the
    // EV_CLEAR flag, we often delete & re-add EVFILT_READ.
    int oldr = int(old_mask & fdevent::read) ? EV_ADD : 0,
        oldw = int(old_mask & fdevent::write);
    if (oldr == 0 && int(old_mask & fdevent::close)) {
        oldr = EV_ADD | EV_CLEAR;
    }
    int newr = int(fdu.mask & fdevent::read) ? EV_ADD : 0,
        neww = int(fdu.mask & fdevent::write);
    if (newr == 0 && int(fdu.mask & fdevent::close)) {
        newr = EV_ADD | EV_CLEAR;
    }
    if (oldr == newr && oldw == neww) {
        return true;
    }
    if (capacity - changes < (oldr == newr ? 0 : 2) + (oldw == neww ? 0 : 1)) {
        clear(pollfd);
    }
    void* udata = reinterpret_cast<void*>(uintptr_t(fdu.epoch));
    if (oldr != newr && oldr) {
        EV_SET(&ev[changes], fdu.fd, EVFILT_READ, EV_DELETE, 0, 0, udata);
        ++changes;
    }
    if (oldr != newr && newr) {
        EV_SET(&ev[changes], fdu.fd, EVFILT_READ, newr, 0, 0, udata);
        ++changes;
    }
    if (oldw != neww) {
        int action = oldw ? EV_DELETE : EV_ADD;
        EV_SET(&ev[changes], fdu.fd, EVFILT_WRITE, action, 0, 0, udata);
        ++changes;
    }
#elif COTAMER_USE_EPOLL
    epoll_event epev;
    epev.events = mask_out(fdu.mask);
    epev.data.u64 = fdu.fd | (uint64_t(fdu.epoch) << 32);
    int op = fdu.mask != fdevent::none
        ? (old_mask != fdevent::none ? EPOLL_CTL_MOD : EPOLL_CTL_ADD)
        : EPOLL_CTL_DEL;
    if (epoll_ctl(pollfd, op, fdu.fd, &epev) < 0) {
        if (errno == EPERM && op == EPOLL_CTL_ADD) {
            return false;
        }
        if (errno == ENOENT && op == EPOLL_CTL_DEL) {
            // already gone (e.g., the fd was closed behind our back)
            return true;
        }
        throw errno_error();
    }
#else
    (void) pollfd, (void) fdu, (void) old_mask;
#endif
    return true;
}

inline std::optional<fd_update> fd_batch::pop() noexcept {
    while (index < size) {
        auto& e = ev[index];
        ++index;
#if COTAMER_USE_KQUEUE
        int fd = e.ident;
        unsigned epoch = reinterpret_cast<uintptr_t>(e.udata);
#elif COTAMER_USE_EPOLL
        int fd = e.data.u64 & 0xFFFF'FFFF;
        unsigned epoch = e.data.u64 >> 32;
#else
        int fd = e.fd;
        unsigned epoch = epochs[index - 1];
#endif
        auto mask = mask_in(e);
        if (mask != fdevent::none) {
            return {{fd, mask, epoch}};
        }
    }
    return std::nullopt;
}

}

void driver::hard_pollfd() {
#if COTAMER_USE_KQUEUE
    if ((pollfd_ = kqueue()) < 0) {
        throw errno_error();
    }
    fcntl(pollfd_, F_SETFD, FD_CLOEXEC);
    // prepare wake event for cross-thread wake
    struct kevent ev;
    EV_SET(&ev, -1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(pollfd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        throw errno_error();
    }
#elif COTAMER_USE_EPOLL
    if ((pollfd_ = epoll_create1(EPOLL_CLOEXEC)) < 0) {
        throw errno_error();
    }
    if ((internal_wakefd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
        int saved = errno;
        ::close(pollfd_);
        pollfd_ = -1;
        throw std::system_error(saved, std::generic_category());
    }
    epoll_event epev;
    epev.events = EPOLLIN;
    epev.data.u64 = internal_wakefd_ | (uint64_t(detail::fd_event_set::internal_epoch) << 32);
    if (epoll_ctl(pollfd_, EPOLL_CTL_ADD, internal_wakefd_, &epev) < 0) {
        throw errno_error();
    }
#else
    // cross-thread wake writes to a self-pipe
    int pfd[2];
    if (pipe(pfd) < 0) {
        throw errno_error();
    }
    for (int i = 0; i != 2; ++i) {
        fcntl(pfd[i], F_SETFD, FD_CLOEXEC);
        fcntl(pfd[i], F_SETFL, fcntl(pfd[i], F_GETFL) | O_NONBLOCK);
    }
    pollfd_ = pfd[0];
    internal_wakefd_ = pfd[1];
#endif
}

void driver::apply_fd_update(detail::fd_batch& batch,
                             const detail::fd_update& fdu) {
    // look up events currently registered on our event notification fd
    // for file `fdu.fd`; we store them in `fdctl_` in bit-packed form
    unsigned fdci = unsigned(fdu.fd) / 16,
        fdcs = (unsigned(fdu.fd) % 16) * 4;
    if (fdci >= fdctl_.size()) {
        fdctl_.resize(fdci + 1, uint64_t(0));
    }

    // return if no change (e.g., firing a read event removed the readable
    // watch, but application code re-installed that watch)
    fdevent old_mask = fdevent((fdctl_[fdci] >> fdcs) & 15);
    if (old_mask == fdu.mask) {
        return;
    }

    // add to the batch of event notification fd updates
    if (!batch.add(pollfd(), fdu, old_mask)) {
        // The kernel can’t watch this fd. Regular files are always ready to
        // read and write (as poll() reports), so trigger those watches now;
        // they run from the asap queue. Close watches never fire. The fd is
        // not recorded as registered, so a later watch tries again.
        auto wix = fds_.take_watch_list(fdu.fd, fdevent::read_write, fdu.epoch);
        while (wix) {
            if (auto eh = fds_.pop_watch_list_event(wix, fdevent::read_write)) {
                eh->trigger();
            }
        }
        return;
    }

    // record the new notification state in `fdctl_`
    fdctl_[fdci] ^= uint64_t(int(old_mask) ^ int(fdu.mask)) << fdcs;
    if (old_mask == fdevent::none) {
        ++nfdctl_;
    } else if (fdu.mask == fdevent::none) {
        --nfdctl_;
    }
}

bool driver::watch_fds(detail::fd_batch& batch, duration timeout) {
    // Ensure pollfd if we are asked to block before any fd registrations
    (void) pollfd();

#if COTAMER_USE_KQUEUE
    // Cross-thread wake injects an EVFILT_USER event onto the kqueue fd
    wakefd_.store(pollfd_, std::memory_order_seq_cst);
#else
    // Cross-thread wake writes to an eventfd() or self-pipe
    wakefd_.store(internal_wakefd_, std::memory_order_seq_cst);
#endif
    if (lock_.load(std::memory_order_seq_cst)) {
        timeout = duration::zero();
    }

#if COTAMER_USE_KQUEUE
    // block in kernel
    struct timespec ts = duration_timespec(timeout);
    batch.size = kevent(pollfd_, batch.ev, batch.changes, batch.ev, batch.capacity, &ts);
    batch.changes = 0;
#elif COTAMER_USE_EPOLL
    batch.size = epoll_wait_duration(pollfd_, batch.ev, batch.capacity, timeout);
#else
    // NB: rebuilds the entire pollfd array on every call (O(max_fd) scan).
    // kqueue/epoll maintain kernel-side state and avoid this cost.
    batch.ev.clear();
    batch.epochs.clear();
    batch.ev.push_back({pollfd_, POLLIN, 0});
    batch.epochs.push_back(detail::fd_event_set::internal_epoch);
    if (signals_) {
        batch.ev.push_back({signals_->readfd, POLLIN, 0});
        batch.epochs.push_back(detail::fd_event_set::internal_epoch);
    }
    int fd = -1;
    while (auto fdu = fds_.next_known(fd)) {
        if (fdu->mask != fdevent::none) {
            batch.ev.push_back({fdu->fd, short(batch.mask_out(fdu->mask)), 0});
            batch.epochs.push_back(fdu->epoch);
        }
        fd = fdu->fd;
    }
    int nready = ::poll(batch.ev.begin(), batch.ev.size(), duration_milliseconds(timeout));
    // `pop()` skips entries with no returned events
    batch.size = nready > 0 ? int(batch.ev.size()) : nready;
#endif
    batch.index = 0;

    // check for unexpected error
    if (batch.size < 0 && errno != EINTR) {
        throw errno_error();
    }

    wakefd_.store(-1, std::memory_order_relaxed);

    // process returned events
    while (auto fdu = batch.pop()) {
        if (signals_ && fdu->fd == signals_->readfd) {
            drain_signal_wakefd(fdu->fd);
            continue;
        }
#if COTAMER_USE_EPOLL
        if (fdu->fd == internal_wakefd_) {
            uint64_t v;
            ssize_t nr = ::read(internal_wakefd_, &v, sizeof(v));
            (void) nr;
            continue;
        }
#elif COTAMER_USE_POLL
        if (fdu->fd == pollfd_) {
            char buf[64];
            while (::read(pollfd_, buf, sizeof(buf)) > 0) {
            }
            continue;
        }
#endif
        auto wix = fds_.take_watch_list(fdu->fd, fdu->mask, fdu->epoch);
        while (wix) {
            auto eh = fds_.pop_watch_list_event(wix, fdu->mask);
            while (auto coh = eh->driver_trigger(this)) {
                coh();
                step_time();
            }
        }
    }

    // return true if we saw any fd activity, possibly including internal
    // activity (like a notification from another thread)
    return batch.size > 0;
}


// signals
//    Our handler is installed while any wait exists, then the previous action
//    returns. Each waiting driver takes a wake fd (eventfd on Linux, else a
//    pipe) from a never-closed pool; the handler counts the signal and writes
//    every wake fd. (Not kqueue EVFILT_SIGNAL: it fires before the handler
//    runs, so a driver could restore the old action too soon.)

namespace {
constexpr int signal_pool_size = 64;   // max drivers waiting for signals

struct signal_pool {
    // accessed by the signal handler
    std::atomic<unsigned> generation{0};
    std::atomic<unsigned> count[NSIG]{};
    std::atomic<int> wakefd[signal_pool_size]{};   // wake fd + 1, or 0
    // protected by `lock`
    std::mutex lock;
    int fds[signal_pool_size][2]{};    // read end, write end (same for eventfd)
    bool fd_open[signal_pool_size]{};
    bool slot_used[signal_pool_size]{};
    unsigned nwaiters[NSIG]{};
    struct sigaction old_action[NSIG]{};
};
static_assert(std::atomic<unsigned>::is_always_lock_free
              && std::atomic<int>::is_always_lock_free);

constinit signal_pool sigpool;

extern "C" void cotamer_signal_handler(int signo) {
    int saved_errno = errno;
    sigpool.count[signo].fetch_add(1);
    sigpool.generation.fetch_add(1);
    for (auto& w : sigpool.wakefd) {
        if (int fd = w.load() - 1; fd >= 0) {
            // a full pipe already has a wake pending
#if COTAMER_SIGNAL_EVENTFD
            uint64_t v = 1;
            ssize_t nw = ::write(fd, &v, sizeof(v));
#else
            char c = 0;
            ssize_t nw = ::write(fd, &c, 1);
#endif
            (void) nw;
        }
    }
    errno = saved_errno;
}

void signal_wait_locked(int signo) {
    if (sigpool.nwaiters[signo] == 0) {
        struct sigaction sa{};
        sa.sa_handler = cotamer_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(signo, &sa, &sigpool.old_action[signo]) != 0) {
            throw errno_error();
        }
    }
    ++sigpool.nwaiters[signo];
}

void signal_unwait_locked(int signo) {
    if (--sigpool.nwaiters[signo] == 0) {
        sigaction(signo, &sigpool.old_action[signo], nullptr);
    }
}

}

event driver::at_signal(int signo) {
    return add_signal_wait(signo, false);
}

event driver::at_signal_weak(int signo) {
    return add_signal_wait(signo, true);
}

event driver::add_signal_wait(int signo, bool weak) {
    if (signo <= 0 || signo >= NSIG || signo == SIGKILL || signo == SIGSTOP) {
        throw std::invalid_argument("bad signal number");
    }
    std::lock_guard guard(sigpool.lock);
    if (!signals_) {
        int slot = 0;
        while (slot != signal_pool_size && sigpool.slot_used[slot]) {
            ++slot;
        }
        if (slot == signal_pool_size) {
            throw std::runtime_error("too many drivers waiting for signals");
        }
        int* p = sigpool.fds[slot];
        if (!sigpool.fd_open[slot]) {
#if COTAMER_SIGNAL_EVENTFD
            if ((p[0] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
                throw errno_error();
            }
            p[1] = p[0];
#else
            if (pipe(p) < 0) {
                throw errno_error();
            }
            for (int i = 0; i != 2; ++i) {
                fcntl(p[i], F_SETFD, FD_CLOEXEC);
                fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
            }
#endif
            sigpool.fd_open[slot] = true;
        }
        drain_signal_wakefd(p[0]);
        // (poll() adds it in watch_fds)
#if COTAMER_USE_KQUEUE
        struct kevent ev;
        EV_SET(&ev, p[0], EVFILT_READ, EV_ADD, 0, 0,
               reinterpret_cast<void*>(uintptr_t(detail::fd_event_set::internal_epoch)));
        if (kevent(pollfd(), &ev, 1, nullptr, 0, nullptr) < 0) {
            throw errno_error();
        }
#elif COTAMER_USE_EPOLL
        epoll_event epev;
        epev.events = EPOLLIN;
        epev.data.u64 = p[0] | (uint64_t(detail::fd_event_set::internal_epoch) << 32);
        if (epoll_ctl(pollfd(), EPOLL_CTL_ADD, p[0], &epev) < 0) {
            throw errno_error();
        }
#endif
        signals_ = new detail::signal_set;
        signals_->slot = slot;
        signals_->readfd = p[0];
        signals_->generation = sigpool.generation.load();
        sigpool.slot_used[slot] = true;
        sigpool.wakefd[slot].store(p[1] + 1);
    }

    // read count before installing handler: earlier signals aren't ours
    event e;
    unsigned count = sigpool.count[signo].load();
    signals_->waiters.push_back({signo, count, weak, e.handle()});
    try {
        signal_wait_locked(signo);
    } catch (...) {
        signals_->waiters.pop_back();
        throw;
    }
    signal_waiting_ = signal_waiting_ || !weak;
    return e;
}

// Trigger waits whose signal arrived (if `all`, every wait).
void driver::deliver_signals(bool all) {
    auto& ss = *signals_;
    unsigned generation = sigpool.generation.load();
    if (generation == ss.generation && !all) {
        return;
    }
    ss.generation = generation;

    std::vector<detail::signal_waiter> fire;
    auto out = ss.waiters.begin();
    for (auto it = ss.waiters.begin(); it != ss.waiters.end(); ++it) {
        // (also drop abandoned waits)
        if (all
            || sigpool.count[it->signo].load() != it->count
            || it->eh.empty()) {
            fire.push_back(std::move(*it));
        } else {
            if (out != it) {
                *out = std::move(*it);
            }
            ++out;
        }
    }
    ss.waiters.erase(out, ss.waiters.end());
    signal_waiting_ = detail::any_strong(ss.waiters);

    for (auto& w : fire) {
        while (auto coh = w.eh->driver_trigger(this)) {
            coh();
            step_time();
        }
    }

    // restore actions after triggering, so re-waiting keeps our handler
    std::lock_guard guard(sigpool.lock);
    for (auto& w : fire) {
        signal_unwait_locked(w.signo);
    }
}

// Drop abandoned waits; return true if waits should keep loop() running:
// non-weak waits, or any waits when clearing (which must trigger them all).
bool driver::cull_signals() {
    std::lock_guard guard(sigpool.lock);
    std::erase_if(signals_->waiters, [](detail::signal_waiter& w) {
        if (w.eh.empty()) {
            signal_unwait_locked(w.signo);
            return true;
        }
        return false;
    });
    signal_waiting_ = detail::any_strong(signals_->waiters);
    return signal_waiting_ || (clearing_ && !signals_->waiters.empty());
}

void driver::close_signals() {
    std::lock_guard guard(sigpool.lock);
    for (auto& w : signals_->waiters) {
        signal_unwait_locked(w.signo);
    }
    int slot = signals_->slot;
#if COTAMER_USE_EPOLL
    epoll_ctl(pollfd_, EPOLL_CTL_DEL, signals_->readfd, nullptr);
#endif
    sigpool.wakefd[slot].store(0);
    sigpool.slot_used[slot] = false;
    delete signals_;
    signals_ = nullptr;
    signal_waiting_ = false;
}

namespace {
// ensure that the return value from `getaddrinfo` is properly freed
struct getaddrinfo_value {
    struct addrinfo hints{};
    struct addrinfo* ai = nullptr;
    int status = 0;
    bool address_any = false;
    ~getaddrinfo_value() {
        if (status == 0 && ai) {
            freeaddrinfo(ai);
        }
    }
};

struct getaddrinfo_error_category_impl : public std::error_category {
public:
    const char* name() const noexcept override { return "getaddrinfo"; }
    std::string message(int value) const override {
        return gai_strerror(value);
    }
};
}

const std::error_category& getaddrinfo_error_category() noexcept {
    static getaddrinfo_error_category_impl c;
    return c;
}

static void getaddrinfo_thread(std::string address,
                               std::shared_ptr<getaddrinfo_value> res,
                               event notifier) {
    auto colon = address.rfind(':');
    res->address_any = colon == 0 || colon == std::string::npos;
    if (colon == std::string::npos) {
        res->status = EAI_NONAME;
        notifier.trigger();
        return;
    }
    auto host = address.substr(0, colon);
    // bracketed IPv6 addresses must be trimmed
    if (host.starts_with('[')
        && host.ends_with(']')
        && host.find(']') == host.size() - 1) {
        if (res->hints.ai_family == AF_UNSPEC) {
            res->hints.ai_family = AF_INET6;
        }
        host = host.substr(1, host.size() - 2);
    }
    auto port = address.substr(colon + 1);
    res->status = getaddrinfo(host.empty() ? nullptr : host.c_str(),
                              port.c_str(), &res->hints, &res->ai);
    notifier.trigger();
}


// Return a nonblocking, close-on-exec socket for `ai`, or an invalid fd (with
// `errno` set) if socket() fails. Setup errors throw; the fd closes itself.
static cotamer::fd nonblocking_socket(const struct addrinfo* ai) {
#ifdef SOCK_CLOEXEC
    // close-on-exec atomically, so a concurrent fork+exec can't inherit it
    cotamer::fd f(socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol));
#else
    cotamer::fd f(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
#endif
    if (f) {
#ifndef SOCK_CLOEXEC
        set_cloexec(f.fileno());
#endif
        set_nonblocking(f);
#ifdef SO_NOSIGPIPE
        set_no_sigpipe(f.fileno());
#endif
    }
    return f;
}

// The passive addresses to bind, in order. An empty host (`:PORT`) means every
// address family: IPv6 goes first, so its dual-stack socket covers IPv4 too
// (getaddrinfo’s order varies: glibc lists IPv4 first, macOS IPv6). If IPv6
// is missing or dual-stack is refused, the IPv4 bind that follows still
// succeeds. Other hosts keep getaddrinfo’s order.
static small_vector<const struct addrinfo*, 4> passive_addresses(const getaddrinfo_value& res) {
    small_vector<const struct addrinfo*, 4> v;
    if (res.address_any) {
        for (auto ai = res.ai; ai; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET6) {
                v.push_back(ai);
            }
        }
    }
    for (auto ai = res.ai; ai; ai = ai->ai_next) {
        if (!res.address_any || ai->ai_family != AF_INET6) {
            v.push_back(ai);
        }
    }
    return v;
}

// Set IPV6_V6ONLY on a passive IPv6 socket. For an empty host, make the
// socket dual-stack unless an IPv4 socket is already bound to the port (on
// Linux, a dual-stack socket would then fail to bind).
static void set_v6only(int fileno, const getaddrinfo_value& res, bool bound_ipv4) {
    int flag = res.address_any && !bound_ipv4 ? 0 : 1;
    setsockopt(fileno, IPPROTO_IPV6, IPV6_V6ONLY, &flag, sizeof(flag));
}


// Create up to `max` TCP sockets passively listening on `address`, which
// should follow the pattern `ADDR:PORT`. If `ADDR` is empty, accepts
// connections from any address; if nonempty, accepts connections only from
// the specified address. Throws on error.

task<fd_list> tcp_listen_all(std::string address, int backlog, size_t max) {
    // DNS lookup can block, so do it on a separate thread
    auto res = std::make_shared<getaddrinfo_value>();
    res->hints.ai_family = AF_UNSPEC;
    res->hints.ai_socktype = SOCK_STREAM;
    res->hints.ai_flags = AI_PASSIVE;

    // start thread for lookup, co_await result
    event notifier;
    driver_guard guard;
    std::thread(getaddrinfo_thread, std::move(address), res, notifier).detach();
    co_await notifier;
    if (res->status != 0) {
        throw std::system_error(res->status, getaddrinfo_error_category());
    }

    // `getaddrinfo` can return multiple addresses (e.g., IPv4 and IPv6);
    // try each one in turn
    fd_list result;
    int last_errno = EADDRNOTAVAIL;
    bool bound_ipv4 = false;
    for (auto ai : passive_addresses(*res)) {
        auto f = nonblocking_socket(ai);
        if (!f) {
            last_errno = errno;
            continue;
        }
        int flag = 1;
        setsockopt(f.fileno(), SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
        if (ai->ai_family == AF_INET6) {
            set_v6only(f.fileno(), *res, bound_ipv4);
        }

        if (bind(f.fileno(), ai->ai_addr, ai->ai_addrlen) == 0
            && listen(f.fileno(), backlog) == 0) {
            bound_ipv4 = bound_ipv4 || ai->ai_family == AF_INET;
            result.push_back(std::move(f));
            if (max != 0 && result.size() == max) {
                break;
            }
        } else {
            last_errno = errno;     // `f` closes itself
        }
    }

    if (result.empty()) {
        throw std::system_error(last_errno, std::generic_category());
    }
    co_return std::move(result);
}

// Like `tcp_listen_all(address, backlog, 1)`. With an empty host (`:PORT`),
// the socket is dual-stack where possible, covering IPv4 and IPv6.
task<fd> tcp_listen(std::string address, int backlog) {
    auto res = co_await tcp_listen_all(std::move(address), backlog, 1);
    co_return std::move(res[0]);
}


// Create a TCP socket actively connected to `address`, which should follow
// the pattern `ADDR:PORT`. Throws on error.

task<fd> tcp_connect(std::string address) {
    auto res = std::make_shared<getaddrinfo_value>();
    res->hints.ai_family = AF_UNSPEC;
    res->hints.ai_socktype = SOCK_STREAM;

    // start thread for lookup, co_await result
    event notifier;
    driver_guard guard;
    std::thread(getaddrinfo_thread, std::move(address), res, notifier).detach();
    co_await notifier;
    if (res->status) {
        throw std::system_error(res->status, getaddrinfo_error_category());
    }

    // try each address in turn
    std::exception_ptr last_err;
    for (auto ai = res->ai; ai; ai = ai->ai_next) {
        auto f = nonblocking_socket(ai);
        if (!f) {
            last_err = std::make_exception_ptr(errno_error());
            continue;
        }
        int flag = 1;
        setsockopt(f.fileno(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        try {
            co_await connect(f, ai->ai_addr, ai->ai_addrlen);
            co_return std::move(f);
        } catch (...) {
            last_err = std::current_exception();
        }
    }
    std::rethrow_exception(last_err);
}


// Create a UDP socket passively bound to `address`, which should follow the
// pattern `ADDR:PORT`. The returned descriptor is ready to receive datagrams
// from any peer (use `recvfrom`/`recvmsg` to capture the sender’s address)
// and to send replies (use `sendto`/`sendmsg`). Throws on error.

task<fd_list> udp_listen_all(std::string address, size_t max) {
    auto res = std::make_shared<getaddrinfo_value>();
    res->hints.ai_family = AF_UNSPEC;
    res->hints.ai_socktype = SOCK_DGRAM;
    res->hints.ai_flags = AI_PASSIVE;

    event notifier;
    driver_guard guard;
    std::thread(getaddrinfo_thread, std::move(address), res, notifier).detach();
    co_await notifier;
    if (res->status != 0) {
        throw std::system_error(res->status, getaddrinfo_error_category());
    }

    fd_list result;
    int last_errno = EADDRNOTAVAIL;
    bool bound_ipv4 = false;
    for (auto ai : passive_addresses(*res)) {
        auto f = nonblocking_socket(ai);
        if (!f) {
            last_errno = errno;
            continue;
        }
        // No SO_REUSEADDR: for UDP on Linux it lets several sockets bind the
        // same address and port, silently splitting incoming datagrams.
        if (ai->ai_family == AF_INET6) {
            set_v6only(f.fileno(), *res, bound_ipv4);
        }

        if (bind(f.fileno(), ai->ai_addr, ai->ai_addrlen) == 0) {
            bound_ipv4 = bound_ipv4 || ai->ai_family == AF_INET;
            result.push_back(std::move(f));
            if (max != 0 && result.size() == max) {
                break;
            }
        } else {
            last_errno = errno;     // `f` closes itself
        }
    }

    if (result.empty()) {
        throw std::system_error(last_errno, std::generic_category());
    }
    co_return std::move(result);
}

task<fd> udp_listen(std::string address) {
    auto res = co_await udp_listen_all(std::move(address), 1);
    co_return std::move(res[0]);
}


// Create a UDP socket actively connected to `address`. After connection, the
// socket only accepts datagrams from that peer, and `send`/`recv` can be used
// directly without needing to specify the destination on each call. Throws on
// error.

task<fd> udp_connect(std::string address) {
    auto res = std::make_shared<getaddrinfo_value>();
    res->hints.ai_family = AF_UNSPEC;
    res->hints.ai_socktype = SOCK_DGRAM;

    event notifier;
    driver_guard guard;
    std::thread(getaddrinfo_thread, std::move(address), res, notifier).detach();
    co_await notifier;
    if (res->status) {
        throw std::system_error(res->status, getaddrinfo_error_category());
    }

    std::exception_ptr last_err;
    for (auto ai = res->ai; ai; ai = ai->ai_next) {
        auto f = nonblocking_socket(ai);
        if (!f) {
            last_err = std::make_exception_ptr(errno_error());
            continue;
        }

        try {
            co_await connect(f, ai->ai_addr, ai->ai_addrlen);
            co_return std::move(f);
        } catch (...) {
            last_err = std::current_exception();
        }
    }
    std::rethrow_exception(last_err);
}


namespace {
template <typename Iovcnt>
inline void update_iovec(iovec*& iov, Iovcnt& iovcnt,
                         std::vector<iovec>& iovcopy, size_t r) {
    while (iovcnt > 0 && r >= iov->iov_len) {
        r -= iov->iov_len;
        ++iov;
        --iovcnt;
    }
    if (r != 0) {
        if (iovcopy.empty()) {
            iovcopy.append_range(std::span(iov, iovcnt));
            iov = iovcopy.data();
        }
        iov->iov_base = reinterpret_cast<char*>(iov->iov_base) + r;
        iov->iov_len -= r;
    }
}

inline bool msghdr_done(const msghdr* mh, size_t r) {
    const iovec* iov = mh->msg_iov;
    auto iovcnt = mh->msg_iovlen;
    while (iovcnt > 0 && r >= iov->iov_len) {
        r -= iov->iov_len;
        ++iov;
        --iovcnt;
    }
    return iovcnt <= 0;
}
}


// Write all bytes in the `iovec`s, suspending as needed. Returns bytes
// written.
task<ioresult> writev(fd f, const iovec* iov, size_t iovcnt) {
    std::vector<iovec> iovcopy;
    size_t nw = 0;
    do {
        ssize_t r = ::writev(f.fileno(), iov, iovcnt);
        if (r > 0) {
            nw += r;
            update_iovec(const_cast<iovec*&>(iov), iovcnt, iovcopy, r);
        } else if (r == 0) {
            // This result is only expected if the original `count` was 0.
            // At other times, treat it like EOF on read.
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            co_await writable(f);
        } else if (nw > 0) {
            break;
        } else {
            co_return std::unexpected(errno_code());
        }
    } while (iovcnt != 0);
    co_return nw;
}


// Send the message in `mh`. Suspends on EAGAIN. If `flags & MSG_WAITALL`,
// suspends as needed until all bytes in `mh->msg_iov` are written (or error);
// this should be used only on connected sockets (e.g., TCP). Returns bytes
// written or error code.

task<ioresult> sendmsg(fd f, const msghdr* mh, int flags) {
    size_t nw = 0;
    msghdr mhx;
    std::vector<iovec> iovcopy;
    int xflags = (flags & ~MSG_WAITALL) | MSG_DONTWAIT | MSG_NOSIGNAL;
    do {
        ssize_t r = ::sendmsg(f.fileno(), mh, xflags);
        if (r >= 0) {
            nw += r;
            if (r == 0 || !(flags & MSG_WAITALL) || msghdr_done(mh, r)) {
                break;
            }
            if (mh != &mhx) {
                mhx = msghdr{};
                mhx.msg_iov = mh->msg_iov;
                mhx.msg_iovlen = mh->msg_iovlen;
                mh = &mhx;
            }
            update_iovec(mhx.msg_iov, mhx.msg_iovlen, iovcopy, r);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            co_await writable(f);
        } else if (nw > 0) {
            break;
        } else {
            co_return std::unexpected(errno_code());
        }
    } while (mh->msg_iovlen > 0);
    co_return nw;
}


// Receive the message to `mh`. Suspends on EAGAIN. If `flags & MSG_WAITALL`,
// additionally suspends as needed until all bytes in `mh->msg_iov` are read (or
// error); this should be used only on connected sockets (e.g., TCP). Returns
// bytes read or error code.

task<ioresult> recvmsg(fd f, msghdr* mh, int flags) {
    size_t nr = 0;
    msghdr mhx;
    std::vector<iovec> iovcopy;
    int xflags = (flags & ~MSG_WAITALL) | MSG_DONTWAIT;
    do {
        ssize_t r = ::recvmsg(f.fileno(), mh, xflags);
        if (r >= 0) {
            nr += r;
            if (r == 0 || !(flags & MSG_WAITALL) || msghdr_done(mh, r)) {
                break;
            }
            if (mh != &mhx) {
                mhx = msghdr{};
                mhx.msg_iov = mh->msg_iov;
                mhx.msg_iovlen = mh->msg_iovlen;
                mh = &mhx;
            }
            update_iovec(mhx.msg_iov, mhx.msg_iovlen, iovcopy, r);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            co_await readable(f);
        } else if (nr > 0) {
            break;
        } else {
            co_return std::unexpected(errno_code());
        }
    } while (mh->msg_iovlen > 0);
    co_return nr;
}

}
