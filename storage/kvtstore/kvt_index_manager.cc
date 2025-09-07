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

#include "kvt_index_manager.h"
#include "kvt_row_codec.h"
#include "sql_class.h"
#include "table.h"
#include "field.h"
#include <sstream>
#include <algorithm>

using namespace kvt_constants;

namespace kvt_index {

// Static member initialization
KVTIndexManager* KVTIndexManager::instance = nullptr;
std::mutex KVTIndexManager::instance_mutex;

KVTIndexManager::KVTIndexManager() {
}

KVTIndexManager::~KVTIndexManager() {
  scan_contexts.clear();
}

KVTIndexManager* KVTIndexManager::get_instance() {
  if (instance == nullptr) {
    std::lock_guard<std::mutex> lock(instance_mutex);
    if (instance == nullptr) {
      instance = new KVTIndexManager();
    }
  }
  return instance;
}

int KVTIndexManager::create_index(const std::string& database, 
                                 const std::string& table,
                                 const IndexMetadata& index_meta) {
  // Store index metadata in catalog
  std::string meta_key = std::string(CATALOG_TABLE_NAME) + SEPARATOR + 
                        database + SEPARATOR + table + SEPARATOR + 
                        "IDXMETA" + SEPARATOR + index_meta.index_name;
  
  std::string meta_value = encode_index_metadata(index_meta);
  
  // Get catalog table ID
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  // Store metadata
  err = kvt_set(0, catalog_table_id, KVTKey(meta_key), meta_value, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  return 0;
}

int KVTIndexManager::drop_index(const std::string& database,
                               const std::string& table,
                               const std::string& index_name) {
  // Delete index metadata
  std::string meta_key = std::string(CATALOG_TABLE_NAME) + SEPARATOR + 
                        database + SEPARATOR + table + SEPARATOR + 
                        "IDXMETA" + SEPARATOR + index_name;
  
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  // Delete metadata
  err = kvt_del(0, catalog_table_id, KVTKey(meta_key), error_msg);
  if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
    return -1;
  }
  
  // TODO: Delete all index entries (requires scan)
  
  return 0;
}

int KVTIndexManager::get_index_metadata(const std::string& database,
                                       const std::string& table,
                                       const std::string& index_name,
                                       IndexMetadata& meta) {
  std::string meta_key = std::string(CATALOG_TABLE_NAME) + SEPARATOR + 
                        database + SEPARATOR + table + SEPARATOR + 
                        "IDXMETA" + SEPARATOR + index_name;
  
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  std::string meta_value;
  err = kvt_get(0, catalog_table_id, KVTKey(meta_key), meta_value, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  return decode_index_metadata(meta_value, meta);
}

int KVTIndexManager::list_indexes(const std::string& database,
                                 const std::string& table,
                                 std::vector<IndexMetadata>& indexes) {
  indexes.clear();
  
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  // Scan for all index metadata entries
  std::string prefix = std::string(CATALOG_TABLE_NAME) + SEPARATOR + 
                      database + SEPARATOR + table + SEPARATOR + 
                      "IDXMETA" + SEPARATOR;
  std::string end_key = prefix;
  end_key[end_key.length() - 1]++;  // Increment last byte for exclusive end
  
  std::vector<std::pair<KVTKey, std::string>> results;
  err = kvt_scan(0, catalog_table_id, KVTKey(prefix), KVTKey(end_key), 100, results, error_msg);
  if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
    return -1;
  }
  
  for (const auto& kv : results) {
    IndexMetadata meta;
    if (decode_index_metadata(kv.second, meta) == 0) {
      indexes.push_back(meta);
    }
  }
  
  return 0;
}

std::string KVTIndexManager::create_primary_key(const uchar* record, TABLE* table) {
  kvt_row_codec::RowCodec codec(table);
  return codec.encode_primary_key(record);
}

std::string KVTIndexManager::create_secondary_key(const uchar* record, TABLE* table,
                                                 const IndexMetadata& index) {
  std::string key;
  
  // Encode each column in the index
  for (uint col_pos : index.column_positions) {
    if (col_pos >= table->s->fields) {
      continue;
    }
    
    Field* field = table->field[col_pos];
    if (field->is_null()) {
      key += SEPARATOR;
      key += "NULL";
    } else {
      // Encode field value
      char buffer[256];
      String str(buffer, sizeof(buffer), field->charset());
      field->val_str(&str);
      key += SEPARATOR;
      key.append(str.ptr(), str.length());
    }
  }
  
  return key;
}

std::string KVTIndexManager::create_index_entry_key(const std::string& database,
                                                   const std::string& table,
                                                   const std::string& index_name,
                                                   const std::string& index_value,
                                                   const std::string& primary_key) {
  // Format: __CATALOG__\x00<db>\x00<table>\x00IDX\x00<idx_name>\x00<idx_val>\x00<pk>
  std::string key = std::string(CATALOG_TABLE_NAME) + SEPARATOR + 
                   database + SEPARATOR + table + SEPARATOR + 
                   "IDX" + SEPARATOR + index_name + 
                   index_value + SEPARATOR + primary_key;
  return key;
}

int KVTIndexManager::insert_index_entry(uint64_t tx_id, uint64_t table_id,
                                       const std::string& database,
                                       const std::string& table,
                                       const std::string& index_name,
                                       const std::string& index_key,
                                       const std::string& primary_key) {
  // Check for unique constraint if needed
  IndexMetadata meta;
  if (get_index_metadata(database, table, index_name, meta) == 0) {
    if (meta.is_unique) {
      if (check_unique_constraint(tx_id, table_id, database, table, 
                                 index_name, index_key)) {
        return HA_ERR_FOUND_DUPP_KEY;
      }
    }
  }
  
  // Create index entry
  std::string entry_key = create_index_entry_key(database, table, index_name,
                                                index_key, primary_key);
  
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  // Store empty value for index entry
  err = kvt_set(tx_id, catalog_table_id, KVTKey(entry_key), "", error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  return 0;
}

int KVTIndexManager::delete_index_entry(uint64_t tx_id, uint64_t table_id,
                                       const std::string& database,
                                       const std::string& table,
                                       const std::string& index_name,
                                       const std::string& index_key,
                                       const std::string& primary_key) {
  std::string entry_key = create_index_entry_key(database, table, index_name,
                                                index_key, primary_key);
  
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  err = kvt_del(tx_id, catalog_table_id, KVTKey(entry_key), error_msg);
  if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
    return -1;
  }
  
  return 0;
}

int KVTIndexManager::update_index_entries(uint64_t tx_id, uint64_t table_id,
                                         const std::string& database,
                                         const std::string& table,
                                         const uchar* old_record,
                                         const uchar* new_record,
                                         TABLE* table_def) {
  // Get all indexes for this table
  std::vector<IndexMetadata> indexes;
  if (list_indexes(database, table, indexes) != 0) {
    return -1;
  }
  
  // Update each index
  for (const auto& index : indexes) {
    if (index.is_primary) {
      // Primary key updates handled separately
      continue;
    }
    
    // Generate old and new index keys
    std::string old_index_key = create_secondary_key(old_record, table_def, index);
    std::string new_index_key = create_secondary_key(new_record, table_def, index);
    
    // If index key changed, update it
    if (old_index_key != new_index_key) {
      std::string primary_key = create_primary_key(new_record, table_def);
      
      // Delete old entry
      int ret = delete_index_entry(tx_id, table_id, database, table,
                                  index.index_name, old_index_key, primary_key);
      if (ret != 0) {
        return ret;
      }
      
      // Insert new entry
      ret = insert_index_entry(tx_id, table_id, database, table,
                             index.index_name, new_index_key, primary_key);
      if (ret != 0) {
        return ret;
      }
    }
  }
  
  return 0;
}

int KVTIndexManager::index_init(void* scan_ctx, uint64_t tx_id, uint64_t table_id,
                               const std::string& database, const std::string& table,
                               const std::string& index_name, bool sorted) {
  std::lock_guard<std::mutex> lock(contexts_mutex);
  
  IndexScanContext& ctx = scan_contexts[scan_ctx];
  ctx.tx_id = tx_id;
  ctx.table_id = table_id;
  ctx.index_name = index_name;
  ctx.ascending = sorted;
  
  // Set up scan range for this index
  ctx.current_key = std::string(CATALOG_TABLE_NAME) + SEPARATOR + 
                   database + SEPARATOR + table + SEPARATOR + 
                   "IDX" + SEPARATOR + index_name + SEPARATOR;
  ctx.end_key = ctx.current_key;
  ctx.end_key[ctx.end_key.length() - 1]++;  // Increment for exclusive end
  
  return 0;
}

int KVTIndexManager::index_read(void* scan_ctx, uchar* buf, const uchar* key,
                               uint key_len, int find_flag) {
  IndexScanContext* ctx = get_scan_context(scan_ctx);
  if (!ctx) {
    return HA_ERR_END_OF_FILE;
  }
  
  // TODO: Implement based on find_flag (HA_READ_KEY_EXACT, HA_READ_KEY_OR_NEXT, etc.)
  // For now, position at first matching key
  
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  // Scan for matching entries
  std::vector<std::pair<KVTKey, std::string>> results;
  err = kvt_scan(ctx->tx_id, catalog_table_id, KVTKey(ctx->current_key), KVTKey(ctx->end_key), 
                1, results, error_msg);
  if (err != KVTError::SUCCESS || results.empty()) {
    return HA_ERR_END_OF_FILE;
  }
  
  // Extract primary key from index entry and fetch row
  // TODO: Parse primary key from index entry key and fetch actual row
  
  return 0;
}

int KVTIndexManager::index_next(void* scan_ctx, uchar* buf) {
  IndexScanContext* ctx = get_scan_context(scan_ctx);
  if (!ctx) {
    return HA_ERR_END_OF_FILE;
  }
  
  // Move to next index entry
  // TODO: Implement index traversal
  
  return HA_ERR_END_OF_FILE;
}

int KVTIndexManager::index_prev(void* scan_ctx, uchar* buf) {
  IndexScanContext* ctx = get_scan_context(scan_ctx);
  if (!ctx || !ctx->ascending) {
    return HA_ERR_END_OF_FILE;
  }
  
  // Move to previous index entry
  // TODO: Implement reverse index traversal
  
  return HA_ERR_END_OF_FILE;
}

int KVTIndexManager::index_first(void* scan_ctx, uchar* buf) {
  IndexScanContext* ctx = get_scan_context(scan_ctx);
  if (!ctx) {
    return HA_ERR_END_OF_FILE;
  }
  
  // Position at first index entry
  ctx->current_key = std::string(CATALOG_TABLE_NAME) + SEPARATOR + 
                    // ... reset to beginning of index
                    "";
  
  return index_next(scan_ctx, buf);
}

int KVTIndexManager::index_last(void* scan_ctx, uchar* buf) {
  IndexScanContext* ctx = get_scan_context(scan_ctx);
  if (!ctx) {
    return HA_ERR_END_OF_FILE;
  }
  
  // Position at last index entry
  // TODO: Implement positioning at end
  
  return HA_ERR_END_OF_FILE;
}

int KVTIndexManager::index_end(void* scan_ctx) {
  remove_scan_context(scan_ctx);
  return 0;
}

bool KVTIndexManager::check_unique_constraint(uint64_t tx_id, uint64_t table_id,
                                             const std::string& database,
                                             const std::string& table,
                                             const std::string& index_name,
                                             const std::string& index_key) {
  // Check if index key already exists
  std::string search_prefix = std::string(CATALOG_TABLE_NAME) + SEPARATOR + 
                            database + SEPARATOR + table + SEPARATOR + 
                            "IDX" + SEPARATOR + index_name + index_key + SEPARATOR;
  
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return false;
  }
  
