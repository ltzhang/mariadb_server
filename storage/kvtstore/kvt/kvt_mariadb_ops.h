/*
   Copyright (c) 2025 KVT Storage Engine - MariaDB Specific Operations

   This header defines MariaDB-specific extensions to the KVT API.
   These APIs provide predefined operations for common SQL patterns,
   eliminating the need for dynamic function generation.
*/

#ifndef KVT_MARIADB_OPS_H
#define KVT_MARIADB_OPS_H

#include "kvt_inc.h"
#include <vector>
#include <string>
#include <cstdint>
#include <optional>
#include <variant>

namespace kvt_mariadb {

// =============================================================================
// Type Definitions
// =============================================================================

/**
 * Column data types matching MariaDB types
 */
enum class KVTColumnType {
    INT8,       // TINYINT
    INT16,      // SMALLINT
    INT32,      // INT
    INT64,      // BIGINT
    UINT8,      // UNSIGNED TINYINT
    UINT16,     // UNSIGNED SMALLINT
    UINT32,     // UNSIGNED INT
    UINT64,     // UNSIGNED BIGINT
    FLOAT,      // FLOAT
    DOUBLE,     // DOUBLE
    DECIMAL,    // DECIMAL
    VARCHAR,    // VARCHAR
    CHAR,       // CHAR
    TEXT,       // TEXT
    BLOB,       // BLOB
    DATE,       // DATE
    TIME,       // TIME
    DATETIME,   // DATETIME
    TIMESTAMP,  // TIMESTAMP
    BOOLEAN,    // BOOLEAN
    JSON        // JSON
};

/**
 * Comparison operators for filters
 */
enum class KVTCompareOp {
    EQ,         // =
    NE,         // !=
    LT,         // <
    LE,         // <=
    GT,         // >
    GE,         // >=
    IS_NULL,    // IS NULL
    IS_NOT_NULL // IS NOT NULL
};

/**
 * Logical operators for composite conditions
 */
enum class KVTLogicalOp {
    AND,
    OR,
    NOT
};

/**
 * Aggregate functions
 */
enum class KVTAggregateFunc {
    COUNT,      // COUNT(*) or COUNT(column)
    SUM,        // SUM(column)
    AVG,        // AVG(column)
    MIN,        // MIN(column)
    MAX,        // MAX(column)
    STD_DEV,    // STD_DEV(column)
    VARIANCE    // VARIANCE(column)
};

/**
 * Conflict handling for batch operations
 */
enum class KVTConflictAction {
    ERROR,      // Return error on conflict (default)
    IGNORE,     // Skip conflicting rows
    REPLACE,    // Replace existing rows
    UPDATE      // Update existing rows (UPSERT)
};

/**
 * Join types
 */
enum class KVTJoinType {
    INNER,
    LEFT,
    RIGHT,
    SEMI,       // For EXISTS/IN
    ANTI        // For NOT EXISTS/NOT IN
};

// =============================================================================
// Data Structures
// =============================================================================

/**
 * Value holder that can store any column type
 */
struct KVTValue {
    KVTColumnType type;
    std::variant<
        int8_t, int16_t, int32_t, int64_t,
        uint8_t, uint16_t, uint32_t, uint64_t,
        float, double,
        std::string,  // For VARCHAR, CHAR, TEXT, DECIMAL, DATE, TIME, DATETIME
        std::vector<uint8_t>  // For BLOB
    > data;
    bool is_null;
    
    KVTValue() : is_null(true) {}
    explicit KVTValue(int32_t v) : type(KVTColumnType::INT32), data(v), is_null(false) {}
    explicit KVTValue(int64_t v) : type(KVTColumnType::INT64), data(v), is_null(false) {}
    explicit KVTValue(double v) : type(KVTColumnType::DOUBLE), data(v), is_null(false) {}
    explicit KVTValue(const std::string& v) : type(KVTColumnType::VARCHAR), data(v), is_null(false) {}
};

/**
 * Column metadata
 */
struct KVTColumnInfo {
    uint32_t index;
    std::string name;
    KVTColumnType type;
    uint32_t max_length;
    bool nullable;
    bool is_primary_key;
    bool is_indexed;
    std::optional<KVTValue> default_value;
};

/**
 * Single filter condition
 */
struct KVTFilterCondition {
    uint32_t column_index;
    KVTCompareOp op;
    KVTValue value;
    
    // For BETWEEN
    std::optional<KVTValue> high_value;
    
