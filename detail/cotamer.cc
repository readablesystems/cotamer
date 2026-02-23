#include "cotamer.hh"
#include <memory>

namespace cotamer {

std::unique_ptr<driver> driver::main{new driver};
bool driver::clearing = false;

driver::driver()
    : now_(std::chrono::system_clock::from_time_t(1634070069)) {
}

driver::~driver() {
    if (!asap_.empty() || !ready_.empty() || !timed_.empty()) {
        // Clear any remaining events and coroutines
        std::unique_ptr<driver> tmp(this);
        tmp.swap(main);
        clear();
        loop();
        tmp.swap(main);
        tmp.release();
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
    }
    clearing = false;
}

void driver::clear() {
    clearing = true;
}

void reset() {
    driver::main.reset(new driver);
}


std::string event::debug_info() const {
    return std::format("#<event {}{}>", static_cast<void*>(handle().get()),
                       triggered() ? " triggered" : "");
}

}
