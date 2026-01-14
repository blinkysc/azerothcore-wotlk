/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU AGPL v3 license
 */

#include <gtest/gtest.h>
#include "GhostActorSystem.h"
#include <thread>
#include <vector>
#include <atomic>

using namespace GhostActor;

class PerformanceStatsTest : public ::testing::Test
{
protected:
    PerformanceStats stats;
};

// Test RecordMessage increments correctly
TEST_F(PerformanceStatsTest, RecordMessageIncrements)
{
    EXPECT_EQ(stats.totalMessagesThisTick.load(), 0u);

    stats.RecordMessage(MessageType::SPELL_HIT);

    size_t idx = static_cast<size_t>(MessageType::SPELL_HIT);
    EXPECT_EQ(stats.messageCountsByType[idx].load(), 1u);
    EXPECT_EQ(stats.totalMessagesThisTick.load(), 1u);
}

// Test multiple message types tracked independently
TEST_F(PerformanceStatsTest, RecordMultipleMessageTypes)
{
    stats.RecordMessage(MessageType::SPELL_HIT);
    stats.RecordMessage(MessageType::SPELL_HIT);
    stats.RecordMessage(MessageType::HEAL);
    stats.RecordMessage(MessageType::MELEE_DAMAGE);

    size_t spellIdx = static_cast<size_t>(MessageType::SPELL_HIT);
    size_t healIdx = static_cast<size_t>(MessageType::HEAL);
    size_t meleeIdx = static_cast<size_t>(MessageType::MELEE_DAMAGE);

    EXPECT_EQ(stats.messageCountsByType[spellIdx].load(), 2u);
    EXPECT_EQ(stats.messageCountsByType[healIdx].load(), 1u);
    EXPECT_EQ(stats.messageCountsByType[meleeIdx].load(), 1u);
    EXPECT_EQ(stats.totalMessagesThisTick.load(), 4u);
}

// Test RecordUpdateTime tracking
TEST_F(PerformanceStatsTest, RecordUpdateTimeTracking)
{
    stats.RecordUpdateTime(100);
    EXPECT_EQ(stats.lastUpdateUs.load(), 100u);

    stats.RecordUpdateTime(200);
    EXPECT_EQ(stats.lastUpdateUs.load(), 200u);
    EXPECT_EQ(stats.maxUpdateUs, 200u);
}

// Test max update time tracking
TEST_F(PerformanceStatsTest, MaxUpdateTimeTracked)
{
    stats.RecordUpdateTime(100);
    stats.RecordUpdateTime(500);
    stats.RecordUpdateTime(200);

    EXPECT_EQ(stats.maxUpdateUs, 500u);
    EXPECT_EQ(stats.lastUpdateUs.load(), 200u);
}

// Test rolling window average calculation
TEST_F(PerformanceStatsTest, RollingWindowAverage)
{
    // Fill the rolling window (100 ticks) with constant value
    for (uint32_t i = 0; i < 100; ++i)
    {
        stats.RecordUpdateTime(100);  // 100us each
    }

    // After 100 ticks, average should be calculated
    EXPECT_EQ(stats.avgUpdateUs, 100u);
    EXPECT_EQ(stats.ticksTracked, 0u);  // Reset after window completes
}

// Test rolling window with varying values
TEST_F(PerformanceStatsTest, RollingWindowVaryingValues)
{
    // Fill with alternating values: 50 and 150 (average = 100)
    for (uint32_t i = 0; i < 100; ++i)
    {
        stats.RecordUpdateTime(i % 2 == 0 ? 50 : 150);
    }

    EXPECT_EQ(stats.avgUpdateUs, 100u);
}

// Test ResetTickCounters
TEST_F(PerformanceStatsTest, ResetTickCounters)
{
    stats.RecordMessage(MessageType::SPELL_HIT);
    stats.RecordMessage(MessageType::HEAL);
    stats.tasksStolen.store(5, std::memory_order_relaxed);

    EXPECT_EQ(stats.totalMessagesThisTick.load(), 2u);
    EXPECT_EQ(stats.tasksStolen.load(), 5u);

    stats.ResetTickCounters();

    EXPECT_EQ(stats.totalMessagesThisTick.load(), 0u);
    EXPECT_EQ(stats.tasksStolen.load(), 0u);

    // messageCountsByType should NOT be reset (cumulative)
    size_t spellIdx = static_cast<size_t>(MessageType::SPELL_HIT);
    EXPECT_EQ(stats.messageCountsByType[spellIdx].load(), 1u);
}

