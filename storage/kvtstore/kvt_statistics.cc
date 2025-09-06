#include "kvt_statistics.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <random>

namespace kvt {

// Statistics table for KVT
constexpr uint32_t STATS_TABLE = 8;

// Singleton instance
StatisticsManager* StatisticsManager::instance_ = nullptr;
std::mutex StatisticsManager::instance_mutex_;

// HyperLogLog implementation
HyperLogLog::HyperLogLog(uint8_t precision) 
    : precision_(precision), register_count_(1 << precision) {
    registers_.resize(register_count_, 0);
}

HyperLogLog::~HyperLogLog() {}

void HyperLogLog::add(const void* data, size_t len) {
    uint32_t hash_value = hash(data, len);
    uint32_t j = hash_value & ((1 << precision_) - 1);
    uint32_t w = hash_value >> precision_;
    uint8_t zeros = leading_zeros(w) + 1;
    
    if (zeros > registers_[j]) {
        registers_[j] = zeros;
    }
}

uint64_t HyperLogLog::estimate() const {
    double raw_estimate = 0;
    uint32_t zeros = 0;
    
    for (uint32_t i = 0; i < register_count_; ++i) {
        raw_estimate += std::pow(2, -registers_[i]);
        if (registers_[i] == 0) {
            zeros++;
        }
    }
    
    double alpha;
    if (register_count_ == 16) alpha = 0.673;
    else if (register_count_ == 32) alpha = 0.697;
    else if (register_count_ == 64) alpha = 0.709;
    else alpha = 0.7213 / (1 + 1.079 / register_count_);
    
    double estimate = alpha * register_count_ * register_count_ / raw_estimate;
    
    // Small range correction
    if (estimate <= 2.5 * register_count_ && zeros != 0) {
        estimate = register_count_ * std::log(static_cast<double>(register_count_) / zeros);
    }
    // Large range correction
    else if (estimate > (1.0 / 30.0) * 4294967296.0) {
        estimate = -4294967296.0 * std::log(1 - estimate / 4294967296.0);
    }
    
    return static_cast<uint64_t>(estimate);
}

void HyperLogLog::merge(const HyperLogLog& other) {
    if (precision_ != other.precision_) {
        return; // Cannot merge different precisions
    }
    
    for (uint32_t i = 0; i < register_count_; ++i) {
        if (other.registers_[i] > registers_[i]) {
            registers_[i] = other.registers_[i];
        }
    }
}

void HyperLogLog::clear() {
    std::fill(registers_.begin(), registers_.end(), 0);
}

std::string HyperLogLog::serialize() const {
    std::string result;
    result.append(reinterpret_cast<const char*>(&precision_), sizeof(precision_));
    result.append(reinterpret_cast<const char*>(registers_.data()), 
                  registers_.size());
    return result;
}

void HyperLogLog::deserialize(const std::string& data) {
    if (data.size() < sizeof(precision_)) {
        return;
    }
    
    precision_ = *reinterpret_cast<const uint8_t*>(data.data());
    register_count_ = 1 << precision_;
    registers_.resize(register_count_);
    
    size_t copy_size = std::min(data.size() - sizeof(precision_), 
                                registers_.size());
    std::memcpy(registers_.data(), data.data() + sizeof(precision_), copy_size);
}

uint32_t HyperLogLog::hash(const void* data, size_t len) const {
    // Simple FNV-1a hash
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash_value = 2166136261U;
    
    for (size_t i = 0; i < len; ++i) {
        hash_value ^= bytes[i];
        hash_value *= 16777619U;
    }
    
    return hash_value;
}

uint8_t HyperLogLog::leading_zeros(uint32_t hash_value) const {
    if (hash_value == 0) return 32;
    
    uint8_t zeros = 0;
    while ((hash_value & 0x80000000) == 0) {
        zeros++;
        hash_value <<= 1;
    }
    return zeros;
}

// StatisticsManager implementation
StatisticsManager::StatisticsManager() {}

StatisticsManager::~StatisticsManager() {}

StatisticsManager* StatisticsManager::get_instance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (!instance_) {
        instance_ = new StatisticsManager();
    }
    return instance_;
}

void StatisticsManager::shutdown() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    delete instance_;
    instance_ = nullptr;
}

