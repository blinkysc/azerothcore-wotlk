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

#include "LOSCache.h"

bool LOSCache::TryGetCachedLOS(ObjectGuid source, ObjectGuid target,
                                float srcX, float srcY, float srcZ,
                                float dstX, float dstY, float dstZ,
                                uint8 checks, uint8 ignoreFlags,
                                uint32 currentTime, bool& result)
{
    LOSCacheKey key{source, target, checks, ignoreFlags};
    auto it = _cache.find(key);

    if (it == _cache.end())
    {
        ++_cacheMisses;
        return false;
    }

    LOSCacheEntry const& entry = it->second;

    // Check if entry has expired
    if (currentTime - entry.timestamp > LOS_CACHE_TTL_MS)
    {
        _cache.erase(it);
        ++_cacheMisses;
        return false;
    }

    // Check if source has moved too much
    float srcDx = srcX - entry.srcX;
    float srcDy = srcY - entry.srcY;
    float srcDz = srcZ - entry.srcZ;
    float srcDistSq = srcDx * srcDx + srcDy * srcDy + srcDz * srcDz;

    if (srcDistSq > LOS_CACHE_MOVE_TOLERANCE_SQ)
    {
        _cache.erase(it);
        ++_cacheMisses;
        return false;
    }

    // Check if target has moved too much
    float dstDx = dstX - entry.dstX;
    float dstDy = dstY - entry.dstY;
    float dstDz = dstZ - entry.dstZ;
    float dstDistSq = dstDx * dstDx + dstDy * dstDy + dstDz * dstDz;

    if (dstDistSq > LOS_CACHE_MOVE_TOLERANCE_SQ)
    {
        _cache.erase(it);
        ++_cacheMisses;
        return false;
    }

    // Cache hit - return cached result
    result = entry.result;
    ++_cacheHits;
    return true;
}

void LOSCache::CacheLOSResult(ObjectGuid source, ObjectGuid target,
                               float srcX, float srcY, float srcZ,
                               float dstX, float dstY, float dstZ,
                               uint8 checks, uint8 ignoreFlags,
                               uint32 currentTime, bool result)
{
    // Cleanup if cache is getting too large
    if (_cache.size() >= LOS_CACHE_MAX_SIZE)
    {
        CleanupOldEntries(currentTime);
    }

    LOSCacheKey key{source, target, checks, ignoreFlags};
    LOSCacheEntry entry;
    entry.result = result;
    entry.timestamp = currentTime;
    entry.srcX = srcX;
    entry.srcY = srcY;
    entry.srcZ = srcZ;
    entry.dstX = dstX;
    entry.dstY = dstY;
    entry.dstZ = dstZ;

    _cache[key] = entry;
}

void LOSCache::CleanupOldEntries(uint32 currentTime)
{
    // Remove entries older than TTL
    for (auto it = _cache.begin(); it != _cache.end(); )
    {
        if (currentTime - it->second.timestamp > LOS_CACHE_TTL_MS)
        {
            it = _cache.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // If still too large, clear half the cache (LRU would be better but more complex)
    if (_cache.size() >= LOS_CACHE_MAX_SIZE)
    {
        size_t toRemove = _cache.size() / 2;
        auto it = _cache.begin();
        while (toRemove > 0 && it != _cache.end())
        {
            it = _cache.erase(it);
            --toRemove;
        }
    }
}
