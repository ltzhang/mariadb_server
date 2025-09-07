/*
   Copyright (c) 2025 KVT Storage Engine

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#ifndef KVT_PUSHDOWN_OPTIMIZER_H
#define KVT_PUSHDOWN_OPTIMIZER_H

#include "my_global.h"
#include "sql_class.h"
#include "sql_select.h"
#include "item.h"
#include "kvt/kvt_inc.h"
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>

namespace kvt_pushdown {

// Forward declarations
class PushdownPlan;
class FilterPushdown;
class AggregationPushdown;
class ProjectionPushdown;

/**
 * Type of pushdown operation
 */
enum class PushdownType {
    NONE = 0,
    FILTER,           // WHERE clause pushdown
    AGGREGATION,      // COUNT/SUM/MIN/MAX pushdown
    PROJECTION,       // SELECT column pushdown
    UPDATE,           // Atomic update operations
    MIXED             // Combination of operations
};

/**
 * Supported atomic operations for kvt_process
 */
enum class AtomicOperation {
    INCREMENT,        // Atomic increment
    DECREMENT,        // Atomic decrement
    APPEND,           // String append
    CONDITIONAL_SET,  // Set if condition met
    FIELD_EXTRACT,    // Extract specific field
    FIELD_UPDATE      // Update specific field
};

/**
 * Result of pushdown analysis
 */
struct PushdownResult {
    bool can_pushdown;
    PushdownType type;
    double estimated_benefit;  // 0.0 to 1.0
    std::string reason;        // Why pushdown was accepted/rejected
};

/**
 * Configuration for column-level operations
 */
struct ColumnOperation {
    uint field_index;
    AtomicOperation operation;
    std::string parameter;      // Operation-specific parameter
    Field* field;               // Field metadata
};

/**
 * Batch operation descriptor
 */
struct BatchOperation {
    enum Type { INSERT, UPDATE, DELETE, MIXED };
    Type type;
    std::vector<std::string> keys;
    std::vector<std::string> values;
    std::vector<ColumnOperation> column_ops;
};

/**
 * Main pushdown optimizer class
 */
class KVTPushdownOptimizer {
public:
    static KVTPushdownOptimizer* get_instance();
    
    /**
     * Analyze a condition for pushdown potential
     */
    PushdownResult analyze_condition(
        const COND* cond,
        TABLE* table,
        uint64_t table_id);
    
    /**
     * Generate KVTProcessFunc for filter pushdown
     */
    KVTProcessFunc generate_filter_function(
        const COND* cond,
        TABLE* table);
    
    /**
     * Generate KVTProcessFunc for aggregation
     */
    KVTProcessFunc generate_aggregation_function(
        Item_sum* agg_item,
        TABLE* table);
    
    /**
     * Generate KVTProcessFunc for column extraction
     */
    KVTProcessFunc generate_projection_function(
        MY_BITMAP* read_set,
        TABLE* table);
    
    /**
     * Generate KVTProcessFunc for atomic operations
     */
    KVTProcessFunc generate_atomic_operation(
        const ColumnOperation& op);
    
    /**
     * Optimize batch operations
     */
    bool optimize_batch_operations(
        BatchOperation& batch,
        uint64_t tx_id,
        uint64_t table_id);
    
    /**
     * Execute pushdown plan
     */
    KVTError execute_pushdown(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& start_key,
        const KVTKey& end_key,
        const KVTProcessFunc& func,
        const std::string& parameter,
        std::vector<std::pair<KVTKey, std::string>>& results,
        std::string& error_msg);
    
    /**
     * Check if condition is pushable
     */
    bool is_condition_pushable(const COND* cond);
    
    /**
     * Check if aggregation is pushable
     */
    bool is_aggregation_pushable(Item_sum* item);
    
    /**
     * Build parameter string for pushdown
     */
    std::string build_pushdown_parameter(
        const COND* cond,
        TABLE* table);
    
    /**
     * Statistics tracking
     */
    struct PushdownStats {
        uint64_t total_attempts;
        uint64_t successful_pushdowns;
        uint64_t filter_pushdowns;
        uint64_t aggregation_pushdowns;
        uint64_t projection_pushdowns;
        uint64_t atomic_operations;
        uint64_t rows_filtered_at_storage;
        uint64_t bytes_saved;
        
        void record_pushdown(PushdownType type, bool success);
        void record_filtering(uint64_t rows_before, uint64_t rows_after);
        void reset();
    };
    
    PushdownStats* get_stats() { return &stats; }

private:
    KVTPushdownOptimizer();
    ~KVTPushdownOptimizer();
    
    static KVTPushdownOptimizer* instance;
    PushdownStats stats;
    
    // Helper methods
    bool analyze_simple_condition(const Item* item);
    bool analyze_comparison(const Item_func* func);
    bool analyze_in_condition(const Item_func_in* func);
    bool analyze_between(const Item_func_between* func);
    bool analyze_like(const Item_func_like* func);
    
    std::string encode_filter_parameter(const Item* item, TABLE* table);
    std::string encode_aggregation_parameter(Item_sum* item);
    std::string encode_projection_parameter(MY_BITMAP* bitmap);
};

/**
 * Filter pushdown helper
 */
