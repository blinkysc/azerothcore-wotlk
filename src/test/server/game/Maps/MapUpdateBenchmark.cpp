/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "MapUpdater.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

struct BenchmarkResult
{
    std::string testName;
    int objectCount;
    int threadCount;
    double sequentialTimeMs;
    double parallelTimeMs;
    double speedup;
    double throughput; // objects/second
    double efficiency; // speedup / threadCount
};

class MapUpdateBenchmark : public ::testing::Test
{
protected:
    void SetUp() override
    {
        results.clear();
    }

    void TearDown() override
    {
        // Print results table
        PrintResultsTable();

        // Optionally save to CSV
        SaveResultsToCSV("benchmark_results.csv");
    }

    struct WorkItem
    {
        std::atomic<uint32> updateCount{0};
        float x, y, z; // Simulate position data

        void Update(uint32 /*diff*/)
        {
            updateCount.fetch_add(1);

            // Simulate typical entity update work
            volatile float result = 0;
            for (int i = 0; i < 200; ++i) // Adjust iteration count to simulate realistic workload
            {
                result += std::sin(x + i) * std::cos(y + i) + std::sqrt(z + i);
            }

            x += result * 0.0001f; // Prevent optimization
        }
    };

    double RunSequentialBenchmark(std::vector<WorkItem>& objects, uint32 diff)
    {
        auto start = std::chrono::high_resolution_clock::now();

        for (auto& obj : objects)
        {
            obj.Update(diff);
        }

        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    double RunParallelBenchmark(std::vector<WorkItem>& objects, uint32 diff, int threadCount)
    {
        MapUpdater updater;
        updater.activate(threadCount);

        auto start = std::chrono::high_resolution_clock::now();

        // Calculate adaptive batch size based on thread count and object count
        // Goal: Create enough batches to keep all threads busy with good load balancing
        // Heuristic: Aim for 8-16 batches per thread to enable work stealing
        size_t targetBatches = threadCount * 12;  // 12 batches per thread
        size_t batchSize = std::max<size_t>(100, objects.size() / targetBatches);  // Min 100 objects per batch

        // Cap maximum batch size to avoid load imbalance
        batchSize = std::min<size_t>(batchSize, 1000);

        for (size_t i = 0; i < objects.size(); i += batchSize)
        {
            size_t end = std::min(i + batchSize, objects.size());

            // Capture batch range instead of individual objects
            updater.submit_task([&objects, i, end, diff]() {
                for (size_t j = i; j < end; ++j)
                {
                    objects[j].Update(diff);
                }
            });
        }

        updater.wait();

        auto end_time = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end_time - start).count();
    }

    void PrintResultsTable()
    {
        if (results.empty())
            return;

        std::cout << "\n========== BENCHMARK RESULTS ==========\n\n";
        std::cout << std::left << std::setw(30) << "Test Name"
                  << std::right << std::setw(10) << "Objects"
                  << std::setw(10) << "Threads"
                  << std::setw(12) << "Seq(ms)"
                  << std::setw(12) << "Par(ms)"
                  << std::setw(10) << "Speedup"
                  << std::setw(12) << "Efficiency"
                  << std::setw(15) << "Throughput(K/s)"
                  << "\n";

        std::cout << std::string(111, '-') << "\n";

        for (const auto& result : results)
        {
            std::cout << std::left << std::setw(30) << result.testName
                      << std::right << std::setw(10) << result.objectCount
                      << std::setw(10) << result.threadCount
                      << std::setw(12) << std::fixed << std::setprecision(2) << result.sequentialTimeMs
                      << std::setw(12) << result.parallelTimeMs
                      << std::setw(10) << std::setprecision(2) << result.speedup << "x"
                      << std::setw(12) << std::setprecision(1) << (result.efficiency * 100) << "%"
                      << std::setw(15) << std::setprecision(1) << (result.throughput / 1000.0)
                      << "\n";
        }

        std::cout << "=======================================\n\n";
    }

    void SaveResultsToCSV(const std::string& filename)
    {
        std::ofstream file(filename);
        if (!file.is_open())
            return;

        file << "TestName,Objects,Threads,SequentialTimeMs,ParallelTimeMs,Speedup,Efficiency,ThroughputObjPerSec\n";

        for (const auto& result : results)
        {
            file << result.testName << ","
                 << result.objectCount << ","
                 << result.threadCount << ","
                 << result.sequentialTimeMs << ","
                 << result.parallelTimeMs << ","
                 << result.speedup << ","
                 << result.efficiency << ","
                 << result.throughput << "\n";
        }

        file.close();
        std::cout << "Results saved to " << filename << "\n";
    }

    std::vector<BenchmarkResult> results;
};

// ==================== Scaling Benchmarks ====================

