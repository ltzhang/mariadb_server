/*
   Copyright (c) 2025 KVT Storage Engine

   Handler integration with MariaDB-specific KVT APIs
*/

#ifndef KVT_MARIADB_HANDLER_H
#define KVT_MARIADB_HANDLER_H

#include "my_global.h"
#include "sql_priv.h"
#include "my_decimal.h"
#include "sql_class.h"
#include "item.h"
#include "item_row.h"
#include "item_func.h"
#include "item_sum.h"
#include "item_cmpfunc.h"
#include "kvt/kvt_mariadb_ops.h"
#include <memory>

namespace kvt_handler {

using namespace kvt_mariadb;

/**
 * Condition analyzer for pushdown
 */
class ConditionAnalyzer {
public:
    /**
     * Analyze a MariaDB condition and convert to KVT filter
     */
    static bool analyze_condition(
        const COND* cond,
        TABLE* table,
        KVTCompositeFilter& filter,
        std::string& reason);
    
    /**
     * Check if condition is pushable
     */
    static bool is_pushable(const COND* cond);
    
private:
    static bool convert_item_to_filter(
        const Item* item,
        TABLE* table,
        KVTFilterCondition& filter);
    
    static bool convert_comparison(
        const Item_func* func,
        TABLE* table,
        KVTFilterCondition& filter);
    
    static bool convert_in_condition(
        const Item_func_in* func,
        TABLE* table,
        KVTFilterCondition& filter);
    
    static bool convert_between(
        const Item_func_between* func,
        TABLE* table,
        KVTFilterCondition& filter);
    
    static bool convert_like(
        const Item_func_like* func,
        TABLE* table,
        KVTFilterCondition& filter);
    
    static bool item_to_kvt_value(
        Item* item,
        KVTValue& value);
    
    static KVTCompareOp func_type_to_compare_op(
        Item_func::Functype type);
};

/**
 * Aggregation analyzer for pushdown
 */
class AggregationAnalyzer {
public:
    /**
     * Check if aggregation can be pushed down
     */
    static bool can_pushdown(Item_sum* item);
    
    /**
     * Convert Item_sum to KVTAggregateSpec
     */
    static bool convert_aggregation(
        Item_sum* item,
        TABLE* table,
        KVTAggregateSpec& spec);
    
    /**
     * Extract GROUP BY information
     */
    static bool extract_group_by(
        ORDER* group_list,
        TABLE* table,
        std::vector<uint32_t>& group_columns);
};

/**
 * Schema converter between MariaDB and KVT
 */
class SchemaConverter {
public:
    /**
     * Convert MariaDB table to KVT schema
     */
    static void convert_table_schema(
        TABLE* table,
        std::vector<KVTColumnInfo>& columns);
    
    /**
     * Convert Field type to KVTColumnType
     */
    static KVTColumnType field_type_to_kvt_type(Field* field);
    
    /**
     * Convert MariaDB row to KVTRow
     */
    static KVTRow convert_row_to_kvt(
        const uchar* buf,
        TABLE* table,
        const KVTKey& key);
    
    /**
     * Convert KVTRow to MariaDB row
     */
    static int convert_kvt_to_row(
        const KVTRow& kvt_row,
        uchar* buf,
        TABLE* table);
    
    /**
     * Extract KVT key from MariaDB row
     */
    static KVTKey extract_row_key(
        const uchar* buf,
        TABLE* table,
        uint64_t row_id);
};

/**
 * Pushdown executor using predefined APIs
 */
class PushdownExecutor {
public:
    /**
     * Execute filter pushdown
     */
    static int execute_filter(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTCompositeFilter& filter,
        const KVTKey& start_key,
        const KVTKey& end_key,
        size_t limit,
        std::vector<KVTRow>& results);
    
    /**
     * Execute aggregation pushdown
     */
    static int execute_aggregation(
        uint64_t tx_id,
        uint64_t table_id,
        const std::vector<KVTAggregateSpec>& aggregates,
        const KVTCompositeFilter* filter,
        const KVTKey& start_key,
        const KVTKey& end_key,
        std::vector<KVTValue>& results);
    
    /**
     * Execute GROUP BY pushdown
     */
    static int execute_group_by(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTGroupBySpec& spec,
        const KVTCompositeFilter* where_filter,
        const KVTKey& start_key,
        const KVTKey& end_key,
        std::vector<KVTGroupResult>& results);
    
    /**
     * Execute atomic increment
     */
    static int execute_increment(
        uint64_t tx_id,
        uint64_t table_id,
        const KVTKey& row_key,
        uint32_t column_index,
        int64_t delta,
        int64_t& old_value);
    
