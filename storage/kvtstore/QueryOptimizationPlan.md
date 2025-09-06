# Query Optimization Implementation Plan

## Overview
Implement proper query optimization support in KVT storage engine to enable the MariaDB optimizer to make intelligent decisions about query execution plans.

## Key Components to Implement

### 1. records_in_range()
This function estimates how many rows exist between two index values. Critical for:
- Choosing between index scan vs table scan
- Selecting best index for multi-index tables
- Join order optimization

### 2. handler::info()
Provides table and index statistics to optimizer:
- Row count (HA_STATUS_VARIABLE)
- Index cardinality (HA_STATUS_CONST)
- Data/index file sizes (HA_STATUS_VARIABLE)
- Auto-increment values (HA_STATUS_AUTO)

### 3. Index Statistics Collection
Track and maintain:
- Number of distinct values per index
- Index selectivity
- Average key size
- Tree depth/height

## Implementation Details

### records_in_range() Algorithm
```cpp
ha_rows records_in_range(uint inx, const key_range *min_key, 
                         const key_range *max_key) {
    1. If no index, return total row count
    2. Convert min/max keys to KVT format
    3. Perform limited scan to estimate:
       - Sample first N entries
       - Calculate density
       - Extrapolate total
    4. Cache results for repeated calls
    5. Return estimate
}
```

### Statistics Storage
Store in KVT metadata space:
```
Key: [STATS_KEYSPACE][table_id][stat_type]
Value: Statistics structure

Types:
- TABLE_STATS: row_count, avg_row_length, data_size
- INDEX_STATS: cardinality, distinct_count, tree_height
- COLUMN_STATS: null_count, distinct_values, min/max
```

### Cardinality Estimation
Use HyperLogLog for efficient cardinality estimation:
- Low memory overhead
- Good accuracy (±2% typical)
- Can merge partial results

## Implementation Steps

1. **Create Statistics Manager Class**
   - Collect statistics during DML operations
   - Periodic background refresh
   - Cache frequently accessed stats

2. **Enhance records_in_range()**
   - Implement sampling-based estimation
   - Use index statistics when available
   - Handle edge cases (NULL, infinity)

3. **Complete info() Implementation**
   - Query statistics from manager
   - Calculate derived metrics
   - Handle all flag combinations

4. **Add ANALYZE TABLE Support**
   - Full index scan for accurate stats
   - Update cached statistics
   - Trigger optimizer re-planning

5. **Testing & Validation**
   - Compare estimates vs actual
   - Benchmark optimizer decisions
   - Verify plan improvements