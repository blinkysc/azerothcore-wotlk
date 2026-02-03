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

#ifndef __CONNECTIONFLOODPROTECTION_H__
#define __CONNECTIONFLOODPROTECTION_H__

#include "Define.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <boost/asio/ip/address.hpp>

class AC_SHARED_API ConnectionFloodProtection
{
public:
    static ConnectionFloodProtection& Instance();

    // Returns true if connection should be REJECTED
    bool ShouldRejectConnection(boost::asio::ip::address const& ip);

    // Call when socket closes
    void OnSocketClosed(boost::asio::ip::address const& ip);

    // Set limits (called during server startup from config)
    void SetLimits(bool enabled, uint32 maxPerIp, uint32 rateLimit, uint32 rateLimitWindowSecs);

    // Remove stale entries (IPs with no active connections and expired rate window)
    void CleanupStaleEntries();

private:
    ConnectionFloodProtection() = default;
    ~ConnectionFloodProtection() = default;

    ConnectionFloodProtection(ConnectionFloodProtection const&) = delete;
    ConnectionFloodProtection& operator=(ConnectionFloodProtection const&) = delete;

    struct IpConnectionInfo
    {
        std::atomic<uint32> activeConnections{0};
        std::atomic<uint32> recentConnectionCount{0};
        std::chrono::steady_clock::time_point windowStart;
        std::chrono::steady_clock::time_point lastActivity;
        std::mutex windowMutex;
    };

    struct IpAddressHasher
    {
        std::size_t operator()(boost::asio::ip::address const& addr) const
        {
            if (addr.is_v4())
                return std::hash<uint32_t>{}(addr.to_v4().to_uint());
            else
            {
                auto bytes = addr.to_v6().to_bytes();
                std::size_t seed = 0;
                for (auto b : bytes)
                    seed ^= std::hash<uint8_t>{}(b) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                return seed;
            }
        }
    };

    std::unordered_map<boost::asio::ip::address, std::unique_ptr<IpConnectionInfo>, IpAddressHasher> _ipTracker;
    mutable std::shared_mutex _trackerMutex;

    std::atomic<bool> _enabled{false};
    std::atomic<uint32> _maxConnectionsPerIp{5};
    std::atomic<uint32> _rateLimit{20};
    std::atomic<uint32> _rateLimitWindowSecs{60};
};

#define sConnectionFloodProtection ConnectionFloodProtection::Instance()

#endif // __CONNECTIONFLOODPROTECTION_H__
