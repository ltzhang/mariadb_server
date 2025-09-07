/*
   Copyright (c) 2025 KVT Storage Engine - MariaDB Specific Operations Implementation

   Implementation of MariaDB-specific KVT APIs for filter, aggregation,
   and other pushdown operations.
*/

#include "kvt_mariadb_ops.h"
#include "kvt_inc.h"
#include <algorithm>
#include <unordered_map>
#include <regex>
#include <cstring>
#include <numeric>

namespace kvt_mariadb {

// =============================================================================
// Internal Helper Functions
// =============================================================================

/**
 * Extract column value from row data
 */
static bool extract_column_value(
    const std::string& row_data,
    uint32_t column_index,
    const KVTColumnInfo& column_info,
    KVTValue& value)
{
    // Simplified implementation - assumes fixed-size columns for now
    // Real implementation would handle variable-length columns
    
    if (row_data.empty()) {
        value.is_null = true;
        return false;
    }
    
    // Calculate offset based on column index (simplified)
    size_t offset = column_index * 8;  // Assume 8 bytes per column
    
    if (row_data.size() < offset + 8) {
        value.is_null = true;
        return false;
    }
    
    // Check NULL bitmap (first byte of each column)
    if (row_data[offset] == 0xFF) {
        value.is_null = true;
        return true;
    }
    
    value.is_null = false;
    
    // Extract value based on type
    switch (column_info.type) {
        case KVTColumnType::INT32: {
            int32_t val;
            std::memcpy(&val, row_data.data() + offset + 1, sizeof(val));
            value.data = val;
            value.type = KVTColumnType::INT32;
            break;
        }
        case KVTColumnType::INT64: {
            int64_t val;
            std::memcpy(&val, row_data.data() + offset + 1, sizeof(val));
            value.data = val;
            value.type = KVTColumnType::INT64;
            break;
        }
        case KVTColumnType::DOUBLE: {
            double val;
            std::memcpy(&val, row_data.data() + offset + 1, sizeof(val));
            value.data = val;
            value.type = KVTColumnType::DOUBLE;
            break;
        }
        case KVTColumnType::VARCHAR: {
            // For VARCHAR, first 2 bytes are length
            uint16_t len;
            std::memcpy(&len, row_data.data() + offset + 1, sizeof(len));
            if (offset + 3 + len <= row_data.size()) {
                value.data = std::string(row_data.data() + offset + 3, len);
                value.type = KVTColumnType::VARCHAR;
            }
            break;
        }
        default:
            return false;
    }
    
    return true;
}

/**
 * Compare two values based on operator
 */
static bool compare_values(
    const KVTValue& left,
    KVTCompareOp op,
    const KVTValue& right)
{
    // Handle NULL comparisons
    if (op == KVTCompareOp::IS_NULL) {
        return left.is_null;
    }
    if (op == KVTCompareOp::IS_NOT_NULL) {
        return !left.is_null;
    }
    
    // NULL comparisons always return false (SQL semantics)
    if (left.is_null || right.is_null) {
        return false;
    }
    
    // Type checking and comparison
    if (left.type != right.type) {
        // Type coercion would go here
        return false;
    }
    
    switch (op) {
        case KVTCompareOp::EQ:
            return left.data == right.data;
        case KVTCompareOp::NE:
            return left.data != right.data;
        case KVTCompareOp::LT:
            return left.data < right.data;
        case KVTCompareOp::LE:
            return left.data <= right.data;
        case KVTCompareOp::GT:
            return left.data > right.data;
        case KVTCompareOp::GE:
            return left.data >= right.data;
        default:
            return false;
    }
}

/**
 * Check if value matches LIKE pattern
 */
static bool matches_like_pattern(const std::string& value, const std::string& pattern) {
    // Convert SQL LIKE pattern to regex
    std::string regex_pattern;
    regex_pattern.reserve(pattern.size() * 2);
    
    for (char c : pattern) {
        switch (c) {
            case '%':
                regex_pattern += ".*";
                break;
            case '_':
                regex_pattern += ".";
                break;
            case '.':
            case '^':
            case '$':
            case '*':
            case '+':
            case '?':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '\\':
            case '|':
                regex_pattern += "\\";
                regex_pattern += c;
                break;
            default:
                regex_pattern += c;
        }
    }
    
    try {
        std::regex re(regex_pattern, std::regex::icase);
        return std::regex_match(value, re);
    } catch (...) {
        return false;
    }
}

/**
 * Evaluate filter condition on a row
 */
static bool evaluate_filter(
    const std::string& row_data,
    const KVTFilterCondition& filter,
    const std::vector<KVTColumnInfo>& schema)
{
    if (filter.column_index >= schema.size()) {
        return false;
    }
    
    KVTValue column_value;
    if (!extract_column_value(row_data, filter.column_index, 
                             schema[filter.column_index], column_value)) {
        return false;
    }
    
    // Handle IN operator
    if (!filter.in_values.empty()) {
        for (const auto& val : filter.in_values) {
            if (compare_values(column_value, KVTCompareOp::EQ, val)) {
                return true;
            }
        }
        return false;
    }
    
    // Handle BETWEEN operator
    if (filter.high_value.has_value()) {
        return compare_values(column_value, KVTCompareOp::GE, filter.value) &&
               compare_values(column_value, KVTCompareOp::LE, filter.high_value.value());
    }
    
    // Handle regular comparison
    return compare_values(column_value, filter.op, filter.value);
}

/**
 * Evaluate composite filter on a row
 */
static bool evaluate_composite_filter(
    const std::string& row_data,
    const KVTCompositeFilter& filter,
    const std::vector<KVTColumnInfo>& schema);

static bool evaluate_filter_variant(
    const std::string& row_data,
    const std::variant<KVTFilterCondition, KVTCompositeFilter>& filter,
    const std::vector<KVTColumnInfo>& schema)
{
    if (std::holds_alternative<KVTFilterCondition>(filter)) {
        return evaluate_filter(row_data, std::get<KVTFilterCondition>(filter), schema);
    } else {
        return evaluate_composite_filter(row_data, std::get<KVTCompositeFilter>(filter), schema);
    }
}

static bool evaluate_composite_filter(
    const std::string& row_data,
    const KVTCompositeFilter& filter,
    const std::vector<KVTColumnInfo>& schema)
{
    switch (filter.op) {
        case KVTLogicalOp::AND: {
            for (const auto& cond : filter.conditions) {
                if (!evaluate_filter_variant(row_data, cond, schema)) {
                    return false;
                }
            }
            return true;
        }
        case KVTLogicalOp::OR: {
            for (const auto& cond : filter.conditions) {
                if (evaluate_filter_variant(row_data, cond, schema)) {
                    return true;
                }
            }
            return false;
        }
        case KVTLogicalOp::NOT: {
            if (filter.conditions.empty()) {
                return true;
            }
            return !evaluate_filter_variant(row_data, filter.conditions[0], schema);
        }
        default:
            return false;
    }
}

/**
 * Convert row data to KVTRow structure
 */
static KVTRow decode_row(const KVTKey& key, const std::string& value,
                         const std::vector<KVTColumnInfo>& schema)
{
    KVTRow row;
    row.key = key;
    row.columns.reserve(schema.size());
    
    for (size_t i = 0; i < schema.size(); i++) {
        KVTValue col_value;
        extract_column_value(value, i, schema[i], col_value);
        row.columns.push_back(col_value);
    }
    
    return row;
}

// =============================================================================
// Schema Management (Internal Cache)
// =============================================================================

static std::unordered_map<uint64_t, std::vector<KVTColumnInfo>> g_table_schemas;
static std::mutex g_schema_mutex;

static bool get_table_schema(uint64_t table_id, std::vector<KVTColumnInfo>& schema) {
    std::lock_guard<std::mutex> lock(g_schema_mutex);
    auto it = g_table_schemas.find(table_id);
    if (it != g_table_schemas.end()) {
        schema = it->second;
        return true;
    }
    return false;
}

// =============================================================================
// Filter API Implementations
// =============================================================================

KVTError kvt_filter_eq(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const KVTValue& value,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg)
{
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    // Create filter condition
    KVTFilterCondition filter;
    filter.column_index = column_index;
    filter.op = KVTCompareOp::EQ;
    filter.value = value;
    
    // Scan and filter
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTError err = kvt_scan(tx_id, table_id, start_key, end_key, 
                           limit * 2, scan_results, error_msg);  // Scan more for filtering
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return err;
    }
    