int StatisticsManager::update_table_stats(uint64_t txn_id, uint64_t table_id,
                                         const TableStats& stats) {
    // Update cache
    {
        std::lock_guard<std::mutex> lock(cache_.mutex);
        cache_.table_cache[table_id] = stats;
    }
    
    // Persist to KVT
    return persist_table_stats(txn_id, table_id, stats);
}

int StatisticsManager::get_table_stats(uint64_t txn_id, uint64_t table_id,
                                       TableStats& stats) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(cache_.mutex);
        auto it = cache_.table_cache.find(table_id);
        if (it != cache_.table_cache.end()) {
            stats = it->second;
            return 0;
        }
    }
    
    // Load from KVT
    int ret = load_table_stats(txn_id, table_id, stats);
    if (ret == 0) {
        // Update cache
        std::lock_guard<std::mutex> lock(cache_.mutex);
        cache_.table_cache[table_id] = stats;
    }
    
    return ret;
}

int StatisticsManager::update_index_stats(uint64_t txn_id, uint64_t table_id,
                                         uint32_t index_id, const IndexStats& stats) {
    // Update cache
    {
        std::lock_guard<std::mutex> lock(cache_.mutex);
        auto& index_vec = cache_.index_cache[table_id];
        
        // Find or add this index
        bool found = false;
        for (auto& idx : index_vec) {
            if (idx.index_id == index_id) {
                idx = stats;
                found = true;
                break;
            }
        }
        if (!found) {
            index_vec.push_back(stats);
        }
    }
    
    // Persist to KVT
    return persist_index_stats(txn_id, table_id, index_id, stats);
}

int StatisticsManager::get_index_stats(uint64_t txn_id, uint64_t table_id,
                                       uint32_t index_id, IndexStats& stats) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(cache_.mutex);
        auto it = cache_.index_cache.find(table_id);
        if (it != cache_.index_cache.end()) {
            for (const auto& idx : it->second) {
                if (idx.index_id == index_id) {
                    stats = idx;
                    return 0;
                }
            }
        }
    }
    
    // Load from KVT
    int ret = load_index_stats(txn_id, table_id, index_id, stats);
    if (ret == 0) {
        // Update cache
        std::lock_guard<std::mutex> lock(cache_.mutex);
        auto& index_vec = cache_.index_cache[table_id];
        
        bool found = false;
        for (auto& idx : index_vec) {
            if (idx.index_id == index_id) {
                idx = stats;
                found = true;
                break;
            }
        }
        if (!found) {
            index_vec.push_back(stats);
        }
    }
    
    return ret;
}

int StatisticsManager::increment_row_count(uint64_t txn_id, uint64_t table_id,
                                          int64_t delta) {
    TableStats stats;
    int ret = get_table_stats(txn_id, table_id, stats);
    if (ret != 0 && ret != static_cast<int>(KVTError::KEY_NOT_FOUND)) {
        return ret;
    }
    
    if (delta > 0) {
        stats.row_count += delta;
    } else if (stats.row_count >= static_cast<uint64_t>(-delta)) {
        stats.row_count -= -delta;
    } else {
        stats.row_count = 0;
    }
    
    stats.update_time = time(nullptr);
    return update_table_stats(txn_id, table_id, stats);
}

int StatisticsManager::update_auto_increment(uint64_t txn_id, uint64_t table_id,
                                            uint64_t value) {
    TableStats stats;
    int ret = get_table_stats(txn_id, table_id, stats);
    if (ret != 0 && ret != static_cast<int>(KVTError::KEY_NOT_FOUND)) {
        return ret;
    }
    
    if (value > stats.auto_increment_value) {
        stats.auto_increment_value = value;
        stats.update_time = time(nullptr);
        return update_table_stats(txn_id, table_id, stats);
    }
    
    return 0;
}

int StatisticsManager::update_index_cardinality(uint64_t txn_id, uint64_t table_id,
                                               uint32_t index_id, const void* key_data,
                                               size_t key_len) {
    std::lock_guard<std::mutex> lock(cardinality_.mutex);
    
    // Get or create HyperLogLog for this index
    uint64_t combined_id = (static_cast<uint64_t>(table_id) << 32) | index_id;
    auto it = cardinality_.estimators.find(combined_id);
    if (it == cardinality_.estimators.end()) {
        cardinality_.estimators[combined_id] = std::make_unique<HyperLogLog>();
        it = cardinality_.estimators.find(combined_id);
    }
    
    // Add key to estimator
    it->second->add(key_data, key_len);
    
    return 0;
}

