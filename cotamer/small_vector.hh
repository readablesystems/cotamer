#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

template <typename T, size_t N>
struct small_vector {
    static_assert(N > 0 && N <= 128);
    // growth isn't exception-safe if moving an element can throw
    static_assert(std::is_nothrow_move_constructible_v<T>);

    using value_type = T;
    using size_type = size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using iterator = T*;
    using const_iterator = const T*;

    small_vector() = default;
    small_vector(const small_vector<T, N>&) = delete;
    small_vector(small_vector<T, N>&& x)
        : sz_(x.sz_), cap_(x.cap_) {
        if (x.cap_ > N) {
            u_.out = x.u_.out;
            x.sz_ = 0;
            x.cap_ = N;
        } else {
            std::uninitialized_move_n(x.begin(), x.sz_, begin());
        }
    }
    small_vector<T, N>& operator=(const small_vector<T, N>&) = delete;
    small_vector<T, N>& operator=(small_vector<T, N>&&) = delete;
    ~small_vector() {
        clear_capacity();
    }

    size_t size() const {
        return sz_;
    }
    bool empty() const {
        return size() == 0;
    }
    void clear() {
        std::destroy_n(begin(), sz_);
        sz_ = 0;
    }
    void clear_capacity() {
        std::destroy_n(begin(), sz_);
        sz_ = 0;
        if (cap_ > N) {
            std::allocator<T> alloc;
            alloc.deallocate(u_.out, cap_);
            cap_ = N;
        }
    }
    iterator erase(const_iterator first, const_iterator last) {
        if (first != last) {
            auto eit = end(), delsz = last - first;
            std::move(const_cast<iterator>(last), eit, const_cast<iterator>(first));
            sz_ -= delsz;
            std::destroy(end(), eit);
        }
        return const_cast<iterator>(first);
    }
    void truncate(const_iterator first) {
        std::destroy(const_cast<iterator>(first), end());
        sz_ = first - begin();
    }

    T* begin() {
        return cap_ <= N ? reinterpret_cast<T*>(u_.inbuf) : u_.out;
    }
    const T* begin() const {
        return cap_ <= N ? reinterpret_cast<const T*>(u_.inbuf) : u_.out;
    }
    T* end() {
        return begin() + size();
    }
    const T* end() const {
        return begin() + size();
    }

    T& front() {
        return *begin();
    }
    const T& front() const {
        return *begin();
    }
    T& back() {
        return *(end() - 1);
    }
    const T& back() const {
        return *(end() - 1);
    }
    T& operator[](size_type i) {
        return begin()[i];
    }
    const T& operator[](size_type i) const {
        return begin()[i];
    }

    void push_back(const T& v) {
        emplace_back(v);
    }
    void push_back(T&& v) {
        emplace_back(std::move(v));
    }
    template <typename... Args>
    T& emplace_back(Args&&... args) {
        T* sp;
        if (sz_ < cap_) {
            sp = std::construct_at(end(), std::forward<Args>(args)...);
        } else {
            sp = hard_emplace_back(std::forward<Args>(args)...);
        }
        ++sz_;
        return *sp;
    }
    void pop_back() {
        std::destroy_at(end() - 1);
        --sz_;
    }

private:
    uint32_t sz_ = 0;
    uint32_t cap_ = N;
    union {
        alignas(T) std::byte inbuf[sizeof(T) * N];
        T* out;
    } u_;

    template <typename... Args>
    T* hard_emplace_back(Args&&... args) {
        // Grow. Construct the new element before moving the old ones, since
        // `args` may refer to an existing element.
        std::allocator<T> alloc;
        T* newptr = alloc.allocate(cap_ * 2);
        T* sp;
        try {
            sp = std::construct_at(newptr + sz_, std::forward<Args>(args)...);
        } catch (...) {
            alloc.deallocate(newptr, cap_ * 2);
            throw;
        }
        std::uninitialized_move_n(begin(), sz_, newptr);
        std::destroy_n(begin(), sz_);
        if (cap_ > N) {
            alloc.deallocate(u_.out, cap_);
        }
        u_.out = newptr;
        cap_ *= 2;
        return sp;
    }
};