  std::string end_key = search_prefix;
  end_key[end_key.length() - 1]++;
  
  std::vector<std::pair<KVTKey, std::string>> results;
  err = kvt_scan(tx_id, catalog_table_id, KVTKey(search_prefix), KVTKey(end_key), 1, results, error_msg);
  
  return !results.empty();  // Returns true if duplicate found
}

std::string KVTIndexManager::encode_index_metadata(const IndexMetadata& meta) {
  std::stringstream ss;
  ss << "name=" << meta.index_name << ";";
  ss << "type=" << static_cast<int>(meta.type) << ";";
  ss << "unique=" << (meta.is_unique ? "1" : "0") << ";";
  ss << "primary=" << (meta.is_primary ? "1" : "0") << ";";
  ss << "columns=";
  for (size_t i = 0; i < meta.column_positions.size(); i++) {
    if (i > 0) ss << ",";
    ss << meta.column_positions[i];
  }
  ss << ";";
  ss << "column_names=";
  for (size_t i = 0; i < meta.column_names.size(); i++) {
    if (i > 0) ss << ",";
    ss << meta.column_names[i];
  }
  return ss.str();
}

int KVTIndexManager::decode_index_metadata(const std::string& data, IndexMetadata& meta) {
  std::istringstream ss(data);
  std::string token;
  
  while (std::getline(ss, token, ';')) {
    size_t eq_pos = token.find('=');
    if (eq_pos == std::string::npos) continue;
    
    std::string key = token.substr(0, eq_pos);
    std::string value = token.substr(eq_pos + 1);
    
    if (key == "name") {
      meta.index_name = value;
    } else if (key == "type") {
      meta.type = static_cast<IndexType>(std::stoi(value));
    } else if (key == "unique") {
      meta.is_unique = (value == "1");
    } else if (key == "primary") {
      meta.is_primary = (value == "1");
    } else if (key == "columns") {
      std::istringstream cols(value);
      std::string col;
      while (std::getline(cols, col, ',')) {
        if (!col.empty()) {
          meta.column_positions.push_back(std::stoul(col));
        }
      }
    } else if (key == "column_names") {
      std::istringstream cols(value);
      std::string col;
      while (std::getline(cols, col, ',')) {
        if (!col.empty()) {
          meta.column_names.push_back(col);
        }
      }
    }
  }
  
  return 0;
}

IndexScanContext* KVTIndexManager::get_scan_context(void* ctx_ptr) {
  std::lock_guard<std::mutex> lock(contexts_mutex);
  auto it = scan_contexts.find(ctx_ptr);
  if (it != scan_contexts.end()) {
    return &it->second;
  }
  return nullptr;
}

void KVTIndexManager::remove_scan_context(void* ctx_ptr) {
  std::lock_guard<std::mutex> lock(contexts_mutex);
  scan_contexts.erase(ctx_ptr);
}

} // namespace kvt_index