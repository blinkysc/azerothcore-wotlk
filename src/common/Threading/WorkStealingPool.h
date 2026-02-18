/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _WORK_STEALING_POOL_H
#define _WORK_STEALING_POOL_H

#include "Define.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

/**
 * @brief Task type for logical queue separation
 *
 * Enables work-assisting waits to only execute tasks of matching type,
 * preventing Map::Update from being executed during cell update waits.
 */
enum class TaskType : uint8_t
{
    MAP = 0,    // Map::Update, LFG updates, map preloads
    CELL = 1,   // CellActor::Update tasks
    COUNT = 2   // Number of task types (for array sizing)
};

/**
 * @brief Lock-free Work-Stealing Deque (Chase-Lev algorithm)
 *
 * Zero locks, zero mutexes. Only atomic operations.
 * Stores pointers to tasks (T*) since std::function isn't trivially copyable.
 */
class WorkStealingDeque
{
public:
    using Task = std::function<void()>;
    using TaskPtr = Task*;

    explicit WorkStealingDeque(size_t capacity = 4096)
        : _capacity(capacity)
        , _mask(capacity - 1)
        , _top(0)
        , _bottom(0)
    {
        // Capacity must be power of 2 for mask to work
        ASSERT((capacity & (capacity - 1)) == 0);
        _buffer = std::make_unique<std::atomic<TaskPtr>[]>(capacity);
        for (size_t i = 0; i < capacity; ++i)
            _buffer[i].store(nullptr, std::memory_order_relaxed);
    }

    ~WorkStealingDeque()
    {
        // Clean up any remaining tasks
        TaskPtr task;
        while (Pop(task))
            delete task;
    }

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

    /**
     * @brief Push task to bottom (can be called from any thread)
     * Modified from classic Chase-Lev to support external submissions.
     */
    void Push(Task task)
    {
        size_t b = _bottom.load(std::memory_order_relaxed);
        size_t t = _top.load(std::memory_order_acquire);

        ASSERT(b - t < _capacity);

        TaskPtr ptr = new Task(std::move(task));
        _buffer[b & _mask].store(ptr, std::memory_order_relaxed);
        // Use release store so Pop/Steal on other threads can see the buffer write
        _bottom.store(b + 1, std::memory_order_release);
    }

