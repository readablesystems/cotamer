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

namespace cotamer::strings {

// View over a comma-separated header value (RFC 9110 §5.6.1, `#rule`).
// Each iterator dereference yields one entry as a string_view with
// surrounding OWS trimmed. Empty entries (`a,,b` or trailing `,`) are
// skipped per the standard's `1#element` semantics.
//
// The view is non-owning — the underlying header string must outlive it.
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
        constexpr std::strong_ordering operator<=>(const iterator& x) const noexcept {
            return value_.data() <=> x.value_.data();
        }

    private:
        std::string_view value_;
        std::string_view rest_;
    };

    explicit http_value_list(std::string header) noexcept
        : header_(std::move(header)) {
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
    bool contains_ci(std::string_view token) const noexcept {
        for (auto entry : *this) {
            if (strings::ieq(entry, token)) {
                return true;
            }
        }
        return false;
    }

private:
    std::string header_;
};


class http_parameter_list {
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

        std::string_view operator*() const noexcept { return span_; }
        iterator& operator++() noexcept;
        iterator operator++(int) noexcept { auto tmp = *this; ++*this; return tmp; }
        constexpr bool operator==(const iterator& x) const noexcept {
            return span_.data() == x.span_.data();
        }
        constexpr std::strong_ordering operator<=>(const iterator& x) const noexcept {
            return span_.data() <=> x.span_.data();
        }

        constexpr std::string_view span() const noexcept {
            return span_;
        }
        constexpr std::string_view name() const noexcept {
            return {span_.data(), namelen_};
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
            return unquote(raw_value());
        }

    private:
        std::string_view span_;
        size_t namelen_;
        std::string_view rest_;
    };

    explicit http_parameter_list(std::string_view value) noexcept
        : value_(std::move(value)) {
    }

    iterator begin() const noexcept {
        return iterator(value_);
    }
    iterator end() const noexcept {
        const char* s = value_.data() + value_.size();
        return iterator({s, 0});
    }

    static std::string unquote(std::string_view);

private:
    std::string_view value_;
};

} // namespace cotamer::strings
