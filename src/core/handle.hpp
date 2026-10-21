#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace apex {

template <typename T> struct Handle {
    static constexpr std::uint32_t invalidIndex = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t index = invalidIndex;
    std::uint32_t generation = 0;

    auto operator<=>(const Handle&) const = default;
};

// Handles belong to the pool that created them. Mutations invalidate iterators; create may
// invalidate pointers. Resolve handles again after mutation rather than retaining pointers.
template <typename T> class Pool {
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation = 1;
        std::uint32_t nextFree = Handle<T>::invalidIndex;
    };

    std::vector<Slot> slots_;
    std::uint32_t free_ = Handle<T>::invalidIndex;
    std::size_t size_ = 0;

    template <bool IsConst> class Iterator {
        using Owner = std::conditional_t<IsConst, const Pool, Pool>;
        Owner* pool_ = nullptr;
        std::size_t index_ = 0;

        void skipEmpty() {
            while (index_ < pool_->slots_.size() && !pool_->slots_[index_].value) {
                ++index_;
            }
        }

      public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<IsConst, const T&, T&>;
        using pointer = std::conditional_t<IsConst, const T*, T*>;
        using iterator_category = std::forward_iterator_tag;

        Iterator() = default;
        Iterator(Owner* pool, std::size_t index) : pool_(pool), index_(index) { skipEmpty(); }
        reference operator*() const { return *pool_->slots_[index_].value; }
        pointer operator->() const { return pool_->slots_[index_].value.operator->(); }
        Iterator& operator++() {
            ++index_;
            skipEmpty();
            return *this;
        }
        Iterator operator++(int) {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const Iterator&) const = default;
    };

  public:
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    Pool() = default;
    Pool(const Pool&) = default;
    Pool& operator=(const Pool&) = default;
    Pool(Pool&& other) noexcept
        : slots_(std::move(other.slots_)),
          free_(std::exchange(other.free_, Handle<T>::invalidIndex)),
          size_(std::exchange(other.size_, 0)) {
        other.slots_.clear();
    }
    Pool& operator=(Pool&& other) noexcept {
        if (this != &other) {
            slots_ = std::move(other.slots_);
            free_ = std::exchange(other.free_, Handle<T>::invalidIndex);
            size_ = std::exchange(other.size_, 0);
            other.slots_.clear();
        }
        return *this;
    }

    Handle<T> create(T value) {
        if (free_ != Handle<T>::invalidIndex) {
            const auto index = free_;
            auto& slot = slots_[index];
            slot.value.emplace(std::move(value));
            free_ = slot.nextFree;
            ++size_;
            return {index, slot.generation};
        }
        if (slots_.size() >= Handle<T>::invalidIndex) {
            throw std::length_error("Pool exhausted its handle indices");
        }
        const auto index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(Slot{std::optional<T>{std::move(value)}});
        ++size_;
        return {index, 1};
    }

    bool destroy(Handle<T> handle) {
        if (!contains(handle)) {
            return false;
        }
        auto& slot = slots_[handle.index];
        slot.value.reset();
        --size_;
        // Retire exhausted generations instead of ever reviving a stale handle on wraparound.
        if (slot.generation != std::numeric_limits<std::uint32_t>::max()) {
            ++slot.generation;
            slot.nextFree = free_;
            free_ = handle.index;
        }
        return true;
    }

    T* get(Handle<T> handle) {
        return contains(handle) ? slots_[handle.index].value.operator->() : nullptr;
    }
    const T* get(Handle<T> handle) const {
        return contains(handle) ? slots_[handle.index].value.operator->() : nullptr;
    }
    bool contains(Handle<T> handle) const {
        return handle.index < slots_.size() && slots_[handle.index].value.has_value() &&
               slots_[handle.index].generation == handle.generation;
    }
    std::size_t size() const { return size_; }
    iterator begin() { return {this, 0}; }
    iterator end() { return {this, slots_.size()}; }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, slots_.size()}; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
};

} // namespace apex

template <typename T> struct std::hash<apex::Handle<T>> {
    std::size_t operator()(apex::Handle<T> handle) const noexcept {
        const auto packed = (static_cast<std::uint64_t>(handle.generation) << 32) | handle.index;
        return std::hash<std::uint64_t>{}(packed);
    }
};
