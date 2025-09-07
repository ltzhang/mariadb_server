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

#ifndef KVT_CONSTANTS_H
#define KVT_CONSTANTS_H

#include <string>
#include <cstdint>

namespace kvt_constants {

// System table names - prefixed with __ to prevent user creation
const char* const CATALOG_TABLE_NAME = "__CATALOG__";
const char* const DATA_TABLE_PREFIX = "__DATA_";
const char* const NULL_SEPARATOR = "\x00";

// Key prefixes in catalog table
const char* const DB_PREFIX = "DB";
const char* const TABLE_PREFIX = "TABLE";
const char* const SEQ_PREFIX = "SEQ";
const char* const SYS_PREFIX = "SYS";

// System keys
const char* const SYS_VERSION_KEY = "SYS\x00version";
const char* const SYS_INITIALIZED_KEY = "SYS\x00initialized";

// Separators
const char SEPARATOR = '\x00';        // NULL character for separation
const char NEXT_SEPARATOR = '\x01';   // SEPARATOR + 1 for range scans

// Version info
const uint32_t CATALOG_VERSION = 1;
const uint32_t ROW_FORMAT_VERSION = 1;

// Limits
const size_t MAX_DATABASE_NAME_LENGTH = 64;
const size_t MAX_TABLE_NAME_LENGTH = 64;
const size_t KVT_MAX_KEY_LENGTH = 3072;

// Helper functions for key generation
inline std::string make_db_key(const std::string& database) {
  return std::string(DB_PREFIX) + SEPARATOR + database;
}

inline std::string make_table_key(const std::string& database, const std::string& table) {
  return std::string(TABLE_PREFIX) + SEPARATOR + database + SEPARATOR + table;
}

inline std::string make_seq_key(const std::string& database, const std::string& table, 
                                const std::string& column) {
  return std::string(SEQ_PREFIX) + SEPARATOR + database + SEPARATOR + 
         table + SEPARATOR + column;
}

inline std::string make_data_table_name(const std::string& database) {
  return std::string(DATA_TABLE_PREFIX) + database;
}

inline std::string make_data_key(const std::string& table, const std::string& row_key) {
  return table + SEPARATOR + row_key;
}

// Range scan helpers
inline std::string make_table_scan_start(const std::string& database) {
  return std::string(TABLE_PREFIX) + SEPARATOR + database + SEPARATOR;
}

inline std::string make_table_scan_end(const std::string& database) {
  return std::string(TABLE_PREFIX) + SEPARATOR + database + NEXT_SEPARATOR;
}

inline std::string make_data_scan_start(const std::string& table) {
  return table + SEPARATOR;
}

inline std::string make_data_scan_end(const std::string& table) {
  return table + NEXT_SEPARATOR;
}

// Validation helpers
inline bool is_valid_name(const std::string& name) {
  // Check for NULL character
  if (name.find(SEPARATOR) != std::string::npos) {
    return false;
  }
  // Check for system prefix
  if (name.length() >= 2 && name[0] == '_' && name[1] == '_') {
    return false;
  }
  return true;
}

} // namespace kvt_constants

#endif // KVT_CONSTANTS_H