class FilterPushdown {
public:
    /**
     * Build filter function for simple comparison
     */
    static KVTProcessFunc build_comparison_filter(
        Item_func::Functype type,
        uint field_index,
        const std::string& value);
    
    /**
     * Build filter function for IN clause
     */
    static KVTProcessFunc build_in_filter(
        uint field_index,
        const std::vector<std::string>& values);
    
    /**
     * Build filter function for BETWEEN
     */
    static KVTProcessFunc build_between_filter(
        uint field_index,
        const std::string& min_value,
        const std::string& max_value);
    
    /**
     * Build filter function for LIKE
     */
    static KVTProcessFunc build_like_filter(
        uint field_index,
        const std::string& pattern);
    
    /**
     * Combine multiple filters with AND
     */
    static KVTProcessFunc combine_and_filters(
        const std::vector<KVTProcessFunc>& filters);
    
    /**
     * Combine multiple filters with OR
     */
    static KVTProcessFunc combine_or_filters(
        const std::vector<KVTProcessFunc>& filters);
};

/**
 * Aggregation pushdown helper
 */
class AggregationPushdown {
public:
    /**
     * Build COUNT(*) function
     */
    static KVTProcessFunc build_count_star();
    
    /**
     * Build COUNT(column) function
     */
    static KVTProcessFunc build_count_column(uint field_index);
    
    /**
     * Build SUM(column) function
     */
    static KVTProcessFunc build_sum(uint field_index, Field* field);
    
    /**
     * Build MIN(column) function
     */
    static KVTProcessFunc build_min(uint field_index, Field* field);
    
    /**
     * Build MAX(column) function
     */
    static KVTProcessFunc build_max(uint field_index, Field* field);
    
    /**
     * Build AVG(column) function
     */
    static KVTProcessFunc build_avg(uint field_index, Field* field);
};

/**
 * Projection pushdown helper
 */
class ProjectionPushdown {
public:
    /**
     * Build function to extract specific columns
     */
    static KVTProcessFunc build_column_extractor(
        const std::vector<uint>& field_indices,
        TABLE* table);
    
    /**
     * Build function to extract single column
     */
    static KVTProcessFunc build_single_column_extractor(
        uint field_index,
        Field* field);
    
    /**
     * Check if projection covers all needed columns
     */
    static bool covers_read_set(
        KEY* key_info,
        MY_BITMAP* read_set);
};

/**
 * Atomic operation generators
 */
class AtomicOperations {
public:
    /**
     * Generate increment operation
     */
    static KVTProcessFunc build_increment(
        uint field_index,
        int64_t delta = 1);
    
    /**
     * Generate decrement operation
     */
    static KVTProcessFunc build_decrement(
        uint field_index,
        int64_t delta = 1);
    
    /**
     * Generate conditional update
     */
    static KVTProcessFunc build_conditional_update(
        uint field_index,
        const std::string& condition,
        const std::string& new_value);
    
    /**
     * Generate append operation for strings
     */
    static KVTProcessFunc build_append(
        uint field_index,
        const std::string& suffix);
    
    /**
     * Generate compare-and-swap operation
     */
    static KVTProcessFunc build_compare_and_swap(
        uint field_index,
        const std::string& expected,
        const std::string& new_value);
};

/**
 * Utility functions for field encoding/decoding
 */
class FieldCodec {
public:
    /**
     * Extract field value from row data
     */
    static std::string extract_field_value(
        const std::string& row_data,
        uint field_index,
        Field* field);
    
    /**
     * Update field value in row data
     */
    static bool update_field_value(
        std::string& row_data,
        uint field_index,
        Field* field,
        const std::string& new_value);
    
    /**
     * Get field offset in row
     */
    static size_t get_field_offset(
        TABLE* table,
        uint field_index);
    
    /**
     * Get field length
     */
    static size_t get_field_length(
        Field* field);
    
    /**
     * Encode value for comparison
     */
    static std::string encode_for_comparison(
        Field* field,
        const Item* value);
};

/**
 * Batch optimization strategies
 */
class BatchOptimizer {
public:
    /**
     * Group operations by key locality
     */
    static void group_by_locality(
        BatchOperation& batch);
    
    /**
     * Combine adjacent operations on same key
     */
    static void combine_operations(
        BatchOperation& batch);
    
    /**
     * Split large batches for parallel execution
     */
    static std::vector<BatchOperation> split_for_parallel(
        const BatchOperation& batch,
        size_t max_batch_size = 1000);
    
    /**
     * Optimize update operations
     */
    static bool optimize_updates(
        BatchOperation& batch,
        TABLE* table);
};

// Helper functions

/**
 * Check if Item is a simple field reference
 */
inline bool is_field_item(const Item* item) {
    return item->type() == Item::FIELD_ITEM;
}

/**
 * Check if Item is a constant
 */
inline bool is_const_item(const Item* item) {
    return item->const_item();
}

/**
 * Get field index from Item_field
 */
inline uint get_field_index(const Item_field* field) {
    return field->field->field_index;
}

/**
 * Check if function is pushable
 */
bool is_function_pushable(Item_func::Functype type);

/**
 * Check if aggregation is pushable
 */
bool is_sum_func_pushable(Item_sum::Sumfunctype type);

} // namespace kvt_pushdown

#endif // KVT_PUSHDOWN_OPTIMIZER_H