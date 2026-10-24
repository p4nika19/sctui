#include "sctui/core/thread_pool.hpp"

namespace sctui::core {

ThreadPool::ThreadPool(size_t num_threads) {
    if (num_threads == 0) num_threads = 1;
    workers_.reserve(num_threads);

    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this](std::stop_token stop_tok) {
            while (!stop_tok.stop_requested()) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    cv_.wait(lock, [this, &stop_tok] {
                        return stop_.load(std::memory_order_relaxed) ||
                               stop_tok.stop_requested() ||
                               !tasks_.empty();
                    });

                    if (tasks_.empty()) {
                        if (stop_.load(std::memory_order_relaxed) || stop_tok.stop_requested()) {
                            return;
                        }
                        continue;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }

                if (task) {
                    task();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::enqueue(std::function<void()> task) {
    if (stop_.load(std::memory_order_relaxed)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::shutdown() {
    bool expected = false;
    if (!stop_.compare_exchange_strong(expected, true)) {
        return;
    }

    cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace sctui::core
