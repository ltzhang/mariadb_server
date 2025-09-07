# MariaDB-Specific KVT API Extensions - Design and Implementation Plan

## Executive Summary
Instead of using generic `kvt_process` with dynamically generated functions, we will extend the KVT API with MariaDB-specific operations. These predefined APIs will handle common database operations like filtering, aggregation, and atomic updates, making the interface cleaner, safer, and more performant.

## Design Philosophy
- **Predefined Operations**: All pushdown operations are predefined in KVT
- **Type Safety**: Strongly-typed parameters instead of generic strings
- **MariaDB Semantics**: APIs follow SQL semantics and MariaDB conventions
- **No Dynamic Code**: No runtime function generation or shipping
- **Clear Contract**: Well-defined input/output specifications

## Phase 1: Filter Pushdown APIs

### 1.1 Design
Create specific APIs for common SQL WHERE clause patterns:

```cpp
// Equality filter: WHERE column = value
KVTError kvt_filter_eq(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const KVTValue& value,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTRow>& results,
    std::string& error_msg);

// Range filter: WHERE column > value or column < value
KVTError kvt_filter_range(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    KVTCompareOp op,  // GT, GE, LT, LE
    const KVTValue& value,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTRow>& results,
    std::string& error_msg);

// IN filter: WHERE column IN (value1, value2, ...)
KVTError kvt_filter_in(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const std::vector<KVTValue>& values,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTRow>& results,
    std::string& error_msg);

// BETWEEN filter: WHERE column BETWEEN low AND high
KVTError kvt_filter_between(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const KVTValue& low,
    const KVTValue& high,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTRow>& results,
    std::string& error_msg);

// Composite filter: Multiple conditions with AND/OR
KVTError kvt_filter_composite(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTFilterCondition& condition,  // Tree structure
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTRow>& results,
    std::string& error_msg);
```

### 1.2 Implementation Steps
1. Define filter condition structures
2. Implement each filter API in KVT
3. Add column metadata for type-aware comparisons
4. Optimize with early termination
5. Add statistics collection

### 1.3 Testing Plan
- Unit tests for each filter type
- Boundary conditions (NULL, empty)
- Performance benchmarks
- Composite filter validation
- Type compatibility tests

## Phase 2: Aggregation Pushdown APIs

### 2.1 Design
Predefined aggregation functions that execute in storage:

```cpp
// COUNT(*) and COUNT(column)
KVTError kvt_aggregate_count(
    uint64_t tx_id,
    uint64_t table_id,
    int32_t column_index,  // -1 for COUNT(*)
    const KVTFilterCondition* filter,  // Optional WHERE
    const KVTKey& start_key,
    const KVTKey& end_key,
    uint64_t& count,
    std::string& error_msg);

// SUM(column)
KVTError kvt_aggregate_sum(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const KVTFilterCondition* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    KVTNumericValue& sum,  // Handles int/float/decimal
    std::string& error_msg);

// MIN/MAX(column)
KVTError kvt_aggregate_min_max(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    bool find_max,  // true for MAX, false for MIN
    const KVTFilterCondition* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    KVTValue& result,
    std::string& error_msg);

// GROUP BY aggregation
KVTError kvt_aggregate_group_by(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<uint32_t>& group_columns,
    const KVTAggregateSpec& agg_spec,  // What to aggregate
    const KVTFilterCondition* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTGroupResult>& results,
    std::string& error_msg);

// Multiple aggregations in one scan
KVTError kvt_aggregate_multi(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<KVTAggregateSpec>& aggregates,
    const KVTFilterCondition* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    KVTAggregateResults& results,
    std::string& error_msg);
```

### 2.2 Implementation Steps
1. Define aggregate specification structures
2. Implement single-pass multi-aggregation
3. Add GROUP BY hash table management
4. Handle NULL values per SQL standard
5. Optimize memory usage for large groups

### 2.3 Testing Plan
- Test all aggregate functions
- NULL handling validation
- GROUP BY with multiple columns
- Large dataset aggregations
- Overflow handling for SUM

## Phase 3: Atomic Operation APIs

### 3.1 Design
Atomic operations for common update patterns:

