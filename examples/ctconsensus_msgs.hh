#pragma once

namespace ctconsensus {

// ctconsensus_msgs.hh
//    Message structures and constants for messages in the Chandra-Toueg
//    consensus protocol.

// - Types of message
enum message_type {
    m_prepare, m_propose, m_ack, m_decide
};

// - Message structure (not all message types use all fields)
struct message {
    message_type type;
    uint64_t round;         // all but `m_decide`
    std::string color;      // all but `m_ack`
    uint64_t color_round;   // only `m_prepare`
    bool ack;               // only `m_ack`
};

// - Helper functions to make messages of specific types
inline message prepare_message(uint64_t round, std::string color, uint64_t color_round) {
    return message{m_prepare, round, color, color_round, false};
}

inline message propose_message(uint64_t round, std::string color) {
    return message{m_propose, round, color, 0, false};
}

inline message ack_message(uint64_t round, bool ack) {
    return message{m_ack, round, "", 0, ack};
}

inline message decide_message(std::string color) {
    return message{m_decide, 0, color, 0, false};
}

}



// - `std::format` and `std::print` support for message types and messages
// (No need to understand this.)

namespace std {
using namespace ctconsensus;

template <typename CharT>
struct formatter<message_type, CharT> : formatter<const char*, CharT> {
    using parent = formatter<const char*, CharT>;
    template <typename FormatContext>
    auto format(message_type mt, FormatContext& ctx) const {
        switch (mt) {
        case m_prepare:     return parent::format("PREPARE", ctx);
        case m_propose:     return parent::format("PROPOSE", ctx);
        case m_ack:         return parent::format("ACK", ctx);
        case m_decide:      return parent::format("DECIDE", ctx);
        default:            return parent::format("???", ctx);
        }
    }
};

template <typename CharT>
struct formatter<message, CharT> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    template <typename FormatContext>
    auto format(const message& m, FormatContext& ctx) const {
        switch (m.type) {
        case m_prepare:
            return std::format_to(ctx.out(), "{}({}, {}, {})", m.type, m.round, m.color, m.color_round);
        case m_propose:
            return std::format_to(ctx.out(), "{}({}, {})", m.type, m.round, m.color);
        case m_ack:
            return std::format_to(ctx.out(), "{}({}, {})", m.type, m.round, m.ack);
        case m_decide:
            return std::format_to(ctx.out(), "{}({})", m.type, m.color);
        default:
            return std::format_to(ctx.out(), "#{}({}, {}, {}, {})", int(m.type), m.round, m.color, m.color_round, m.ack);
        }
    }
};

}
