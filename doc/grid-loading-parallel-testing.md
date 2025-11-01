# Parallel Grid Loading - Testing Guide

## Build and Configuration

### 1. Build with Tests Enabled
```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
```

### 2. Configure worldserver.conf
```conf
# Enable parallel grid loading
Map.ParallelGridLoading = 1

# Set number of worker threads
MapUpdate.Threads = 4  # Or match your CPU cores

# Optional: Adjust batch size
Map.ParallelGridLoadingBatchSize = 4

# Optional: Adjust threshold for parallel mode
Map.ParallelGridLoadingMinGrids = 4
```

## Test Scenarios

### Test 1: Server Startup Time (LoadAllGrids)

**Objective:** Measure grid loading performance during server startup

**Steps:**
1. Enable map preloading in config (if available)
2. Start worldserver with parallel loading enabled
3. Note the timing logs: "Loaded all grids for map X in Y ms"
4. Restart with `Map.ParallelGridLoading = 0`
5. Compare timing logs

**Expected Results:**
- Parallel mode should log "parallel mode, N threads"
- Sequential mode should complete without errors
- Both modes should load the same number of grids

**Log Example:**
```
Loading all grids for map 0 (Eastern Kingdoms) in parallel mode...
Loaded all grids for map 0 (Eastern Kingdoms) in 8432 ms (parallel mode, 4 threads)
```

### Test 2: Teleport Performance (LoadGridsInRange)

**Objective:** Test grid loading during player teleports

**Steps:**
1. Log in with a character
2. Teleport to various locations rapidly:
   ```
   .tele Orgrimmar
   .tele Stormwind
   .tele Dalaran
   .tele Ironforge
   ```
3. Monitor for lag or delays
4. Check logs for errors
5. Repeat with `Map.ParallelGridLoading = 0`

**Expected Results:**
- No crashes or errors
- Smooth teleports
- Character should be at correct location after teleport

### Test 3: Multiple Players Login Simultaneously

**Objective:** Stress test concurrent grid loading

**Steps:**
1. Have multiple test accounts ready
2. Log in with 5-10 characters simultaneously from different locations
3. Monitor server CPU usage
4. Check for any errors or crashes
5. Verify all players loaded correctly

**Expected Results:**
- No crashes
- All players connect successfully
- CPU usage distributed across threads
- No "grid already loaded" race condition errors

### Test 4: Rapid Zone Changes

**Objective:** Test grid loading under rapid teleports

**Steps:**
1. Create a script or macro to teleport rapidly:
   ```
   .tele loc1
   wait 1 second
   .tele loc2
   wait 1 second
   .tele loc3
   ...
   ```
2. Execute 20-30 rapid teleports
3. Monitor for errors or crashes

**Expected Results:**
- No crashes
- Character position correct after each teleport
- No stuck or frozen character state

### Test 5: Error Handling

**Objective:** Verify exception handling works correctly

**Steps:**
1. Test with corrupted/missing map files (if safe to do so)
2. Monitor error logs for proper error messages
3. Verify server doesn't crash on grid load failures

