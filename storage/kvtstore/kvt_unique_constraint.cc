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

#include "kvt_unique_constraint.h"
#include "kvt_composite_index.h"
#include <sstream>
#include <cstring>

namespace kvt_unique {

// Static member initialization
KVTUniqueConstraintManager* KVTUniqueConstraintManager::instance = nullptr;

// Global statistics
static UniqueConstraintStats global_unique_stats;

KVTUniqueConstraintManager* KVTUniqueConstraintManager::get_instance() {
    if (!instance) {
        instance = new KVTUniqueConstraintManager();
    }
    return instance;
}

KVTUniqueConstraintManager::KVTUniqueConstraintManager() {
    mysql_mutex_init(0, &mutex, MY_MUTEX_INIT_FAST);
}

KVTUniqueConstraintManager::~KVTUniqueConstraintManager() {
    mysql_mutex_destroy(&mutex);
}

UniqueCheckResult KVTUniqueConstraintManager::check_unique_constraints(
    TABLE* table,
    const uchar* new_record,
    const uchar* old_record,
    uint64_t kvt_tx_id,
    uint64_t table_id)
{
    UniqueCheckResult result;
    result.is_unique = true;
    
    // Check each unique index
    for (uint i = 0; i < table->s->keys; i++) {
        KEY* key_info = &table->key_info[i];
        
        // Skip non-unique indexes
        if (!is_unique_index(key_info)) {
            continue;
        }
        
        // Skip full-text and spatial indexes
        if (key_info->algorithm == HA_KEY_ALG_FULLTEXT ||
            (key_info->flags & HA_SPATIAL)) {
            continue;
        }
        
        // Check this unique constraint
        result = check_single_constraint(
            key_info, i, new_record, old_record, kvt_tx_id, table_id);
        
        if (!result.is_unique) {
            // Found a violation
            result.violated_index = i;
            global_unique_stats.record_check(false, true, i);
            return result;
        }
    }
    
    global_unique_stats.record_check(false, false);
    return result;
}

UniqueCheckResult KVTUniqueConstraintManager::check_single_constraint(
    KEY* key_info,
    uint index_id,
    const uchar* new_record,
    const uchar* old_record,
    uint64_t kvt_tx_id,
    uint64_t table_id)
{
    UniqueCheckResult result;
    result.is_unique = true;
    result.violated_index = index_id;
    
    // Check if the unique key contains NULL
    if (contains_null_in_unique_key(key_info, new_record)) {
        // NULL values don't violate uniqueness per SQL standard
        global_unique_stats.record_check(true, false);
        return result;
    }
    
    // For updates, check if the unique key changed
    if (old_record && same_unique_key(key_info, old_record, new_record)) {
        // Key didn't change, no need to check
        global_unique_stats.record_same_row_update();
        return result;
    }
    
    // Build the unique key
    std::string unique_key = build_unique_key(table_id, index_id, key_info, new_record);
    
    // Check if key exists in KVT
    std::string existing_value;
    std::string error_msg;
    KVTError err = kvt_get(kvt_tx_id, table_id, unique_key, existing_value, error_msg);
    
    if (err == KVTError::SUCCESS) {
        // Key exists - check if it's the same row (for updates)
        if (old_record) {
            // For updates, we need to check if the existing key belongs to the same row
            // Extract row_id from existing value
            if (existing_value.length() >= 8) {
                uint64_t existing_row_id;
                std::memcpy(&existing_row_id, existing_value.data(), sizeof(existing_row_id));
                existing_row_id = be64toh(existing_row_id);
                
                // Build old unique key to get old row_id
                std::string old_unique_key = build_unique_key(table_id, index_id, key_info, old_record);
                std::string old_value;
                KVTError old_err = kvt_get(kvt_tx_id, table_id, old_unique_key, old_value, error_msg);
                
                if (old_err == KVTError::SUCCESS && old_value.length() >= 8) {
                    uint64_t old_row_id;
                    std::memcpy(&old_row_id, old_value.data(), sizeof(old_row_id));
                    old_row_id = be64toh(old_row_id);
                    
                    if (existing_row_id == old_row_id) {
                        // Same row, update is allowed
                        global_unique_stats.record_same_row_update();
                        return result;
                    }
                }
            }
        }
        
        // Duplicate found
        result.is_unique = false;
        result.duplicate_key = unique_key;
        result.error_message = format_duplicate_error(key_info, new_record);
        global_unique_stats.record_check(false, true, index_id);
        return result;
    } else if (err == KVTError::KEY_NOT_FOUND) {
        // No duplicate, constraint satisfied
        global_unique_stats.record_check(false, false);
        return result;
    } else {
        // KVT error - treat as constraint violation to be safe
        result.is_unique = false;
        result.error_message = "Unique constraint check failed: " + error_msg;
        return result;
    }
}

void KVTUniqueConstraintManager::register_unique_index(
    uint64_t table_id,
    uint index_id,
    KEY* key_info)
{
    mysql_mutex_lock(&mutex);
    
    UniqueIndexInfo info;
    info.table_id = table_id;
    info.index_id = index_id;
    info.is_primary_key = (key_info->flags & HA_NOSAME) && 
                          strcmp(key_info->name.str, "PRIMARY") == 0;
    info.allows_nulls = true;  // Most unique indexes allow NULL
    info.constraint_name = key_info->name.str;
    info.key_info = key_info;
    
    table_unique_indexes[table_id].push_back(info);
    
    mysql_mutex_unlock(&mutex);
}

void KVTUniqueConstraintManager::clear_table_indexes(uint64_t table_id) {
    mysql_mutex_lock(&mutex);
    table_unique_indexes.erase(table_id);
    mysql_mutex_unlock(&mutex);
}

std::string KVTUniqueConstraintManager::format_duplicate_error(
    KEY* key_info,
    const uchar* record)
{
    std::vector<std::string> values = extract_key_values_for_error(key_info, record);
    
    std::stringstream error;
    error << "Duplicate entry '";
    
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0) error << "-";
        error << values[i];
    }
    
    error << "' for key '" << key_info->name.str << "'";
    
    return error.str();
}

