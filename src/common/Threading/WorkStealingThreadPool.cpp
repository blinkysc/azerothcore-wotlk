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

#include "WorkStealingThreadPool.h"
#include "Errors.h"
#include "Log.h"
#include <algorithm>

WorkStealingThreadPool::WorkStealingThreadPool() = default;

WorkStealingThreadPool::~WorkStealingThreadPool()
{
    if (IsActivated())
    {
        Deactivate();
    }
}

void WorkStealingThreadPool::Activate(std::size_t numThreads)
{
    ASSERT(!IsActivated(), "Thread pool already activated");
    ASSERT(numThreads > 0, "Thread pool requires at least one thread");

    _shutdown.store(false, std::memory_order_release);
    _taskCount.store(0, std::memory_order_release);
    _nextQueue.store(0, std::memory_order_release);
    _activeThreads.store(0, std::memory_order_release);

    // Create per-thread work queues
    _workerQueues.reserve(numThreads);
    for (std::size_t i = 0; i < numThreads; ++i)
    {
        _workerQueues.push_back(std::make_unique<WorkQueue>());
    }

    // Create worker threads
    _workerThreads.reserve(numThreads);
    for (uint32 i = 0; i < numThreads; ++i)
    {
        _workerThreads.emplace_back(&WorkStealingThreadPool::WorkerThread, this, i);
    }

    // Mark as activated (must be after threads are created)
    _activated.store(true, std::memory_order_release);

    LOG_INFO("server.loading", ">> Work-stealing thread pool activated with {} threads", numThreads);
}

void WorkStealingThreadPool::Deactivate()
{
    ASSERT(IsActivated(), "Thread pool not activated");

    // Mark as deactivated (before shutdown to prevent new submissions)
    _activated.store(false, std::memory_order_release);

    // Signal shutdown
    _shutdown.store(true, std::memory_order_release);

    // Notify waiting threads
    _waitCondition.notify_all();

    // Join all threads
    for (auto& thread : _workerThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    _workerThreads.clear();
    _workerQueues.clear();

    LOG_INFO("server.loading", ">> Work-stealing thread pool deactivated");
}

void WorkStealingThreadPool::Submit(Task task)
{
    ASSERT(IsActivated(), "Cannot submit tasks to inactive thread pool");

    // Increment task counter
    _taskCount.fetch_add(1, std::memory_order_release);

    // Round-robin distribution to worker queues
    uint32 queueId = _nextQueue.fetch_add(1, std::memory_order_acq_rel) % _workerQueues.size();
    _workerQueues[queueId]->Push(std::move(task));
}

void WorkStealingThreadPool::WaitForCompletion()
{
    // Use lock-free polling to avoid mutex contention with many threads
    // This is more efficient than condition variables when thread counts are high (64+)
    while (true)
    {
        uint32 taskCount = _taskCount.load(std::memory_order_acquire);
        uint32 activeThreads = _activeThreads.load(std::memory_order_acquire);

        if (taskCount == 0 && activeThreads == 0)
        {
            break;
        }

        // Brief sleep to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void WorkStealingThreadPool::WorkerThread(uint32 threadId)
{
    WorkQueue* myQueue = _workerQueues[threadId].get();
    Task task;

    while (!_shutdown.load(std::memory_order_acquire))
    {
        bool foundWork = false;

        // Try to pop from own queue first
        if (myQueue->Pop(task))
        {
            foundWork = true;
        }
        // If own queue is empty, try to steal from others
        else if (TryStealWork(threadId, task))
        {
            foundWork = true;
        }

        if (foundWork)
        {
            // Mark thread as active
            _activeThreads.fetch_add(1, std::memory_order_release);

            try
            {
                // Execute the task
                task();
            }
            catch (const std::exception& ex)
            {
                LOG_ERROR("server.worldserver", "Exception in thread pool task: {}", ex.what());
            }
            catch (...)
            {
                LOG_ERROR("server.worldserver", "Unknown exception in thread pool task");
            }

            // Mark completion
            _activeThreads.fetch_sub(1, std::memory_order_acq_rel);
            OnTaskComplete();
        }
        else
        {
            // No work found, yield to avoid busy-waiting
            std::this_thread::yield();

            // If still no tasks and shutdown not requested, brief sleep
            if (_taskCount.load(std::memory_order_acquire) == 0 &&
                !_shutdown.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }
}

bool WorkStealingThreadPool::TryStealWork(uint32 thiefId, Task& task)
{
    // Try to steal from all other queues
    uint32 numQueues = static_cast<uint32>(_workerQueues.size());

    // Start from the next queue (circular)
    for (uint32 offset = 1; offset < numQueues; ++offset)
    {
        uint32 victimId = (thiefId + offset) % numQueues;

        if (_workerQueues[victimId]->Steal(task))
        {
            return true;
        }
    }

    return false;
}

void WorkStealingThreadPool::OnTaskComplete()
{
    // Decrement task counter
    // WaitForCompletion() polls this atomically, no notification needed
    _taskCount.fetch_sub(1, std::memory_order_acq_rel);
}

// WorkQueue implementation
void WorkStealingThreadPool::WorkQueue::Push(Task task)
{
    std::lock_guard<std::mutex> lock(mutex);
    tasks.push_front(std::move(task));
}

bool WorkStealingThreadPool::WorkQueue::Pop(Task& task)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (tasks.empty())
    {
        return false;
    }

    task = std::move(tasks.front());
    tasks.pop_front();
    return true;
}

bool WorkStealingThreadPool::WorkQueue::Steal(Task& task)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (tasks.empty())
    {
        return false;
    }

    // Steal from the back (opposite end from Pop)
    task = std::move(tasks.back());
    tasks.pop_back();
    return true;
}

std::size_t WorkStealingThreadPool::WorkQueue::Size() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return tasks.size();
}
