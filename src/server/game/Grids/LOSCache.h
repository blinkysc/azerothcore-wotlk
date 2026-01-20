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

#ifndef _LOS_CACHE_H
#define _LOS_CACHE_H

#include "Define.h"
#include "ObjectGuid.h"
#include <unordered_map>

// Cache entry for a single LOS check result
struct LOSCacheEntry
{
    bool result;           // LOS check result
    uint32 timestamp;      // GameTime when check was performed
    float srcX, srcY, srcZ; // Source position at time of check
    float dstX, dstY, dstZ; // Target position at time of check
};

// Combined key for source-target pair including check parameters
struct LOSCacheKey
{
    ObjectGuid source;
    ObjectGuid target;
    uint8 checks;       // LineOfSightChecks flags
    uint8 ignoreFlags;  // ModelIgnoreFlags

    bool operator==(LOSCacheKey const& other) const
    {
        return source == other.source && target == other.target &&
               checks == other.checks && ignoreFlags == other.ignoreFlags;
    }
};

// Hash function for LOSCacheKey
struct LOSCacheKeyHash
{
    std::size_t operator()(LOSCacheKey const& key) const
    {
        // Combine all fields into a single hash
        std::size_t h1 = std::hash<uint64>{}(key.source.GetRawValue());
        std::size_t h2 = std::hash<uint64>{}(key.target.GetRawValue());
        std::size_t h3 = std::hash<uint8>{}(key.checks);
        std::size_t h4 = std::hash<uint8>{}(key.ignoreFlags);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};

/**
 * LOSCache - Caches Line of Sight check results to avoid redundant raycasts
 *
 * Cache entries are invalidated when:
 * - Entry is older than LOS_CACHE_TTL_MS milliseconds
 * - Either unit has moved more than LOS_CACHE_MOVE_TOLERANCE yards
 */
class LOSCache
{
public:
    // Cache time-to-live in milliseconds
    static constexpr uint32 LOS_CACHE_TTL_MS = 500;

    // Movement tolerance - invalidate if unit moved more than this (squared for comparison)
    static constexpr float LOS_CACHE_MOVE_TOLERANCE_SQ = 2.0f * 2.0f;  // 2 yards squared

    // Maximum cache size before cleanup
    static constexpr size_t LOS_CACHE_MAX_SIZE = 4096;

    LOSCache() = default;

    // Try to get cached LOS result
    // Returns true if cache hit, false if cache miss
    // If hit, 'result' is set to the cached LOS value
    bool TryGetCachedLOS(ObjectGuid source, ObjectGuid target,
                         float srcX, float srcY, float srcZ,
                         float dstX, float dstY, float dstZ,
                         uint8 checks, uint8 ignoreFlags,
                         uint32 currentTime, bool& result);

    // Store a LOS result in cache
    void CacheLOSResult(ObjectGuid source, ObjectGuid target,
                        float srcX, float srcY, float srcZ,
                        float dstX, float dstY, float dstZ,
                        uint8 checks, uint8 ignoreFlags,
                        uint32 currentTime, bool result);

    // Clear all cache entries
    void Clear() { _cache.clear(); }

    // Get cache statistics
    size_t GetCacheSize() const { return _cache.size(); }
    uint64 GetCacheHits() const { return _cacheHits; }
    uint64 GetCacheMisses() const { return _cacheMisses; }

private:
    // Cleanup old entries when cache gets too large
    void CleanupOldEntries(uint32 currentTime);

    std::unordered_map<LOSCacheKey, LOSCacheEntry, LOSCacheKeyHash> _cache;

    // Statistics
    uint64 _cacheHits = 0;
    uint64 _cacheMisses = 0;
};

#endif // _LOS_CACHE_H