```cpp
// Atomic increment: SET column = column + delta
KVTError kvt_atomic_increment(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    uint32_t column_index,
    int64_t delta,
    int64_t& old_value,
    std::string& error_msg);

// Atomic compare-and-swap
KVTError kvt_atomic_cas(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    uint32_t column_index,
    const KVTValue& expected,
    const KVTValue& new_value,
    bool& swapped,
    std::string& error_msg);

// Conditional update: UPDATE ... WHERE condition
KVTError kvt_atomic_update_if(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    const std::vector<KVTColumnUpdate>& updates,
    const KVTRowCondition& condition,
    bool& updated,
    std::string& error_msg);

// Batch atomic operations
KVTError kvt_atomic_batch(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<KVTAtomicOp>& operations,
    std::vector<KVTAtomicResult>& results,
    std::string& error_msg);

// String append operation
KVTError kvt_atomic_append(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    uint32_t column_index,
    const std::string& suffix,
    uint32_t max_length,
    bool& truncated,
    std::string& error_msg);
```

### 3.2 Implementation Steps
1. Define atomic operation structures
2. Implement lock-free algorithms
3. Add transaction isolation support
4. Handle type conversions
5. Optimize for batch operations

### 3.3 Testing Plan
- Concurrent increment tests
- CAS race condition tests
- Conditional update validation
- Batch operation atomicity
- Rollback scenarios

## Phase 4: Projection and Index APIs

### 4.1 Design
Column selection and index-only operations:

```cpp
// Projection: SELECT specific columns
KVTError kvt_project_columns(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<uint32_t>& column_indices,
    const KVTFilterCondition* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTPartialRow>& results,
    std::string& error_msg);

// Index-only scan
KVTError kvt_index_only_scan(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t index_id,
    const std::vector<uint32_t>& column_indices,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTIndexRow>& results,
    std::string& error_msg);

// Index range count (fast COUNT using index)
KVTError kvt_index_range_count(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t index_id,
    const KVTKey& start_key,
    const KVTKey& end_key,
    uint64_t& count,
    std::string& error_msg);
```

### 4.2 Implementation Steps
1. Define partial row structures
2. Implement column extraction
3. Optimize memory allocation
4. Add index metadata caching
5. Implement covering index detection

### 4.3 Testing Plan
- Projection accuracy tests
- Memory usage validation
- Index-only scan verification
- Performance comparisons
- NULL column handling

## Phase 5: Batch DML APIs

### 5.1 Design
Optimized batch operations for DML:

```cpp
// Batch insert with conflict handling
KVTError kvt_batch_insert(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<KVTRow>& rows,
    KVTConflictAction on_conflict,  // ERROR, IGNORE, REPLACE
    std::vector<KVTInsertResult>& results,
    std::string& error_msg);

// Batch update with conditions
KVTError kvt_batch_update(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<KVTUpdateSpec>& updates,
    std::vector<uint64_t>& affected_counts,
    std::string& error_msg);

// Batch delete with conditions
KVTError kvt_batch_delete(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTFilterCondition& condition,
    uint64_t& deleted_count,
    std::string& error_msg);

// Mixed batch operations
KVTError kvt_batch_mixed(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<KVTBatchOp>& operations,
    std::vector<KVTBatchResult>& results,
    std::string& error_msg);
```

### 5.2 Implementation Steps
1. Define batch operation structures
2. Implement conflict resolution
3. Add operation ordering optimization
4. Implement rollback on partial failure
5. Add progress reporting for large batches

### 5.3 Testing Plan
- Large batch performance tests
- Conflict resolution validation
- Transaction rollback tests
- Mixed operation ordering
- Memory pressure tests

## Phase 6: Join and Lookup APIs

### 6.1 Design
Support for join operations in storage:

