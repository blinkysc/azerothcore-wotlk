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

#include "ConnectionFloodProtection.h"

ConnectionFloodProtection& ConnectionFloodProtection::Instance()
{
    static ConnectionFloodProtection instance;
    return instance;
}

void ConnectionFloodProtection::SetLimits(bool enabled, uint32 maxPerIp, uint32 rateLimit, uint32 rateLimitWindowSecs)
{
    _enabled.store(enabled);
    _maxConnectionsPerIp.store(maxPerIp);
    _rateLimit.store(rateLimit);
    _rateLimitWindowSecs.store(rateLimitWindowSecs);
}

bool ConnectionFloodProtection::ShouldRejectConnection(boost::asio::ip::address const& ip)
{
    if (!_enabled.load())
        return false;

    uint32 maxPerIp = _maxConnectionsPerIp.load();
    uint32 rateLimit = _rateLimit.load();

    // Both disabled means accept all
    if (maxPerIp == 0 && rateLimit == 0)
        return false;

    IpConnectionInfo* info = nullptr;
    {
        // Try to find existing entry with shared lock first
        std::shared_lock<std::shared_mutex> readLock(_trackerMutex);
        auto it = _ipTracker.find(ip);
        if (it != _ipTracker.end())
            info = it->second.get();
    }

    if (!info)
    {
        // Need to create new entry with exclusive lock
        std::unique_lock<std::shared_mutex> writeLock(_trackerMutex);
        // Double-check after acquiring write lock
        auto it = _ipTracker.find(ip);
        if (it != _ipTracker.end())
        {
            info = it->second.get();
        }
        else
        {
            auto newInfo = std::make_unique<IpConnectionInfo>();
            info = newInfo.get();
            _ipTracker[ip] = std::move(newInfo);
        }
    }

    // Check max connections per IP
    if (maxPerIp > 0)
    {
        uint32 currentConnections = info->activeConnections.load();
        if (currentConnections >= maxPerIp)
            return true;
    }

    // Check rate limit
    if (rateLimit > 0)
    {
        auto now = std::chrono::steady_clock::now();
        uint32 windowSecs = _rateLimitWindowSecs.load();
        std::chrono::seconds window(windowSecs);

        std::lock_guard<std::mutex> windowLock(info->windowMutex);

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - info->windowStart);
        if (elapsed >= window)
        {
            // Reset window
            info->windowStart = now;
            info->recentConnectionCount.store(1);
        }
        else
        {
            uint32 count = info->recentConnectionCount.fetch_add(1) + 1;
            if (count > rateLimit)
                return true;
        }
    }

    // Connection accepted - increment active count
    info->activeConnections.fetch_add(1);
    return false;
}

void ConnectionFloodProtection::OnSocketClosed(boost::asio::ip::address const& ip)
{
    if (!_enabled.load())
        return;

    std::shared_lock<std::shared_mutex> readLock(_trackerMutex);
    auto it = _ipTracker.find(ip);
    if (it != _ipTracker.end())
    {
        uint32 prev = it->second->activeConnections.fetch_sub(1);
        // Prevent underflow
        if (prev == 0)
            it->second->activeConnections.store(0);
    }
}
