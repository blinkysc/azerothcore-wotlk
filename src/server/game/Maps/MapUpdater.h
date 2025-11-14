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

#ifndef _MAP_UPDATER_H_INCLUDED
#define _MAP_UPDATER_H_INCLUDED

#include "Define.h"
#include "WorkStealingThreadPool.h"
#include <atomic>
#include <functional>
#include <memory>

class Map;

/**
 * @brief Map update scheduler using work-stealing thread pool
 *
 * This class manages parallel map updates using a work-stealing thread pool
 * for improved load balancing and performance.
 */
class MapUpdater
{
public:
    MapUpdater();
    ~MapUpdater();

    /**
     * @brief Schedule a map update task
     * @param map The map to update
     * @param diff Time difference for full update
     * @param s_diff Time difference for session update
     */
    void schedule_update(Map& map, uint32 diff, uint32 s_diff);

    /**
     * @brief Schedule a map preload task
     * @param mapid Map ID to preload
     */
    void schedule_map_preload(uint32 mapid);

    /**
     * @brief Schedule LFG update task
     * @param diff Time difference
     */
    void schedule_lfg_update(uint32 diff);

    /**
     * @brief Wait for all pending tasks to complete
     */
    void wait();

    /**
     * @brief Activate the thread pool
     * @param num_threads Number of worker threads
     */
    void activate(std::size_t num_threads);

    /**
     * @brief Deactivate the thread pool
     */
    void deactivate();

    /**
     * @brief Check if the thread pool is active
     * @return true if activated
     */
    [[nodiscard]] bool activated() const;

    /**
     * @brief Submit a generic task to the thread pool
     * @param task Function to execute
     */
    void submit_task(std::function<void()> task);

    /**
     * @brief Get the work-stealing thread pool instance
     * @return Pointer to thread pool (may be null if not activated)
     */
    [[nodiscard]] WorkStealingThreadPool* GetThreadPool() { return _threadPool.get(); }

    /**
     * @brief Get the number of worker threads
     * @return Thread count (0 if not activated)
     */
    [[nodiscard]] uint32 GetThreadCount() const
    {
        return _threadPool ? static_cast<uint32>(_threadPool->GetThreadCount()) : 0;
    }

private:
    std::unique_ptr<WorkStealingThreadPool> _threadPool;  ///< Work-stealing thread pool
};

#endif //_MAP_UPDATER_H_INCLUDED