    /**
     * @brief Pop task from bottom (worker thread)
     * Modified to use acquire ordering for cross-thread Push visibility.
     */
    bool Pop(TaskPtr& result)
    {
        // Use acquire to sync with Push's release store
        size_t b = _bottom.load(std::memory_order_acquire);
        if (b == 0)
            return false;

        b = b - 1;
        _bottom.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        size_t t = _top.load(std::memory_order_acquire);

        if (t <= b)
        {
            result = _buffer[b & _mask].load(std::memory_order_acquire);

            if (t == b)
            {
                // Last element - race with stealers
                if (!_top.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst, std::memory_order_relaxed))
                {
                    _bottom.store(b + 1, std::memory_order_relaxed);
                    return false;
                }
                _bottom.store(b + 1, std::memory_order_relaxed);
            }
            return true;
        }
        else
        {
            _bottom.store(b + 1, std::memory_order_relaxed);
            return false;
        }
    }

    /**
     * @brief Steal task from top (other threads)
     */
    bool Steal(TaskPtr& result)
    {
        size_t t = _top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        size_t b = _bottom.load(std::memory_order_acquire);

        if (t < b)
        {
            // Use acquire to sync with Push's buffer store
            result = _buffer[t & _mask].load(std::memory_order_acquire);

            if (!_top.compare_exchange_strong(t, t + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                return false;
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] bool Empty() const
    {
        size_t t = _top.load(std::memory_order_relaxed);
        size_t b = _bottom.load(std::memory_order_relaxed);
        return t >= b;
    }

    [[nodiscard]] size_t Size() const
    {
        size_t t = _top.load(std::memory_order_relaxed);
        size_t b = _bottom.load(std::memory_order_relaxed);
        return (b > t) ? (b - t) : 0;
    }

private:
    size_t const _capacity;
    size_t const _mask;
    alignas(64) std::atomic<size_t> _top;    // Cache line separated
    alignas(64) std::atomic<size_t> _bottom; // to avoid false sharing
    std::unique_ptr<std::atomic<TaskPtr>[]> _buffer;
};

/**
 * @brief Lock-Free MPSC Queue for external task submission
 * Multi-producer (external threads), single-consumer (worker).
 */
template<typename T>
class MPSCTaskQueue
{
    struct Node
    {
        std::atomic<Node*> next{nullptr};
        T data;
    };

public:
    MPSCTaskQueue()
    {
        Node* dummy = new Node();
        _head.store(dummy, std::memory_order_relaxed);
        _tail = dummy;
    }

    ~MPSCTaskQueue()
    {
        T dummy;
        while (Pop(dummy)) { delete dummy; }
        delete _tail;
    }

    // Called by external producer threads (lock-free)
    void Push(T item)
    {
        Node* node = new Node();
        node->data = item;

        Node* prev = _head.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    // Called by single consumer (worker thread)
    bool Pop(T& result)
    {
        Node* tail = _tail;
        Node* next = tail->next.load(std::memory_order_acquire);

        if (next == nullptr)
            return false;

        result = next->data;
        _tail = next;
        delete tail;
        return true;
    }

    [[nodiscard]] bool Empty() const
    {
        return _tail->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    alignas(64) std::atomic<Node*> _head;
    Node* _tail;  // Only accessed by consumer
};

/**
 * @brief Lock-Free Work-Stealing Thread Pool
 *
 * NO MUTEXES. NO LOCKS. Only atomics.
 * External submissions go to MPSC queue, workers drain to local deque.
 *
 * Supports task types for logical queue separation - TryExecuteOne(type)
 * only executes tasks of the specified type, preventing cross-type interference.
 */
class WorkStealingPool
{
public:
    using Task = std::function<void()>;
    using TaskPtr = Task*;

    static constexpr size_t NUM_TASK_TYPES = static_cast<size_t>(TaskType::COUNT);

    explicit WorkStealingPool(size_t numThreads);
    ~WorkStealingPool();

    WorkStealingPool(const WorkStealingPool&) = delete;
    WorkStealingPool& operator=(const WorkStealingPool&) = delete;

    // Type-aware submission
    void Submit(TaskType type, Task task);
    void SubmitToWorker(TaskType type, size_t workerIndex, Task task);

    // Backward compatible (defaults to MAP type)
    void Submit(Task task) { Submit(TaskType::MAP, std::move(task)); }
    void SubmitToWorker(size_t workerIndex, Task task) {
        SubmitToWorker(TaskType::MAP, workerIndex, std::move(task));
    }

    // Type-aware wait
    void Wait(TaskType type);
    void Wait() { Wait(TaskType::MAP); }  // Backward compat

    void Shutdown();

    // Type-filtered task execution (for work-assisting waits)
    // Only executes tasks of the specified type
    bool TryExecuteOne(TaskType type);
    bool TryExecuteOne() { return TryExecuteOne(TaskType::MAP); }

    [[nodiscard]] size_t NumWorkers() const { return _numWorkers; }
    [[nodiscard]] bool IsActive() const { return !_shutdown.load(std::memory_order_relaxed); }

private:
    void WorkerLoop(size_t workerIndex);
    bool TryExecuteFromType(size_t workerIndex, TaskType type);
    bool TrySteal(size_t thiefIndex, TaskType type, TaskPtr& task);

    size_t _numWorkers;

    // Per-worker, per-type queues: [workerIndex][taskType]
    std::vector<std::array<std::unique_ptr<WorkStealingDeque>, NUM_TASK_TYPES>> _deques;
    std::vector<std::array<std::unique_ptr<MPSCTaskQueue<TaskPtr>>, NUM_TASK_TYPES>> _inboxes;
    std::vector<std::thread> _workers;

    alignas(64) std::atomic<bool> _shutdown{false};
    // Per-type pending counters
    alignas(64) std::array<std::atomic<size_t>, NUM_TASK_TYPES> _pendingTasks{};
    alignas(64) std::atomic<size_t> _nextWorker{0};
    alignas(64) std::atomic<size_t> _activeWorkers{0};
};

#endif // _WORK_STEALING_POOL_H
