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

#include "GhostActorSystem.h"
#include "gtest/gtest.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace GhostActor;

class MPSCQueueTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Basic push/pop functionality in single thread
TEST_F(MPSCQueueTest, PushPopSingleThread)
{
    MPSCQueue<int> queue;

    queue.Push(42);
    queue.Push(123);
    queue.Push(999);

    int result;
    ASSERT_TRUE(queue.Pop(result));
    EXPECT_EQ(result, 42);

    ASSERT_TRUE(queue.Pop(result));
    EXPECT_EQ(result, 123);

    ASSERT_TRUE(queue.Pop(result));
    EXPECT_EQ(result, 999);

    EXPECT_FALSE(queue.Pop(result));
}

// Pop on empty queue returns false
TEST_F(MPSCQueueTest, EmptyQueueReturnsNull)
{
    MPSCQueue<int> queue;

    int result;
    EXPECT_FALSE(queue.Pop(result));
    EXPECT_TRUE(queue.Empty());
}

// Messages are dequeued in FIFO order
TEST_F(MPSCQueueTest, FIFOOrdering)
{
    MPSCQueue<int> queue;

    constexpr int COUNT = 100;
    for (int i = 0; i < COUNT; ++i)
    {
        queue.Push(i);
    }

    int result;
    for (int i = 0; i < COUNT; ++i)
    {
        ASSERT_TRUE(queue.Pop(result));
        EXPECT_EQ(result, i);
    }

    EXPECT_FALSE(queue.Pop(result));
}

// Multiple producer threads, single consumer
// Scale: 16 workers x 10k messages = 160k messages (simulates ~10-15k players)
TEST_F(MPSCQueueTest, MultiProducerSingleConsumer)
{
    MPSCQueue<int> queue;
    // 16 workers typical for high-pop server
    constexpr int NUM_PRODUCERS = 16;
    // ~10k messages per worker per update cycle at peak load
    constexpr int ITEMS_PER_PRODUCER = 10000;
    std::atomic<int> producersReady{0};
    std::atomic<bool> startFlag{false};

    std::vector<std::thread> producers;

    // Start producer threads
    for (int p = 0; p < NUM_PRODUCERS; ++p)
    {
        producers.emplace_back([&queue, &producersReady, &startFlag, p]()
        {
            producersReady++;
            while (!startFlag.load(std::memory_order_acquire)) {}

            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i)
            {
                queue.Push(p * ITEMS_PER_PRODUCER + i);
            }
        });
    }

    // Wait for all producers to be ready
    while (producersReady.load() < NUM_PRODUCERS) {}

    // Start all producers simultaneously
    startFlag.store(true, std::memory_order_release);

    // Wait for producers to finish
    for (auto& t : producers)
    {
        t.join();
    }

    // Count consumed items
    int consumed = 0;
    int result;
    while (queue.Pop(result))
    {
        consumed++;
    }

    EXPECT_EQ(consumed, NUM_PRODUCERS * ITEMS_PER_PRODUCER);
}

// High contention stress test with concurrent produce/consume
// Scale: 16 workers producing while consumer drains = realistic server load
// Total: 320k messages (simulates sustained high-pop combat scenarios)
TEST_F(MPSCQueueTest, HighContention)
{
    MPSCQueue<int> queue;
    // 16 workers + concurrent consumption
    constexpr int NUM_PRODUCERS = 16;
    // 20k messages per worker = 320k total (heavy combat scenario)
    constexpr int ITEMS_PER_PRODUCER = 20000;
    std::atomic<int> producersReady{0};
    std::atomic<bool> startFlag{false};
    std::atomic<int> consumed{0};
    std::atomic<bool> consumerDone{false};

    std::vector<std::thread> producers;

    // Consumer thread (simulates cell actor processing messages)
    std::thread consumer([&queue, &consumed, &consumerDone, &startFlag]()
    {
        while (!startFlag.load(std::memory_order_acquire)) {}

        int result;
        int expectedTotal = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

        // Keep consuming until we have all items
        while (consumed.load() < expectedTotal)
        {
            if (queue.Pop(result))
            {
                consumed++;
            }
            else
            {
                std::this_thread::yield();
            }
        }
        consumerDone = true;
    });

    // Start producer threads
    for (int p = 0; p < NUM_PRODUCERS; ++p)
    {
        producers.emplace_back([&queue, &producersReady, &startFlag]()
        {
            producersReady++;
            while (!startFlag.load(std::memory_order_acquire)) {}

            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i)
            {
                queue.Push(i);
            }
        });
    }

    // Wait for all threads to be ready
    while (producersReady.load() < NUM_PRODUCERS) {}

    // Start all threads simultaneously
    startFlag.store(true, std::memory_order_release);

    // Wait for producers to finish
    for (auto& t : producers)
    {
        t.join();
    }

    // Wait for consumer to finish
    consumer.join();

    EXPECT_EQ(consumed.load(), NUM_PRODUCERS * ITEMS_PER_PRODUCER);
    EXPECT_TRUE(queue.Empty());
}

// Test with complex types (ActorMessage)
TEST_F(MPSCQueueTest, ComplexType)
{
    MPSCQueue<ActorMessage> queue;

    ActorMessage msg1;
    msg1.type = MessageType::SPELL_HIT;
    msg1.sourceGuid = 12345;
    msg1.targetGuid = 67890;
    msg1.intParam1 = 100;

    ActorMessage msg2;
    msg2.type = MessageType::HEALTH_CHANGED;
    msg2.sourceGuid = 11111;
    msg2.targetGuid = 22222;
    msg2.intParam1 = 500;

    queue.Push(std::move(msg1));
    queue.Push(std::move(msg2));

    ActorMessage result;
    ASSERT_TRUE(queue.Pop(result));
    EXPECT_EQ(result.type, MessageType::SPELL_HIT);
    EXPECT_EQ(result.sourceGuid, 12345u);
    EXPECT_EQ(result.targetGuid, 67890u);
    EXPECT_EQ(result.intParam1, 100);

    ASSERT_TRUE(queue.Pop(result));
    EXPECT_EQ(result.type, MessageType::HEALTH_CHANGED);
    EXPECT_EQ(result.sourceGuid, 11111u);
    EXPECT_EQ(result.targetGuid, 22222u);
    EXPECT_EQ(result.intParam1, 500);

    EXPECT_FALSE(queue.Pop(result));
}

// Test Empty() method
TEST_F(MPSCQueueTest, EmptyCheck)
{
    MPSCQueue<int> queue;

    EXPECT_TRUE(queue.Empty());

    queue.Push(1);
    EXPECT_FALSE(queue.Empty());

    int result;
    queue.Pop(result);
    EXPECT_TRUE(queue.Empty());
}
