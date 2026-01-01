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

// Phase 8A: Tuned backoff - more spinning (cheap), less yielding (expensive syscall)
static constexpr uint32 SPIN_COUNT_MAX = 64;   // 16x more spins before yield
static constexpr uint32 YIELD_COUNT_MAX = 4;   // Double yields before sleep
static constexpr uint32 SLEEP_MICROS = 1000;   // 1ms sleep (was 5ms)

// Thread-local worker index for CELL task routing
// SIZE_MAX means "not a worker thread"
static thread_local size_t tl_workerIndex = SIZE_MAX;

WorkStealingPool::WorkStealingPool(size_t numThreads)
    : _numWorkers(numThreads)
    , _shutdown(false)
    , _nextWorker(0)
    , _activeWorkers(0)
{
    ASSERT(numThreads > 0);

    // Initialize per-type pending counters
    for (size_t t = 0; t < NUM_TASK_TYPES; ++t)
        _pendingTasks[t].store(0, std::memory_order_relaxed);

    // Create per-worker, per-type queues
    _deques.reserve(numThreads);
    _inboxes.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        std::array<std::unique_ptr<WorkStealingDeque>, NUM_TASK_TYPES> workerDeques;
        std::array<std::unique_ptr<MPSCTaskQueue<TaskPtr>>, NUM_TASK_TYPES> workerInboxes;

        for (size_t t = 0; t < NUM_TASK_TYPES; ++t)
        {
            workerDeques[t] = std::make_unique<WorkStealingDeque>();
            workerInboxes[t] = std::make_unique<MPSCTaskQueue<TaskPtr>>();
        }

        _deques.push_back(std::move(workerDeques));
        _inboxes.push_back(std::move(workerInboxes));
    }

    _workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        _workers.emplace_back(&WorkStealingPool::WorkerLoop, this, i);
    }

    LOG_INFO("server.loading", ">> Work-stealing pool started with {} workers, {} task types (lock-free)",
        numThreads, NUM_TASK_TYPES);
}

WorkStealingPool::~WorkStealingPool()
{
    Shutdown();
}

void WorkStealingPool::Submit(TaskType type, Task task)
{
    if (_shutdown.load(std::memory_order_relaxed))
        return;

    size_t worker = _nextWorker.fetch_add(1, std::memory_order_relaxed) % _numWorkers;
    SubmitToWorker(type, worker, std::move(task));
}

void WorkStealingPool::SubmitToWorker(TaskType type, size_t workerIndex, Task task)
{
    if (_shutdown.load(std::memory_order_relaxed))
        return;

    ASSERT(workerIndex < _numWorkers);
    size_t typeIdx = static_cast<size_t>(type);
    ASSERT(typeIdx < NUM_TASK_TYPES);

    _pendingTasks[typeIdx].fetch_add(1, std::memory_order_release);

    if (type == TaskType::CELL)
    {
        // CELL tasks go directly to deque for immediate stealability
        // Use thread-local worker index - CELL submissions always happen from
        // within Map::Update which runs on a worker thread
        size_t targetWorker = tl_workerIndex;
        ASSERT(targetWorker != SIZE_MAX && "CELL tasks must be submitted from worker threads");
        ASSERT(targetWorker < _numWorkers);
        _deques[targetWorker][typeIdx]->Push(std::move(task));
    }
    else
    {
        // MAP tasks go through inbox (normal path)
        TaskPtr ptr = new Task(std::move(task));
        _inboxes[workerIndex][typeIdx]->Push(ptr);
    }
}

void WorkStealingPool::Wait(TaskType type)
{
    size_t typeIdx = static_cast<size_t>(type);

    // Simple spin-wait with fixed backoff
    uint32 spinCount = 0;
    uint32 yieldCount = 0;

    while (_pendingTasks[typeIdx].load(std::memory_order_acquire) > 0)
    {
        if (spinCount < SPIN_COUNT_MAX)
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
        else if (yieldCount < YIELD_COUNT_MAX)
        {
            std::this_thread::yield();
            ++yieldCount;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::microseconds(SLEEP_MICROS));
        }
    }
}