uint64_t StatisticsManager::estimate_cardinality(uint64_t txn_id, uint64_t table_id,
                                                uint32_t index_id) {
    std::lock_guard<std::mutex> lock(cardinality_.mutex);
    
    uint64_t combined_id = (static_cast<uint64_t>(table_id) << 32) | index_id;
    auto it = cardinality_.estimators.find(combined_id);
    if (it != cardinality_.estimators.end()) {
        return it->second->estimate();
    }
    
    // No estimator found, try to load from persisted stats
    IndexStats stats;
    if (get_index_stats(txn_id, table_id, index_id, stats) == 0) {
        return stats.cardinality;
    }
    
    return 0;
}

int StatisticsManager::analyze_table(uint64_t txn_id, uint64_t table_id) {
    TableStats table_stats;
    
    // Full table scan
    int ret = scan_table_full(txn_id, table_id, table_stats);
    if (ret != 0) {
        return ret;
    }
    
    // Update table statistics
    ret = update_table_stats(txn_id, table_id, table_stats);
    if (ret != 0) {
        return ret;
    }
    
    // TODO: Analyze all indexes
    // This would iterate through all indexes and call analyze_index
    
    return 0;
}

int StatisticsManager::analyze_index(uint64_t txn_id, uint64_t table_id,
                                    uint32_t index_id) {
    IndexStats stats;
    
    // Full index scan
    int ret = scan_index_full(txn_id, table_id, index_id, stats);
    if (ret != 0) {
        return ret;
    }
    
    // Update index statistics
    return update_index_stats(txn_id, table_id, index_id, stats);
}

void StatisticsManager::clear_cache(uint64_t table_id) {
    cache_.clear(table_id);
}

void StatisticsManager::refresh_cache(uint64_t txn_id, uint64_t table_id) {
    clear_cache(table_id);
    
    // Reload table stats
    TableStats table_stats;
    get_table_stats(txn_id, table_id, table_stats);
    
    // TODO: Reload index stats
}

void StatisticsManager::make_stats_key(uint8_t* key_buf, size_t* key_len,
                                       uint64_t table_id, StatsType type,
                                       uint32_t sub_id) {
    size_t offset = 0;
    
    // Key space
    key_buf[offset++] = STATS_KEYSPACE;
    
    // Table ID (8 bytes)
    uint64_t table_id_be = htobe64(table_id);
    std::memcpy(key_buf + offset, &table_id_be, sizeof(table_id_be));
    offset += sizeof(table_id_be);
    
    // Stats type
    key_buf[offset++] = type;
    
    // Sub ID for index/column stats
    if (type != TABLE_STATS) {
        uint32_t sub_id_be = htobe32(sub_id);
        std::memcpy(key_buf + offset, &sub_id_be, sizeof(sub_id_be));
        offset += sizeof(sub_id_be);
    }
    
    *key_len = offset;
}

int StatisticsManager::persist_table_stats(uint64_t txn_id, uint64_t table_id,
                                          const TableStats& stats) {
    uint8_t key_buf[256];
    size_t key_len;
    make_stats_key(key_buf, &key_len, table_id, TABLE_STATS);
    
    std::string error_msg;
    KVTError err = kvt_set(
        txn_id,
        STATS_TABLE,
        KVTKey(std::string(reinterpret_cast<char*>(key_buf), key_len)),
        std::string(reinterpret_cast<const char*>(&stats), sizeof(stats)),
        error_msg
    );
    
    return (err == KVTError::SUCCESS) ? 0 : -1;
}

int StatisticsManager::load_table_stats(uint64_t txn_id, uint64_t table_id,
                                        TableStats& stats) {
    uint8_t key_buf[256];
    size_t key_len;
    make_stats_key(key_buf, &key_len, table_id, TABLE_STATS);
    
    std::string value;
    std::string error_msg;
    KVTError err = kvt_get(
        txn_id,
        STATS_TABLE,
        KVTKey(std::string(reinterpret_cast<char*>(key_buf), key_len)),
        value,
        error_msg
    );
    
    if (err == KVTError::KEY_NOT_FOUND) {
        // Initialize with defaults
        stats = TableStats();
        return static_cast<int>(KVTError::KEY_NOT_FOUND);
    }
    
    if (err != KVTError::SUCCESS) {
        return -1;
    }
    
    if (value.size() != sizeof(TableStats)) {
        return -1;
    }
    
    std::memcpy(&stats, value.data(), sizeof(stats));
    return 0;
}

