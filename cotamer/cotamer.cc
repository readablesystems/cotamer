#include "cotamer.hh"
#include <iterator>
#include <memory>
#include <thread>

namespace cotamer {

#if COTAMER_STATS
statistics stats;
#endif


inline void driver::migrate_wake() {
    // to be added
}

void driver::migrate_asap(detail::event_handle eh) {
    auto df = lock();
    migrate_.emplace_back(std::move(eh));
    unlock(df | driver::df_nonempty);
    migrate_wake();
}


// driver methods

thread_local std::unique_ptr<driver> driver::current{new driver};

driver::driver()
    : virtual_epoch_(std::chrono::system_clock::from_time_t(1634070069)) {
}

driver::~driver() {
    if (!asap_.empty()
        || !timed_.empty()
        || lock_.load(std::memory_order_relaxed)
        || guard_count_ > 0
        || !keepalives_.empty()) {
        // Clear any remaining events and coroutines
        clear();
        loop();
    }
}

void driver::loop(looptype lt) {
    while (true) {
        // import migrated tasks and fd close events
        if (lock_.load(std::memory_order_relaxed) != 0) {
            finish_migrate();
        }

        // process an aliquot of asap and migrated tasks
        for (size_t n = asap_quota; n != 0 && !asap_.empty(); --n) {
            auto eh = std::move(asap_.front());
            asap_.pop_front();
            while (auto coh = eh->driver_trigger(this)) {
                coh();
                step_time();
            }
        }

        // remove dead keepalives
        while (!keepalives_.empty() && keepalives_.back()->triggered()) {
            keepalives_.pop_back();
        }

        // exit if nothing to do
        timed_.cull();
        if (timed_.empty()
            && !lock_.load(std::memory_order_relaxed)
            && guard_count_ == 0
            && keepalives_.empty()) {
            break;
        }

        // update time
        if (!timed_.empty()
            && asap_.empty()) {
            snow_ = timed_.top_time();
        }

        // process timers
        while (!timed_.empty()
               && timed_.top_time() <= snow_) {
            auto eh = std::move(timed_.top());
            timed_.pop();
            while (auto coh = eh->driver_trigger(this)) {
                coh();
                step_time();
            }
        }

        // exit if polling
        if (lt == looptype::poll) {
            break;
        }
    }
    clearing_ = false;
}

void driver::finish_migrate() {
    std::vector<detail::event_handle> migrate;

    uint32_t flags = lock();
    migrate_.swap(migrate);
    unlock(flags & ~df_nonempty);

    std::move(migrate.begin(), migrate.end(), std::back_inserter(asap_));
}

void driver::clear() {
    clearing_ = true;
}

void reset() {
    driver::current.reset(new driver);
}



// event functions

std::string event::debug_info() const {
    return std::format("#<event {}{}>", static_cast<void*>(handle().get()),
                       triggered() ? " triggered" : "");
}


// error functions

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
