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

#ifndef KVT_QUERY_OPTIMIZER_H
#define KVT_QUERY_OPTIMIZER_H

#include "my_global.h"
#include "kvt/kvt_inc.h"
#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <map>
#include <mutex>

// Forward declarations
class Item;
typedef Item COND;  // COND is a typedef for Item in MariaDB
class Field;
class TABLE;
class THD;

namespace kvt_optimizer {

// Query statistics for learning and optimization
struct QueryStatistics {
  uint64_t rows_examined;
  uint64_t rows_returned;
  uint64_t bytes_read;
  uint64_t bytes_returned;
  std::chrono::milliseconds execution_time;
  double selectivity;
  
  QueryStatistics() : 
    rows_examined(0), rows_returned(0), 
    bytes_read(0), bytes_returned(0),
    execution_time(0), selectivity(1.0) {}
  
  void update_selectivity() {
    if (rows_examined > 0) {
      selectivity = static_cast<double>(rows_returned) / rows_examined;
    }
  }
  
  bool is_selective() const {
    return selectivity < 0.3;  // Less than 30% of rows returned
  }
};

// Condition analysis for pushdown
class ConditionAnalyzer {
public:
  enum ConditionType {
    SIMPLE_COMPARISON,  // =, <, >, <=, >=, !=
    RANGE_CONDITION,    // BETWEEN, IN
    PATTERN_MATCH,      // LIKE
    NULL_CHECK,         // IS NULL, IS NOT NULL
    COMPLEX,            // AND, OR combinations
    NOT_PUSHABLE        // Cannot be pushed to KVT
  };
  
  struct AnalyzedCondition {
    ConditionType type;
    std::string field_name;
    std::string operator_type;
    std::string value;
    bool is_pushable;
    std::vector<AnalyzedCondition> sub_conditions;  // For AND/OR
  };
  
  ConditionAnalyzer();
  ~ConditionAnalyzer();
  
  // Analyze a condition tree
  AnalyzedCondition analyze(const COND* cond, TABLE* table);
  
  // Check if condition can be pushed to KVT
  bool is_pushable(const AnalyzedCondition& condition);
  
  // Generate KVT filter function
  std::string generate_kvt_filter(const AnalyzedCondition& condition);
  
  // Get field index from condition
  int get_field_index(const Item* item, TABLE* table);
  
private:
  // Helper methods
  AnalyzedCondition analyze_comparison(const Item* item, TABLE* table);
  AnalyzedCondition analyze_between(const Item* item, TABLE* table);
  AnalyzedCondition analyze_in(const Item* item, TABLE* table);
  AnalyzedCondition analyze_like(const Item* item, TABLE* table);
  AnalyzedCondition analyze_null(const Item* item, TABLE* table);
  AnalyzedCondition analyze_bool_func(const Item* item, TABLE* table);
};

// Column projection optimizer
class ColumnProjection {
public:
  ColumnProjection(TABLE* table);
  ~ColumnProjection();
  
  // Mark column as used in query
  void mark_column_used(uint field_index);
  
  // Check if projection is beneficial
  bool should_use_projection() const;
  
  // Get list of needed columns
  std::vector<uint> get_needed_columns() const;
  
  // Calculate projected row size
  size_t calculate_projected_size() const;
  
  // Generate KVT projection function
  std::string generate_projection_function() const;
  
  // Decode only selected columns
  int decode_projected_columns(const std::string& value, 
                              uchar* buf,
                              const std::vector<uint>& columns);
  
private:
  TABLE* table_;
  std::vector<bool> column_used_;
  size_t total_columns_;
  size_t used_columns_;
};

// Batch operation optimizer
class BatchOptimizer {
public:
  // Batch operation types
  enum BatchType {
    BATCH_INSERT,
    BATCH_UPDATE,
    BATCH_DELETE,
    BATCH_MIXED
  };
  
  BatchOptimizer();
  ~BatchOptimizer();
  
  // Calculate optimal batch size based on available memory and row size
  size_t calculate_optimal_batch_size(size_t row_size, 
                                     size_t available_memory,
                                     double selectivity = 1.0);
  
