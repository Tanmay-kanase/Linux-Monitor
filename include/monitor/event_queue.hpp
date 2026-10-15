#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace monitor {

// Thread-safe FIFO queue: the hand-off point between event producers
// (DirWatcher, ProcessMonitor) and the consumer (EventLogger).
//
// Shutdown protocol: producers stop, then someone calls close(). After close(),
// push() is a no-op and pop() keeps returning the remaining items, then
// std::nullopt - so consumers drain everything and exit cleanly.
template <typename T>
class EventQueue {
public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            items_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    // Blocks until an item is available, or the queue is closed and empty.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !items_.empty() || closed_; });
        return take_locked();
    }

    // Like pop(), but gives up after `timeout` and returns std::nullopt.
    template <typename Rep, typename Period>
    std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return !items_.empty() || closed_; });
        return take_locked();
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    std::optional<T> take_locked() {
        if (items_.empty()) return std::nullopt;
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> items_;
    bool closed_ = false;
};

}  // namespace monitor
