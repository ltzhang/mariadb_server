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

#ifndef KVT_COMPOSITE_INDEX_H
#define KVT_COMPOSITE_INDEX_H

#include "my_global.h"
#include "sql_class.h"
#include "key.h"
#include "field.h"
#include <string>
#include <vector>

namespace kvt_composite {

// Constants for composite key encoding
constexpr uint8_t NULL_MARKER = 0xFF;      // Marks NULL values
constexpr uint8_t COLUMN_SEPARATOR = 0x00; // Separates columns
constexpr uint8_t DESC_XOR_MASK = 0xFF;    // For DESC column encoding

/**
 * Builds a composite index key from multiple columns
 * 
 * @param table_id     Table identifier
 * @param index_id     Index identifier
 * @param key_info     Key metadata from MariaDB
 * @param key_data     Raw key data from MariaDB
 * @param keypart_map  Bitmap of which key parts are provided
 * @param row_id       Row identifier (for uniqueness)
 * @return Encoded composite key string
 */
std::string build_composite_index_key(
    uint64_t table_id,
    uint32_t index_id,
    KEY* key_info,
    const uchar* key_data,
    key_part_map keypart_map,
    uint64_t row_id);

/**
 * Builds a composite index key from record buffer
 * 
 * @param table_id     Table identifier
 * @param index_id     Index identifier
 * @param key_info     Key metadata from MariaDB
 * @param record       Record buffer
 * @param row_id       Row identifier
 * @return Encoded composite key string
 */
std::string build_composite_key_from_record(
    uint64_t table_id,
    uint32_t index_id,
    KEY* key_info,
    const uchar* record,
    uint64_t row_id);

/**
 * Extracts column values from a composite key
 * 
 * @param key          Composite key string
 * @param key_info     Key metadata
 * @param values       Output: extracted column values
 * @return 0 on success, error code otherwise
 */
int extract_composite_key_parts(
    const std::string& key,
    KEY* key_info,
    std::vector<std::string>& values);

/**
 * Compares two composite keys
 * 
 * @param key1         First composite key
 * @param key2         Second composite key
 * @param key_info     Key metadata
 * @param num_parts    Number of key parts to compare
 * @return <0 if key1<key2, 0 if equal, >0 if key1>key2
 */
int compare_composite_keys(
    const std::string& key1,
    const std::string& key2,
    KEY* key_info,
    uint num_parts);

/**
 * Creates a range end key for partial composite key searches
 * 
 * @param partial_key  Partial composite key
 * @return End key for range scan
 */
std::string create_composite_range_end_key(const std::string& partial_key);

/**
 * Checks if a key matches a partial key prefix
 * 
 * @param full_key     Full composite key
 * @param partial_key  Partial key to match
 * @param key_info     Key metadata
 * @param num_parts    Number of parts in partial key
 * @return true if matches, false otherwise
 */
bool matches_composite_prefix(
    const std::string& full_key,
    const std::string& partial_key,
    KEY* key_info,
    uint num_parts);

// Helper functions for encoding different data types

/**
 * Encodes an integer value for composite key
 * 
 * @param value        Integer value
 * @param is_unsigned  Whether the integer is unsigned
 * @param is_desc      Whether this is a DESC column
 * @return Encoded string
 */
std::string encode_int_for_key(int64_t value, bool is_unsigned, bool is_desc);

/**
 * Encodes a varchar/string value for composite key
 * 
 * @param value        String value
 * @param max_length   Maximum length for this column
 * @param is_desc      Whether this is a DESC column
 * @return Encoded string
 */
std::string encode_varchar_for_key(const std::string& value, uint max_length, bool is_desc);

/**
 * Encodes a date/datetime value for composite key
 * 
 * @param value        Date value (as MySQL internal format)
 * @param is_desc      Whether this is a DESC column
 * @return Encoded string
 */
std::string encode_date_for_key(uint64_t value, bool is_desc);

/**
 * Encodes a decimal value for composite key
 * 
 * @param decimal      Decimal value
 * @param precision    Decimal precision
 * @param scale        Decimal scale
 * @param is_desc      Whether this is a DESC column
 * @return Encoded string
 */
std::string encode_decimal_for_key(const my_decimal* decimal, uint precision, uint scale, bool is_desc);

/**
 * Helper to append big-endian encoded value
 */
inline void append_bigendian(std::string& str, uint64_t value) {
    uint64_t be_value = htobe64(value);
    str.append(reinterpret_cast<const char*>(&be_value), sizeof(be_value));
}

inline void append_bigendian(std::string& str, uint32_t value) {
    uint32_t be_value = htobe32(value);
    str.append(reinterpret_cast<const char*>(&be_value), sizeof(be_value));
}

/**
 * Helper to count the number of set bits in keypart_map
 */
inline uint count_key_parts(key_part_map keypart_map) {
    uint count = 0;
    while (keypart_map) {
        if (keypart_map & 1) count++;
        keypart_map >>= 1;
    }
    return count;
}

} // namespace kvt_composite

#endif // KVT_COMPOSITE_INDEX_H