    // Apply filter
    results.clear();
    for (const auto& [key, value_str] : scan_results) {
        if (evaluate_filter(value_str, filter, schema)) {
            results.push_back(decode_row(key, value_str, schema));
            if (results.size() >= limit) {
                break;
            }
        }
    }
    
    return KVTError::SUCCESS;
}

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
    std::string& error_msg)
{
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    // Create filter condition
    KVTFilterCondition filter;
    filter.column_index = column_index;
    filter.op = op;
    filter.value = value;
    
    // Scan and filter
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTError err = kvt_scan(tx_id, table_id, start_key, end_key,
                           limit * 3, scan_results, error_msg);  // Scan more for filtering
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return err;
    }
    
    // Apply filter
    results.clear();
    for (const auto& [key, value_str] : scan_results) {
        if (evaluate_filter(value_str, filter, schema)) {
            results.push_back(decode_row(key, value_str, schema));
            if (results.size() >= limit) {
                break;
            }
        }
    }
    
    return KVTError::SUCCESS;
}

KVTError kvt_filter_in(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const std::vector<KVTValue>& values,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg)
{
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    // Create filter condition
    KVTFilterCondition filter;
    filter.column_index = column_index;
    filter.op = KVTCompareOp::EQ;  // Will check against multiple values
    filter.in_values = values;
    
    // Scan and filter
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTError err = kvt_scan(tx_id, table_id, start_key, end_key,
                           limit * values.size(), scan_results, error_msg);
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return err;
    }
    
    // Apply filter
    results.clear();
    for (const auto& [key, value_str] : scan_results) {
        if (evaluate_filter(value_str, filter, schema)) {
            results.push_back(decode_row(key, value_str, schema));
            if (results.size() >= limit) {
                break;
            }
        }
    }
    
    return KVTError::SUCCESS;
}

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
    std::string& error_msg)
{
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    // Create filter condition
    KVTFilterCondition filter;
    filter.column_index = column_index;
    filter.op = KVTCompareOp::GE;  // Will check both bounds
    filter.value = low;
    filter.high_value = high;
    
    // Scan and filter
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTError err = kvt_scan(tx_id, table_id, start_key, end_key,
                           limit * 2, scan_results, error_msg);
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return err;
    }
    
    // Apply filter
    results.clear();
    for (const auto& [key, value_str] : scan_results) {
        if (evaluate_filter(value_str, filter, schema)) {
            results.push_back(decode_row(key, value_str, schema));
            if (results.size() >= limit) {
                break;
            }
        }
    }
    
    return KVTError::SUCCESS;
}

