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

#include "kvt_catalog.h"
#include "sql_class.h"
#include "field.h"
#include "table.h"
#include <sstream>
#include <ctime>
#include <cstring>

namespace kvt_catalog {

CatalogManager* CatalogManager::instance = nullptr;

// Simple serialization format: key=value pairs separated by newlines
std::string TableMetadata::serialize() const {
  std::stringstream ss;
  
  // Basic fields
  ss << "database_name=" << database_name << "\n";
  ss << "table_name=" << table_name << "\n";
  ss << "schema_version=" << schema_version << "\n";
  ss << "partition_method=" << partition_method << "\n";
  ss << "row_format=" << row_format << "\n";
  ss << "charset=" << charset << "\n";
  ss << "row_count=" << row_count << "\n";
  ss << "created_timestamp=" << created_timestamp << "\n";
  ss << "updated_timestamp=" << updated_timestamp << "\n";
  
  // Serialize columns
  ss << "column_count=" << columns.size() << "\n";
  for (size_t i = 0; i < columns.size(); i++) {
    const auto& col = columns[i];
    ss << "col_" << i << "_name=" << col.name << "\n";
    ss << "col_" << i << "_type=" << (int)col.type << "\n";
    ss << "col_" << i << "_length=" << col.length << "\n";
    ss << "col_" << i << "_flags=" << col.flags << "\n";
    ss << "col_" << i << "_default=" << col.default_value << "\n";
    ss << "col_" << i << "_index=" << col.field_index << "\n";
  }
  
  // Serialize indexes
  ss << "index_count=" << indexes.size() << "\n";
  for (size_t i = 0; i < indexes.size(); i++) {
    const auto& idx = indexes[i];
    ss << "idx_" << i << "_name=" << idx.name << "\n";
    ss << "idx_" << i << "_primary=" << idx.is_primary << "\n";
    ss << "idx_" << i << "_unique=" << idx.is_unique << "\n";
    ss << "idx_" << i << "_cols=";
    for (size_t j = 0; j < idx.column_indices.size(); j++) {
      if (j > 0) ss << ",";
      ss << idx.column_indices[j];
    }
    ss << "\n";
  }
  
  // Serialize auto-increment values
  ss << "auto_inc_count=" << auto_increment_values.size() << "\n";
  for (const auto& [col, val] : auto_increment_values) {
    ss << "auto_inc_" << col << "=" << val << "\n";
  }
  
  return ss.str();
}

bool TableMetadata::deserialize(const std::string& data) {
  std::istringstream ss(data);
  std::string line;
  std::map<std::string, std::string> values;
  
  // Parse key=value pairs
  while (std::getline(ss, line)) {
    size_t eq_pos = line.find('=');
    if (eq_pos != std::string::npos) {
      std::string key = line.substr(0, eq_pos);
      std::string value = line.substr(eq_pos + 1);
      values[key] = value;
    }
  }
  
  // Basic fields
  database_name = values["database_name"];
  table_name = values["table_name"];
  schema_version = std::stoul(values["schema_version"]);
  partition_method = values.count("partition_method") ? values["partition_method"] : "hash";
  row_format = values.count("row_format") ? values["row_format"] : "DYNAMIC";
  charset = values.count("charset") ? values["charset"] : "utf8mb4";
  row_count = std::stoull(values["row_count"]);
  created_timestamp = std::stoull(values["created_timestamp"]);
  updated_timestamp = std::stoull(values["updated_timestamp"]);
  
  // Deserialize columns
  columns.clear();
  int column_count = std::stoi(values["column_count"]);
  for (int i = 0; i < column_count; i++) {
    ColumnDef col;
    col.name = values["col_" + std::to_string(i) + "_name"];
    col.type = (enum_field_types)std::stoi(values["col_" + std::to_string(i) + "_type"]);
    col.length = std::stoul(values["col_" + std::to_string(i) + "_length"]);
    col.flags = std::stoul(values["col_" + std::to_string(i) + "_flags"]);
    col.default_value = values["col_" + std::to_string(i) + "_default"];
    col.field_index = std::stoul(values["col_" + std::to_string(i) + "_index"]);
    columns.push_back(col);
  }
  
  // Deserialize indexes
  indexes.clear();
  int index_count = values.count("index_count") ? std::stoi(values["index_count"]) : 0;
  for (int i = 0; i < index_count; i++) {
    IndexDef idx;
    idx.name = values["idx_" + std::to_string(i) + "_name"];
    idx.is_primary = (values["idx_" + std::to_string(i) + "_primary"] == "1");
    idx.is_unique = (values["idx_" + std::to_string(i) + "_unique"] == "1");
    
    // Parse column indices
    std::string cols_str = values["idx_" + std::to_string(i) + "_cols"];
    std::istringstream cols_ss(cols_str);
    std::string col_idx;
    while (std::getline(cols_ss, col_idx, ',')) {
      if (!col_idx.empty()) {
        idx.column_indices.push_back(std::stoul(col_idx));
      }
    }
    
    indexes.push_back(idx);
  }
  
  // Deserialize auto-increment values
  auto_increment_values.clear();
  // int auto_inc_count = values.count("auto_inc_count") ? std::stoi(values["auto_inc_count"]) : 0;
  for (const auto& [key, val] : values) {
    if (key.substr(0, 9) == "auto_inc_" && key != "auto_inc_count") {
      std::string col_name = key.substr(9);
      auto_increment_values[col_name] = std::stoull(val);
    }
  }
  
  return true;
}

// CatalogManager implementation
CatalogManager::CatalogManager() : catalog_table_id(0), initialized(false) {
}

CatalogManager* CatalogManager::get_instance() {
  if (!instance) {
    instance = new CatalogManager();
  }
  return instance;
}

void CatalogManager::cleanup() {
  if (instance) {
    delete instance;
    instance = nullptr;
  }
}

int CatalogManager::initialize() {
  if (initialized) {
    return 0;
  }
  
  int ret = ensure_catalog_exists();
  if (ret == 0) {
    initialized = true;
  }
  return ret;
}

int CatalogManager::ensure_catalog_exists() {
  std::string error_msg;
  
  // Try to get catalog table ID
  KVTError err = kvt_get_table_id(kvt_constants::CATALOG_TABLE_NAME, 
                                  catalog_table_id, error_msg);
  
  if (err == KVTError::TABLE_NOT_FOUND) {
    // Create catalog table
    err = kvt_create_table(kvt_constants::CATALOG_TABLE_NAME, "range",
                          catalog_table_id, error_msg);
    if (err != KVTError::SUCCESS) {
      return HA_ERR_GENERIC;
    }
    
    // Store initialization marker
    std::string init_key = kvt_constants::SYS_INITIALIZED_KEY;
    std::string init_value = std::to_string(std::time(nullptr));
    err = kvt_set(0, catalog_table_id, KVTKey(init_key), init_value, error_msg);
    
    // Store version
    std::string ver_key = kvt_constants::SYS_VERSION_KEY;
    std::string ver_value = std::to_string(kvt_constants::CATALOG_VERSION);
    err = kvt_set(0, catalog_table_id, KVTKey(ver_key), ver_value, error_msg);
  }
  
  return (err == KVTError::SUCCESS) ? 0 : HA_ERR_GENERIC;
}

uint64_t CatalogManager::get_catalog_table_id() {
  if (catalog_table_id == 0) {
    std::string error_msg;
    kvt_get_table_id(kvt_constants::CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  }
  return catalog_table_id;
}

int CatalogManager::create_database(const std::string& database) {
  if (!kvt_constants::is_valid_name(database)) {
    return HA_ERR_WRONG_COMMAND;
  }
  
  std::string error_msg;
  
  // Create data table for this database
  std::string data_table_name = kvt_constants::make_data_table_name(database);
  uint64_t data_table_id;
  KVTError err = kvt_create_table(data_table_name, "range", data_table_id, error_msg);
  if (err != KVTError::SUCCESS && err != KVTError::TABLE_ALREADY_EXISTS) {
    return HA_ERR_GENERIC;
  }
  
  // Store database info in catalog
  DatabaseInfo info;
  info.name = database;
  info.kvt_data_table_id = data_table_id;
  info.created_timestamp = std::time(nullptr);
  
  return store_database_info(database, info);
}

int CatalogManager::drop_database(const std::string& database) {
  std::string error_msg;
  
  // Get catalog table ID
  uint64_t cat_id = get_catalog_table_id();
  if (cat_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  // Find all tables in database
  std::vector<std::string> table_names;
  discover_table_names(database, table_names);
  
  // Delete all table metadata
  for (const auto& table : table_names) {
    delete_table_metadata(database, table);
  }
  
  // Drop the data table
  std::string data_table_name = kvt_constants::make_data_table_name(database);
  uint64_t data_table_id;
  KVTError err = kvt_get_table_id(data_table_name, data_table_id, error_msg);
  if (err == KVTError::SUCCESS) {
    kvt_drop_table(data_table_id, error_msg);
  }
  
  // Delete database info
  return delete_database_info(database);
}

int CatalogManager::get_database_info(const std::string& database, DatabaseInfo& info) {
  std::string error_msg;
  uint64_t cat_id = get_catalog_table_id();
  if (cat_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  std::string key = kvt_constants::make_db_key(database);
  std::string value;
  
  KVTError err = kvt_get(0, cat_id, KVTKey(key), value, error_msg);
  if (err != KVTError::SUCCESS) {
    return HA_ERR_KEY_NOT_FOUND;
  }
  
  // Parse database info (simplified - should use JSON)
  info.name = database;
  // Parse kvt_data_table_id from value...
  
  return 0;
}

uint64_t CatalogManager::get_data_table_id(const std::string& database) {
  // Check cache first
  auto it = db_table_id_cache.find(database);
  if (it != db_table_id_cache.end()) {
    return it->second;
  }
  
  // Look up from KVT
  std::string data_table_name = kvt_constants::make_data_table_name(database);
  uint64_t data_table_id = 0;
  std::string error_msg;
  
  KVTError err = kvt_get_table_id(data_table_name, data_table_id, error_msg);
  if (err == KVTError::SUCCESS && data_table_id != 0) {
    cache_db_table_id(database, data_table_id);
  }
  
  return data_table_id;
}

int CatalogManager::create_table(const std::string& database, const std::string& table,
                                const TABLE* form, HA_CREATE_INFO* create_info) {
  if (!kvt_constants::is_valid_name(database) || !kvt_constants::is_valid_name(table)) {
    return HA_ERR_WRONG_COMMAND;
  }
  
  // Create metadata from TABLE structure
  std::unique_ptr<TableMetadata> metadata(create_metadata_from_table(form));
  metadata->database_name = database;
  metadata->table_name = table;
  metadata->created_timestamp = std::time(nullptr);
  metadata->updated_timestamp = metadata->created_timestamp;
  metadata->schema_version = kvt_constants::CATALOG_VERSION;
  
  // Determine partition method
  if (form->s->keys > 0 && form->key_info[0].flags & HA_NOSAME) {
    metadata->partition_method = "range";
  } else {
    metadata->partition_method = "hash";
  }
  
  // Store metadata
  return store_table_metadata(database, table, *metadata);
}

int CatalogManager::drop_table(const std::string& database, const std::string& table) {
  // Delete all rows
  int ret = delete_all_table_rows(database, table);
  if (ret != 0 && ret != HA_ERR_KEY_NOT_FOUND) {
    return ret;
  }
  
  // Delete metadata
  return delete_table_metadata(database, table);
}

int CatalogManager::store_table_metadata(const std::string& database, 
                                        const std::string& table,
                                        const TableMetadata& metadata) {
  std::string error_msg;
  uint64_t cat_id = get_catalog_table_id();
  if (cat_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  std::string key = kvt_constants::make_table_key(database, table);
  std::string value = metadata.serialize();
  
  KVTError err = kvt_set(0, cat_id, KVTKey(key), value, error_msg);
  
  if (err == KVTError::SUCCESS) {
    // Cache the metadata
    auto metadata_copy = std::make_shared<TableMetadata>(metadata);
    cache_table_metadata(database, table, metadata_copy);
  }
  
  return (err == KVTError::SUCCESS) ? 0 : HA_ERR_GENERIC;
}

int CatalogManager::load_table_metadata(const std::string& database,
                                       const std::string& table,
                                       TableMetadata& metadata) {
  // Check cache first
  std::string cache_key = database + "." + table;
  auto it = table_metadata_cache.find(cache_key);
  if (it != table_metadata_cache.end()) {
    metadata = *(it->second);
    return 0;
  }
  
  std::string error_msg;
  uint64_t cat_id = get_catalog_table_id();
  if (cat_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  std::string key = kvt_constants::make_table_key(database, table);
  std::string value;
  
  KVTError err = kvt_get(0, cat_id, KVTKey(key), value, error_msg);
  if (err != KVTError::SUCCESS) {
    return HA_ERR_KEY_NOT_FOUND;
  }
  
  if (!metadata.deserialize(value)) {
    return HA_ERR_GENERIC;
  }
  
  // Cache the metadata
  auto metadata_copy = std::make_shared<TableMetadata>(metadata);
  cache_table_metadata(database, table, metadata_copy);
  
  return 0;
}

int CatalogManager::delete_table_metadata(const std::string& database,
                                         const std::string& table) {
  std::string error_msg;
  uint64_t cat_id = get_catalog_table_id();
  if (cat_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  // Delete table metadata
  std::string key = kvt_constants::make_table_key(database, table);
  KVTError err = kvt_del(0, cat_id, KVTKey(key), error_msg);
  
  // Delete sequences
  // TODO: Range delete all sequences for this table
  
  // Clear from cache
  std::string cache_key = database + "." + table;
  table_metadata_cache.erase(cache_key);
  
  return (err == KVTError::SUCCESS || err == KVTError::KEY_NOT_FOUND) ? 0 : HA_ERR_GENERIC;
}

int CatalogManager::discover_table_names(const std::string& database,
                                        std::vector<std::string>& table_names) {
  std::string error_msg;
  uint64_t cat_id = get_catalog_table_id();
  if (cat_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  table_names.clear();
  
  // Range scan for all tables in database
  KVTKey start_key(kvt_constants::make_table_scan_start(database));
  KVTKey end_key(kvt_constants::make_table_scan_end(database));
  
  std::vector<std::pair<KVTKey, std::string>> results;
  KVTError err = kvt_scan(0, cat_id, start_key, end_key, 10000, results, error_msg);
  
  if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
    return HA_ERR_GENERIC;
  }
  
  // Extract table names from keys
  std::string prefix = std::string(kvt_constants::TABLE_PREFIX) + 
                      kvt_constants::SEPARATOR + database + kvt_constants::SEPARATOR;
  size_t prefix_len = prefix.length();
  
  for (const auto& [key, value] : results) {
    if (key.length() > prefix_len) {
      std::string table_name = key.substr(prefix_len);
      table_names.push_back(table_name);
    }
  }
  
  return 0;
}

int CatalogManager::delete_all_table_rows(const std::string& database,
                                         const std::string& table) {
  std::string error_msg;
  uint64_t data_table_id = get_data_table_id(database);
  if (data_table_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  // Use range update with delete function
  KVTKey start_key(kvt_constants::make_data_scan_start(table));
  KVTKey end_key(kvt_constants::make_data_scan_end(table));
  
  // Define a delete function
  KVUpdateFunc delete_func = [](const KVTKey& key, const std::string& old_val,
                                const std::string& param, std::string& new_val,
                                std::string& result) -> std::tuple<bool, bool, bool> {
    // Return: success=true, update=false (delete), return_value=false
    return std::make_tuple(true, false, false);
  };
  
  std::vector<std::pair<KVTKey, std::string>> results;
  KVTError err = kvt_range_update(0, data_table_id, start_key, end_key,
                                 1000000, delete_func, "", results, error_msg);
  
  return (err == KVTError::SUCCESS || err == KVTError::KEY_NOT_FOUND) ? 0 : HA_ERR_GENERIC;
}

int CatalogManager::get_next_auto_increment(const std::string& database,
                                           const std::string& table,
                                           const std::string& column,
                                           uint64_t& value) {
  std::string error_msg;
  uint64_t cat_id = get_catalog_table_id();
  if (cat_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  std::string key = kvt_constants::make_seq_key(database, table, column);
  
  // Use update function to atomically increment
  KVUpdateFunc inc_func = [&value](const KVTKey& key, const std::string& old_val,
                                   const std::string& param, std::string& new_val,
                                   std::string& result) -> std::tuple<bool, bool, bool> {
    if (old_val.empty()) {
      value = 1;
    } else {
      value = std::stoull(old_val) + 1;
    }
    new_val = std::to_string(value);
    result = new_val;
    return std::make_tuple(true, true, true);
  };
  
  std::string result;
  KVTError err = kvt_update(0, cat_id, KVTKey(key), inc_func, "", result, error_msg);
  
  return (err == KVTError::SUCCESS) ? 0 : HA_ERR_GENERIC;
}

bool CatalogManager::parse_table_path(const char* path, std::string& database,
                                     std::string& table) {
  std::string path_str(path);
  
  // Find last directory separator
  size_t last_slash = path_str.find_last_of("/\\");
  if (last_slash == std::string::npos) {
    return false;
  }
  
  // Extract table name (remove extension)
  std::string filename = path_str.substr(last_slash + 1);
  size_t dot_pos = filename.find('.');
  if (dot_pos != std::string::npos) {
    table = filename.substr(0, dot_pos);
  } else {
    table = filename;
  }
  
  // Find database name (previous directory)
  size_t prev_slash = path_str.find_last_of("/\\", last_slash - 1);
  if (prev_slash != std::string::npos) {
    database = path_str.substr(prev_slash + 1, last_slash - prev_slash - 1);
  } else {
    database = "test";  // Default database
  }
  
  return true;
}

TableMetadata* CatalogManager::create_metadata_from_table(const TABLE* form) {
  TableMetadata* metadata = new TableMetadata();
  
  // Extract columns
  for (uint i = 0; i < form->s->fields; i++) {
    Field* field = form->field[i];
    ColumnDef col;
    col.name = field->field_name.str;
    col.type = field->real_type();
    col.length = field->field_length;
    col.flags = field->flags;
    col.field_index = i;
    
    // Get default value if exists
    // Check if field has default value
    if (field->default_value) {
      col.default_value = "";
    }
    
    metadata->columns.push_back(col);
    
    // Track auto-increment columns
    if (field->flags & AUTO_INCREMENT_FLAG) {
      metadata->auto_increment_values[col.name] = 0;
    }
  }
  
  // Extract indexes
  for (uint i = 0; i < form->s->keys; i++) {
    KEY* key_info = &form->key_info[i];
    IndexDef idx;
    idx.name = key_info->name.str;
    idx.is_primary = (strcmp(idx.name.c_str(), "PRIMARY") == 0);
    idx.is_unique = (key_info->flags & HA_NOSAME) != 0;
    
    for (uint j = 0; j < key_info->user_defined_key_parts; j++) {
      KEY_PART_INFO* key_part = &key_info->key_part[j];
      idx.column_indices.push_back(key_part->fieldnr - 1);
      idx.key_parts_length.push_back(key_part->length);
    }
    
    metadata->indexes.push_back(idx);
  }
  
  return metadata;
}

int CatalogManager::store_database_info(const std::string& database,
                                       const DatabaseInfo& info) {
  std::string error_msg;
  uint64_t cat_id = get_catalog_table_id();
  if (cat_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  // Serialize database info
  std::stringstream ss;
  ss << "name=" << info.name << "\n";
  ss << "kvt_data_table_id=" << info.kvt_data_table_id << "\n";
  ss << "created_timestamp=" << info.created_timestamp << "\n";
  std::string value = ss.str();
  
  std::string key = kvt_constants::make_db_key(database);
  KVTError err = kvt_set(0, cat_id, KVTKey(key), value, error_msg);
  
  if (err == KVTError::SUCCESS) {
    cache_db_table_id(database, info.kvt_data_table_id);
  }
  
  return (err == KVTError::SUCCESS) ? 0 : HA_ERR_GENERIC;
}

int CatalogManager::delete_database_info(const std::string& database) {
  std::string error_msg;
  uint64_t cat_id = get_catalog_table_id();
  if (cat_id == 0) {
    return HA_ERR_GENERIC;
  }
  
  std::string key = kvt_constants::make_db_key(database);
  KVTError err = kvt_del(0, cat_id, KVTKey(key), error_msg);
  
  // Clear from cache
  db_table_id_cache.erase(database);
  
  return (err == KVTError::SUCCESS || err == KVTError::KEY_NOT_FOUND) ? 0 : HA_ERR_GENERIC;
}

void CatalogManager::clear_cache() {
  db_table_id_cache.clear();
  table_metadata_cache.clear();
}

void CatalogManager::cache_db_table_id(const std::string& database, uint64_t table_id) {
  db_table_id_cache[database] = table_id;
}

void CatalogManager::cache_table_metadata(const std::string& database,
                                         const std::string& table,
                                         std::shared_ptr<TableMetadata> metadata) {
  std::string key = database + "." + table;
  table_metadata_cache[key] = metadata;
}

} // namespace kvt_catalog