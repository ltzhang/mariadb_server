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

#ifndef KVT_INDEX_MANAGER_H
#define KVT_INDEX_MANAGER_H

#include "my_global.h"
#include "kvt/kvt_inc.h"
#include "kvt_constants.h"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <mutex>

class Field;
class TABLE;

namespace kvt_index {

// Index types supported
enum class IndexType {
  PRIMARY,
  UNIQUE,
  SECONDARY
};

// Index metadata structure
struct IndexMetadata {
  std::string index_name;
  IndexType type;
  std::vector<uint> column_positions;  // Column positions in table
  std::vector<std::string> column_names;
  bool is_unique;
  bool is_primary;
  
  IndexMetadata() : type(IndexType::SECONDARY), is_unique(false), is_primary(false) {}
};

// Index scan context
struct IndexScanContext {
  uint64_t tx_id;
  uint64_t table_id;
  std::string index_name;
  std::string current_key;
  std::string end_key;
  bool ascending;
  bool index_only_scan;
  std::vector<std::string> covering_columns;
  
  IndexScanContext() : tx_id(0), table_id(0), ascending(true), index_only_scan(false) {}
};

class KVTIndexManager {
private:
  // Singleton instance
  static KVTIndexManager* instance;
  static std::mutex instance_mutex;
  
  // Current scan contexts per table
  std::map<void*, IndexScanContext> scan_contexts;
  std::mutex contexts_mutex;
  
  // Private constructor for singleton
  KVTIndexManager();
  
public:
  ~KVTIndexManager();
  
  // Singleton access
  static KVTIndexManager* get_instance();
  
  // Index metadata management
  int create_index(const std::string& database, const std::string& table,
                   const IndexMetadata& index_meta);
  int drop_index(const std::string& database, const std::string& table,
                const std::string& index_name);
  int get_index_metadata(const std::string& database, const std::string& table,
                        const std::string& index_name, IndexMetadata& meta);
  int list_indexes(const std::string& database, const std::string& table,
                   std::vector<IndexMetadata>& indexes);
  
  // Index key generation
  std::string create_primary_key(const uchar* record, TABLE* table);
  std::string create_secondary_key(const uchar* record, TABLE* table,
                                  const IndexMetadata& index);
  std::string create_index_entry_key(const std::string& database,
                                    const std::string& table,
                                    const std::string& index_name,
                                    const std::string& index_value,
                                    const std::string& primary_key);
  
  // Index maintenance operations
  int insert_index_entry(uint64_t tx_id, uint64_t table_id,
                        const std::string& database, const std::string& table,
                        const std::string& index_name,
                        const std::string& index_key,
                        const std::string& primary_key);
  int delete_index_entry(uint64_t tx_id, uint64_t table_id,
                        const std::string& database, const std::string& table,
                        const std::string& index_name,
                        const std::string& index_key,
                        const std::string& primary_key);
  int update_index_entries(uint64_t tx_id, uint64_t table_id,
                          const std::string& database, const std::string& table,
                          const uchar* old_record, const uchar* new_record,
                          TABLE* table_def);
  
  // Index scan operations
  int index_init(void* scan_ctx, uint64_t tx_id, uint64_t table_id,
                const std::string& database, const std::string& table,
                const std::string& index_name, bool sorted);
  int index_read(void* scan_ctx, uchar* buf, const uchar* key,
                uint key_len, int find_flag);
  int index_next(void* scan_ctx, uchar* buf);
  int index_prev(void* scan_ctx, uchar* buf);
  int index_first(void* scan_ctx, uchar* buf);
  int index_last(void* scan_ctx, uchar* buf);
  int index_end(void* scan_ctx);
  
  // Index statistics
  int estimate_index_cardinality(const std::string& database,
                                const std::string& table,
                                const std::string& index_name,
                                uint64_t& cardinality);
  int analyze_index(const std::string& database, const std::string& table,
                   const std::string& index_name);
  
  // Utility functions
  bool is_index_unique(const std::string& database, const std::string& table,
                      const std::string& index_name);
  bool check_unique_constraint(uint64_t tx_id, uint64_t table_id,
                              const std::string& database, const std::string& table,
                              const std::string& index_name,
                              const std::string& index_key);
  
  // Index optimization hints
  std::string suggest_index_for_query(const std::string& database,
                                     const std::string& table,
                                     const std::vector<std::string>& columns,
                                     bool needs_ordering);
  
private:
  // Helper methods
  std::string encode_index_metadata(const IndexMetadata& meta);
  int decode_index_metadata(const std::string& data, IndexMetadata& meta);
  int maintain_all_indexes_for_row(uint64_t tx_id, uint64_t table_id,
                                  const std::string& database,
                                  const std::string& table,
                                  const uchar* record, TABLE* table_def,
                                  bool is_insert);
  IndexScanContext* get_scan_context(void* ctx_ptr);
  void remove_scan_context(void* ctx_ptr);
};

// Helper class for index statistics
class IndexStatistics {
public:
  uint64_t total_entries;
  uint64_t distinct_values;
  double average_key_size;
  time_t last_analyzed;
  
  IndexStatistics() : total_entries(0), distinct_values(0), 
                     average_key_size(0), last_analyzed(0) {}
  
  double selectivity() const {
    return (total_entries > 0) ? 
           static_cast<double>(distinct_values) / total_entries : 0;
  }
};

} // namespace kvt_index

#endif // KVT_INDEX_MANAGER_H