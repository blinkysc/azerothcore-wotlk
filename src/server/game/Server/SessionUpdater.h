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

#ifndef _SESSION_UPDATER_H_INCLUDED
#define _SESSION_UPDATER_H_INCLUDED

#include "Define.h"
#include "PCQueue.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class WorldSession;

/**
 * @brief Parallel session update worker pool
 *
 * This class manages a thread pool for updating WorldSessions in parallel.
 * Sessions are independent units - each processes its own packet queue
 * without needing to access other sessions' data.
 *
 * Thread Safety:
 * - Session::Update() is thread-safe for packet processing
 * - Session removal is deferred and handled by main thread after wait()
 * - New session addition happens before parallel phase
 */
class SessionUpdater
{
public:
    struct SessionUpdateResult
    {
        WorldSession* session;
        bool keepSession;  // false = session should be removed
    };

    SessionUpdater();
    ~SessionUpdater() = default;

    /// Activate the worker threads
    void Activate(std::size_t numThreads);

    /// Deactivate and join all worker threads
    void Deactivate();

    /// Check if the updater is active
    bool IsActivated() const;

    /// Schedule a session update task
    void ScheduleUpdate(WorldSession* session, uint32 diff);

    /// Wait for all pending updates to complete
    void Wait();

    /// Get results after Wait() - sessions that need removal
    std::vector<SessionUpdateResult> GetResults();

    /// Clear results for next update cycle
    void ClearResults();

private:
    class SessionUpdateRequest;

    void WorkerThread();
    void OnUpdateFinished(WorldSession* session, bool keepSession);

    ProducerConsumerQueue<SessionUpdateRequest*> _queue;
    std::atomic<int> _pendingRequests{0};
    std::atomic<bool> _cancelationToken{false};
    std::vector<std::thread> _workerThreads;

    std::mutex _lock;
    std::condition_variable _condition;

    // Results collection (protected by _resultsLock)
    std::mutex _resultsLock;
    std::vector<SessionUpdateResult> _results;
};

#endif // _SESSION_UPDATER_H_INCLUDED
