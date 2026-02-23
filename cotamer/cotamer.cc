#include "cotamer.hh"
#include <iterator>
#include <memory>

namespace cotamer {

// driver methods

thread_local std::unique_ptr<driver> driver::main{new driver};

driver::driver()
    : now_(clock::from_time_t(1634070069)) {
}

driver::~driver() {
    if (!asap_.empty()
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
        std::coroutine_handle<> coh;

        if (lock_.load(std::memory_order_relaxed) != 0) {
            uint32_t flags = lock();
            std::deque<detail::event_handle> migrate;
            migrate_.swap(migrate);
            unlock(flags & ~df_nonempty);
            std::move(migrate.begin(), migrate.end(), std::back_inserter(asap_));
        }

        while (!asap_.empty()) {
            auto eh = std::move(asap_.front());
            asap_.pop_front();
            while ((coh = eh->driver_trigger(this))) {
                coh();
                now_ += clock::duration{1};
            }
            again = true;
        }

        // update time
        timed_.cull();
        if (asap_.empty() && !timed_.empty()) {
            now_ = timed_.top_time();
        }

        while (!timed_.empty() && timed_.top_time() <= now_) {
            auto eh = std::move(timed_.top());
            timed_.pop();
            while ((coh = eh->driver_trigger(this))) {
                coh();
                now_ += clock::duration{1};
            }
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
