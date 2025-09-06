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

#ifndef KVT_INDEX_ONLY_SCAN_H
#define KVT_INDEX_ONLY_SCAN_H

#include "my_global.h"
#include "sql_class.h"
#include "key.h"
#include "field.h"
#include "my_bitmap.h"

// Define HA_SPATIAL if not defined
#ifndef HA_SPATIAL
  #define HA_SPATIAL 0x00001000
#endif
#include <string>
#include <vector>

namespace kvt_index_only {

// Configuration for index-only scans
struct IndexOnlyConfig {
    bool enabled = true;                    // Global enable/disable
    uint max_covered_columns = 10;          // Max columns to cover
    size_t max_covered_size = 1024;         // Max bytes for covered data
    bool auto_cover_primary_key = true;     // Auto-include PK columns
};

// Metadata about covered columns for an index
struct CoveredColumnInfo {
    uint field_index;           // Field index in table
    uint offset_in_value;       // Offset in index value
    uint length;                // Length in index value (0 for variable)
    bool is_nullable;           // Can contain NULL
};

/**
 * Check if a query can use index-only scan
 * 
 * @param table        Table being queried
 * @param index        Index number
 * @param read_set     Bitmap of columns needed by query
 * @param key_info     Key metadata
 * @return true if index-only scan is possible
 */
bool can_use_index_only_scan(
    TABLE* table,
    uint index,
    MY_BITMAP* read_set,
    KEY* key_info);

/**
 * Get list of columns that should be covered by an index
 * 
 * @param table        Table definition
 * @param key_info     Index definition
 * @param config       Configuration options
 * @return Bitmap of columns to cover
 */
MY_BITMAP* get_covered_columns_for_index(
    TABLE* table,
    KEY* key_info,
    const IndexOnlyConfig& config);

/**
 * Encode covered column values into index value
 * 
 * @param row_id       Row identifier
 * @param record       Record buffer
 * @param table        Table definition
 * @param covered_cols Bitmap of columns to include
 * @return Encoded index value with covered columns
 */
std::string encode_index_value_with_covered_columns(
    uint64_t row_id,
    const uchar* record,
    TABLE* table,
    MY_BITMAP* covered_cols);

/**
 * Decode covered columns from index value
 * 
 * @param value        Index value containing covered columns
 * @param row_id       Output: extracted row ID
 * @param buf          Output buffer for record
 * @param table        Table definition
 * @param covered_cols Bitmap of covered columns
 * @param read_set     Bitmap of requested columns
 * @return 0 on success, error code otherwise
 */
int decode_covered_columns_from_index_value(
    const std::string& value,
    uint64_t& row_id,
    uchar* buf,
    TABLE* table,
    MY_BITMAP* covered_cols,
    MY_BITMAP* read_set);

/**
 * Check if a field is covered by an index
 * 
 * @param key_info     Index definition
 * @param field_index  Field index to check
 * @param covered_cols Bitmap of covered columns
 * @return true if field is covered
 */
bool is_field_covered_by_index(
    KEY* key_info,
    uint field_index,
    MY_BITMAP* covered_cols);

/**
 * Determine if an index should include covered columns
 * 
 * @param key_info     Index definition
 * @param table        Table definition
 * @param config       Configuration options
 * @return true if index should store covered columns
 */
bool should_store_covered_columns(
    KEY* key_info,
    TABLE* table,
    const IndexOnlyConfig& config);

/**
 * Fill non-covered fields with default/NULL values
 * 
 * @param buf          Record buffer
 * @param table        Table definition
 * @param covered_cols Bitmap of covered columns
 */
void fill_non_covered_fields_with_defaults(
    uchar* buf,
    TABLE* table,
    MY_BITMAP* covered_cols);

/**
 * Calculate size of covered column data
 * 
 * @param record       Record buffer
 * @param table        Table definition
 * @param covered_cols Bitmap of columns to cover
 * @return Size in bytes
 */
size_t calculate_covered_data_size(
    const uchar* record,
    TABLE* table,
    MY_BITMAP* covered_cols);

/**
 * Get metadata about covered columns for an index
 * 
 * @param key_info     Index definition
 * @param table        Table definition
 * @param covered_cols Bitmap of covered columns
 * @return Vector of covered column information
 */
std::vector<CoveredColumnInfo> get_covered_column_metadata(
    KEY* key_info,
    TABLE* table,
    MY_BITMAP* covered_cols);

// Helper functions for encoding/decoding

/**
 * Encode a single field for storage in index value
 * 
 * @param field        Field to encode
 * @return Encoded field data
 */
std::string encode_field_for_index(Field* field);

/**
 * Decode a field from index value data
 * 
 * @param field        Field to decode into
 * @param data         Data pointer
 * @param length       Data length
 * @return Bytes consumed
 */
size_t decode_field_from_index(Field* field, const uchar* data, size_t length);

// Statistics tracking

struct IndexOnlyScanStats {
    uint64_t total_scans = 0;           // Total index scans
    uint64_t index_only_scans = 0;      // Scans that used index-only
    uint64_t rows_from_index = 0;       // Rows returned from index
    uint64_t rows_fetched = 0;          // Rows that required fetch
    uint64_t bytes_saved = 0;           // Estimated I/O saved
};

/**
 * Update statistics for index-only scan usage
 * 
 * @param stats        Statistics structure
 * @param used_index_only Whether index-only scan was used
 * @param rows_returned   Number of rows returned
 */
void update_index_only_stats(
    IndexOnlyScanStats& stats,
    bool used_index_only,
    uint64_t rows_returned);

// Utility functions

/**
 * Check if all fields in a bitmap are part of index key
 * 
 * @param key_info     Index definition
 * @param read_set     Bitmap of required fields
 * @return true if all fields are in index key
 */
bool all_fields_in_index_key(KEY* key_info, MY_BITMAP* read_set);

/**
 * Create a bitmap of fields that are part of index key
 * 
 * @param key_info     Index definition
 * @param table        Table definition
 * @return Bitmap of index key fields
 */
MY_BITMAP* create_index_key_field_bitmap(KEY* key_info, TABLE* table);

} // namespace kvt_index_only

#endif // KVT_INDEX_ONLY_SCAN_H