KVTError kvt_filter_like(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const std::string& pattern,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg)
{
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    if (column_index >= schema.size()) {
        error_msg = "Invalid column index";
        return KVTError::UNKNOWN_ERROR;
    }
    
    // Scan and filter
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTError err = kvt_scan(tx_id, table_id, start_key, end_key,
                           limit * 3, scan_results, error_msg);
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return err;
    }
    
    // Apply LIKE filter
    results.clear();
    for (const auto& [key, value_str] : scan_results) {
        KVTValue col_value;
        if (extract_column_value(value_str, column_index, schema[column_index], col_value)) {
            if (!col_value.is_null && 
                std::holds_alternative<std::string>(col_value.data)) {
                const std::string& str_val = std::get<std::string>(col_value.data);
                if (matches_like_pattern(str_val, pattern)) {
                    results.push_back(decode_row(key, value_str, schema));
                    if (results.size() >= limit) {
                        break;
                    }
                }
            }
        }
    }
    
    return KVTError::SUCCESS;
}

KVTError kvt_filter_composite(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTCompositeFilter& filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results,
    std::string& error_msg)
{
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    // Scan and filter
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTError err = kvt_scan(tx_id, table_id, start_key, end_key,
                           limit * 4, scan_results, error_msg);
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return err;
    }
    
    // Apply composite filter
    results.clear();
    for (const auto& [key, value_str] : scan_results) {
        if (evaluate_composite_filter(value_str, filter, schema)) {
            results.push_back(decode_row(key, value_str, schema));
            if (results.size() >= limit) {
                break;
            }
        }
    }
    
    return KVTError::SUCCESS;
}

