#pragma once
#include "cotamer/cotamer.hh"
#include <cstddef>
#include <iterator>
#include <string_view>

// cotamer/http_fields.hh
//    Iterators for HTTP field-value grammar (RFC 9110 §5.6) — comma-
//    separated lists (`#rule`) and semicolon-separated parameter lists.
//    Building blocks for parsing structured headers like `Connection`,
//    `Upgrade`, `Accept`, `Cache-Control`, `Sec-WebSocket-Extensions`.
//
//    Living in `cotamer::http::`, so callers can drop the `http_` prefix:
//        for (auto t : http::field_value_list(req.find_header("connection").value())) {
//            if (strings::ieq(t, "upgrade")) ...
//        }

namespace cotamer::http {

// View over a comma-separated header value (RFC 9110 §5.6.1, `#rule`).
// Each iterator dereference yields one entry as a string_view with
// surrounding OWS trimmed. Empty entries (`a,,b` or trailing `,`) are
// skipped per the standard's `1#element` semantics.
//
// The view is non-owning — the underlying header string must outlive it.
class field_value_list {
public:
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using reference = std::string_view;
        using pointer = void;

        iterator() = default;
        iterator(std::string_view header, size_t pos) noexcept
            : header_(header), pos_(pos) { advance_to_next(); }

        std::string_view operator*() const noexcept { return current_; }
        iterator& operator++() noexcept {
            pos_ = next_pos_;
            advance_to_next();
            return *this;
        }
        iterator operator++(int) noexcept {
            auto tmp = *this; ++*this; return tmp;
        }
        bool operator==(const iterator& x) const noexcept {
            return pos_ == x.pos_ && header_.data() == x.header_.data();
        }

    private:
        // Find the next non-empty entry starting at pos_; populate
        // current_ and next_pos_. If no more entries, set pos_ ==
        // header_.size() (== end sentinel).
        void advance_to_next() noexcept {
            while (pos_ < header_.size()) {
                size_t comma = header_.find(',', pos_);
                size_t end = (comma == std::string_view::npos
                              ? header_.size() : comma);
                std::string_view entry = strings::trim_hws(
                    header_.substr(pos_, end - pos_));
                next_pos_ = (comma == std::string_view::npos
                             ? header_.size() : comma + 1);
                if (!entry.empty()) {
                    current_ = entry;
                    return;
                }
                pos_ = next_pos_;
            }
            current_ = {};
            next_pos_ = pos_;
        }

        std::string_view header_;
        size_t pos_ = 0;
        size_t next_pos_ = 0;
        std::string_view current_;
    };

    explicit field_value_list(std::string_view header) noexcept : header_(header) {}

    iterator begin() const noexcept { return iterator(header_, 0); }
    iterator end() const noexcept { return iterator(header_, header_.size()); }

    // Case-insensitive token-list membership test. Convenience for the
    // common `Connection: upgrade`, `Upgrade: websocket` style checks.
    bool contains_ci(std::string_view token) const noexcept {
        for (auto entry : *this) {
            if (strings::ieq(entry, token)) {
                return true;
            }
        }
        return false;
    }

private:
    std::string_view header_;
};


// One entry from a `parameters` iteration.
struct parameter {
    std::string_view name;       // OWS-trimmed token; empty if entry was empty
    std::string_view value;      // empty if no `=` was present
    bool quoted = false;         // value came from a quoted-string

    bool name_ieq(std::string_view s) const noexcept {
        return strings::ieq(name, s);
    }
};


// View over a `;`-separated parameter list (RFC 9110 §5.6.6,
// `parameters = *( OWS ";" OWS [ parameter ] )`). The leading
// element (before the first `;`) is *included* as the first
// iteration with an empty `value` — this matches the natural
// "extension token followed by parameters" shape used by headers
// like Sec-WebSocket-Extensions and Content-Type.
//
// Quoted-string values have their surrounding `"` stripped and
// `quoted` is true. Backslash-escapes inside the quoted-string are
// not unescaped here — callers that need that should copy the raw
// bytes and decode themselves.
class parameters {
public:
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = parameter;
        using difference_type = std::ptrdiff_t;
        using reference = parameter;
        using pointer = void;

        iterator() = default;
        iterator(std::string_view src, size_t pos) noexcept
            : src_(src), pos_(pos) { advance_to_next(); }

