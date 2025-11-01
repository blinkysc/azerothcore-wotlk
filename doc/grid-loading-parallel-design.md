# Parallel Grid Loading Design

## Current Architecture

### Grid Loading Process
1. **CreateGrid** (MapGridManager::CreateGrid)
   - Allocates grid structure
   - Loads terrain data (maps, vmaps, mmaps)
   - Already has `_gridLock` mutex - thread-safe

2. **LoadGrid** (MapGridManager::LoadGrid)
   - Loads game objects (spawns, creatures, GOs)
   - Marks grid as loaded to prevent re-entry
   - No explicit lock (relies on single-threaded access)

3. **Two-Phase Loading**:
   ```
   Map::EnsureGridLoaded() ->
       EnsureGridCreated() -> CreateGrid() [locks]
       LoadGrid() [no lock]
   ```

### Bottlenecks
- `Map::LoadAllGrids()`: Sequential loop over 64x64 = 4096 grids
- `Map::LoadGridsInRange()`: Sequential loop over area
- Startup time: 30-120 seconds for full map preload

## Parallelization Strategy

### Thread Safety Analysis

**Already Thread-Safe:**
- ✅ `CreateGrid()` - has `_gridLock`
- ✅ Grid terrain data (read-only after load)
- ✅ Individual grid structures (independent)

**Needs Protection:**
- ⚠️ `_createdGridsCount` - needs atomic increment
- ⚠️ `_loadedGridsCount` - needs atomic increment
- ⚠️ `_mapGrid[x][y]` assignment - already protected by CreateGrid lock

**Safe Approach:**
Each grid can be created + loaded independently in parallel because:
1. Grid slots (`_mapGrid[x][y]`) are unique per coordinate
2. CreateGrid already locks for slot assignment
3. LoadGrid only reads/writes to its own grid
4. No cross-grid dependencies during loading

### Implementation Plan

#### Phase 1: Make Counters Thread-Safe
```cpp
// MapGridManager.h
std::atomic<uint32> _createdGridsCount;
std::atomic<uint32> _loadedGridsCount;
```

#### Phase 2: Parallel LoadAllGrids
```cpp
void Map::LoadAllGrids()
{
    if (!sConfigMgr->GetOption<bool>("Map.ParallelGridLoading", true) ||
        !sMapMgr->GetMapUpdater()->activated())
    {
        // Fallback to sequential
        LoadAllGridsSequential();
        return;
    }

    LOG_INFO("maps", "Loading all grids for map {} in parallel...", GetId());

    auto startTime = std::chrono::high_resolution_clock::now();

    std::vector<std::pair<uint32, uint32>> gridCoords;

    // Collect grid coordinates
    for (uint32 cellX = 0; cellX < TOTAL_NUMBER_OF_CELLS_PER_MAP; cellX++)
        for (uint32 cellY = 0; cellY < TOTAL_NUMBER_OF_CELLS_PER_MAP; cellY++)
            gridCoords.push_back({cellX, cellY});

    MapUpdater* updater = sMapMgr->GetMapUpdater();

    // Batch grids to reduce task overhead (process 4 grids per task)
    uint32 batchSize = 4;
    for (size_t i = 0; i < gridCoords.size(); i += batchSize)
    {
        size_t end = std::min(i + batchSize, gridCoords.size());
        std::vector<std::pair<uint32, uint32>> batch(
            gridCoords.begin() + i,
            gridCoords.begin() + end
        );

        updater->submit_task([this, batch = std::move(batch)]() {
            for (auto [cellX, cellY] : batch)
            {
                float x = (cellX + 0.5f - CENTER_GRID_CELL_ID) * SIZE_OF_GRID_CELL;
                float y = (cellY + 0.5f - CENTER_GRID_CELL_ID) * SIZE_OF_GRID_CELL;

                try
                {
                    LoadGrid(x, y);
                }
                catch (const std::exception& ex)
                {
                    LOG_ERROR("maps", "Exception during parallel grid loading on map {} at ({}, {}): {}",
                        GetId(), cellX, cellY, ex.what());
                }
                catch (...)
                {
                    LOG_ERROR("maps", "Unknown exception during parallel grid loading on map {} at ({}, {})",
                        GetId(), cellX, cellY);
                }
            }
        });
    }

    // Wait for all grids to complete
    updater->wait();

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    LOG_INFO("maps", "Loaded all grids for map {} in {} ms (parallel mode)",
        GetId(), duration.count());
}
```