    // For IN
    std::vector<KVTValue> in_values;
};

/**
 * Composite filter (tree structure for AND/OR/NOT)
 */
struct KVTCompositeFilter {
    KVTLogicalOp op;
    std::vector<std::variant<KVTFilterCondition, KVTCompositeFilter>> conditions;
};

/**
 * Aggregate specification
 */
struct KVTAggregateSpec {
    KVTAggregateFunc func;
    int32_t column_index;  // -1 for COUNT(*)
    std::string alias;      // Result column name
};

/**
 * GROUP BY specification
 */
struct KVTGroupBySpec {
    std::vector<uint32_t> group_columns;
    std::vector<KVTAggregateSpec> aggregates;
    std::optional<KVTCompositeFilter> having_condition;
};

/**
 * Column update specification
 */
struct KVTColumnUpdate {
    uint32_t column_index;
    KVTValue new_value;
};

/**
 * Atomic operation specification
 */
struct KVTAtomicOp {
    enum OpType {
        INCREMENT,
        DECREMENT,
        CAS,        // Compare-and-swap
        APPEND,
        PREPEND
    } type;
    
    KVTKey row_key;
    uint32_t column_index;
    KVTValue value1;  // Delta for INCREMENT, expected for CAS, string for APPEND
    std::optional<KVTValue> value2;  // New value for CAS
};

/**
 * Join condition
 */
struct KVTJoinCondition {
    uint32_t left_column;
    KVTCompareOp op;
    uint32_t right_column;
    
