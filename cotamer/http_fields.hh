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
//    Example:
//        if (auto c = req.header("Connection");
//            cot::strings::http_value_list(c).icontains("upgrade")) ...

namespace cotamer::strings {

std::string http_unquote(std::string_view);


// View over a comma-separated header value (RFC 9110 §5.6.1 `#rule`).
// Iteration is single-pass, forward-only; each `*it` is one entry as a
// `string_view` with leading/trailing OWS (SP/HTAB) trimmed. Empty
// entries (`a,,b`, leading or trailing `,`) are skipped per
// `1#element` semantics. A quoted-string entry (`"a,b"`) is preserved
// verbatim with its surrounding `"`s, is not split on embedded commas,
// and treats `\X` inside the quotes as an escape; pass it to
// `http_unquote` to get the bare bytes.
//
// Suits `Connection`, `Upgrade`, `Accept-Encoding`,
// `Sec-WebSocket-Protocol`, and similar `#token` / `#element` fields.
// `icontains` covers the common "is this token present?" check.
//
// The view is non-owning — the underlying header bytes must outlive it.
class http_value_list {
public:
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using reference = std::string_view;
        using pointer = void;

        iterator() = default;
        iterator(std::string_view header) noexcept
            : rest_(header) {
            ++*this;
        }

        std::string_view operator*() const noexcept { return value_; }
        iterator& operator++() noexcept;
        iterator operator++(int) noexcept { auto tmp = *this; ++*this; return tmp; }
        constexpr bool operator==(const iterator& x) const noexcept {
            return value_.data() == x.value_.data();
        }

    private:
        std::string_view value_;
        std::string_view rest_;
    };

    explicit http_value_list(std::string_view header) noexcept
        : header_(header) {
    }
    explicit http_value_list(const char* header) noexcept
        : header_(header) {
    }

    iterator begin() const noexcept {
        return iterator(header_);
    }
    iterator end() const noexcept {
        const char* s = header_.data() + header_.size();
        return iterator({s, 0});
    }

    // Case-insensitive token-list membership test. Convenience for the
    // common `Connection: upgrade`, `Upgrade: websocket` style checks.
    bool icontains(std::string_view token) const noexcept {
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


// View over a semicolon-separated parameter list (RFC 9110 §5.6.6).
// Iteration is single-pass, forward-only; each entry is either a bare
// token or `name=value`. The first entry of a typical parameterized
// value is the leading type/extension token (e.g. `text/html`,
// `permessage-deflate`); subsequent entries are its parameters.
//
//     *it / span()    the entry's raw bytes, OWS-trimmed on both ends
//     name()          token left of `=`, or the whole span if no `=`
//     name_ci(s)      case-insensitive name match
//     has_value()     true iff the entry contained a `=`
//     raw_value()     bytes right of `=` (with the surrounding `"`s
//                     intact for quoted values)
//     value()         `raw_value()` run through `http_unquote`
//
// Splitting is quoted-string aware: `;` inside `"…"` does not split,
// and `\X` inside the quotes is an escape. The `=` parse is strict per
// RFC 9110: any OWS around `=` defeats the split, so `foo = bar`
// iterates as a single standalone whose `name()` is the entire span
// and `has_value()` is false.
//
// Typically nested inside `http_value_list` (outer `#rule`, inner
// parameters) for headers like `Sec-WebSocket-Extensions`,
// `Content-Type`, `Cache-Control`.
//
// The view is non-owning — the underlying header bytes must outlive it.
class http_parameter_list {
public:
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using reference = std::string_view;
        using pointer = void;

        iterator()
            : span_(), namelen_(0), rest_(span_) {
        }
        iterator(std::string_view header) noexcept
            : rest_(header) {
            ++*this;
        }

        std::string_view operator*() const noexcept { return span_; }
        iterator& operator++() noexcept;
        iterator operator++(int) noexcept { auto tmp = *this; ++*this; return tmp; }
        constexpr bool operator==(const iterator& x) const noexcept {
            return span_.data() == x.span_.data();
        }

        constexpr std::string_view span() const noexcept {
            return span_;
        }
        constexpr std::string_view name() const noexcept {
            return {span_.data(), namelen_};
        }
        bool name_ci(std::string_view s) const noexcept {
            return strings::ieq(name(), s);
        }
        constexpr bool has_value() const noexcept {
            return namelen_ != span_.length();
        }
        std::string_view raw_value() const noexcept {
            if (namelen_ == span_.length()) {
                return span_;
            }
            return {span_.data() + namelen_ + 1, span_.length() - namelen_ - 1};
        }
        std::string value() const {
            return http_unquote(raw_value());
        }

    private:
        std::string_view span_;
        size_t namelen_;
        std::string_view rest_;
    };

    explicit http_parameter_list(std::string_view value) noexcept
        : value_(value) {
    }

    iterator begin() const noexcept {
        return iterator(value_);
    }
    iterator end() const noexcept {
        const char* s = value_.data() + value_.size();
        return iterator({s, 0});
    }

private:
    std::string_view value_;
};

} // namespace cotamer::strings