        parameter operator*() const noexcept { return current_; }
        iterator& operator++() noexcept {
            pos_ = next_pos_;
            advance_to_next();
            return *this;
        }
        iterator operator++(int) noexcept {
            auto tmp = *this; ++*this; return tmp;
        }
        bool operator==(const iterator& x) const noexcept {
            return pos_ == x.pos_ && src_.data() == x.src_.data();
        }

    private:
        void advance_to_next() noexcept {
            // Find the next non-empty `;`-delimited segment.
            while (pos_ < src_.size() || pos_ == 0) {
                size_t semi = find_semi(pos_);
                size_t end = (semi == std::string_view::npos
                              ? src_.size() : semi);
                std::string_view seg = strings::trim_hws(
                    src_.substr(pos_, end - pos_));
                next_pos_ = (semi == std::string_view::npos
                             ? src_.size() : semi + 1);
                if (!seg.empty()) {
                    parse_segment(seg);
                    return;
                }
                if (pos_ == 0 && semi == std::string_view::npos) {
                    // Wholly empty input — done.
                    break;
                }
                pos_ = next_pos_;
            }
            current_ = {};
            pos_ = src_.size();
            next_pos_ = pos_;
        }

        // Scan for the next `;` not inside a quoted-string.
        size_t find_semi(size_t from) const noexcept {
            bool in_quote = false;
            for (size_t i = from; i < src_.size(); ++i) {
                char c = src_[i];
                if (in_quote) {
                    if (c == '\\' && i + 1 < src_.size()) {
                        ++i;            // skip escaped char
                    } else if (c == '"') {
                        in_quote = false;
                    }
                } else if (c == '"') {
                    in_quote = true;
                } else if (c == ';') {
                    return i;
                }
            }
            return std::string_view::npos;
        }

        void parse_segment(std::string_view seg) noexcept {
            // seg is OWS-trimmed and non-empty.
            size_t eq = std::string_view::npos;
            bool in_quote = false;
            for (size_t i = 0; i < seg.size(); ++i) {
                char c = seg[i];
                if (in_quote) {
                    if (c == '\\' && i + 1 < seg.size()) {
                        ++i;
                    } else if (c == '"') {
                        in_quote = false;
                    }
                } else if (c == '"') {
                    in_quote = true;
                } else if (c == '=') {
                    eq = i;
                    break;
                }
            }
            if (eq == std::string_view::npos) {
                current_ = parameter{strings::trim_hws(seg), {}, false};
                return;
            }
            std::string_view name = strings::trim_hws(seg.substr(0, eq));
            std::string_view value = strings::trim_hws(seg.substr(eq + 1));
            bool quoted = false;
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value.remove_prefix(1);
                value.remove_suffix(1);
                quoted = true;
            }
            current_ = parameter{name, value, quoted};
        }

        std::string_view src_;
        size_t pos_ = 0;
        size_t next_pos_ = 0;
        parameter current_;
    };

    explicit parameters(std::string_view src) noexcept : src_(src) {}

    iterator begin() const noexcept { return iterator(src_, 0); }
    iterator end() const noexcept { return iterator(src_, src_.size()); }

private:
    std::string_view src_;
};


// A header entry of the shape `token *( OWS ";" OWS parameter )` —
// the leading token plus its semicolon-separated parameters.
// Convenient for `Sec-WebSocket-Extensions`, `Content-Type`,
// `Cache-Control` directives, etc.
//
// Build directly from a `field_value_list` entry:
//     for (auto entry : http::field_value_list(header)) {
//         http::parameterized pv(entry);
//         if (strings::ieq(pv.name(), "permessage-deflate")) {
//             for (auto p : pv.params()) { ... }
//         }
//     }
class parameterized {
public:
    explicit parameterized(std::string_view entry) noexcept {
        size_t semi = entry.find(';');  // top-level `;` (no quoting in tokens)
        if (semi == std::string_view::npos) {
            name_ = strings::trim_hws(entry);
        } else {
            name_ = strings::trim_hws(entry.substr(0, semi));
            params_src_ = entry.substr(semi + 1);
        }
    }

    std::string_view name() const noexcept { return name_; }
    parameters params() const noexcept { return parameters(params_src_); }

    bool name_ieq(std::string_view s) const noexcept {
        return strings::ieq(name_, s);
    }

private:
    std::string_view name_;
    std::string_view params_src_;
};

} // namespace cotamer::http
