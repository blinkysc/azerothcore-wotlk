/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ACORE_WORK_STEALING_THREAD_POOL_H
#define ACORE_WORK_STEALING_THREAD_POOL_H

#include "Define.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/**
 * @brief Work-stealing thread pool for efficient load balancing
 *
 * This thread pool uses per-thread work queues with work-stealing to achieve
 * better load balancing than traditional producer-consumer queues.
 *
 * Features:
 * - Per-thread deques for reduced contention
 * - Work stealing from idle threads
 * - Round-robin task distribution
 * - Automatic load balancing
 */
class WorkStealingThreadPool
{
public:
    using Task = std::function<void()>;

    WorkStealingThreadPool();
    ~WorkStealingThreadPool();

    /**
     * @brief Initialize and activate the thread pool
     * @param numThreads Number of worker threads to create
     */
    void Activate(std::size_t numThreads);

    /**
     * @brief Shutdown the thread pool and join all threads
     */
    void Deactivate();

    /**
     * @brief Check if the thread pool is active
     * @return true if threads are running
     */
    [[nodiscard]] bool IsActivated() const { return _activated.load(std::memory_order_acquire); }

    /**
     * @brief Submit a task to the thread pool
     * @param task Function to execute
     */
    void Submit(Task task);

    /**
     * @brief Wait for all pending tasks to complete
     */
    void WaitForCompletion();

    /**
     * @brief Get the number of worker threads
     * @return Thread count
     */
    [[nodiscard]] std::size_t GetThreadCount() const { return _workerThreads.size(); }

    /**
     * @brief Get the current number of pending tasks
     * @return Task count
     */
    [[nodiscard]] uint32 GetPendingTaskCount() const { return _taskCount.load(std::memory_order_acquire); }

private:
    /**
     * @brief Per-thread work queue for task storage
     */
    struct WorkQueue
    {
        std::deque<Task> tasks;
        mutable std::mutex mutex;

        /**
         * @brief Push a task to the front of the queue (owner thread)
         * @param task Task to add
         */
        void Push(Task task);

        /**
         * @brief Pop a task from the front of the queue (owner thread)
         * @param task Output parameter for popped task
         * @return true if a task was popped
         */
        bool Pop(Task& task);

        /**
         * @brief Steal a task from the back of the queue (thief thread)
         * @param task Output parameter for stolen task
         * @return true if a task was stolen
         */
        bool Steal(Task& task);

        /**
         * @brief Get approximate queue size (for debugging)
         * @return Number of tasks in queue
         */
        [[nodiscard]] std::size_t Size() const;
    };

    /**
     * @brief Worker thread main loop
     * @param threadId Unique identifier for this thread
     */
    void WorkerThread(uint32 threadId);

    /**
     * @brief Try to steal work from another thread
     * @param thiefId ID of the thread attempting to steal
     * @param task Output parameter for stolen task
     * @return true if work was stolen
     */
    bool TryStealWork(uint32 thiefId, Task& task);

    /**
     * @brief Task completion callback
     */
    void OnTaskComplete();

    std::vector<std::unique_ptr<WorkQueue>> _workerQueues;  ///< Per-thread work queues
    std::vector<std::thread> _workerThreads;                ///< Worker threads
    std::atomic<bool> _activated{false};                    ///< Activation flag (thread-safe)
    std::atomic<uint32> _taskCount{0};                      ///< Pending task counter
    std::atomic<uint32> _nextQueue{0};                      ///< Round-robin distribution
    std::atomic<bool> _shutdown{false};                     ///< Shutdown flag
    std::mutex _waitMutex;                                  ///< Wait synchronization
    std::condition_variable _waitCondition;                 ///< Wait notification
    std::atomic<uint32> _activeThreads{0};                  ///< Active working threads
};

#endif // ACORE_WORK_STEALING_THREAD_POOL_H