void WorkStealingPool::Shutdown()
{
    if (_shutdown.exchange(true, std::memory_order_acq_rel))
        return;

    // Wait for all pending work of all types
    for (size_t t = 0; t < NUM_TASK_TYPES; ++t)
        Wait(static_cast<TaskType>(t));

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
    // Set thread-local worker index for CELL task routing
    tl_workerIndex = workerIndex;

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
        bool foundWork = false;

        // Check all task types (MAP first, then CELL)
        for (size_t t = 0; t < NUM_TASK_TYPES && !foundWork; ++t)
        {
            TaskType type = static_cast<TaskType>(t);
            if (TryExecuteFromType(workerIndex, type))
            {
                foundWork = true;
            }
        }

        // Try stealing from other workers if no local work
        if (!foundWork)
        {
            TaskPtr taskPtr = nullptr;
            for (size_t t = 0; t < NUM_TASK_TYPES && !foundWork; ++t)
            {
                TaskType type = static_cast<TaskType>(t);
                if (TrySteal(workerIndex, type, taskPtr))
                {
                    // Execute and cleanup
                    (*taskPtr)();
                    delete taskPtr;

                    // Decrement type-specific pending count
                    _pendingTasks[t].fetch_sub(1, std::memory_order_release);
                    foundWork = true;
                }
            }
        }

        if (foundWork)
        {
            // Reset backoff
            spinCount = 0;
            yieldCount = 0;
        }
        else
        {
            // Phase 8A: Tuned backoff - more spinning (cheap), less yielding (expensive)
            if (spinCount < SPIN_COUNT_MAX)
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
            else if (yieldCount < YIELD_COUNT_MAX)
            {
                std::this_thread::yield();
                ++yieldCount;
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::microseconds(SLEEP_MICROS));
            }
        }
    }

    _activeWorkers.fetch_sub(1, std::memory_order_relaxed);
}

bool WorkStealingPool::TryExecuteFromType(size_t workerIndex, TaskType type)
{
    size_t typeIdx = static_cast<size_t>(type);
    TaskPtr taskPtr = nullptr;

    // 1. Check type-specific MPSC inbox
    if (_inboxes[workerIndex][typeIdx]->Pop(taskPtr))
    {
        (*taskPtr)();
        delete taskPtr;
        _pendingTasks[typeIdx].fetch_sub(1, std::memory_order_release);
        return true;
    }

    // 2. Try own type-specific deque
    if (_deques[workerIndex][typeIdx]->Pop(taskPtr))
    {
        (*taskPtr)();
        delete taskPtr;
        _pendingTasks[typeIdx].fetch_sub(1, std::memory_order_release);
        return true;
    }

    return false;
}

bool WorkStealingPool::TrySteal(size_t thiefIndex, TaskType type, TaskPtr& task)
{
    if (_numWorkers <= 1)
        return false;

    size_t typeIdx = static_cast<size_t>(type);

    // Start from a pseudo-random victim to avoid contention
    size_t start = thiefIndex + 1;

    for (size_t i = 0; i < _numWorkers - 1; ++i)
    {
        size_t victim = (start + i) % _numWorkers;

        if (_deques[victim][typeIdx]->Steal(task))
            return true;
    }

    return false;
}

bool WorkStealingPool::TryExecuteOne(TaskType type)
{
    if (_shutdown.load(std::memory_order_relaxed))
        return false;

    size_t typeIdx = static_cast<size_t>(type);
    TaskPtr taskPtr = nullptr;

    // Only steal from deques - DO NOT touch inboxes!
    // Inboxes are MPSC (single consumer = owning worker only)
    // Multiple threads calling Pop() on inboxes causes race conditions
    for (size_t i = 0; i < _numWorkers; ++i)
    {
        if (_deques[i][typeIdx]->Steal(taskPtr))
        {
            // Execute the stolen task
            (*taskPtr)();
            delete taskPtr;

            // Decrement type-specific pending count
            _pendingTasks[typeIdx].fetch_sub(1, std::memory_order_release);
            return true;
        }
    }

    return false;
}
