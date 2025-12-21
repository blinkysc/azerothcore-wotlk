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

#include "SessionUpdater.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Metric.h"
#include "WorldSession.h"

class SessionUpdater::SessionUpdateRequest
{
public:
    SessionUpdateRequest(WorldSession* session, SessionUpdater& updater, uint32 diff)
        : _session(session), _updater(updater), _diff(diff)
    {
    }

    void Call()
    {
        WorldSessionFilter filter(_session);
        bool keepSession = _session->Update(_diff, filter);
        _updater.OnUpdateFinished(_session, keepSession);
    }

    WorldSession* GetSession() const { return _session; }

private:
    WorldSession* _session;
    SessionUpdater& _updater;
    uint32 _diff;
};

SessionUpdater::SessionUpdater() = default;

void SessionUpdater::Activate(std::size_t numThreads)
{
    if (numThreads == 0)
        return;

    LOG_INFO("server.loading", ">> Activating SessionUpdater with {} worker threads", numThreads);

    _workerThreads.reserve(numThreads);
    for (std::size_t i = 0; i < numThreads; ++i)
    {
        _workerThreads.emplace_back(&SessionUpdater::WorkerThread, this);
    }
}

void SessionUpdater::Deactivate()
{
    if (_workerThreads.empty())
        return;

    _cancelationToken = true;

    Wait();

    _queue.Cancel();

    for (auto& thread : _workerThreads)
    {
        if (thread.joinable())
            thread.join();
    }

    _workerThreads.clear();
}

bool SessionUpdater::IsActivated() const
{
    return !_workerThreads.empty();
}

void SessionUpdater::ScheduleUpdate(WorldSession* session, uint32 diff)
{
    _pendingRequests.fetch_add(1, std::memory_order_release);
    _queue.Push(new SessionUpdateRequest(session, *this, diff));
}

void SessionUpdater::Wait()
{
    std::unique_lock<std::mutex> guard(_lock);
    _condition.wait(guard, [this] {
        return _pendingRequests.load(std::memory_order_acquire) == 0;
    });
}

void SessionUpdater::OnUpdateFinished(WorldSession* session, bool keepSession)
{
    // Store result for main thread to process
    {
        std::lock_guard<std::mutex> lock(_resultsLock);
        _results.push_back({session, keepSession});
    }

    // Decrement pending and notify if done
    if (_pendingRequests.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _condition.notify_all();
    }
}

std::vector<SessionUpdater::SessionUpdateResult> SessionUpdater::GetResults()
{
    std::lock_guard<std::mutex> lock(_resultsLock);
    return std::move(_results);
}

void SessionUpdater::ClearResults()
{
    std::lock_guard<std::mutex> lock(_resultsLock);
    _results.clear();
}

void SessionUpdater::WorkerThread()
{
    // Warn about sync database queries in worker threads
    LoginDatabase.WarnAboutSyncQueries(true);
    CharacterDatabase.WarnAboutSyncQueries(true);
    WorldDatabase.WarnAboutSyncQueries(true);

    while (!_cancelationToken)
    {
        SessionUpdateRequest* request = nullptr;

        _queue.WaitAndPop(request);

        if (!_cancelationToken && request)
        {
            METRIC_DETAILED_TIMER("session_update_worker_time",
                METRIC_TAG("account_id", std::to_string(request->GetSession()->GetAccountId())));

            request->Call();
            delete request;
        }
    }
}
