//
// Created by user on 7/21/2024.
//

#ifndef FINALPROJECT_THREAD_SAFE_QUEUE_H
#define FINALPROJECT_THREAD_SAFE_QUEUE_H

#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>

template <typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue;
    mutable std::mutex mutex;
    std::condition_variable cond;
    bool finished = false;

public:
    bool is_finished() const {
        std::lock_guard<std::mutex> lock(mutex);
        return finished;
    }

    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(std::move(value));
        cond.notify_one();
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait(lock, [this] { return !queue.empty() || finished; });
        if (queue.empty()) {
            return false;
        }
        value = std::move(queue.front());
        queue.pop();
        return true;
    }

    void setFinished() {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
        cond.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.empty();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) {
            queue.pop();
        }
        finished = false;
    }
};
#endif //FINALPROJECT_THREAD_SAFE_QUEUE_H