TEST_F(MapUpdateBenchmark, ThreadCountScaling)
{
    constexpr int NUM_OBJECTS = 10000;
    constexpr uint32 DIFF = 100;

    std::cout << "\n=== Testing thread count scaling with " << NUM_OBJECTS << " objects ===\n";

    // Baseline sequential
    std::vector<WorkItem> baselineObjects(NUM_OBJECTS);
    double sequentialTime = RunSequentialBenchmark(baselineObjects, DIFF);

    std::cout << "Sequential baseline: " << sequentialTime << " ms\n\n";

    for (int threadCount : {1, 2, 4, 8, 16})
    {
        std::vector<WorkItem> objects(NUM_OBJECTS);
        double parallelTime = RunParallelBenchmark(objects, DIFF, threadCount);

        BenchmarkResult result;
        result.testName = "ThreadScaling";
        result.objectCount = NUM_OBJECTS;
        result.threadCount = threadCount;
        result.sequentialTimeMs = sequentialTime;
        result.parallelTimeMs = parallelTime;
        result.speedup = sequentialTime / parallelTime;
        result.efficiency = result.speedup / threadCount;
        result.throughput = NUM_OBJECTS / (parallelTime / 1000.0);

        results.push_back(result);

        std::cout << "Threads: " << threadCount
                  << ", Time: " << parallelTime << " ms"
                  << ", Speedup: " << result.speedup << "x"
                  << ", Efficiency: " << (result.efficiency * 100) << "%\n";
    }
}

TEST_F(MapUpdateBenchmark, ObjectCountScaling)
{
    constexpr int THREAD_COUNT = 4;

    std::cout << "\n=== Testing object count scaling with " << THREAD_COUNT << " threads ===\n\n";

    for (int objectCount : {100, 500, 1000, 5000, 10000, 50000})
    {
        std::vector<WorkItem> seqObjects(objectCount);
        double sequentialTime = RunSequentialBenchmark(seqObjects, 100);

        std::vector<WorkItem> parObjects(objectCount);
        double parallelTime = RunParallelBenchmark(parObjects, 100, THREAD_COUNT);

        BenchmarkResult result;
        result.testName = "ObjectScaling";
        result.objectCount = objectCount;
        result.threadCount = THREAD_COUNT;
        result.sequentialTimeMs = sequentialTime;
        result.parallelTimeMs = parallelTime;
        result.speedup = sequentialTime / parallelTime;
        result.efficiency = result.speedup / THREAD_COUNT;
        result.throughput = objectCount / (parallelTime / 1000.0);

        results.push_back(result);

        std::cout << "Objects: " << objectCount
                  << ", Sequential: " << sequentialTime << " ms"
                  << ", Parallel: " << parallelTime << " ms"
                  << ", Speedup: " << result.speedup << "x\n";
    }
}

// ==================== Workload Variation Benchmarks ====================

TEST_F(MapUpdateBenchmark, VariableWorkload)
{
    constexpr int NUM_OBJECTS = 5000;
    constexpr int THREAD_COUNT = 4;

    std::cout << "\n=== Testing variable workload distribution ===\n\n";

    struct WorkloadPattern
    {
        std::string name;
        int lightWorkIterations;
        int heavyWorkIterations;
        float heavyWorkRatio; // Percentage of objects with heavy workload
    };

    std::vector<WorkloadPattern> patterns = {
        {"Uniform-Light", 100, 100, 0.0f},
        {"Uniform-Heavy", 500, 500, 0.0f},
        {"Mixed-20Heavy", 100, 500, 0.2f},
        {"Mixed-50Heavy", 100, 500, 0.5f},
        {"Imbalanced-10Heavy", 50, 1000, 0.1f}
    };

    for (const auto& pattern : patterns)
    {
        std::vector<WorkItem> objects(NUM_OBJECTS);

        // Sequential
        auto seqStart = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < objects.size(); ++i)
        {
            int iterations = (i < objects.size() * pattern.heavyWorkRatio)
                                 ? pattern.heavyWorkIterations
                                 : pattern.lightWorkIterations;

            volatile float result = 0;
            for (int j = 0; j < iterations; ++j)
            {
                result += std::sin(j) * std::cos(j);
            }
        }
        auto seqEnd = std::chrono::high_resolution_clock::now();
        double seqTime = std::chrono::duration<double, std::milli>(seqEnd - seqStart).count();

        // Parallel
        MapUpdater updater;
        updater.activate(THREAD_COUNT);

        auto parStart = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < objects.size(); ++i)
        {
            int iterations = (i < objects.size() * pattern.heavyWorkRatio)
                                 ? pattern.heavyWorkIterations
                                 : pattern.lightWorkIterations;

            updater.submit_task([iterations]() {
                volatile float result = 0;
                for (int j = 0; j < iterations; ++j)
                {
                    result += std::sin(j) * std::cos(j);
                }
            });
        }
        updater.wait();
        auto parEnd = std::chrono::high_resolution_clock::now();
        double parTime = std::chrono::duration<double, std::milli>(parEnd - parStart).count();

        BenchmarkResult result;
        result.testName = pattern.name;
        result.objectCount = NUM_OBJECTS;
        result.threadCount = THREAD_COUNT;
        result.sequentialTimeMs = seqTime;
        result.parallelTimeMs = parTime;
        result.speedup = seqTime / parTime;
        result.efficiency = result.speedup / THREAD_COUNT;
        result.throughput = NUM_OBJECTS / (parTime / 1000.0);

        results.push_back(result);

        std::cout << pattern.name << ": Speedup = " << result.speedup << "x, "
                  << "Efficiency = " << (result.efficiency * 100) << "%\n";
    }
}

