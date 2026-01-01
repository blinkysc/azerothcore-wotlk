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

#include "WorkStealingPool.h"
#include "DatabaseEnv.h"
#include "Log.h"

// Exponential backoff parameters
static constexpr uint32 SPIN_COUNT_BEFORE_YIELD = 32;
static constexpr uint32 YIELD_COUNT_BEFORE_SLEEP = 16;
static constexpr uint32 SLEEP_MICROSECONDS = 100;

WorkStealingPool::WorkStealingPool(size_t numThreads)
    : _numWorkers(numThreads)
    , _shutdown(false)
    , _pendingTasks(0)
    , _nextWorker(0)
    , _activeWorkers(0)
{
    ASSERT(numThreads > 0);

    _deques.reserve(numThreads);
    _inboxes.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        _deques.push_back(std::make_unique<WorkStealingDeque>());
        _inboxes.push_back(std::make_unique<MPSCTaskQueue<TaskPtr>>());
    }

    _workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        _workers.emplace_back(&WorkStealingPool::WorkerLoop, this, i);
    }

    LOG_INFO("server.loading", ">> Work-stealing pool started with {} workers (lock-free)", numThreads);
}

WorkStealingPool::~WorkStealingPool()
{
    Shutdown();
}

void WorkStealingPool::Submit(Task task)
{
    if (_shutdown.load(std::memory_order_relaxed))
        return;

    size_t worker = _nextWorker.fetch_add(1, std::memory_order_relaxed) % _numWorkers;
    SubmitToWorker(worker, std::move(task));
}

void WorkStealingPool::SubmitToWorker(size_t workerIndex, Task task)
{
    if (_shutdown.load(std::memory_order_relaxed))
        return;

    ASSERT(workerIndex < _numWorkers);

    _pendingTasks.fetch_add(1, std::memory_order_release);

    // Push to worker's MPSC inbox (safe for external threads)
    TaskPtr ptr = new Task(std::move(task));
    _inboxes[workerIndex]->Push(ptr);
}

void WorkStealingPool::Wait()
{
    // Spin-wait with exponential backoff - NO LOCKS
    uint32 spinCount = 0;
    uint32 yieldCount = 0;

    while (_pendingTasks.load(std::memory_order_acquire) > 0)
    {
        if (spinCount < SPIN_COUNT_BEFORE_YIELD)
        {
            // Busy spin with pause instruction
            for (int i = 0; i < 16; ++i)
            {
                #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                    __builtin_ia32_pause();
                #else
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                #endif
            }
            ++spinCount;
        }
        else if (yieldCount < YIELD_COUNT_BEFORE_SLEEP)
        {
            std::this_thread::yield();
            ++yieldCount;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::microseconds(SLEEP_MICROSECONDS));
        }
    }
}

void WorkStealingPool::Shutdown()
{
    if (_shutdown.exchange(true, std::memory_order_acq_rel))
        return;

    // Wait for pending work
    Wait();

    // Join all workers
    for (auto& thread : _workers)
    {
        if (thread.joinable())
            thread.join();
    }

    LOG_INFO("server.loading", ">> Work-stealing pool shutdown complete");
}

void WorkStealingPool::WorkerLoop(size_t workerIndex)
{
    LOG_INFO("server", "WorkStealingPool: Worker {} starting", workerIndex);

    // Warn about sync queries from worker threads
    LoginDatabase.WarnAboutSyncQueries(true);
    CharacterDatabase.WarnAboutSyncQueries(true);
    WorldDatabase.WarnAboutSyncQueries(true);

    _activeWorkers.fetch_add(1, std::memory_order_relaxed);

    LOG_INFO("server", "WorkStealingPool: Worker {} ready", workerIndex);

    uint32 spinCount = 0;
    uint32 yieldCount = 0;

    while (!_shutdown.load(std::memory_order_relaxed))
    {
        TaskPtr taskPtr = nullptr;
        bool foundWork = false;

        // 1. Check MPSC inbox for externally submitted tasks
        if (_inboxes[workerIndex]->Pop(taskPtr))
        {
            foundWork = true;
        }
        // 2. Try own deque (for subtasks spawned by worker)
        else if (_deques[workerIndex]->Pop(taskPtr))
        {
            foundWork = true;
        }
        // 3. Try stealing from others
        else if (TrySteal(workerIndex, taskPtr))
        {
            foundWork = true;
        }

        if (foundWork)
        {
            // Reset backoff
            spinCount = 0;
            yieldCount = 0;

            // Execute and cleanup
            (*taskPtr)();
            delete taskPtr;

            // Decrement pending count
            _pendingTasks.fetch_sub(1, std::memory_order_release);
        }
        else
        {
            // Exponential backoff - NO LOCKS
            if (spinCount < SPIN_COUNT_BEFORE_YIELD)
            {
                for (int i = 0; i < 16; ++i)
                {
                    #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                        __builtin_ia32_pause();
                    #else
                        std::atomic_signal_fence(std::memory_order_seq_cst);
                    #endif
                }
                ++spinCount;
            }
            else if (yieldCount < YIELD_COUNT_BEFORE_SLEEP)
            {
                std::this_thread::yield();
                ++yieldCount;
            }
            else
            {
                // Brief sleep to avoid burning CPU when truly idle
                std::this_thread::sleep_for(std::chrono::microseconds(SLEEP_MICROSECONDS));
            }
        }
    }

    _activeWorkers.fetch_sub(1, std::memory_order_relaxed);
}

bool WorkStealingPool::TrySteal(size_t thiefIndex, TaskPtr& task)
{
    if (_numWorkers <= 1)
        return false;

    // Start from a pseudo-random victim to avoid contention
    size_t start = thiefIndex + 1;

    for (size_t i = 0; i < _numWorkers - 1; ++i)
    {
        size_t victim = (start + i) % _numWorkers;

        if (_deques[victim]->Steal(task))
            return true;
    }

    return false;
}
