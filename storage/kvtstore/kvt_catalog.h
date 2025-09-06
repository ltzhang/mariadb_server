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

#ifndef KVT_CATALOG_H
#define KVT_CATALOG_H

#include "kvt_constants.h"
#include "kvt/kvt_inc.h"
#include "handler.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

// Forward declarations
struct TABLE;
class Field;

namespace kvt_catalog {

// Column definition
struct ColumnDef {
  std::string name;
  enum_field_types type;
  uint32_t length;
  uint32_t flags;  // NOT NULL, AUTO_INCREMENT, etc.
  std::string default_value;
  uint32_t field_index;  // Position in table
};

// Index definition
struct IndexDef {
  std::string name;
  bool is_primary;
  bool is_unique;
  std::vector<uint16_t> column_indices;
  std::vector<uint32_t> key_parts_length;
};

// Database metadata
struct DatabaseInfo {
  std::string name;
  uint64_t kvt_data_table_id;
  uint64_t created_timestamp;
  std::map<std::string, std::string> options;
};

// Table metadata
struct TableMetadata {
  // Identity
  std::string database_name;
  std::string table_name;
  uint32_t schema_version;
  
  // Schema
  std::vector<ColumnDef> columns;
  std::vector<IndexDef> indexes;
  
  // Storage options
  std::string partition_method;  // "hash" or "range"
  std::string row_format;
  std::string charset;
  
  // Statistics
  uint64_t row_count;
  std::map<std::string, uint64_t> auto_increment_values;
  
  // Timestamps
  uint64_t created_timestamp;
  uint64_t updated_timestamp;
  
  // Serialization
  std::string serialize() const;
  bool deserialize(const std::string& data);
};

// Main catalog manager class
class CatalogManager {
private:
  static CatalogManager* instance;
  uint64_t catalog_table_id;
  bool initialized;
  
  // Cache for frequently accessed metadata
  std::map<std::string, uint64_t> db_table_id_cache;
  std::map<std::string, std::shared_ptr<TableMetadata>> table_metadata_cache;
  
  CatalogManager();
  
public:
  static CatalogManager* get_instance();
  static void cleanup();
  
  // Initialization
  int initialize();
  bool is_initialized() const { return initialized; }
  
  // Catalog table operations
  uint64_t get_catalog_table_id();
  int ensure_catalog_exists();
  
  // Database operations
  int create_database(const std::string& database);
  int drop_database(const std::string& database);
  int get_database_info(const std::string& database, DatabaseInfo& info);
  int list_databases(std::vector<std::string>& databases);
  uint64_t get_data_table_id(const std::string& database);
  
  // Table operations
  int create_table(const std::string& database, const std::string& table,
                  const TABLE* form, HA_CREATE_INFO* create_info);
  int drop_table(const std::string& database, const std::string& table);
  int rename_table(const std::string& from_db, const std::string& from_table,
                  const std::string& to_db, const std::string& to_table);
  
  // Metadata operations
  int store_table_metadata(const std::string& database, const std::string& table,
                          const TableMetadata& metadata);
  int load_table_metadata(const std::string& database, const std::string& table,
                         TableMetadata& metadata);
  int delete_table_metadata(const std::string& database, const std::string& table);
  int update_table_statistics(const std::string& database, const std::string& table,
                             uint64_t row_count);
  
  // Discovery operations
  int discover_table_names(const std::string& database, 
                          std::vector<std::string>& table_names);
  int discover_all_tables(std::vector<std::pair<std::string, std::string>>& tables);
  
  // Sequence operations
  int get_next_auto_increment(const std::string& database, const std::string& table,
                             const std::string& column, uint64_t& value);
  int set_auto_increment(const std::string& database, const std::string& table,
                        const std::string& column, uint64_t value);
  
  // Data operations helpers
  int delete_all_table_rows(const std::string& database, const std::string& table);
  
  // Utility functions
  static bool parse_table_path(const char* path, std::string& database, 
                              std::string& table);
  static TableMetadata* create_metadata_from_table(const TABLE* form);
  
private:
  // Helper methods
  int store_database_info(const std::string& database, const DatabaseInfo& info);
  int delete_database_info(const std::string& database);
  void clear_cache();
  void cache_db_table_id(const std::string& database, uint64_t table_id);
  void cache_table_metadata(const std::string& database, const std::string& table,
                           std::shared_ptr<TableMetadata> metadata);
};

} // namespace kvt_catalog

#endif // KVT_CATALOG_H