std::string build_unique_key(
    uint64_t table_id,
    uint index_id,
    KEY* key_info,
    const uchar* record)
{
    // Use the composite index key building, but without row_id for uniqueness check
    // We'll store the row_id as the value instead
    return kvt_composite::build_composite_key_from_record(
        table_id, index_id, key_info, record, 0);  // 0 row_id for unique check
}

bool contains_null_in_unique_key(
    KEY* key_info,
    const uchar* record)
{
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        Field* field = key_info->key_part[i].field;
        
        if (field->is_null_in_record(record)) {
            return true;
        }
    }
    
    return false;
}

std::vector<std::string> extract_key_values_for_error(
    KEY* key_info,
    const uchar* record)
{
    std::vector<std::string> values;
    
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        Field* field = key_info->key_part[i].field;
        
        if (field->is_null_in_record(record)) {
            values.push_back("NULL");
        } else {
            values.push_back(field_value_to_string(field, record));
        }
    }
    
    return values;
}

bool same_unique_key(
    KEY* key_info,
    const uchar* record1,
    const uchar* record2)
{
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        Field* field = key_info->key_part[i].field;
        
        // Check NULL status
        bool null1 = field->is_null_in_record(record1);
        bool null2 = field->is_null_in_record(record2);
        
        if (null1 != null2) {
            return false;  // One is NULL, other isn't
        }
        
        if (!null1) {  // Both non-NULL, compare values
            // Get field offsets
            uint offset1 = field->offset(record1);
            uint offset2 = field->offset(record2);
            
            // Compare field values
            if (field->key_cmp(record1 + offset1, record2 + offset2) != 0) {
                return false;
            }
        }
    }
    
    return true;  // All fields match
}

std::string field_value_to_string(Field* field, const uchar* record) {
    if (field->is_null_in_record(record)) {
        return "NULL";
    }
    
    char buff[MAX_FIELD_WIDTH];
    String str(buff, sizeof(buff), field->charset());
    
    // Save current field pointer
    const uchar* saved_ptr = field->ptr;
    
    // Point field to record data
    field->move_field_offset((my_ptrdiff_t)(record - field->table->record[0]));
    
    // Get string representation
    field->val_str(&str);
    
    // Restore field pointer
    field->move_field_offset(-(my_ptrdiff_t)(record - field->table->record[0]));
    
    return std::string(str.ptr(), str.length());
}

// BatchUniqueChecker implementation

BatchUniqueChecker::BatchUniqueChecker(uint64_t table_id, uint64_t kvt_tx_id)
    : table_id(table_id), kvt_tx_id(kvt_tx_id) {
}

BatchUniqueChecker::~BatchUniqueChecker() {
    clear();
}

bool BatchUniqueChecker::add_pending(uint index_id, const std::string& key) {
    // Check if key already exists in pending set for this index
    auto& index_keys = pending_keys[index_id];
    if (index_keys.count(key) > 0) {
        return false;  // Duplicate in batch
    }
    
    index_keys.insert(key);
    key_to_index[key] = index_id;
    return true;
}

UniqueCheckResult BatchUniqueChecker::check_all_pending() {
    UniqueCheckResult result;
    result.is_unique = true;
    
    // Check each pending key against storage
    for (const auto& index_pair : pending_keys) {
        uint index_id = index_pair.first;
        const auto& keys = index_pair.second;
        
        for (const auto& key : keys) {
            std::string existing_value;
            std::string error_msg;
            
            KVTError err = kvt_get(kvt_tx_id, table_id, key, existing_value, error_msg);
            
            if (err == KVTError::SUCCESS) {
                // Duplicate found
                result.is_unique = false;
                result.violated_index = index_id;
                result.duplicate_key = key;
                result.error_message = "Duplicate key in batch: " + key;
                return result;
            } else if (err != KVTError::KEY_NOT_FOUND) {
                // Error during check
                result.is_unique = false;
                result.error_message = "Batch unique check failed: " + error_msg;
                return result;
            }
        }
    }
    
    return result;
}

void BatchUniqueChecker::clear() {
    pending_keys.clear();
    key_to_index.clear();
}

// Statistics implementation

void UniqueConstraintStats::record_check(bool has_null, bool violated, uint index_id) {
    total_checks++;
    
    if (has_null) {
        null_keys++;
    }
    
    if (violated) {
        violations++;
        violations_per_index[index_id]++;
    }
}

void UniqueConstraintStats::record_same_row_update() {
    same_row_updates++;
}

void UniqueConstraintStats::reset() {
    total_checks = 0;
    violations = 0;
    null_keys = 0;
    same_row_updates = 0;
    violations_per_index.clear();
}

UniqueConstraintStats* get_unique_stats() {
    return &global_unique_stats;
}

UniqueConflictAction get_conflict_action(THD* thd) {
    // For now, always return ERROR action
    // Full implementation would require access to LEX structure
    // which requires including sql_lex.h and handling version differences
    return UniqueConflictAction::ERROR;
}

} // namespace kvt_unique