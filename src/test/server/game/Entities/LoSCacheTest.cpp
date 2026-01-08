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

#include "gtest/gtest.h"
#include <unordered_map>
#include <cstdint>

/**
 * @brief Standalone LoS cache logic tests
 *
 * These tests verify the cache data structure behavior used by
 * WorldObject::IsWithinLOSInMap. Since WorldObject requires extensive
 * mocking (Map, GameTime, visibility), we test the cache logic in isolation.
 */

// Mirror of WorldObject::LoSCacheEntry
struct LoSCacheEntry
{
    bool hasLoS;
    uint32_t expireTimeMs;
};

// Simulated cache operations
class LoSCacheSimulator
{
public:
    // Check cache, return true if cache hit (with result in hasLoS)
    bool CheckCache(uint32_t targetKey, uint32_t currentTimeMs, bool& hasLoS)
    {
        auto it = _cache.find(targetKey);
        if (it != _cache.end() && it->second.expireTimeMs > currentTimeMs)
        {
            hasLoS = it->second.hasLoS;
            return true; // Cache hit
        }
        return false; // Cache miss
    }

    // Store result in cache
    void StoreResult(uint32_t targetKey, bool hasLoS, uint32_t currentTimeMs, uint32_t ttlMs = 500)
    {
        _cache[targetKey] = { hasLoS, currentTimeMs + ttlMs };
    }

    // Invalidate cache (called on position change)
    void Invalidate()
    {
        _cache.clear();
    }

    size_t Size() const { return _cache.size(); }

private:
    std::unordered_map<uint32_t, LoSCacheEntry> _cache;
};

class LoSCacheTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Second call within TTL returns cached result
TEST_F(LoSCacheTest, CacheHit)
{
    LoSCacheSimulator cache;
    uint32_t targetA = 1001;
    uint32_t now = 10000;
    bool hasLoS;

    // First call - cache miss
    EXPECT_FALSE(cache.CheckCache(targetA, now, hasLoS));

    // Store result
    cache.StoreResult(targetA, true, now);

    // Second call - cache hit
    EXPECT_TRUE(cache.CheckCache(targetA, now + 100, hasLoS));
    EXPECT_TRUE(hasLoS);
}

// Call after TTL expires returns cache miss
TEST_F(LoSCacheTest, CacheExpiry)
{
    LoSCacheSimulator cache;
    uint32_t targetA = 1001;
    uint32_t now = 10000;
    bool hasLoS;

    // Store result with 500ms TTL
    cache.StoreResult(targetA, true, now, 500);

    // Check at 400ms - should hit
    EXPECT_TRUE(cache.CheckCache(targetA, now + 400, hasLoS));
    EXPECT_TRUE(hasLoS);

    // Check at 600ms - should miss (expired)
    EXPECT_FALSE(cache.CheckCache(targetA, now + 600, hasLoS));
}

// Invalidate clears cache
TEST_F(LoSCacheTest, InvalidateOnMove)
{
    LoSCacheSimulator cache;
    uint32_t now = 10000;
    bool hasLoS;

    cache.StoreResult(1001, true, now);
    cache.StoreResult(1002, false, now);
    cache.StoreResult(1003, true, now);

    EXPECT_EQ(cache.Size(), 3u);

    // Invalidate (simulates position change)
    cache.Invalidate();

    EXPECT_EQ(cache.Size(), 0u);

    // All should be cache misses now
    EXPECT_FALSE(cache.CheckCache(1001, now, hasLoS));
    EXPECT_FALSE(cache.CheckCache(1002, now, hasLoS));
    EXPECT_FALSE(cache.CheckCache(1003, now, hasLoS));
}

// Cache is keyed by target GUID
TEST_F(LoSCacheTest, DifferentTargets)
{
    LoSCacheSimulator cache;
    uint32_t now = 10000;
    bool hasLoS;

    // Store different results for different targets
    cache.StoreResult(1001, true, now);
    cache.StoreResult(1002, false, now);

    // Check target A
    EXPECT_TRUE(cache.CheckCache(1001, now + 100, hasLoS));
    EXPECT_TRUE(hasLoS);

    // Check target B
    EXPECT_TRUE(cache.CheckCache(1002, now + 100, hasLoS));
    EXPECT_FALSE(hasLoS);

    // Target C not in cache
    EXPECT_FALSE(cache.CheckCache(1003, now + 100, hasLoS));
}

// Test cache with many entries
TEST_F(LoSCacheTest, ManyEntries)
{
    LoSCacheSimulator cache;
    uint32_t now = 10000;

    constexpr int NUM_TARGETS = 100;

    // Store results for many targets
    for (uint32_t i = 0; i < NUM_TARGETS; ++i)
    {
        cache.StoreResult(i, (i % 2) == 0, now);
    }

    EXPECT_EQ(cache.Size(), static_cast<size_t>(NUM_TARGETS));

    // Verify all entries
    bool hasLoS;
    for (uint32_t i = 0; i < NUM_TARGETS; ++i)
    {
        EXPECT_TRUE(cache.CheckCache(i, now + 100, hasLoS));
        EXPECT_EQ(hasLoS, (i % 2) == 0);
    }
}

// Test cache update overwrites old entry
TEST_F(LoSCacheTest, UpdateOverwrite)
{
    LoSCacheSimulator cache;
    uint32_t targetA = 1001;
    uint32_t now = 10000;
    bool hasLoS;

    // Initial store
    cache.StoreResult(targetA, true, now);
    EXPECT_TRUE(cache.CheckCache(targetA, now + 100, hasLoS));
    EXPECT_TRUE(hasLoS);

    // Update with different value
    cache.StoreResult(targetA, false, now + 200);
    EXPECT_TRUE(cache.CheckCache(targetA, now + 300, hasLoS));
    EXPECT_FALSE(hasLoS);

    // Also has new expiry time
    EXPECT_TRUE(cache.CheckCache(targetA, now + 600, hasLoS));  // Within new TTL
    EXPECT_FALSE(cache.CheckCache(targetA, now + 800, hasLoS)); // Expired
}

// Test edge case: expire time exactly at current time
TEST_F(LoSCacheTest, ExpireTimeEdgeCase)
{
    LoSCacheSimulator cache;
    uint32_t now = 10000;
    bool hasLoS;

    cache.StoreResult(1001, true, now, 500);

    // Check exactly at expiry time - should miss (not > current)
    EXPECT_FALSE(cache.CheckCache(1001, now + 500, hasLoS));

    // Check 1ms before - should hit
    EXPECT_TRUE(cache.CheckCache(1001, now + 499, hasLoS));
}
