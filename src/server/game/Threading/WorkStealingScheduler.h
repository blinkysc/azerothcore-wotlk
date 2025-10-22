#ifndef ACORE_WORK_STEALING_SCHEDULER_H
#define ACORE_WORK_STEALING_SCHEDULER_H

#include "Define.h"
#include "WorkStealingDeque.h"
#include <atomic>
#include <thread>
#include <vector>
#include <random>

namespace Acore
{
    class AC_GAME_API WorkStealingScheduler
    {
    public:
        struct Task
        {
            void (*function)(void*);
            void* data;
            Task* parent;
            std::atomic<int32> unfinished_jobs{1};
            
            // Padding to cache line size (64 bytes)
            alignas(64) char padding[40];
        };
        
        explicit WorkStealingScheduler(uint32 numThreads = 0);
        ~WorkStealingScheduler();
        
        // Spawn a new task
        Task* Spawn(void (*func)(void*), void* data, Task* parent = nullptr);
        
        // Wait for task completion
        void Wait(Task* task);
        
        // Check if task completed
        bool IsCompleted(Task* task);
        
        // Reset frame allocator
        void ResetFrame();
        
        // Shutdown scheduler
        void Shutdown();
        
        uint32 GetNumWorkers() const { return static_cast<uint32>(_workers.size()); }
        
    private:
        struct WorkerThread
        {
            std::thread thread;
            WorkStealingDeque<Task*> deque;
            std::atomic<bool> shouldExit{false};
            uint32 threadId;
            std::mt19937 rng;  // Per-thread RNG for victim selection
            
            alignas(64) char pad[64];  // Prevent false sharing
        };
        
        struct TaskPool
        {
            std::vector<Task> tasks;
            std::atomic<size_t> allocated{0};
            static constexpr size_t POOL_SIZE = 16384;
            
            TaskPool();
            Task* Allocate();
            void Reset();
        };
        
        std::vector<WorkerThread> _workers;
        TaskPool _taskPool;
        std::atomic<bool> _shutdown{false};
        
        thread_local static uint32 _tlsWorkerId;
        
        void WorkerMain(uint32 workerId);
        Task* TrySteal();
        void ExecuteTask(Task* task);
        void FinishTask(Task* task);
    };
}

#endif // ACORE_WORK_STEALING_SCHEDULER_H
