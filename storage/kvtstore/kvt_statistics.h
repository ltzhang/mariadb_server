#ifndef KVT_STATISTICS_H
#define KVT_STATISTICS_H

#include <cstdint>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include "kvt/kvt_inc.h"

namespace kvt {

// Statistics key space
constexpr uint8_t STATS_KEYSPACE = 0x08;

// Statistics types
enum StatsType : uint8_t {
    TABLE_STATS = 0x01,
    INDEX_STATS = 0x02,
    COLUMN_STATS = 0x03
};

// Table statistics structure
struct TableStats {
    uint64_t row_count;
    uint64_t data_size;
    uint64_t index_size;
    uint64_t avg_row_length;
    uint64_t auto_increment_value;
    uint64_t check_time;
    uint64_t update_time;
    
    TableStats() : row_count(0), data_size(0), index_size(0), 
                   avg_row_length(0), auto_increment_value(0),
                   check_time(0), update_time(0) {}
};

// Index statistics structure
struct IndexStats {
    uint32_t index_id;
    uint64_t cardinality;      // Number of unique values
    uint64_t pages;             // Estimated pages
    uint32_t tree_height;       // B-tree height
    uint64_t entry_count;       // Total entries
    double selectivity;         // 0.0 to 1.0
    
    IndexStats() : index_id(0), cardinality(0), pages(0), 
                   tree_height(0), entry_count(0), selectivity(1.0) {}
};

// Column statistics structure
struct ColumnStats {
    uint32_t column_id;
    uint64_t null_count;
    uint64_t distinct_values;
    double min_value;
    double max_value;
    double avg_length;
    
    ColumnStats() : column_id(0), null_count(0), distinct_values(0),
                    min_value(0), max_value(0), avg_length(0) {}
};

// HyperLogLog for cardinality estimation
class HyperLogLog {
public:
    HyperLogLog(uint8_t precision = 14);
    ~HyperLogLog();
    
    void add(const void* data, size_t len);
    uint64_t estimate() const;
    void merge(const HyperLogLog& other);
    void clear();
    
    // Serialize/deserialize for persistence
    std::string serialize() const;
    void deserialize(const std::string& data);
    
private:
    uint8_t precision_;
    uint32_t register_count_;
    std::vector<uint8_t> registers_;
    
    uint32_t hash(const void* data, size_t len) const;
    uint8_t leading_zeros(uint32_t hash_value) const;
};

// Statistics manager class
class StatisticsManager {
public:
    static StatisticsManager* get_instance();
    static void shutdown();
    
    // Table statistics
    int update_table_stats(uint64_t txn_id, uint64_t table_id, 
                          const TableStats& stats);
    int get_table_stats(uint64_t txn_id, uint64_t table_id, 
                       TableStats& stats);
    
    // Index statistics
    int update_index_stats(uint64_t txn_id, uint64_t table_id,
                          uint32_t index_id, const IndexStats& stats);
    int get_index_stats(uint64_t txn_id, uint64_t table_id,
                       uint32_t index_id, IndexStats& stats);
    int get_all_index_stats(uint64_t txn_id, uint64_t table_id,
                           std::vector<IndexStats>& stats);
    
    // Column statistics
    int update_column_stats(uint64_t txn_id, uint64_t table_id,
                           uint32_t column_id, const ColumnStats& stats);
    int get_column_stats(uint64_t txn_id, uint64_t table_id,
                        uint32_t column_id, ColumnStats& stats);
    
    // Incremental updates
    int increment_row_count(uint64_t txn_id, uint64_t table_id, int64_t delta);
    int update_auto_increment(uint64_t txn_id, uint64_t table_id, uint64_t value);
    
    // Cardinality estimation
    int update_index_cardinality(uint64_t txn_id, uint64_t table_id,
                                uint32_t index_id, const void* key_data,
                                size_t key_len);
    uint64_t estimate_cardinality(uint64_t txn_id, uint64_t table_id,
                                  uint32_t index_id);
    
    // ANALYZE TABLE support
    int analyze_table(uint64_t txn_id, uint64_t table_id);
    int analyze_index(uint64_t txn_id, uint64_t table_id, uint32_t index_id);
    
    // Cache management
    void clear_cache(uint64_t table_id = 0);
    void refresh_cache(uint64_t txn_id, uint64_t table_id);
    
private:
    StatisticsManager();
    ~StatisticsManager();
    
    static StatisticsManager* instance_;
    static std::mutex instance_mutex_;
    
    // Statistics cache
    struct StatsCache {
        std::unordered_map<uint64_t, TableStats> table_cache;
        std::unordered_map<uint64_t, std::vector<IndexStats>> index_cache;
        std::unordered_map<uint64_t, std::vector<ColumnStats>> column_cache;
        std::mutex mutex;
        
        void clear(uint64_t table_id = 0);
    };
    StatsCache cache_;
    
    // HyperLogLog estimators for each index
    struct CardinalityEstimator {
        std::unordered_map<uint64_t, std::unique_ptr<HyperLogLog>> estimators;
        std::mutex mutex;
    };
    CardinalityEstimator cardinality_;
    
    // Helper functions
    void make_stats_key(uint8_t* key_buf, size_t* key_len,
                       uint64_t table_id, StatsType type,
                       uint32_t sub_id = 0);
    
    int persist_table_stats(uint64_t txn_id, uint64_t table_id,
                           const TableStats& stats);
    int load_table_stats(uint64_t txn_id, uint64_t table_id,
                        TableStats& stats);
    
    int persist_index_stats(uint64_t txn_id, uint64_t table_id,
                           uint32_t index_id, const IndexStats& stats);
    int load_index_stats(uint64_t txn_id, uint64_t table_id,
                        uint32_t index_id, IndexStats& stats);
    
    // Full table scan for ANALYZE
    int scan_table_full(uint64_t txn_id, uint64_t table_id,
                       TableStats& stats);
    int scan_index_full(uint64_t txn_id, uint64_t table_id,
                       uint32_t index_id, IndexStats& stats);
};

// Sampling-based statistics collector
class StatisticsSampler {
public:
    StatisticsSampler(uint64_t txn_id, uint64_t table_id);
    ~StatisticsSampler();
    
    // Sample-based estimation
    int estimate_table_stats(TableStats& stats, uint32_t sample_size = 1000);
    int estimate_index_selectivity(uint32_t index_id, 
                                  const void* min_key, size_t min_len,
                                  const void* max_key, size_t max_len,
                                  double& selectivity);
    
    // Histogram building
    int build_histogram(uint32_t column_id, uint32_t buckets = 100);
    
private:
    uint64_t txn_id_;
    uint64_t table_id_;
    
    // Random sampling
    std::vector<std::string> get_random_sample(uint32_t size);
    
    // Statistics calculation
    void calculate_avg_row_length(const std::vector<std::string>& sample,
                                 uint64_t& avg_length);
};

} // namespace kvt

#endif // KVT_STATISTICS_H