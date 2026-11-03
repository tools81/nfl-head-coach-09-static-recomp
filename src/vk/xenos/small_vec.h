#pragma once
// A vector of trivially copyable elements with the first N in the object itself: the
// native front end builds a DrawOp per draw (registers, inline vertex data) and the
// hand-over a Cmd (fetch overrides); most fit inline, so a draw costs one heap
// allocation instead of several. Portable C++20.
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace d3dn {

template <class T, uint32_t N>
class SmallVec
{
    // Elements are moved with memcpy (std::pair of integers is not formally trivially copyable).
    static_assert(std::is_trivially_destructible_v<T> && std::is_standard_layout_v<T>, "SmallVec holds plain data");

public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    SmallVec() = default;
    SmallVec(const SmallVec& o) { assign(o.begin(), o.end()); }
    SmallVec(SmallVec&& o) noexcept { Take(o); }
    SmallVec& operator=(const SmallVec& o)
    {
        if (this != &o) assign(o.begin(), o.end());
        return *this;
    }
    SmallVec& operator=(SmallVec&& o) noexcept
    {
        if (this != &o) { Free(); Take(o); }
        return *this;
    }
    ~SmallVec() { Free(); }

    uint32_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    T* data() { return heap_ ? heap_ : Inline(); }
    const T* data() const { return heap_ ? heap_ : Inline(); }
    T* begin() { return data(); }
    T* end() { return data() + n_; }
    const T* begin() const { return data(); }
    const T* end() const { return data() + n_; }
    T& operator[](size_t i) { return data()[i]; }
    const T& operator[](size_t i) const { return data()[i]; }
    T& back() { return data()[n_ - 1]; }
    const T& back() const { return data()[n_ - 1]; }
    void clear() { n_ = 0; }
    void reserve(size_t n)
    {
        if (n > cap_) Grow(uint32_t(n));
    }
    void push_back(const T& v)
    {
        if (n_ == cap_) Grow(n_ + 1);
        memcpy(static_cast<void*>(data() + n_), &v, sizeof(T));
        ++n_;
    }
    template <class... A>
    T& emplace_back(A&&... a)
    {
        if (n_ == cap_) Grow(n_ + 1);
        T* p = new (data() + n_) T(std::forward<A>(a)...);
        ++n_;
        return *p;
    }
    // n more elements, not initialized: the caller writes them.
    T* append(size_t n)
    {
        if (n_ + n > cap_) Grow(uint32_t(n_ + n));
        T* p = data() + n_;
        n_ += uint32_t(n);
        return p;
    }
    template <class It>
    void assign(It first, It last)
    {
        size_t n = size_t(std::distance(first, last));
        n_ = 0;
        reserve(n);
        T* d = data();
        if constexpr (std::contiguous_iterator<It> && std::is_same_v<std::iter_value_t<It>, T>)
        {
            if (n) memcpy(static_cast<void*>(d), std::to_address(first), n * sizeof(T));
            n_ = uint32_t(n);
        }
        else
            for (; first != last; ++first) new (d + n_++) T(*first);
    }

private:
    T* Inline() { return reinterpret_cast<T*>(buf_); }
    const T* Inline() const { return reinterpret_cast<const T*>(buf_); }
    void Grow(uint32_t need)
    {
        uint32_t cap = cap_ * 2 > need ? cap_ * 2 : need;
        T* p = static_cast<T*>(::operator new(size_t(cap) * sizeof(T)));
        if (n_) memcpy(static_cast<void*>(p), data(), size_t(n_) * sizeof(T));
        Free();
        heap_ = p;
        cap_ = cap;
    }
    void Free()
    {
        if (heap_) ::operator delete(heap_);
        heap_ = nullptr;
        cap_ = N;
    }
    void Take(SmallVec& o)
    {
        n_ = o.n_;
        if (o.heap_)
        {
            heap_ = o.heap_;
            cap_ = o.cap_;
            o.heap_ = nullptr;
            o.cap_ = N;
        }
        else
        {
            heap_ = nullptr;
            cap_ = N;
            if (n_) memcpy(buf_, o.buf_, size_t(n_) * sizeof(T));
        }
        o.n_ = 0;
    }
    T* heap_ = nullptr;
    uint32_t n_ = 0, cap_ = N;
    alignas(T) unsigned char buf_[N * sizeof(T)];
};

}  // namespace d3dn