**Expected Results:**
- Errors logged with grid coordinates
- Server continues running (doesn't crash)
- Other grids continue loading

### Test 6: Configuration Validation

**Objective:** Verify configuration options work correctly

**Test 6a: Disable Parallel Loading**
```conf
Map.ParallelGridLoading = 0
```
- Should use sequential loading
- Logs should NOT mention "parallel mode"

**Test 6b: Adjust Batch Size**
```conf
Map.ParallelGridLoadingBatchSize = 1  # Small batches
Map.ParallelGridLoadingBatchSize = 16 # Large batches
```
- Should complete successfully
- May affect timing

**Test 6c: Adjust Threshold**
```conf
Map.ParallelGridLoadingMinGrids = 100  # Very high threshold
```
- LoadGridsInRange should use sequential mode more often

**Test 6d: No Thread Pool**
```conf
MapUpdate.Threads = 0
```
- Should automatically fall back to sequential loading
- Should work without errors

## Thread Safety Testing

### Test 7: ThreadSanitizer (if available)

**Build with ThreadSanitizer:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build
```

**Run:**
```bash
./build/src/server/apps/worldserver/worldserver
```

**Expected Results:**
- No data race warnings
- No deadlock warnings
- Server runs normally

### Test 8: Concurrent Map Preloading

**Objective:** Test multiple maps loading simultaneously

**Steps:**
1. Configure server to preload multiple maps
2. Start server and monitor
3. Check that all maps load correctly

**Expected Results:**
- All maps loaded successfully
- No grid ID conflicts
- Correct grid counts for each map

## Performance Benchmarking

### Test 9: Measure Actual Speedup

**Methodology:**
1. Record 5 startup times with parallel loading enabled
2. Record 5 startup times with parallel loading disabled
3. Calculate average and standard deviation
4. Compare results

**Metrics to Collect:**
- Total startup time
- Grid loading time per map
- CPU usage during loading
- Memory usage during loading

**Data Template:**
```
Configuration: Parallel Enabled, 4 threads, Batch Size 4
Run 1: Map 0 loaded in X ms
Run 2: Map 0 loaded in Y ms
...
Average: Z ms

Configuration: Sequential
Run 1: Map 0 loaded in A ms
Run 2: Map 0 loaded in B ms
...
Average: C ms

Speedup: Z/C = X.XX x
```

### Test 10: Different Thread Counts

**Objective:** Find optimal thread count

**Test Configurations:**
- MapUpdate.Threads = 1 (sequential baseline)
- MapUpdate.Threads = 2
- MapUpdate.Threads = 4
- MapUpdate.Threads = 8
- MapUpdate.Threads = 16

**Measure:**
- Grid loading time
- CPU efficiency
- Diminishing returns point

## Correctness Validation

### Test 11: Grid Content Verification

**Objective:** Ensure parallel loading produces identical results to sequential

**Steps:**
1. Load grids with sequential mode, note counts:
   - Creature count per grid
   - GameObject count per grid
   - Grid activation state
2. Restart and load with parallel mode
3. Compare counts and states

**Expected Results:**
- Identical creature counts
- Identical GameObject counts
- Same grids marked as loaded

### Test 12: Grid Unload/Reload

**Objective:** Test grid lifecycle with parallel loading

**Steps:**
1. Load grids in an area
2. Move player away (trigger grid unload)
3. Move player back (trigger grid reload)
4. Repeat several times

**Expected Results:**
- Grids unload correctly
- Grids reload correctly
- No memory leaks
- No orphaned objects

## Known Issues to Watch For

### Potential Issues:
1. **Race condition** in CreateGrid if lock is insufficient
2. **Memory spike** if all grids load simultaneously
3. **File handle exhaustion** on systems with low limits
4. **Deadlock** if grid loading has circular dependencies
5. **Incorrect grid counts** if atomic increments fail

### What to Monitor:
- System memory usage (watch for spikes)
- Open file descriptors (`lsof` on Linux)
- CPU usage distribution across threads
- Any "grid already loaded" warnings
- Creature/GameObject spawn integrity

## Success Criteria

✅ **Must Pass:**
- No crashes during any test scenario
- No data races detected by ThreadSanitizer
- Identical grid content (sequential vs parallel)
- Proper error handling and logging
- Graceful fallback when parallel disabled

✅ **Should Pass:**
- Measurable time improvement with parallel loading
- CPU usage distributed across threads
- No significant memory overhead
- Smooth player experience (no lag spikes)

⚠️ **Nice to Have:**
- Linear speedup with thread count (up to CPU cores)
- Consistent performance across different maps
- Low overhead for small grid counts

## Reporting Results

When reporting test results, include:
1. CPU model and core count
2. Thread count configuration
3. Map being tested (density matters)
4. Timing measurements (avg, min, max, stddev)
5. Any errors or warnings observed
6. System resource usage (CPU%, memory)

## Example Test Report

```
Test Environment:
- CPU: AMD Ryzen 7 5800X (8 cores, 16 threads)
- MapUpdate.Threads: 8
- Map.ParallelGridLoadingBatchSize: 4

Test 1: LoadAllGrids - Map 0 (Eastern Kingdoms)
- Parallel Mode: 6.2s, 6.5s, 6.1s, 6.4s, 6.3s → Avg: 6.3s
- Sequential Mode: 24.1s, 23.8s, 24.5s, 24.2s, 23.9s → Avg: 24.1s
- Speedup: 3.8x
- No errors observed

Test 2: Rapid Teleports (20 teleports)
- All teleports successful
- No lag spikes
- No errors in logs

Test 11: Grid Content Verification
- Sequential: 1,247 creatures, 893 GOs
- Parallel: 1,247 creatures, 893 GOs
- ✓ Content matches
```
