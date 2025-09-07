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

#ifndef KVT_ATOMIC_OPERATIONS_H
#define KVT_ATOMIC_OPERATIONS_H

#include "my_global.h"
#include "sql_class.h"
#include "kvt/kvt_inc.h"
#include "kvt_pushdown_optimizer.h"
#include <string>
#include <memory>

namespace kvt_atomic {

/**
 * Atomic increment/decrement support for numeric columns
 */
class AtomicIncrement {
public:
    /**
     * Perform atomic increment on a numeric column
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param row_key Row key
     * @param field Field to increment
     * @param delta Increment value (negative for decrement)
     * @param old_value Output: Previous value before increment
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError increment_field(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& row_key,
        Field* field,
        int64_t delta,
        int64_t& old_value,
        std::string& error_msg);
    
    /**
     * Batch increment multiple rows
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param operations Vector of (row_key, field_index, delta) tuples
     * @param results Output: Old values for each row
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError batch_increment(
        uint64_t tx_id,
        uint64_t table_id,
        const std::vector<std::tuple<KVTKey, uint, int64_t>>& operations,
        std::vector<int64_t>& results,
        std::string& error_msg);
};

/**
 * Conditional update operations
 */
class ConditionalUpdate {
public:
    enum CompareOp {
        EQ,  // Equal
        NE,  // Not equal
        LT,  // Less than
        LE,  // Less than or equal
        GT,  // Greater than
        GE   // Greater than or equal
    };
    
    /**
     * Update field if condition is met
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param row_key Row key
     * @param field Field to check and update
     * @param op Comparison operator
     * @param compare_value Value to compare against
     * @param new_value Value to set if condition is true
     * @param updated Output: Whether update was performed
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError update_if(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& row_key,
        Field* field,
        CompareOp op,
        const std::string& compare_value,
        const std::string& new_value,
        bool& updated,
        std::string& error_msg);
    
    /**
     * Compare and swap operation
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param row_key Row key
     * @param field Field to update
     * @param expected_value Expected current value
     * @param new_value New value to set
     * @param swapped Output: Whether swap was performed
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError compare_and_swap(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& row_key,
        Field* field,
        const std::string& expected_value,
        const std::string& new_value,
        bool& swapped,
        std::string& error_msg);
};

/**
 * String append operations
 */
class StringOperations {
public:
    /**
     * Atomically append to a string field
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param row_key Row key
     * @param field String field to append to
     * @param suffix String to append
     * @param max_length Maximum allowed length (0 for no limit)
     * @param truncated Output: Whether result was truncated
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError append(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& row_key,
        Field* field,
        const std::string& suffix,
        size_t max_length,
        bool& truncated,
        std::string& error_msg);
    
    /**
     * Atomically prepend to a string field
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param row_key Row key
     * @param field String field to prepend to
     * @param prefix String to prepend
     * @param max_length Maximum allowed length (0 for no limit)
     * @param truncated Output: Whether result was truncated
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError prepend(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& row_key,
        Field* field,
        const std::string& prefix,
        size_t max_length,
        bool& truncated,
        std::string& error_msg);
};

/**
 * Partial field updates
 */
class PartialUpdate {
public:
    /**
     * Update specific fields without fetching entire row
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param row_key Row key
     * @param field_updates Map of field_index to new value
     * @param table Table metadata
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError update_fields(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& row_key,
        const std::unordered_map<uint, std::string>& field_updates,
        TABLE* table,
        std::string& error_msg);
    
    /**
     * Extract specific fields without fetching entire row
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param row_key Row key
     * @param field_indices Fields to extract
     * @param table Table metadata
     * @param field_values Output: Extracted field values
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError extract_fields(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& row_key,
        const std::vector<uint>& field_indices,
        TABLE* table,
        std::vector<std::string>& field_values,
        std::string& error_msg);
};

/**
 * Aggregation pushdown operations
 */
class AggregationOps {
public:
    /**
     * Perform COUNT(*) on a range
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param start_key Range start (inclusive)
     * @param end_key Range end (exclusive)
     * @param filter Optional filter function
     * @param count Output: Row count
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError count_range(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& start_key,
        const KVTKey& end_key,
        const KVTProcessFunc& filter,
        uint64_t& count,
        std::string& error_msg);
    
    /**
     * Perform SUM on a numeric column
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param start_key Range start (inclusive)
     * @param end_key Range end (exclusive)
     * @param field Field to sum
     * @param filter Optional filter function
     * @param sum Output: Sum result
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError sum_range(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& start_key,
        const KVTKey& end_key,
        Field* field,
        const KVTProcessFunc& filter,
        double& sum,
        std::string& error_msg);
    
    /**
     * Find MIN/MAX value in a range
     * 
     * @param tx_id Transaction ID
     * @param table_id Table ID
     * @param start_key Range start (inclusive)
     * @param end_key Range end (exclusive)
     * @param field Field to find min/max
     * @param find_max True for MAX, false for MIN
     * @param filter Optional filter function
     * @param result Output: Min/Max value
     * @param error_msg Error message if operation fails
     * @return KVTError status
     */
    static KVTError min_max_range(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& start_key,
        const KVTKey& end_key,
        Field* field,
        bool find_max,
        const KVTProcessFunc& filter,
        std::string& result,
        std::string& error_msg);
};

/**
 * Manager for atomic operations
 */
class AtomicOperationManager {
public:
    static AtomicOperationManager* get_instance();
    
    /**
     * Check if field supports atomic operations
     */
    bool supports_atomic_ops(Field* field);
    
    /**
     * Check if atomic increment is supported for field
     */
    bool supports_increment(Field* field);
    
    /**
     * Get statistics for atomic operations
     */
    struct AtomicStats {
        uint64_t increments;
        uint64_t conditional_updates;
        uint64_t compare_and_swaps;
        uint64_t string_appends;
        uint64_t partial_updates;
        uint64_t aggregations;
        
        void reset();
    };
    
    AtomicStats* get_stats() { return &stats; }

private:
    AtomicOperationManager();
    ~AtomicOperationManager();
    
    static AtomicOperationManager* instance;
    AtomicStats stats;
};

/**
 * Helper to build atomic operation from SQL UPDATE
 */
class AtomicUpdateBuilder {
public:
    /**
     * Analyze UPDATE statement for atomic operation potential
     * 
     * @param fields Fields being updated
     * @param values New values
     * @param table Table metadata
     * @return Vector of atomic operations that can be pushed down
     */
    static std::vector<kvt_pushdown::ColumnOperation> analyze_update(
        const std::vector<Field*>& fields,
        const std::vector<Item*>& values,
        TABLE* table);
    
    /**
     * Check if expression is an increment/decrement
     * 
     * @param field Field being updated
     * @param value_expr Expression for new value
     * @param delta Output: Increment delta if detected
     * @return True if increment pattern detected
     */
    static bool is_increment_pattern(
        Field* field,
        Item* value_expr,
        int64_t& delta);
    
    /**
     * Check if expression is an append operation
     * 
     * @param field Field being updated
     * @param value_expr Expression for new value
     * @param suffix Output: String to append if detected
     * @return True if append pattern detected
     */
    static bool is_append_pattern(
        Field* field,
        Item* value_expr,
        std::string& suffix);
};

} // namespace kvt_atomic

#endif // KVT_ATOMIC_OPERATIONS_H