  // Check if batching should be used
  bool should_use_batching(size_t expected_rows, BatchType type);
  
  // Aggregate similar operations
  int aggregate_operations(KVTBatchOps& ops);
  
  // Execute batch with optimal chunking
  int execute_batch(uint64_t tx_id, uint64_t table_id,
                   const KVTBatchOps& ops);
  
  // Get current batch statistics
  const QueryStatistics& get_statistics() const { return stats_; }
  
private:
  QueryStatistics stats_;
  size_t max_batch_size_;
  size_t min_batch_size_;
  
  // Memory management
  size_t get_available_memory() const;
  void update_batch_statistics(size_t batch_size, 
                              std::chrono::milliseconds time);
};

// Range query optimizer
class RangeOptimizer {
public:
  struct RangeSpec {
    std::string start_key;
    std::string end_key;
    bool include_start;
    bool include_end;
    std::string filter_function;  // Optional KVT filter
  };
  
  RangeOptimizer();
  ~RangeOptimizer();
  
  // Analyze range conditions
  RangeSpec analyze_range(const COND* cond, TABLE* table);
  
  // Optimize range scan with pushdown
  int optimize_range_scan(uint64_t tx_id, uint64_t table_id,
                         const RangeSpec& range,
                         std::vector<std::pair<KVTKey, std::string>>& results);
  
  // Estimate range selectivity
  double estimate_selectivity(const RangeSpec& range);
  
  // Check if parallel scan would help
  bool should_parallelize(const RangeSpec& range);
  
private:
  // Helper methods
  std::string encode_range_key(const Item* item, TABLE* table);
  bool is_range_condition(const Item* item);
};

// Main query optimizer coordinator
class KVTQueryOptimizer {
private:
  // Singleton instance
  static KVTQueryOptimizer* instance;
  static std::mutex instance_mutex;
  
  // Components
  std::unique_ptr<ConditionAnalyzer> condition_analyzer_;
  std::unique_ptr<BatchOptimizer> batch_optimizer_;
  std::unique_ptr<RangeOptimizer> range_optimizer_;
  
  // Statistics tracking
  std::map<std::string, QueryStatistics> query_stats_;
  std::mutex stats_mutex_;
  
  // Private constructor for singleton
  KVTQueryOptimizer();
  
public:
  ~KVTQueryOptimizer();
  
  // Singleton access
  static KVTQueryOptimizer* get_instance();
  static void cleanup_instance();
  
  // Main optimization entry point
  int optimize_query(THD* thd, TABLE* table, const COND* where_cond);
  
  // Condition pushdown
  bool push_condition(uint64_t tx_id, uint64_t table_id,
                     const COND* cond, TABLE* table);
  
  // Column projection
  ColumnProjection* create_projection(TABLE* table);
  
  // Batch operations
  BatchOptimizer* get_batch_optimizer() { return batch_optimizer_.get(); }
  
  // Range optimization
  RangeOptimizer* get_range_optimizer() { return range_optimizer_.get(); }
  
  // Statistics management
  void record_query_stats(const std::string& query_id, 
                         const QueryStatistics& stats);
  QueryStatistics get_query_stats(const std::string& query_id);
  
  // Learning and adaptation
  void update_optimization_strategy(const std::string& query_id);
  double get_learned_selectivity(const std::string& pattern);
  
  // Performance monitoring
  void start_monitoring();
  void stop_monitoring();
  void dump_statistics();
  
private:
  // Helper methods
  std::string generate_query_id(TABLE* table, const COND* cond);
  void cleanup_old_statistics();
};

// Utility functions for optimization
namespace OptimizationUtils {
  // Check if a condition is simple enough to push
  bool is_simple_condition(const COND* cond);
  
  // Estimate row size after projection
  size_t estimate_row_size(TABLE* table, const std::vector<uint>& columns);
  
  // Calculate memory pressure
  double get_memory_pressure();
  
  // Check if optimization is enabled
  bool is_optimization_enabled();
  
  // Format statistics for logging
  std::string format_statistics(const QueryStatistics& stats);
}

} // namespace kvt_optimizer

#endif // KVT_QUERY_OPTIMIZER_H