// Test thread safety of RecordMessage with concurrent producers
TEST_F(PerformanceStatsTest, ConcurrentRecordMessage)
{
    constexpr size_t NUM_THREADS = 8;
    constexpr size_t MESSAGES_PER_THREAD = 10000;

    std::vector<std::thread> threads;
    std::atomic<int> producersReady{0};
    std::atomic<bool> startSignal{false};

    for (size_t t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&, t]() {
            producersReady.fetch_add(1, std::memory_order_release);

            while (!startSignal.load(std::memory_order_acquire))
                std::this_thread::yield();

            // Each thread uses a different message type
            MessageType type = static_cast<MessageType>(t % 4);
            for (size_t i = 0; i < MESSAGES_PER_THREAD; ++i)
            {
                stats.RecordMessage(type);
            }
        });
    }

    // Wait for all threads to be ready
    while (producersReady.load(std::memory_order_acquire) < static_cast<int>(NUM_THREADS))
        std::this_thread::yield();

    // Start all threads simultaneously
    startSignal.store(true, std::memory_order_release);

    for (auto& t : threads)
        t.join();

    // Total should be NUM_THREADS * MESSAGES_PER_THREAD
    EXPECT_EQ(stats.totalMessagesThisTick.load(), NUM_THREADS * MESSAGES_PER_THREAD);
}

// Test thread safety of RecordUpdateTime
TEST_F(PerformanceStatsTest, ConcurrentRecordUpdateTime)
{
    constexpr size_t NUM_THREADS = 4;
    constexpr size_t UPDATES_PER_THREAD = 1000;

    std::vector<std::thread> threads;
    std::atomic<bool> start{false};

    for (size_t t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&, t]() {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t i = 0; i < UPDATES_PER_THREAD; ++i)
            {
                stats.RecordUpdateTime(100 + t * 10 + (i % 50));
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& t : threads)
        t.join();

    // lastUpdateUs should be set to something reasonable
    EXPECT_GT(stats.lastUpdateUs.load(), 0u);
    EXPECT_LT(stats.lastUpdateUs.load(), 500u);
}

// Test all message types can be recorded without overflow
TEST_F(PerformanceStatsTest, AllMessageTypesRecordable)
{
    // Record each message type once
    for (size_t i = 0; i < PerformanceStats::MAX_MESSAGE_TYPES; ++i)
    {
        stats.RecordMessage(static_cast<MessageType>(i));
    }

    // Verify each type was recorded
    for (size_t i = 0; i < PerformanceStats::MAX_MESSAGE_TYPES; ++i)
    {
        EXPECT_EQ(stats.messageCountsByType[i].load(), 1u)
            << "Message type " << i << " not recorded correctly";
    }

    EXPECT_EQ(stats.totalMessagesThisTick.load(), PerformanceStats::MAX_MESSAGE_TYPES);
}

// Test message counts persist across ResetTickCounters
TEST_F(PerformanceStatsTest, MessageCountsPersistAfterReset)
{
    stats.RecordMessage(MessageType::SPELL_HIT);
    stats.RecordMessage(MessageType::SPELL_HIT);
    stats.RecordMessage(MessageType::HEAL);

    stats.ResetTickCounters();

    // Record more messages
    stats.RecordMessage(MessageType::SPELL_HIT);
    stats.RecordMessage(MessageType::MELEE_DAMAGE);

    // Per-type counts should be cumulative
    size_t spellIdx = static_cast<size_t>(MessageType::SPELL_HIT);
    size_t healIdx = static_cast<size_t>(MessageType::HEAL);
    size_t meleeIdx = static_cast<size_t>(MessageType::MELEE_DAMAGE);

    EXPECT_EQ(stats.messageCountsByType[spellIdx].load(), 3u);  // 2 + 1
    EXPECT_EQ(stats.messageCountsByType[healIdx].load(), 1u);
    EXPECT_EQ(stats.messageCountsByType[meleeIdx].load(), 1u);

    // Per-tick counter should only have post-reset messages
    EXPECT_EQ(stats.totalMessagesThisTick.load(), 2u);
}

// Test update time stats don't interfere with message stats
TEST_F(PerformanceStatsTest, UpdateTimeAndMessageStatsIndependent)
{
    stats.RecordMessage(MessageType::SPELL_HIT);
    stats.RecordUpdateTime(500);
    stats.RecordMessage(MessageType::HEAL);
    stats.RecordUpdateTime(1000);

    size_t spellIdx = static_cast<size_t>(MessageType::SPELL_HIT);
    size_t healIdx = static_cast<size_t>(MessageType::HEAL);

    EXPECT_EQ(stats.messageCountsByType[spellIdx].load(), 1u);
    EXPECT_EQ(stats.messageCountsByType[healIdx].load(), 1u);
    EXPECT_EQ(stats.totalMessagesThisTick.load(), 2u);
    EXPECT_EQ(stats.lastUpdateUs.load(), 1000u);
    EXPECT_EQ(stats.maxUpdateUs, 1000u);
}

// Test tasks stolen counter
TEST_F(PerformanceStatsTest, TasksStolenCounter)
{
    EXPECT_EQ(stats.tasksStolen.load(), 0u);

    stats.tasksStolen.fetch_add(1, std::memory_order_relaxed);
    stats.tasksStolen.fetch_add(1, std::memory_order_relaxed);
    stats.tasksStolen.fetch_add(1, std::memory_order_relaxed);

    EXPECT_EQ(stats.tasksStolen.load(), 3u);

    stats.ResetTickCounters();
    EXPECT_EQ(stats.tasksStolen.load(), 0u);
}
