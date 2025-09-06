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

#include "kvt_composite_index.h"
#include "sql_class.h"
#include "field.h"
#include <cstring>
#include <algorithm>

namespace kvt_composite {

// Helper function to encode field value based on type
static std::string encode_field_value(Field* field, bool is_desc) {
    std::string result;
    
    // Handle NULL
    if (field->is_null()) {
        result.append(1, NULL_MARKER);
        return result;
    }
    
    // Add non-NULL marker
    result.append(1, 0x00);
    
    switch (field->type()) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG: {
            // Integer types
            int64_t value = field->val_int();
            bool is_unsigned = field->flags & UNSIGNED_FLAG;
            
            // Convert to big-endian for proper sorting
            uint64_t sort_value;
            if (is_unsigned) {
                sort_value = static_cast<uint64_t>(value);
            } else {
                // For signed integers, flip the sign bit for proper sorting
                sort_value = static_cast<uint64_t>(value) ^ (1ULL << 63);
            }
            
            uint64_t be_value = htobe64(sort_value);
            
            // Apply DESC mask if needed
            if (is_desc) {
                uint8_t* bytes = reinterpret_cast<uint8_t*>(&be_value);
                for (int i = 0; i < 8; i++) {
                    bytes[i] ^= DESC_XOR_MASK;
                }
            }
            
            result.append(reinterpret_cast<const char*>(&be_value), sizeof(be_value));
            break;
        }
        
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING: {
            // String types
            char buff[MAX_FIELD_WIDTH];
            String str(buff, sizeof(buff), field->charset());
            field->val_str(&str);
            
            // Add length prefix (2 bytes)
            uint16_t len = str.length();
            uint16_t be_len = htobe16(len);
            result.append(reinterpret_cast<const char*>(&be_len), sizeof(be_len));
            
            // Add string data
            if (is_desc) {
                // Invert bytes for DESC
                for (uint i = 0; i < str.length(); i++) {
                    result.append(1, str.ptr()[i] ^ DESC_XOR_MASK);
                }
            } else {
                result.append(str.ptr(), str.length());
            }
            break;
        }
        
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP: {
            // Date/time types - stored as 8-byte integer
            int64_t value = field->val_int();
            uint64_t be_value = htobe64(value);
            
            if (is_desc) {
                uint8_t* bytes = reinterpret_cast<uint8_t*>(&be_value);
                for (int i = 0; i < 8; i++) {
                    bytes[i] ^= DESC_XOR_MASK;
                }
            }
            
            result.append(reinterpret_cast<const char*>(&be_value), sizeof(be_value));
            break;
        }
        
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
            // Decimal types
            my_decimal decimal_value;
            field->val_decimal(&decimal_value);
            
            // Convert to binary comparable format
            // For simplicity, convert to string and pad
            char dec_buff[DECIMAL_MAX_STR_LENGTH];
            int dec_len = sizeof(dec_buff);
            decimal2string(&decimal_value, dec_buff, &dec_len, 0, 0, 0);
            
            // Ensure fixed length for comparison
            std::string padded(40, '0');  // Use fixed 40 bytes for decimals
            if (dec_len > 0) {
                std::memcpy(&padded[40 - dec_len], dec_buff, 
                           std::min(dec_len, 40));
            }
            
            if (is_desc) {
                for (char& c : padded) {
                    c ^= DESC_XOR_MASK;
                }
            }
            
            result.append(padded);
            break;
        }
        
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE: {
            // Floating point types
            double value = field->val_real();
            
            // Convert to sortable binary format
            uint64_t sort_value;
            std::memcpy(&sort_value, &value, sizeof(value));
            
            // Make negative numbers sort correctly
            if (value < 0) {
                sort_value = ~sort_value;
            } else {
                sort_value |= (1ULL << 63);
            }
            
            uint64_t be_value = htobe64(sort_value);
            
            if (is_desc) {
                uint8_t* bytes = reinterpret_cast<uint8_t*>(&be_value);
                for (int i = 0; i < 8; i++) {
                    bytes[i] ^= DESC_XOR_MASK;
                }
            }
            
            result.append(reinterpret_cast<const char*>(&be_value), sizeof(be_value));
            break;
        }
        
        default:
            // For unsupported types, use string representation
            char buff[MAX_FIELD_WIDTH];
            String str(buff, sizeof(buff), field->charset());
            field->val_str(&str);
            
            uint16_t len = str.length();
            uint16_t be_len = htobe16(len);
            result.append(reinterpret_cast<const char*>(&be_len), sizeof(be_len));
            result.append(str.ptr(), str.length());
            break;
    }
    
    return result;
}

