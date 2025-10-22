#include "WorkStealingScheduler.h"
#include "Log.h"
#include <thread>

namespace Acore
{
    thread_local uint32 WorkStealingScheduler::_tlsWorkerId = UINT32_MAX;
    
    WorkStealingScheduler::TaskPool::TaskPool() : tasks(POOL_SIZE) {}
    
    WorkStealingScheduler::Task* WorkStealingScheduler::TaskPool::Allocate()
    {
        size_t index = allocated.fetch_add(1, std::memory_order_relaxed);
        if (index >= POOL_SIZE)
        {
            LOG_WARN("server.worldserver", "TaskPool exhausted! Allocating from heap.");
            return new Task();
        }
        return &tasks[index];
    }
    
    void WorkStealingScheduler::TaskPool::Reset()
    {
        allocated.store(0, std::memory_order_relaxed);
    }
    
    WorkStealingScheduler::WorkStealingScheduler(uint32 numThreads)
    {
        if (numThreads == 0)
        {
            numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
        }
        
        LOG_INFO("server.worldserver", "Initializing WorkStealingScheduler with {} threads", numThreads);
        
        _workers.resize(numThreads);
        for (uint32 i = 0; i < numThreads; ++i)
        {
            _workers[i].threadId = i;
            _workers[i].rng.seed(i);  // Seed RNG per thread
            _workers[i].thread = std::thread(&WorkStealingScheduler::WorkerMain, this, i);
        }
    }
    
    WorkStealingScheduler::~WorkStealingScheduler()
    {
        Shutdown();
    }
    
    void WorkStealingScheduler::Shutdown()
    {
        _shutdown.store(true, std::memory_order_release);
        
        for (auto& worker : _workers)
        {
            worker.shouldExit.store(true, std::memory_order_release);
        }
        
        for (auto& worker : _workers)
        {
            if (worker.thread.joinable())
            {
                worker.thread.join();
            }
        }
    }
    
    WorkStealingScheduler::Task* WorkStealingScheduler::Spawn(void (*func)(void*), void* data, Task* parent)
    {
        Task* task = _taskPool.Allocate();
        task->function = func;
        task->data = data;
        task->parent = parent;
        task->unfinished_jobs.store(1, std::memory_order_relaxed);
        
        if (parent)
        {
            parent->unfinished_jobs.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Push to current worker's deque
        if (_tlsWorkerId != UINT32_MAX && _tlsWorkerId < _workers.size())
        {
            _workers[_tlsWorkerId].deque.Push(task);
        }
        else
        {
            // Called from main thread - push to worker 0
            _workers[0].deque.Push(task);
        }
        
        return task;
    }
    
    void WorkStealingScheduler::Wait(Task* task)
    {
        while (!IsCompleted(task))
        {
            // Try local work first
            if (_tlsWorkerId != UINT32_MAX && _tlsWorkerId < _workers.size())
            {
                if (auto t = _workers[_tlsWorkerId].deque.Pop())
                {
                    ExecuteTask(*t);
                    continue;
                }
            }
            
            // Try stealing
            if (Task* stolen = TrySteal())
            {
                ExecuteTask(stolen);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    }
    
    bool WorkStealingScheduler::IsCompleted(Task* task)
    {
        return task->unfinished_jobs.load(std::memory_order_acquire) == -1;
    }
    
    void WorkStealingScheduler::ResetFrame()
    {
        _taskPool.Reset();
    }
    
    void WorkStealingScheduler::WorkerMain(uint32 workerId)
    {
        _tlsWorkerId = workerId;
        
        LOG_DEBUG("server.worldserver", "Worker thread {} started", workerId);
        
        while (!_workers[workerId].shouldExit.load(std::memory_order_acquire))
        {
            // Try local work
            if (auto task = _workers[workerId].deque.Pop())
            {
                ExecuteTask(*task);
                continue;
            }
            
            // Try stealing
            if (Task* stolen = TrySteal())
            {
                ExecuteTask(stolen);
                continue;
            }
            
            // No work available
            std::this_thread::yield();
        }
        
        LOG_DEBUG("server.worldserver", "Worker thread {} exiting", workerId);
    }
    
    WorkStealingScheduler::Task* WorkStealingScheduler::TrySteal()
    {
        uint32 numWorkers = static_cast<uint32>(_workers.size());
        if (numWorkers == 0) return nullptr;
        
        // Random victim selection (Blumofe-Leiserson algorithm)
        uint32 myId = _tlsWorkerId;
        if (myId >= numWorkers) myId = 0;
        
        uint32 victimId = _workers[myId].rng() % numWorkers;
        
        return _workers[victimId].deque.Steal().value_or(nullptr);
    }
    
    void WorkStealingScheduler::ExecuteTask(Task* task)
    {
        task->function(task->data);
        FinishTask(task);
    }
    
    void WorkStealingScheduler::FinishTask(Task* task)
    {
        int32 remaining = task->unfinished_jobs.fetch_sub(1, std::memory_order_acq_rel);
        
        if (remaining == 1)
        {
            // All children completed
            if (task->parent)
            {
                FinishTask(task->parent);
            }
            
            // Mark completed
            task->unfinished_jobs.store(-1, std::memory_order_release);
        }
    }
}