```cpp
// Nested loop join with pushdown
KVTError kvt_join_nested_loop(
    uint64_t tx_id,
    uint64_t outer_table_id,
    uint64_t inner_table_id,
    const KVTJoinCondition& join_condition,
    const std::vector<uint32_t>& select_columns,
    std::vector<KVTJoinRow>& results,
    std::string& error_msg);

// Index lookup join
KVTError kvt_join_index_lookup(
    uint64_t tx_id,
    uint64_t outer_table_id,
    uint64_t inner_table_id,
    uint32_t inner_index_id,
    const KVTJoinSpec& join_spec,
    std::vector<KVTJoinRow>& results,
    std::string& error_msg);

// Semi-join for EXISTS/IN subqueries
KVTError kvt_semi_join(
    uint64_t tx_id,
    uint64_t outer_table_id,
    uint64_t inner_table_id,
    const KVTJoinCondition& condition,
    std::vector<KVTKey>& matching_keys,
    std::string& error_msg);
```

### 6.2 Implementation Steps
1. Define join structures
2. Implement join algorithms
3. Add join order optimization
4. Implement memory management
5. Add statistics for join planning

### 6.3 Testing Plan
- Join correctness tests
- Performance benchmarks
- Memory usage validation
- Large table joins
- Multi-way joins

## Phase 7: Statistics and Monitoring APIs

### 7.1 Design
APIs for optimization and monitoring:

```cpp
// Table statistics
KVTError kvt_get_table_stats(
    uint64_t table_id,
    KVTTableStats& stats,
    std::string& error_msg);

// Column histogram
KVTError kvt_get_column_histogram(
    uint64_t table_id,
    uint32_t column_index,
    KVTHistogram& histogram,
    std::string& error_msg);

// Query execution statistics
KVTError kvt_get_query_stats(
    uint64_t tx_id,
    KVTQueryStats& stats,
    std::string& error_msg);

// Optimization hints
KVTError kvt_analyze_query(
    const KVTQueryPlan& plan,
    KVTOptimizationHints& hints,
    std::string& error_msg);
```

### 7.2 Implementation Steps
1. Define statistics structures
2. Implement statistics collection
3. Add histogram generation
4. Implement cardinality estimation
5. Add cost model calculations

### 7.3 Testing Plan
- Statistics accuracy tests
- Histogram validation
- Performance overhead measurement
- Cardinality estimation tests

## Implementation Timeline

### Week 1-2: Foundation
- Design and implement core structures
- Set up build system
- Create test framework

### Week 3-4: Filter APIs
- Implement all filter operations
- Add type system
- Create filter tests

### Week 5-6: Aggregation APIs
- Implement aggregation functions
- Add GROUP BY support
- Performance optimization

### Week 7-8: Atomic Operations
- Implement atomic APIs
- Add transaction support
- Concurrency testing

### Week 9-10: Batch Operations
- Implement batch APIs
- Optimization algorithms
- Scale testing

### Week 11-12: Advanced Features
- Join operations
- Statistics APIs
- Final optimization

## Success Metrics

### Performance Targets
- Filter pushdown: 60% reduction in data transfer
- Aggregation: 80% faster than client-side
- Atomic ops: 50% faster than read-modify-write
- Batch ops: 40% throughput improvement
- Joins: 30% faster for indexed joins

### Quality Metrics
- Test coverage: >90%
- API documentation: 100% complete
- Performance regression: <5%
- Memory overhead: <10%

## Risk Mitigation

### Technical Risks
1. **API Complexity**: Keep interfaces simple and focused
2. **Performance**: Profile and optimize critical paths
3. **Compatibility**: Maintain backward compatibility
4. **Concurrency**: Extensive testing under load
5. **Memory**: Monitor and limit memory usage

### Mitigation Strategies
- Incremental development and testing
- Performance benchmarks at each phase
- Code reviews and documentation
- Stress testing and fuzzing
- Fallback mechanisms

## Conclusion

This plan transforms the generic KVT interface into a MariaDB-specific API that:
1. **Eliminates dynamic function generation** - All operations are predefined
2. **Provides type safety** - Strongly-typed parameters and returns
3. **Follows SQL semantics** - Consistent with MariaDB behavior
4. **Optimizes common patterns** - Specific APIs for common operations
5. **Enables better optimization** - KVT can optimize knowing the exact operation

The phased approach ensures each component is thoroughly designed, implemented, and tested before moving to the next phase. This creates a robust, performant, and maintainable storage engine interface.