std::string build_composite_index_key(
    uint64_t table_id,
    uint32_t index_id,
    KEY* key_info,
    const uchar* key_data,
    key_part_map keypart_map,
    uint64_t row_id)
{
    std::string result;
    
    // Add table_id and index_id
    append_bigendian(result, table_id);
    append_bigendian(result, index_id);
    
    // Process each key part specified in keypart_map
    const uchar* key_ptr = key_data;
    
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        // Check if this key part is included
        if (!(keypart_map & (key_part_map(1) << i))) {
            break;  // No more key parts in the map
        }
        
        KEY_PART_INFO* key_part = &key_info->key_part[i];
        Field* field = key_part->field;
        
        // Skip null flag byte if present
        if (field->real_maybe_null()) {
            if (*key_ptr) {
                // NULL value
                result.append(1, NULL_MARKER);
                key_ptr++;
                continue;
            }
            key_ptr++;  // Skip null flag
        }
        
        // Create temporary field pointing to key data
        field->set_key_image(key_ptr, key_part->length);
        
        // Encode the field value
        bool is_desc = (key_part->key_part_flag & HA_REVERSE_SORT);
        std::string encoded = encode_field_value(field, is_desc);
        result.append(encoded);
        
        // Add separator between columns (except after last)
        if (i < key_info->user_defined_key_parts - 1) {
            result.append(1, COLUMN_SEPARATOR);
        }
        
        // Move to next key part
        key_ptr += key_part->store_length;
    }
    
    // Add row_id at the end for uniqueness
    append_bigendian(result, row_id);
    
    return result;
}

std::string build_composite_key_from_record(
    uint64_t table_id,
    uint32_t index_id,
    KEY* key_info,
    const uchar* record,
    uint64_t row_id)
{
    std::string result;
    
    // Add table_id and index_id
    append_bigendian(result, table_id);
    append_bigendian(result, index_id);
    
    // Process each key part from the record
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        KEY_PART_INFO* key_part = &key_info->key_part[i];
        Field* field = key_part->field;
        
        // Point field to the record
        my_ptrdiff_t old_offset = field->ptr - field->table->record[0];
        field->move_field_offset(record - field->table->record[0] - old_offset);
        
        // Encode the field value
        bool is_desc = (key_part->key_part_flag & HA_REVERSE_SORT);
        std::string encoded = encode_field_value(field, is_desc);
        result.append(encoded);
        
        // Restore field pointer
        field->move_field_offset(old_offset);
        
        // Add separator between columns (except after last)
        if (i < key_info->user_defined_key_parts - 1) {
            result.append(1, COLUMN_SEPARATOR);
        }
    }
    
    // Add row_id at the end
    append_bigendian(result, row_id);
    
    return result;
}

int compare_composite_keys(
    const std::string& key1,
    const std::string& key2,
    KEY* key_info,
    uint num_parts)
{
    size_t offset1 = 12;  // Skip table_id (8) and index_id (4)
    size_t offset2 = 12;
    
    for (uint i = 0; i < num_parts && i < key_info->user_defined_key_parts; i++) {
        KEY_PART_INFO* key_part = &key_info->key_part[i];
        bool is_desc = (key_part->key_part_flag & HA_REVERSE_SORT);
        
        // Check for end of key
        if (offset1 >= key1.length() || offset2 >= key2.length()) {
            // Shorter key comes first
            return (offset1 >= key1.length()) ? -1 : 1;
        }
        
        // Check for NULL
        uint8_t null1 = key1[offset1];
        uint8_t null2 = key2[offset2];
        
        if (null1 == NULL_MARKER || null2 == NULL_MARKER) {
            if (null1 == null2) {
                offset1++;
                offset2++;
                continue;  // Both NULL, equal for this column
            }
            // NULL sorts after non-NULL
            return (null1 == NULL_MARKER) ? 1 : -1;
        }
        
        // Skip non-NULL marker
        offset1++;
        offset2++;
        
        // Compare based on field type
        Field* field = key_part->field;
        size_t compare_len = 0;
        
        switch (field->type()) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_DATETIME:
            case MYSQL_TYPE_TIMESTAMP:
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
                compare_len = 8;  // All stored as 8 bytes
                break;
                
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_STRING: {
                // Read length prefix
                uint16_t len1, len2;
                std::memcpy(&len1, &key1[offset1], 2);
                std::memcpy(&len2, &key2[offset2], 2);
                len1 = be16toh(len1);
                len2 = be16toh(len2);
                
                offset1 += 2;
                offset2 += 2;
                
                // Compare strings
                size_t min_len = std::min(len1, len2);
                int cmp = std::memcmp(&key1[offset1], &key2[offset2], min_len);
                if (cmp != 0) {
                    return is_desc ? -cmp : cmp;
                }
                
                // If equal prefix, shorter string comes first
                if (len1 != len2) {
                    return is_desc ? (len2 - len1) : (len1 - len2);
                }
                
                offset1 += len1;
                offset2 += len2;
                continue;
            }
            
            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL:
                compare_len = 40;  // Fixed size for decimals
                break;
                
            default:
                // Skip this field - can't compare
                return 0;
        }
        
        // Compare fixed-length fields
        if (compare_len > 0) {
            int cmp = std::memcmp(&key1[offset1], &key2[offset2], compare_len);
            if (cmp != 0) {
                return is_desc ? -cmp : cmp;
            }
            offset1 += compare_len;
            offset2 += compare_len;
        }
        
        // Skip separator if not last column
        if (i < num_parts - 1 && offset1 < key1.length() && offset2 < key2.length()) {
            if (key1[offset1] == COLUMN_SEPARATOR) offset1++;
            if (key2[offset2] == COLUMN_SEPARATOR) offset2++;
        }
    }
    
    return 0;  // Keys are equal for the compared parts
}