    /**
     * Execute batch insert
     */
    static int execute_batch_insert(
        uint64_t tx_id,
        uint64_t table_id,
        const std::vector<KVTRow>& rows,
        KVTConflictAction on_conflict);
    
    /**
     * Execute index-only scan
     */
    static int execute_index_only_scan(
        uint64_t tx_id,
        uint64_t table_id,
        uint32_t index_id,
        const std::vector<uint32_t>& column_indices,
        const KVTKey& start_key,
        const KVTKey& end_key,
        size_t limit,
        std::vector<KVTPartialRow>& results);
};

/**
 * Statistics manager for optimization
 */
class StatisticsManager {
public:
    /**
     * Update table statistics
     */
    static int update_table_stats(
        uint64_t table_id,
        uint64_t row_count);
    
    /**
     * Get cardinality estimate
     */
    static int estimate_cardinality(
        uint64_t table_id,
        const KVTCompositeFilter& filter,
        uint64_t& estimated_rows);
    
    /**
     * Analyze table
     */
    static int analyze_table(
        uint64_t table_id,
        const std::vector<uint32_t>& columns);
    
    /**
     * Get column statistics
     */
    static int get_column_stats(
        uint64_t table_id,
        uint32_t column_index,
        KVTColumnStats& stats);
};

/**
 * Batch operation manager
 */
class BatchManager {
public:
    BatchManager(uint64_t tx_id, uint64_t table_id)
        : tx_id_(tx_id), table_id_(table_id) {}
    
    /**
     * Add row to batch
     */
    void add_row(const KVTRow& row);
    
    /**
     * Flush batch
     */
    int flush(KVTConflictAction on_conflict = KVTConflictAction::ERROR);
    
    /**
     * Clear batch
     */
    void clear();
    
    /**
     * Get batch size
     */
    size_t size() const { return batch_.size(); }
    
    /**
     * Set batch size limit
     */
    void set_batch_limit(size_t limit) { batch_limit_ = limit; }
    
private:
    uint64_t tx_id_;
    uint64_t table_id_;
    std::vector<KVTRow> batch_;
    size_t batch_limit_ = 1000;
};

/**
 * Pushdown decision maker
 */
class PushdownDecision {
public:
    enum Decision {
        NO_PUSHDOWN,
        FILTER_ONLY,
        AGGREGATION_ONLY,
        FILTER_AND_AGGREGATION,
        INDEX_ONLY_SCAN,
        FULL_PUSHDOWN
    };
    
    /**
     * Decide what to push down based on query analysis
     */
    static Decision make_decision(
        const COND* where_clause,
        const std::vector<Item_sum*>& aggregates,
        ORDER* group_by,
        TABLE* table,
        double& estimated_benefit);
    
    /**
     * Check if index-only scan is beneficial
     */
    static bool should_use_index_only(
        uint index_id,
        MY_BITMAP* read_set,
        TABLE* table);
    
    /**
     * Estimate filter selectivity
     */
    static double estimate_selectivity(
        const KVTCompositeFilter& filter,
        uint64_t table_id);
};

/**
 * Error handler for KVT operations
 */
class ErrorHandler {
public:
    /**
     * Convert KVT error to MySQL error
     */
    static int kvt_to_mysql_error(KVTError err);
    
    /**
     * Set MySQL error message
     */
    static void set_error_message(THD* thd, const std::string& msg);
    
    /**
     * Log KVT operation for debugging
     */
    static void log_operation(
        const char* operation,
        uint64_t tx_id,
        uint64_t table_id,
        KVTError result);
};

/**
 * Performance statistics for pushdown operations
 */
class PushdownStats {
public:
    struct Stats {
        uint64_t filter_pushdowns = 0;
        uint64_t aggregation_pushdowns = 0;
        uint64_t index_only_scans = 0;
        uint64_t atomic_operations = 0;
        uint64_t batch_operations = 0;
        uint64_t rows_filtered_at_storage = 0;
        uint64_t bytes_saved = 0;
        uint64_t pushdown_failures = 0;
        
        void reset() {
            *this = Stats();
        }
    };
    
    static Stats& get_stats() {
        static Stats stats;
        return stats;
    }
    
    static void record_filter_pushdown(bool success, uint64_t rows_filtered = 0);
    static void record_aggregation_pushdown(bool success);
    static void record_index_only_scan(uint64_t rows);
    static void record_atomic_operation();
    static void record_batch_operation(size_t batch_size);
    static void print_stats();
};

} // namespace kvt_handler

#endif // KVT_MARIADB_HANDLER_H