int StatisticsManager::persist_index_stats(uint64_t txn_id, uint64_t table_id,
                                          uint32_t index_id, const IndexStats& stats) {
    uint8_t key_buf[256];
    size_t key_len;
    make_stats_key(key_buf, &key_len, table_id, INDEX_STATS, index_id);
    
    std::string error_msg;
    KVTError err = kvt_set(
        txn_id,
        STATS_TABLE,
        KVTKey(std::string(reinterpret_cast<char*>(key_buf), key_len)),
        std::string(reinterpret_cast<const char*>(&stats), sizeof(stats)),
        error_msg
    );
    
    return (err == KVTError::SUCCESS) ? 0 : -1;
}

int StatisticsManager::load_index_stats(uint64_t txn_id, uint64_t table_id,
                                        uint32_t index_id, IndexStats& stats) {
    uint8_t key_buf[256];
    size_t key_len;
    make_stats_key(key_buf, &key_len, table_id, INDEX_STATS, index_id);
    
    std::string value;
    std::string error_msg;
    KVTError err = kvt_get(
        txn_id,
        STATS_TABLE,
        KVTKey(std::string(reinterpret_cast<char*>(key_buf), key_len)),
        value,
        error_msg
    );
    
    if (err == KVTError::KEY_NOT_FOUND) {
        // Initialize with defaults
        stats = IndexStats();
        stats.index_id = index_id;
        return static_cast<int>(KVTError::KEY_NOT_FOUND);
    }
    
    if (err != KVTError::SUCCESS) {
        return -1;
    }
    
    if (value.size() != sizeof(IndexStats)) {
        return -1;
    }
    
    std::memcpy(&stats, value.data(), sizeof(stats));
    return 0;
}

int StatisticsManager::scan_table_full(uint64_t txn_id, uint64_t table_id,
                                       TableStats& stats) {
    stats = TableStats();
    
    // Scan data table to count rows and calculate sizes
    uint8_t start_key[16];
    uint8_t end_key[16];
    size_t start_len = 0, end_len = 0;
    
    // Build key range for this table
    uint64_t table_id_be = htobe64(table_id);
    std::memcpy(start_key, &table_id_be, sizeof(table_id_be));
    start_len = sizeof(table_id_be);
    
    uint64_t next_table_id_be = htobe64(table_id + 1);
    std::memcpy(end_key, &next_table_id_be, sizeof(next_table_id_be));
    end_len = sizeof(next_table_id_be);
    
    std::vector<std::pair<KVTKey, std::string>> results;
    std::string error_msg;
    KVTError err = kvt_scan(
        txn_id,
        2, // DATA_TABLE
        KVTKey(std::string(reinterpret_cast<char*>(start_key), start_len)),
        KVTKey(std::string(reinterpret_cast<char*>(end_key), end_len)),
        0, // No limit
        results,
        error_msg
    );
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return -1;
    }
    
    uint64_t total_size = 0;
    for (const auto& [key, value] : results) {
        stats.row_count++;
        total_size += value.size();
    }
    
    if (stats.row_count > 0) {
        stats.avg_row_length = total_size / stats.row_count;
    }
    stats.data_size = total_size;
    stats.update_time = time(nullptr);
    
    return 0;
}