std::string create_composite_range_end_key(const std::string& partial_key) {
    // Create an end key by appending 0xFF bytes
    std::string end_key = partial_key;
    
    // Add maximum possible values to make this an upper bound
    end_key.append(8, 0xFF);  // Add space for potential row_id
    
    return end_key;
}

bool matches_composite_prefix(
    const std::string& full_key,
    const std::string& partial_key,
    KEY* key_info,
    uint num_parts)
{
    // Check if full_key starts with partial_key (considering encoding)
    if (full_key.length() < partial_key.length()) {
        return false;
    }
    
    // Compare the prefix
    return std::memcmp(full_key.data(), partial_key.data(), partial_key.length()) == 0;
}

// Encoding helper functions

std::string encode_int_for_key(int64_t value, bool is_unsigned, bool is_desc) {
    std::string result;
    
    uint64_t sort_value;
    if (is_unsigned) {
        sort_value = static_cast<uint64_t>(value);
    } else {
        // Flip sign bit for proper sorting of negative numbers
        sort_value = static_cast<uint64_t>(value) ^ (1ULL << 63);
    }
    
    uint64_t be_value = htobe64(sort_value);
    
    if (is_desc) {
        uint8_t* bytes = reinterpret_cast<uint8_t*>(&be_value);
        for (int i = 0; i < 8; i++) {
            bytes[i] ^= DESC_XOR_MASK;
        }
    }
    
    result.append(reinterpret_cast<const char*>(&be_value), sizeof(be_value));
    return result;
}

std::string encode_varchar_for_key(const std::string& value, uint max_length, bool is_desc) {
    std::string result;
    
    // Add length prefix
    uint16_t len = std::min(static_cast<uint16_t>(value.length()), 
                            static_cast<uint16_t>(max_length));
    uint16_t be_len = htobe16(len);
    result.append(reinterpret_cast<const char*>(&be_len), sizeof(be_len));
    
    // Add string data
    if (is_desc) {
        for (uint i = 0; i < len; i++) {
            result.append(1, value[i] ^ DESC_XOR_MASK);
        }
    } else {
        result.append(value.data(), len);
    }
    
    return result;
}

std::string encode_date_for_key(uint64_t value, bool is_desc) {
    uint64_t be_value = htobe64(value);
    
    if (is_desc) {
        uint8_t* bytes = reinterpret_cast<uint8_t*>(&be_value);
        for (int i = 0; i < 8; i++) {
            bytes[i] ^= DESC_XOR_MASK;
        }
    }
    
    std::string result;
    result.append(reinterpret_cast<const char*>(&be_value), sizeof(be_value));
    return result;
}

std::string encode_decimal_for_key(const my_decimal* decimal, uint precision, uint scale, bool is_desc) {
    // Convert decimal to string for comparison
    char buff[DECIMAL_MAX_STR_LENGTH];
    int str_len = sizeof(buff);
    decimal2string(decimal, buff, &str_len, 0, 0, 0);
    
    // Pad to fixed length for proper comparison
    std::string result(40, '0');
    if (str_len > 0) {
        int copy_len = std::min(str_len, 40);
        std::memcpy(&result[40 - copy_len], buff, copy_len);
    }
    
    if (is_desc) {
        for (char& c : result) {
            c ^= DESC_XOR_MASK;
        }
    }
    
    return result;
}

} // namespace kvt_composite