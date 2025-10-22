/*
 * This file is part of the AzerothCore Project.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your option),
 * any later version.
 */

#ifndef ACORE_WORK_STEALING_DEQUE_H
#define ACORE_WORK_STEALING_DEQUE_H

#include "Define.h"
#include <atomic>
#include <optional>
#include <vector>

namespace Acore
{
    template<typename T>
    class WorkStealingDeque
    {
    private:
        std::atomic<int64> _top{0};
        std::atomic<int64> _bottom{0};
        std::vector<T> _array;
        static constexpr int64 MASK = 4095;  // 4096-1 (power of 2)
        
    public:
        WorkStealingDeque() : _array(4096) {}
        
        // Owner thread: Push to bottom (LIFO end)
        void Push(T item)
        {
            int64 b = _bottom.load(std::memory_order_relaxed);
            int64 t = _top.load(std::memory_order_acquire);
            
            // Check if resize needed (simplified - production should handle this)
            if (b - t >= static_cast<int64>(_array.size()) - 1)
            {
                // Queue full - in production, resize here
                return;
            }
            
            _array[b & MASK] = item;
            std::atomic_thread_fence(std::memory_order_release);
            _bottom.store(b + 1, std::memory_order_relaxed);
        }
        
        // Owner thread: Pop from bottom (LIFO end)
        std::optional<T> Pop()
        {
            int64 b = _bottom.load(std::memory_order_relaxed) - 1;
            _bottom.store(b, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64 t = _top.load(std::memory_order_relaxed);
            
            if (t <= b)
            {
                T item = _array[b & MASK];
                if (t == b)
                {
                    // Last element - race with steal
                    if (!_top.compare_exchange_strong(t, t + 1,
                            std::memory_order_seq_cst,
                            std::memory_order_relaxed))
                    {
                        _bottom.store(b + 1, std::memory_order_relaxed);
                        return std::nullopt;
                    }
                    _bottom.store(b + 1, std::memory_order_relaxed);
                }
                return item;
            }
            else
            {
                _bottom.store(t, std::memory_order_relaxed);
                return std::nullopt;
            }
        }
        
        // Any thief: Steal from top (FIFO end)
        std::optional<T> Steal()
        {
            int64 t = _top.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64 b = _bottom.load(std::memory_order_acquire);
            
            if (t < b)
            {
                T item = _array[t & MASK];
                if (!_top.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst,
                        std::memory_order_relaxed))
                {
                    return std::nullopt;
                }
                return item;
            }
            return std::nullopt;
        }
    };
}

#endif // ACORE_WORK_STEALING_DEQUE_H
