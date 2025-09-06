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

#include "kvt_index_only_scan.h"
#include <cstring>
#include <algorithm>

namespace kvt_index_only {

bool can_use_index_only_scan(
    TABLE* table,
    uint index,
    MY_BITMAP* read_set,
    KEY* key_info)
{
    if (!table || !read_set || !key_info) {
        return false;
    }
    
    // Check each requested field
    for (uint i = 0; i < table->s->fields; i++) {
        if (bitmap_is_set(read_set, i)) {
            bool found = false;
            
            // Check if field is part of index key
            for (uint j = 0; j < key_info->user_defined_key_parts; j++) {
                if (key_info->key_part[j].field->field_index == i) {
                    found = true;
                    break;
                }
            }
            
            // If not in key, check if it's a covered column
            // For now, we'll assume PRIMARY KEY fields are always covered
            if (!found && table->s->primary_key != MAX_KEY) {
                KEY* pk = &table->key_info[table->s->primary_key];
                for (uint j = 0; j < pk->user_defined_key_parts; j++) {
                    if (pk->key_part[j].field->field_index == i) {
                        found = true;
                        break;
                    }
                }
            }
            
            if (!found) {
                return false;  // Field not available in index
            }
        }
    }
    
    return true;
}

MY_BITMAP* get_covered_columns_for_index(
    TABLE* table,
    KEY* key_info,
    const IndexOnlyConfig& config)
{
    if (!config.enabled) {
        return nullptr;
    }
    
    MY_BITMAP* covered = (MY_BITMAP*)malloc(sizeof(MY_BITMAP));
    my_bitmap_map* buf = (my_bitmap_map*)malloc(bitmap_buffer_size(table->s->fields));
    my_bitmap_init(covered, buf, table->s->fields);
    bitmap_clear_all(covered);
    
    uint covered_count = 0;
    size_t covered_size = 0;
    
    // First, mark all index key columns as covered
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        uint field_idx = key_info->key_part[i].field->field_index;
        bitmap_set_bit(covered, field_idx);
        covered_count++;
        covered_size += key_info->key_part[i].field->pack_length();
    }
    
    // Auto-cover primary key columns if configured
    if (config.auto_cover_primary_key && 
        table->s->primary_key != MAX_KEY) {
        
        KEY* pk = &table->key_info[table->s->primary_key];
        for (uint i = 0; i < pk->user_defined_key_parts; i++) {
            uint field_idx = pk->key_part[i].field->field_index;
            if (!bitmap_is_set(covered, field_idx)) {
                Field* field = pk->key_part[i].field;
                size_t field_size = field->pack_length();
                
                // Check limits
                if (covered_count >= config.max_covered_columns ||
                    covered_size + field_size > config.max_covered_size) {
                    break;
                }
                
                bitmap_set_bit(covered, field_idx);
                covered_count++;
                covered_size += field_size;
            }
        }
    }
    
    return covered;
}

std::string encode_index_value_with_covered_columns(
    uint64_t row_id,
    const uchar* record,
    TABLE* table,
    MY_BITMAP* covered_cols)
{
    std::string value;
    
    // Add row_id first (8 bytes, big-endian)
    uint64_t be_row_id = htobe64(row_id);
    value.append(reinterpret_cast<const char*>(&be_row_id), sizeof(be_row_id));
    
    // If no covered columns, just return row_id
    if (!covered_cols || bitmap_is_clear_all(covered_cols)) {
        return value;
    }
    
    // Add covered column count (2 bytes)
    uint16_t col_count = 0;
    for (uint i = 0; i < table->s->fields; i++) {
        if (bitmap_is_set(covered_cols, i)) {
            col_count++;
        }
    }
    uint16_t be_count = htobe16(col_count);
    value.append(reinterpret_cast<const char*>(&be_count), sizeof(be_count));
    
    // Add each covered column
    for (uint i = 0; i < table->s->fields; i++) {
        if (bitmap_is_set(covered_cols, i)) {
            Field* field = table->field[i];
            
            // Add field index (2 bytes)
            uint16_t be_field_idx = htobe16(i);
            value.append(reinterpret_cast<const char*>(&be_field_idx), sizeof(be_field_idx));
            
            // Add NULL flag (1 byte)
            if (field->is_null_in_record(record)) {
                value.append(1, 0x00);  // NULL marker
            } else {
                value.append(1, 0x01);  // Non-NULL marker
                
                // Add field data
                uint field_offset = field->offset(record);
                if (field->real_type() == MYSQL_TYPE_VARCHAR ||
                    field->real_type() == MYSQL_TYPE_STRING) {
                    // Variable length field - get actual length
                    uint32_t length = field->data_length();
                    uint16_t be_length = htobe16(std::min(length, (uint32_t)65535));
                    value.append(reinterpret_cast<const char*>(&be_length), sizeof(be_length));
                    
                    // Add actual data
                    value.append(reinterpret_cast<const char*>(record + field_offset), 
                                std::min(length, (uint32_t)65535));
                } else {
                    // Fixed length field
                    value.append(reinterpret_cast<const char*>(record + field_offset), 
                                field->pack_length());
                }
            }
        }
    }
    
    return value;
}