// =============================================================================
// Aggregation API Implementations
// =============================================================================

KVTError kvt_aggregate_count(
    uint64_t tx_id,
    uint64_t table_id,
    int32_t column_index,
    const KVTCompositeFilter* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    uint64_t& count,
    std::string& error_msg)
{
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    // Scan all rows
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTError err = kvt_scan(tx_id, table_id, start_key, end_key,
                           SIZE_MAX, scan_results, error_msg);
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return err;
    }
    
    count = 0;
    
    // Count rows
    for (const auto& [key, value_str] : scan_results) {
        // Apply filter if present
        if (filter) {
            if (!evaluate_composite_filter(value_str, *filter, schema)) {
                continue;
            }
        }
        
        // For COUNT(column), check if column is not NULL
        if (column_index >= 0) {
            KVTValue col_value;
            if (!extract_column_value(value_str, column_index, 
                                     schema[column_index], col_value) ||
                col_value.is_null) {
                continue;
            }
        }
        
        count++;
    }
    
    return KVTError::SUCCESS;
}

KVTError kvt_aggregate_sum(
    uint64_t tx_id,
    uint64_t table_id,
    uint32_t column_index,
    const KVTCompositeFilter* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    KVTValue& sum,
    std::string& error_msg)
{
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    if (column_index >= schema.size()) {
        error_msg = "Invalid column index";
        return KVTError::UNKNOWN_ERROR;
    }
    
    // Scan all rows
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTError err = kvt_scan(tx_id, table_id, start_key, end_key,
                           SIZE_MAX, scan_results, error_msg);
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return err;
    }
    
    // Initialize sum based on column type
    double double_sum = 0.0;
    int64_t int_sum = 0;
    bool has_value = false;
    bool is_integer = (schema[column_index].type == KVTColumnType::INT32 ||
                      schema[column_index].type == KVTColumnType::INT64);
    
    // Calculate sum
    for (const auto& [key, value_str] : scan_results) {
        // Apply filter if present
        if (filter) {
            if (!evaluate_composite_filter(value_str, *filter, schema)) {
                continue;
            }
        }
        
        KVTValue col_value;
        if (!extract_column_value(value_str, column_index, 
                                 schema[column_index], col_value) ||
            col_value.is_null) {
            continue;
        }
        
        has_value = true;
        
        // Add to sum based on type
        if (std::holds_alternative<int32_t>(col_value.data)) {
            int_sum += std::get<int32_t>(col_value.data);
        } else if (std::holds_alternative<int64_t>(col_value.data)) {
            int_sum += std::get<int64_t>(col_value.data);
        } else if (std::holds_alternative<double>(col_value.data)) {
            double_sum += std::get<double>(col_value.data);
            is_integer = false;
        } else if (std::holds_alternative<float>(col_value.data)) {
            double_sum += std::get<float>(col_value.data);
            is_integer = false;
        }
    }
    
    // Set result
    if (!has_value) {
        sum.is_null = true;
    } else if (is_integer) {
        sum = KVTValue(int_sum);
    } else {
        sum = KVTValue(double_sum + int_sum);
    }
    
    return KVTError::SUCCESS;
}

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
    std::string& error_msg)
{
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    if (column_index >= schema.size()) {
        error_msg = "Invalid column index";
        return KVTError::UNKNOWN_ERROR;
    }
    
    // Scan all rows
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTError err = kvt_scan(tx_id, table_id, start_key, end_key,
                           SIZE_MAX, scan_results, error_msg);
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return err;
    }
    
    found = false;
    
    // Find min/max
    for (const auto& [key, value_str] : scan_results) {
        // Apply filter if present
        if (filter) {
            if (!evaluate_composite_filter(value_str, *filter, schema)) {
                continue;
            }
        }
        
        KVTValue col_value;
        if (!extract_column_value(value_str, column_index, 
                                 schema[column_index], col_value) ||
            col_value.is_null) {
            continue;
        }
        
        if (!found) {
            result = col_value;
            found = true;
        } else {
            bool should_update = find_max ? 
                (col_value.data > result.data) : 
                (col_value.data < result.data);
            if (should_update) {
                result = col_value;
            }
        }
    }
    
    if (!found) {
        result.is_null = true;
    }
    
    return KVTError::SUCCESS;
}

