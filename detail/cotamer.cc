#include "cotamer.hh"
#include <iterator>
#include <memory>

namespace cotamer {

thread_local std::unique_ptr<driver> driver::main{new driver};

driver::driver()
    : now_(clock::from_time_t(1634070069)) {
}

driver::~driver() {
    if (!asap_.empty()
        || !ready_.empty()
        || !timed_.empty()
        || lock_.load(std::memory_order_relaxed)) {
        // Clear any remaining events and coroutines
        clear();
        loop();
    }
}

void driver::loop() {
    bool again = true;
    while (again) {
        again = false;

        while (!asap_.empty()) {
            asap_.front().trigger();
            asap_.pop_front();
            again = true;
        }

        if (lock_.load(std::memory_order_relaxed)) {
            uint32_t flags = lock();
            std::deque<std::coroutine_handle<>> remote_ready;
            remote_ready_.swap(remote_ready);
            unlock(flags & ~df_nonempty);
            ready_.append_range(remote_ready);
        }

        while (!ready_.empty()) {
            auto ch = ready_.front();
            ready_.pop_front();
            ch();
            now_ += clock::duration{1};
            again = true;
        }

        // update time
        timed_.cull();
        if (asap_.empty() && !timed_.empty()) {
            now_ = timed_.top_time();
        }

        while (!timed_.empty() && timed_.top_time() <= now_) {
            timed_.top()->trigger();
            timed_.pop();
            again = true;
        }

        // go again if incoming
        if (lock_.load(std::memory_order_relaxed)) {
            again = true;
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


std::string event::debug_info() const {
    return std::format("#<event {}{}>", static_cast<void*>(handle().get()),
                       triggered() ? " triggered" : "");
}


inline bool mutex::allow(bool is_shared, latch_type l) const noexcept {
    return is_shared
        ? awoken_ != mf_lock_excl && !(l & mf_lock_excl)
        : awoken_ == 0 && l < mf_lock_excl;
}

inline void mutex::notify_locked(latch_type l) {
    while (!waiters_.empty()) {
        auto& fw = waiters_.front();
        if (!fw.empty()) {
            bool fws = waiter_shared(fw);
            if (!allow(fws, l)) {
                break;
            }
            if (fw->trigger()) {
                awoken_ += fws ? mf_lock_shared : mf_lock_excl;
            }
        }
        waiters_.pop_front();
    }
}

task<> mutex::lock_impl(bool is_shared) {
    latch_type l = latch();
    notify_locked(l);
    if (!waiters_.empty() || !allow(is_shared, l)) {
        event e;
        if (is_shared) {
            e.handle().get()->set_user_flags(detail::ef_user);
        }
        waiters_.push_back(e.handle());
        unlatch(l);

        co_await std::move(e);

        l = latch();
        assert(is_shared ? awoken_ >= mf_lock_shared : awoken_ == mf_lock_excl);
        awoken_ -= is_shared ? mf_lock_shared : mf_lock_excl;
    }
    unlatch(l + (is_shared ? mf_lock_shared : mf_lock_excl));
}

void mutex::unlock_impl(bool is_shared) {
    latch_type l = latch();
    l -= is_shared ? mf_lock_shared : mf_lock_excl;
    notify_locked(l);
    unlatch(l);
}

}