// ==================== Latency Benchmarks ====================

TEST_F(MapUpdateBenchmark, LatencyMeasurement)
{
    constexpr int NUM_MEASUREMENTS = 1000;
    constexpr int OBJECTS_PER_UPDATE = 1000;
    constexpr int THREAD_COUNT = 4;

    std::cout << "\n=== Measuring update latency ===\n\n";

    std::vector<double> sequentialLatencies;
    std::vector<double> parallelLatencies;

    // Sequential latency
    for (int i = 0; i < NUM_MEASUREMENTS; ++i)
    {
        std::vector<WorkItem> objects(OBJECTS_PER_UPDATE);
        auto start = std::chrono::high_resolution_clock::now();

        for (auto& obj : objects)
        {
            obj.Update(100);
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        sequentialLatencies.push_back(latency);
    }

    // Parallel latency
    MapUpdater updater;
    updater.activate(THREAD_COUNT);

    for (int i = 0; i < NUM_MEASUREMENTS; ++i)
    {
        std::vector<WorkItem> objects(OBJECTS_PER_UPDATE);
        auto start = std::chrono::high_resolution_clock::now();

        for (auto& obj : objects)
        {
            updater.submit_task([&obj]() {
                obj.Update(100);
            });
        }

        updater.wait();

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        parallelLatencies.push_back(latency);
    }

    // Calculate statistics
    auto calcStats = [](const std::vector<double>& data) {
        double sum = 0, min = data[0], max = data[0];
        for (double val : data)
        {
            sum += val;
            if (val < min)
                min = val;
            if (val > max)
                max = val;
        }
        double mean = sum / data.size();

        double variance = 0;
        for (double val : data)
        {
            variance += (val - mean) * (val - mean);
        }
        double stddev = std::sqrt(variance / data.size());

        return std::make_tuple(mean, stddev, min, max);
    };

    auto [seqMean, seqStddev, seqMin, seqMax] = calcStats(sequentialLatencies);
    auto [parMean, parStddev, parMin, parMax] = calcStats(parallelLatencies);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Sequential - Mean: " << seqMean << " ms, StdDev: " << seqStddev
              << " ms, Min: " << seqMin << " ms, Max: " << seqMax << " ms\n";
    std::cout << "Parallel   - Mean: " << parMean << " ms, StdDev: " << parStddev
              << " ms, Min: " << parMin << " ms, Max: " << parMax << " ms\n";
    std::cout << "Latency Improvement: " << ((seqMean - parMean) / seqMean * 100) << "%\n";
}

// ==================== Throughput Benchmarks ====================

TEST_F(MapUpdateBenchmark, SustainedThroughput)
{
    constexpr int DURATION_SECONDS = 10;
    constexpr int OBJECTS_PER_UPDATE = 1000;
    constexpr int THREAD_COUNT = 4;

    std::cout << "\n=== Measuring sustained throughput over " << DURATION_SECONDS << " seconds ===\n\n";

    // Sequential throughput
    {
        auto endTime = std::chrono::steady_clock::now() + std::chrono::seconds(DURATION_SECONDS);
        uint64_t totalObjects = 0;

        while (std::chrono::steady_clock::now() < endTime)
        {
            std::vector<WorkItem> objects(OBJECTS_PER_UPDATE);
            for (auto& obj : objects)
            {
                obj.Update(100);
            }
            totalObjects += OBJECTS_PER_UPDATE;
        }

        double throughput = static_cast<double>(totalObjects) / DURATION_SECONDS;
        std::cout << "Sequential throughput: " << (throughput / 1000.0) << " K objects/sec\n";
    }

    // Parallel throughput
    {
        MapUpdater updater;
        updater.activate(THREAD_COUNT);

        auto endTime = std::chrono::steady_clock::now() + std::chrono::seconds(DURATION_SECONDS);
        uint64_t totalObjects = 0;

        while (std::chrono::steady_clock::now() < endTime)
        {
            std::vector<WorkItem> objects(OBJECTS_PER_UPDATE);
            for (auto& obj : objects)
            {
                updater.submit_task([&obj]() {
                    obj.Update(100);
                });
            }
            updater.wait();
            totalObjects += OBJECTS_PER_UPDATE;
        }

        double throughput = static_cast<double>(totalObjects) / DURATION_SECONDS;
        std::cout << "Parallel throughput: " << (throughput / 1000.0) << " K objects/sec\n";
    }
}