int StatisticsManager::scan_index_full(uint64_t txn_id, uint64_t table_id,
                                       uint32_t index_id, IndexStats& stats) {
    stats = IndexStats();
    stats.index_id = index_id;
    
    // Build key range for this index
    uint8_t start_key[20];
    uint8_t end_key[20];
    size_t start_len = 0, end_len = 0;
    
    uint64_t table_id_be = htobe64(table_id);
    std::memcpy(start_key, &table_id_be, sizeof(table_id_be));
    start_len = sizeof(table_id_be);
    
    uint32_t index_id_be = htobe32(index_id);
    std::memcpy(start_key + start_len, &index_id_be, sizeof(index_id_be));
    start_len += sizeof(index_id_be);
    
    std::memcpy(end_key, &table_id_be, sizeof(table_id_be));
    end_len = sizeof(table_id_be);
    
    uint32_t next_index_id_be = htobe32(index_id + 1);
    std::memcpy(end_key + end_len, &next_index_id_be, sizeof(next_index_id_be));
    end_len += sizeof(next_index_id_be);
    
    std::vector<std::pair<KVTKey, std::string>> results;
    std::string error_msg;
    KVTError err = kvt_scan(
        txn_id,
        3, // INDEX_TABLE
        KVTKey(std::string(reinterpret_cast<char*>(start_key), start_len)),
        KVTKey(std::string(reinterpret_cast<char*>(end_key), end_len)),
        0, // No limit
        results,
        error_msg
    );
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return -1;
    }
    
    HyperLogLog estimator;
    std::string last_key;
    uint64_t unique_count = 0;
    
    for (const auto& [key, value] : results) {
        std::string key_str = key.data();
        size_t key_len = key_str.size();
        
        // Extract index value part (skip table_id, index_id, and row_id)
        if (key_len > sizeof(table_id_be) + sizeof(index_id_be) + 8) {
            size_t value_start = sizeof(table_id_be) + sizeof(index_id_be);
            size_t value_len = key_len - value_start - 8; // Subtract row_id
            
            std::string index_value(key_str.data() + value_start, value_len);
            if (index_value != last_key) {
                unique_count++;
                last_key = index_value;
            }
            
            estimator.add(index_value.data(), index_value.size());
        }
        
        stats.entry_count++;
    }
    
    stats.cardinality = estimator.estimate();
    if (stats.entry_count > 0) {
        stats.selectivity = static_cast<double>(stats.cardinality) / stats.entry_count;
    }
    
    // Estimate tree height based on entries (assuming B-tree with fanout ~100)
    if (stats.entry_count > 0) {
        stats.tree_height = static_cast<uint32_t>(
            std::ceil(std::log(stats.entry_count) / std::log(100))
        );
    }
    
    return 0;
}

void StatisticsManager::StatsCache::clear(uint64_t table_id) {
    std::lock_guard<std::mutex> lock(mutex);
    
    if (table_id == 0) {
        // Clear all
        table_cache.clear();
        index_cache.clear();
        column_cache.clear();
    } else {
        // Clear specific table
        table_cache.erase(table_id);
        index_cache.erase(table_id);
        column_cache.erase(table_id);
    }
}

// StatisticsSampler implementation
StatisticsSampler::StatisticsSampler(uint64_t txn_id, uint64_t table_id)
    : txn_id_(txn_id), table_id_(table_id) {}

StatisticsSampler::~StatisticsSampler() {}

int StatisticsSampler::estimate_table_stats(TableStats& stats, uint32_t sample_size) {
    std::vector<std::string> sample = get_random_sample(sample_size);
    
    if (sample.empty()) {
        return -1;
    }
    
    // Calculate average row length from sample
    uint64_t total_length = 0;
    for (const auto& row : sample) {
        total_length += row.size();
    }
    stats.avg_row_length = total_length / sample.size();
    
    // Estimate total rows (simplified - would need total key count)
    // For now, just use sample size as a placeholder
    stats.row_count = sample_size * 10; // Placeholder multiplier
    
    stats.data_size = stats.row_count * stats.avg_row_length;
    stats.update_time = time(nullptr);
    
    return 0;
}

std::vector<std::string> StatisticsSampler::get_random_sample(uint32_t size) {
    std::vector<std::string> sample;
    
    // Build key range for this table
    uint8_t start_key[16];
    uint8_t end_key[16];
    size_t start_len = 0, end_len = 0;
    
    uint64_t table_id_be = htobe64(table_id_);
    std::memcpy(start_key, &table_id_be, sizeof(table_id_be));
    start_len = sizeof(table_id_be);
    
    uint64_t next_table_id_be = htobe64(table_id_ + 1);
    std::memcpy(end_key, &next_table_id_be, sizeof(next_table_id_be));
    end_len = sizeof(next_table_id_be);
    
    std::vector<std::pair<KVTKey, std::string>> results;
    std::string error_msg;
    KVTError err = kvt_scan(
        txn_id_,
        2, // DATA_TABLE
        KVTKey(std::string(reinterpret_cast<char*>(start_key), start_len)),
        KVTKey(std::string(reinterpret_cast<char*>(end_key), end_len)),
        size,
        results,
        error_msg
    );
    
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
        return sample;
    }
    
    for (const auto& [key, value] : results) {
        if (sample.size() >= size) break;
        sample.push_back(value);
    }
    return sample;
}

} // namespace kvt