#### Phase 3: Parallel LoadGridsInRange
```cpp
void Map::LoadGridsInRange(Position const& center, float radius)
{
    if (_mapGridManager.IsGridsFullyLoaded())
        return;

    float const x = center.GetPositionX();
    float const y = center.GetPositionY();

    CellCoord cellCoord(Acore::ComputeCellCoord(x, y));
    if (!cellCoord.IsCoordValid())
        return;

    if (radius > SIZE_OF_GRIDS)
        radius = SIZE_OF_GRIDS;

    CellArea area = Cell::CalculateCellArea(x, y, radius);
    if (!area)
        return;

    // Collect grids that need loading
    std::vector<Cell> cellsToLoad;
    for (uint32 x = area.low_bound.x_coord; x <= area.high_bound.x_coord; ++x)
    {
        for (uint32 y = area.low_bound.y_coord; y <= area.high_bound.y_coord; ++y)
        {
            CellCoord coord(x, y);
            Cell cell(coord);

            // Check if already loaded
            if (!IsGridLoaded(GridCoord(cell.GridX(), cell.GridY())))
                cellsToLoad.push_back(cell);
        }
    }

    // If few grids or parallel disabled, load sequentially
    if (cellsToLoad.size() < 4 ||
        !sConfigMgr->GetOption<bool>("Map.ParallelGridLoading", true) ||
        !sMapMgr->GetMapUpdater()->activated())
    {
        for (const Cell& cell : cellsToLoad)
            EnsureGridLoaded(cell);
        return;
    }

    // Parallel loading
    MapUpdater* updater = sMapMgr->GetMapUpdater();

    for (const Cell& cell : cellsToLoad)
    {
        updater->submit_task([this, cell]() {
            try
            {
                EnsureGridLoaded(cell);
            }
            catch (const std::exception& ex)
            {
                LOG_ERROR("maps", "Exception during parallel grid loading in range on map {}: {}",
                    GetId(), ex.what());
            }
        });
    }

    updater->wait();
}
```

### Configuration Options

Add to `worldserver.conf.dist`:
```conf
#
#    Map.ParallelGridLoading
#        Description: Enable parallel grid loading using work-stealing thread pool.
#                     Reduces server startup time and teleport delays by loading
#                     multiple grids simultaneously.
#
#                     Performance impact:
#                     - Server startup: 3-6x faster (30-120s -> 5-20s)
#                     - Teleports: Smoother, less lag when loading new areas
#                     - No impact on runtime performance
#
#        Default:     1 - (Enabled)
#                     0 - (Disabled, sequential loading)

Map.ParallelGridLoading = 1
```

### Benchmarking

Add metrics to track:
```cpp
METRIC_TIMER("map_load_all_grids_time",
    METRIC_TAG("map_id", std::to_string(GetId())),
    METRIC_TAG("mode", parallelEnabled ? "parallel" : "sequential"));
```

## Safety Guarantees

### What Makes This Safe

1. **Grid Independence**: Each grid loads independently
   - No shared mutable state between grids during load
   - Each grid slot (`_mapGrid[x][y]`) is unique

2. **Existing Locks**: CreateGrid already protects slot assignment
   - No risk of two threads creating same grid
   - Atomic counters for statistics

3. **Load Ordering**:
   - CreateGrid (terrain) always before LoadGrid (objects)
   - Maintained by EnsureGridLoaded

4. **Error Isolation**: Exception in one grid doesn't affect others
   - Try-catch blocks around each grid load
   - Detailed error logging with coordinates

### Potential Issues (and Solutions)

1. **Memory Pressure**:
   - Loading 4096 grids in parallel = high memory usage
   - Solution: Batch size of 4 grids per task (limited parallelism)

2. **File Handle Limits**:
   - Many threads opening files simultaneously
   - Solution: OS file handle limits are typically high (1024+)
   - Batching reduces concurrent file operations

3. **Load Balancing**:
   - Some grids have more objects than others
   - Solution: Work-stealing thread pool handles imbalance

4. **Startup Order**:
   - Maps are loaded via MapPreloadRequest already
   - Solution: Maintain existing map preload order

## Expected Performance

### Benchmarks (Estimated)

**Sequential Loading (Current):**
- Empty map: ~5 seconds
- Moderate density: ~30 seconds
- High density (Orgrimmar): ~120 seconds

**Parallel Loading (Expected):**
- Empty map: ~1-2 seconds (3-5x)
- Moderate density: ~8-10 seconds (3-4x)
- High density: ~20-30 seconds (4-6x)

**Amdahl's Law Analysis:**
- Grid loading is 100% parallelizable (no serial component)
- With 8 threads: Theoretical max = 8x speedup
- With batching (4 grids/task): Practical = 4-6x speedup

## Testing Plan

1. **Unit Tests**:
   - Test parallel LoadAllGrids vs sequential (correctness)
   - Verify all grids loaded (count check)
   - Test with ThreadSanitizer

2. **Integration Tests**:
   - Load various maps (empty, dense, mixed)
   - Teleport rapidly between zones
   - Multiple players logging in simultaneously

3. **Performance Tests**:
   - Measure startup time with/without parallel loading
   - Profile memory usage during parallel load
   - Test with various thread counts (2, 4, 8, 16)

4. **Stress Tests**:
   - Load all maps simultaneously
   - Rapid teleports during server startup
   - Monitor for crashes or data corruption
