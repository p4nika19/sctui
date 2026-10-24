#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <vector>
#include <utility>

namespace sctui::core {

#if defined(__cpp_lib_hardware_interference_size)
    constexpr size_t CacheLineSize = std::hardware_destructive_interference_size;
#else
    constexpr size_t CacheLineSize = 64;
#endif

template <typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    SPSCQueue() : buffer_(new T[Capacity]) {}

    ~SPSCQueue() {
        delete[] buffer_;
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    template <typename... Args>
    bool emplace(Args&&... args) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_cached_;

        if ((tail - head) >= Capacity) {
            head_cached_ = head_.load(std::memory_order_acquire);
            if ((tail - head_cached_) >= Capacity) {
                return false; // Queue full
            }
        }

        buffer_[tail & Mask] = T(std::forward<Args>(args)...);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool push(const T& item) {
        return emplace(item);
    }

    bool push(T&& item) {
        return emplace(std::move(item));
    }

    std::optional<T> pop() {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_cached_;

        if (head == tail) {
            tail_cached_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cached_) {
                return std::nullopt; // Queue empty
            }
        }

        T item = std::move(buffer_[head & Mask]);
        head_.store(head + 1, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t size() const noexcept {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_relaxed);
        return (tail >= head) ? (tail - head) : 0;
    }

private:
    static constexpr size_t Mask = Capacity - 1;

    T* buffer_{nullptr};

    alignas(CacheLineSize) std::atomic<size_t> tail_{0};
    alignas(CacheLineSize) size_t head_cached_{0};

    alignas(CacheLineSize) std::atomic<size_t> head_{0};
    alignas(CacheLineSize) size_t tail_cached_{0};
};

} // namespace sctui::core