int decode_covered_columns_from_index_value(
    const std::string& value,
    uint64_t& row_id,
    uchar* buf,
    TABLE* table,
    MY_BITMAP* covered_cols,
    MY_BITMAP* read_set)
{
    if (value.length() < 8) {
        return HA_ERR_CORRUPT_EVENT;  // Too short for row_id
    }
    
    size_t offset = 0;
    
    // Extract row_id
    uint64_t be_row_id;
    std::memcpy(&be_row_id, value.data() + offset, sizeof(be_row_id));
    row_id = be64toh(be_row_id);
    offset += sizeof(be_row_id);
    
    // Check if we have covered columns
    if (offset >= value.length()) {
        // No covered columns, just row_id
        // Fill all fields with defaults/NULLs
        fill_non_covered_fields_with_defaults(buf, table, nullptr);
        return 0;
    }
    
    // Read covered column count
    if (offset + 2 > value.length()) {
        return HA_ERR_CORRUPT_EVENT;
    }
    uint16_t be_count;
    std::memcpy(&be_count, value.data() + offset, sizeof(be_count));
    uint16_t col_count = be16toh(be_count);
    offset += sizeof(be_count);
    
    // First, clear the record and set defaults
    // empty_record is a macro that takes only table parameter
    empty_record(table);
    
    // Decode each covered column
    for (uint16_t i = 0; i < col_count; i++) {
        if (offset + 2 > value.length()) {
            return HA_ERR_CORRUPT_EVENT;
        }
        
        // Read field index
        uint16_t be_field_idx;
        std::memcpy(&be_field_idx, value.data() + offset, sizeof(be_field_idx));
        uint16_t field_idx = be16toh(be_field_idx);
        offset += sizeof(be_field_idx);
        
        if (field_idx >= table->s->fields) {
            return HA_ERR_CORRUPT_EVENT;
        }
        
        Field* field = table->field[field_idx];
        
        // Read NULL flag
        if (offset >= value.length()) {
            return HA_ERR_CORRUPT_EVENT;
        }
        
        if (value[offset++] == 0x00) {
            // NULL value
            field->set_null();
        } else {
            // Non-NULL value
            field->set_notnull();
            
            if (field->real_type() == MYSQL_TYPE_VARCHAR ||
                field->real_type() == MYSQL_TYPE_STRING) {
                // Read length prefix
                if (offset + 2 > value.length()) {
                    return HA_ERR_CORRUPT_EVENT;
                }
                uint16_t be_length;
                std::memcpy(&be_length, value.data() + offset, sizeof(be_length));
                uint16_t length = be16toh(be_length);
                offset += sizeof(be_length);
                
                if (offset + length > value.length()) {
                    return HA_ERR_CORRUPT_EVENT;
                }
                
                // Copy data to field
                field->store(value.data() + offset, length, field->charset());
                offset += length;
            } else {
                // Fixed length field
                uint32_t length = field->pack_length();
                if (offset + length > value.length()) {
                    return HA_ERR_CORRUPT_EVENT;
                }
                
                std::memcpy(field->ptr, value.data() + offset, length);
                offset += length;
            }
        }
    }
    
    // For any fields not covered but needed, they should be fetched separately
    // Mark them as needing fetch if they're in read_set but not covered
    for (uint i = 0; i < table->s->fields; i++) {
        if (bitmap_is_set(read_set, i) && 
            (!covered_cols || !bitmap_is_set(covered_cols, i))) {
            // This field is needed but not covered
            // Handler should fetch the row for this field
            return HA_ERR_KEY_NOT_FOUND;  // Signal need for row fetch
        }
    }
    
    return 0;
}

bool is_field_covered_by_index(
    KEY* key_info,
    uint field_index,
    MY_BITMAP* covered_cols)
{
    // Check if field is part of index key
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        if (key_info->key_part[i].field->field_index == field_index) {
            return true;
        }
    }
    
    // Check if field is in covered columns bitmap
    if (covered_cols && bitmap_is_set(covered_cols, field_index)) {
        return true;
    }
    
    return false;
}

bool should_store_covered_columns(
    KEY* key_info,
    TABLE* table,
    const IndexOnlyConfig& config)
{
    if (!config.enabled) {
        return false;
    }
    
    // Don't store covered columns for special index types
    if (key_info->algorithm == HA_KEY_ALG_FULLTEXT ||
        (key_info->flags & HA_SPATIAL)) {
        return false;
    }
    
    // Always store for UNIQUE and PRIMARY KEY indexes
    if ((key_info->flags & HA_NOSAME) || 
        true) {  // Always consider for PRIMARY and UNIQUE
        return true;
    }
    
    // For regular indexes, check if it makes sense
    // (e.g., not too many columns, reasonable size)
    size_t total_size = 0;
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        total_size += key_info->key_part[i].field->pack_length();
    }
    
    return total_size <= config.max_covered_size;
}