    // Additional filter on joined result
    std::optional<KVTCompositeFilter> where_condition;
};

/**
 * Row data structure
 */
struct KVTRow {
    KVTKey key;
    std::vector<KVTValue> columns;
};

/**
 * Partial row (for projections)
 */
struct KVTPartialRow {
    KVTKey key;
    std::vector<uint32_t> column_indices;
    std::vector<KVTValue> values;
};

/**
 * Group result
 */
struct KVTGroupResult {
    std::vector<KVTValue> group_values;
    std::vector<KVTValue> aggregate_results;
};

/**
 * Join result row
 */
struct KVTJoinRow {
    KVTKey left_key;
    KVTKey right_key;
    std::vector<KVTValue> values;
};

/**
 * Statistics structures
 */
struct KVTTableStats {
    uint64_t row_count;
    uint64_t data_size_bytes;
    uint64_t index_size_bytes;
    std::chrono::system_clock::time_point last_analyzed;
};

struct KVTColumnStats {
    uint64_t distinct_count;
    uint64_t null_count;
    KVTValue min_value;
    KVTValue max_value;
    double average_length;  // For variable-length columns
};

// =============================================================================
// Filter APIs
// =============================================================================

/**
 * Simple equality filter: WHERE column = value
 */
KVTError kvt_filter_eq(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const KVTValue& value,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg);

/**
 * Range filter: WHERE column > value, column >= value, etc.
 */
KVTError kvt_filter_range(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    KVTCompareOp op,
    const KVTValue& value,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg);

/**
 * IN filter: WHERE column IN (value1, value2, ...)
 */
KVTError kvt_filter_in(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const std::vector<KVTValue>& values,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg);

/**
 * BETWEEN filter: WHERE column BETWEEN low AND high
 */
KVTError kvt_filter_between(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const KVTValue& low,
    const KVTValue& high,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg);

/**
 * LIKE filter: WHERE column LIKE pattern
 */
KVTError kvt_filter_like(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const std::string& pattern,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg);

/**
 * Composite filter: Multiple conditions with AND/OR/NOT
 */
KVTError kvt_filter_composite(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTCompositeFilter& filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg);

// =============================================================================
// Aggregation APIs
// =============================================================================

/**
 * COUNT aggregation
 */
KVTError kvt_aggregate_count(
    uint64_t tx_id,
    uint64_t table_id,
    int32_t column_index,  // -1 for COUNT(*)
    const KVTCompositeFilter* filter,  // Optional WHERE
    const KVTKey& start_key,
    const KVTKey& end_key,
    uint64_t& count,
    std::string& error_msg);

/**
 * SUM aggregation
 */
KVTError kvt_aggregate_sum(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const KVTCompositeFilter* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    KVTValue& sum,
    std::string& error_msg);

/**
 * MIN/MAX aggregation
 */
KVTError kvt_aggregate_min_max(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    bool find_max,
    const KVTCompositeFilter* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    KVTValue& result,
    bool& found,
    std::string& error_msg);

/**
 * AVG aggregation
 */
KVTError kvt_aggregate_avg(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const KVTCompositeFilter* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    double& average,
    uint64_t& count,
    std::string& error_msg);

/**
 * GROUP BY aggregation
 */
KVTError kvt_aggregate_group_by(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTGroupBySpec& spec,
    const KVTCompositeFilter* where_filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTGroupResult>& results,
    std::string& error_msg);

/**
 * Multiple aggregations in single scan
 */
KVTError kvt_aggregate_multi(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<KVTAggregateSpec>& aggregates,
    const KVTCompositeFilter* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTValue>& results,
    std::string& error_msg);

// =============================================================================
// Atomic Operation APIs
// =============================================================================

/**
 * Atomic increment/decrement
 */
KVTError kvt_atomic_increment(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    uint32_t column_index,
    int64_t delta,
    int64_t& old_value,
    std::string& error_msg);

/**
 * Atomic compare-and-swap
 */
KVTError kvt_atomic_cas(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    uint32_t column_index,
    const KVTValue& expected,
    const KVTValue& new_value,
    bool& swapped,
    KVTValue& actual_value,
    std::string& error_msg);

/**
 * Conditional update
 */
KVTError kvt_atomic_update_if(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    const std::vector<KVTColumnUpdate>& updates,
    const KVTFilterCondition& condition,
    bool& updated,
    std::string& error_msg);

/**
 * String append
 */
KVTError kvt_atomic_append(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    uint32_t column_index,
    const std::string& suffix,
    uint32_t max_length,
    bool& truncated,
    std::string& error_msg);

/**
 * Batch atomic operations
 */
KVTError kvt_atomic_batch(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<KVTAtomicOp>& operations,
    std::vector<KVTValue>& results,
    std::string& error_msg);

// =============================================================================
// Projection APIs
// =============================================================================

/**
 * Project specific columns
 */
KVTError kvt_project_columns(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<uint32_t>& column_indices,
    const KVTCompositeFilter* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTPartialRow>& results,
    std::string& error_msg);

/**
 * Index-only scan
 */
KVTError kvt_index_only_scan(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t index_id,
    const std::vector<uint32_t>& column_indices,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTPartialRow>& results,
    std::string& error_msg);

// =============================================================================
// Batch DML APIs
// =============================================================================

/**
 * Batch insert
 */
KVTError kvt_batch_insert(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<KVTRow>& rows,
    KVTConflictAction on_conflict,
    std::vector<bool>& success,
    std::string& error_msg);

/**
 * Batch update
 */
KVTError kvt_batch_update(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTCompositeFilter& filter,
    const std::vector<KVTColumnUpdate>& updates,
    uint64_t& affected_rows,
    std::string& error_msg);

/**
 * Batch delete
 */
KVTError kvt_batch_delete(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTCompositeFilter& filter,
    uint64_t& deleted_rows,
    std::string& error_msg);

/**
 * UPSERT operation
 */
KVTError kvt_upsert(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTRow& row,
    const std::vector<KVTColumnUpdate>& on_update,
    bool& inserted,
    std::string& error_msg);

// =============================================================================
// Join APIs
// =============================================================================

/**
 * Nested loop join
 */
KVTError kvt_join_nested_loop(
    uint64_t tx_id,
    uint64_t left_table_id,
    uint64_t right_table_id,
    KVTJoinType join_type,
    const KVTJoinCondition& condition,
    const std::vector<uint32_t>& select_columns,
    size_t limit,
    std::vector<KVTJoinRow>& results,
    std::string& error_msg);

/**
 * Index lookup join
 */
KVTError kvt_join_index_lookup(
    uint64_t tx_id,
    uint64_t left_table_id,
    uint64_t right_table_id,
    uint32_t right_index_id,
    const KVTJoinCondition& condition,
    const std::vector<uint32_t>& select_columns,
    size_t limit,
    std::vector<KVTJoinRow>& results,
    std::string& error_msg);

/**
 * Semi-join for EXISTS/IN subqueries
 */
KVTError kvt_semi_join(
    uint64_t tx_id,
    uint64_t left_table_id,
    uint64_t right_table_id,
    const KVTJoinCondition& condition,
    std::vector<KVTKey>& matching_keys,
    std::string& error_msg);

// =============================================================================
// Statistics APIs
// =============================================================================

/**
 * Get table statistics
 */
KVTError kvt_get_table_stats(
    uint64_t table_id,
    KVTTableStats& stats,
    std::string& error_msg);

/**
 * Get column statistics
 */
KVTError kvt_get_column_stats(
    uint64_t table_id,
    uint32_t column_index,
    KVTColumnStats& stats,
    std::string& error_msg);

/**
 * Analyze table (update statistics)
 */
KVTError kvt_analyze_table(
    uint64_t table_id,
    const std::vector<uint32_t>& column_indices,  // Empty for all columns
    std::string& error_msg);

/**
 * Estimate row count for filter
 */
KVTError kvt_estimate_rows(
    uint64_t table_id,
    const KVTCompositeFilter& filter,
    uint64_t& estimated_rows,
    double& selectivity,
    std::string& error_msg);

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * Register table schema with KVT
 */
KVTError kvt_register_table_schema(
    uint64_t table_id,
    const std::vector<KVTColumnInfo>& columns,
    std::string& error_msg);

/**
 * Get table schema
 */
KVTError kvt_get_table_schema(
    uint64_t table_id,
    std::vector<KVTColumnInfo>& columns,
    std::string& error_msg);

/**
 * Set optimization hints
 */
KVTError kvt_set_hints(
    uint64_t tx_id,
    const std::string& hint_string,
    std::string& error_msg);

} // namespace kvt_mariadb

#endif // KVT_MARIADB_OPS_H