// =============================================================================
// Atomic Operation API Implementations
// =============================================================================

KVTError kvt_atomic_increment(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    uint32_t column_index,
    int64_t delta,
    int64_t& old_value,
    std::string& error_msg)
{
    // Get current value
    std::string current_row;
    KVTError err = kvt_get(tx_id, table_id, row_key, current_row, error_msg);
    
    if (err != KVTError::SUCCESS) {
        return err;
    }
    
    // Get table schema
    std::vector<KVTColumnInfo> schema;
    if (!get_table_schema(table_id, schema)) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    
    if (column_index >= schema.size()) {
        error_msg = "Invalid column index";
        return KVTError::UNKNOWN_ERROR;
    }
    
    // Extract current value
    KVTValue col_value;
    if (!extract_column_value(current_row, column_index, 
                             schema[column_index], col_value)) {
        error_msg = "Failed to extract column value";
        return KVTError::UNKNOWN_ERROR;
    }
    
    // Get old value
    if (std::holds_alternative<int64_t>(col_value.data)) {
        old_value = std::get<int64_t>(col_value.data);
    } else if (std::holds_alternative<int32_t>(col_value.data)) {
        old_value = std::get<int32_t>(col_value.data);
    } else {
        error_msg = "Column is not numeric";
        return KVTError::UNKNOWN_ERROR;
    }
    
    // Calculate new value
    int64_t new_value = old_value + delta;
    
    // Update the row (simplified - assumes fixed layout)
    size_t offset = column_index * 8 + 1;  // Skip NULL byte
    if (current_row.size() >= offset + sizeof(int64_t)) {
        std::memcpy(current_row.data() + offset, &new_value, sizeof(new_value));
        
        // Write back
        err = kvt_set(tx_id, table_id, row_key, current_row, error_msg);
        if (err != KVTError::SUCCESS) {
            return err;
        }
    }
    
    return KVTError::SUCCESS;
}

// =============================================================================
// Schema Registration API
// =============================================================================

KVTError kvt_register_table_schema(
    uint64_t table_id,
    const std::vector<KVTColumnInfo>& columns,
    std::string& error_msg)
{
    std::lock_guard<std::mutex> lock(g_schema_mutex);
    g_table_schemas[table_id] = columns;
    return KVTError::SUCCESS;
}

KVTError kvt_get_table_schema(
    uint64_t table_id,
    std::vector<KVTColumnInfo>& columns,
    std::string& error_msg)
{
    std::lock_guard<std::mutex> lock(g_schema_mutex);
    auto it = g_table_schemas.find(table_id);
    if (it == g_table_schemas.end()) {
        error_msg = "Table schema not found";
        return KVTError::TABLE_NOT_FOUND;
    }
    columns = it->second;
    return KVTError::SUCCESS;
}

} // namespace kvt_mariadb