void fill_non_covered_fields_with_defaults(
    uchar* buf,
    TABLE* table,
    MY_BITMAP* covered_cols)
{
    for (uint i = 0; i < table->s->fields; i++) {
        if (!covered_cols || !bitmap_is_set(covered_cols, i)) {
            Field* field = table->field[i];
            
            // Set to NULL if nullable, otherwise to default value
            if (field->maybe_null()) {
                field->set_null();
            } else {
                field->set_default();
            }
        }
    }
}

size_t calculate_covered_data_size(
    const uchar* record,
    TABLE* table,
    MY_BITMAP* covered_cols)
{
    size_t size = 0;
    
    // Row ID
    size += 8;
    
    // Column count
    size += 2;
    
    // For each covered column
    for (uint i = 0; i < table->s->fields; i++) {
        if (bitmap_is_set(covered_cols, i)) {
            Field* field = table->field[i];
            
            // Field index
            size += 2;
            
            // NULL flag
            size += 1;
            
            if (!field->is_null_in_record(record)) {
                if (field->real_type() == MYSQL_TYPE_VARCHAR ||
                field->real_type() == MYSQL_TYPE_STRING) {
                    // Length prefix
                    size += 2;
                    // Actual data
                    size += field->data_length();
                } else {
                    // Fixed length data
                    size += field->pack_length();
                }
            }
        }
    }
    
    return size;
}

std::vector<CoveredColumnInfo> get_covered_column_metadata(
    KEY* key_info,
    TABLE* table,
    MY_BITMAP* covered_cols)
{
    std::vector<CoveredColumnInfo> metadata;
    uint offset = 10;  // After row_id (8) and count (2)
    
    for (uint i = 0; i < table->s->fields; i++) {
        if (bitmap_is_set(covered_cols, i)) {
            CoveredColumnInfo info;
            info.field_index = i;
            info.offset_in_value = offset;
            
            Field* field = table->field[i];
            info.is_nullable = field->maybe_null();
            
            // Field index (2) + NULL flag (1)
            offset += 3;
            
            if (field->real_type() == MYSQL_TYPE_VARCHAR ||
                field->real_type() == MYSQL_TYPE_STRING) {
                info.length = 0;  // Variable length
                offset += 2;  // Length prefix
                // Actual data size is variable
            } else {
                info.length = field->pack_length();
                offset += info.length;
            }
            
            metadata.push_back(info);
        }
    }
    
    return metadata;
}

std::string encode_field_for_index(Field* field)
{
    std::string result;
    
    if (field->is_null()) {
        return result;  // Empty for NULL
    }
    
    // Get field data
    if (field->real_type() == MYSQL_TYPE_VARCHAR ||
        field->real_type() == MYSQL_TYPE_STRING) {
        uint32_t length = field->data_length();
        result.append(reinterpret_cast<const char*>(field->ptr), length);
    } else {
        result.append(reinterpret_cast<const char*>(field->ptr), 
                     field->pack_length());
    }
    
    return result;
}

size_t decode_field_from_index(Field* field, const uchar* data, size_t length)
{
    if (field->real_type() == MYSQL_TYPE_VARCHAR ||
        field->real_type() == MYSQL_TYPE_STRING) {
        field->store(reinterpret_cast<const char*>(data), length, field->charset());
        return length;
    } else {
        size_t field_len = field->pack_length();
        if (length < field_len) {
            return 0;  // Not enough data
        }
        std::memcpy(field->ptr, data, field_len);
        return field_len;
    }
}

void update_index_only_stats(
    IndexOnlyScanStats& stats,
    bool used_index_only,
    uint64_t rows_returned)
{
    stats.total_scans++;
    
    if (used_index_only) {
        stats.index_only_scans++;
        stats.rows_from_index += rows_returned;
        // Estimate bytes saved (assuming average row size of 100 bytes)
        stats.bytes_saved += rows_returned * 100;
    } else {
        stats.rows_fetched += rows_returned;
    }
}

bool all_fields_in_index_key(KEY* key_info, MY_BITMAP* read_set)
{
    for (uint i = 0; i < key_info->table->s->fields; i++) {
        if (bitmap_is_set(read_set, i)) {
            bool found = false;
            for (uint j = 0; j < key_info->user_defined_key_parts; j++) {
                if (key_info->key_part[j].field->field_index == i) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
    }
    return true;
}

MY_BITMAP* create_index_key_field_bitmap(KEY* key_info, TABLE* table)
{
    MY_BITMAP* bitmap = (MY_BITMAP*)malloc(sizeof(MY_BITMAP));
    my_bitmap_map* buf = (my_bitmap_map*)malloc(bitmap_buffer_size(table->s->fields));
    my_bitmap_init(bitmap, buf, table->s->fields);
    bitmap_clear_all(bitmap);
    
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        bitmap_set_bit(bitmap, key_info->key_part[i].field->field_index);
    }
    
    return bitmap;
}

} // namespace kvt_index_only