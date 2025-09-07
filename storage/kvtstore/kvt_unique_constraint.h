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

#ifndef KVT_UNIQUE_CONSTRAINT_H
#define KVT_UNIQUE_CONSTRAINT_H

#include "my_global.h"
#include "sql_class.h"
#include "key.h"
#include "field.h"
#include "kvt/kvt_inc.h"

// Define HA_SPATIAL if not defined
#ifndef HA_SPATIAL
  #define HA_SPATIAL 0x00001000
#endif
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace kvt_unique {

// Forward declarations
class UniqueConstraintChecker;

// Result of unique constraint check
struct UniqueCheckResult {
    bool is_unique;              // True if no duplicate found
    uint violated_index;         // Index that was violated (if any)
    std::string duplicate_key;   // The duplicate key value
    std::string error_message;   // Formatted error message
};

// Information about a unique index
struct UniqueIndexInfo {
    uint64_t table_id;
    uint index_id;
    bool is_primary_key;
    bool allows_nulls;
    std::string constraint_name;
    KEY* key_info;
};

/**
 * Manager for unique constraint validation
 */
class KVTUniqueConstraintManager {
public:
    static KVTUniqueConstraintManager* get_instance();
    static void cleanup_instance();
    
    /**
     * Check all unique constraints for a record
     * 
     * @param table        Table being modified
     * @param new_record   New record data
     * @param old_record   Old record data (for updates, nullptr for inserts)
     * @param kvt_tx_id    KVT transaction ID
     * @param table_id     KVT table ID
     * @return Check result with violation details if any
     */
    UniqueCheckResult check_unique_constraints(
        TABLE* table,
        const uchar* new_record,
        const uchar* old_record,
        uint64_t kvt_tx_id,
        uint64_t table_id);
    
    /**
     * Check a single unique constraint
     * 
     * @param key_info     Index metadata
     * @param index_id     Index identifier
     * @param new_record   New record data
     * @param old_record   Old record data (nullable)
     * @param kvt_tx_id    KVT transaction ID
     * @param table_id     KVT table ID
     * @return Check result
     */
    UniqueCheckResult check_single_constraint(
        KEY* key_info,
        uint index_id,
        const uchar* new_record,
        const uchar* old_record,
        uint64_t kvt_tx_id,
        uint64_t table_id);
    
    /**
     * Register a unique index
     * 
     * @param table_id     Table identifier
     * @param index_id     Index identifier
     * @param key_info     Index metadata
     */
    void register_unique_index(
        uint64_t table_id,
        uint index_id,
        KEY* key_info);
    
    /**
     * Clear unique indexes for a table
     * 
     * @param table_id     Table identifier
     */
    void clear_table_indexes(uint64_t table_id);
    
    /**
     * Format duplicate key error message
     * 
     * @param key_info     Index that was violated
     * @param record       Record with duplicate values
     * @return Formatted error message
     */
    std::string format_duplicate_error(
        KEY* key_info,
        const uchar* record);

private:
    KVTUniqueConstraintManager();
    ~KVTUniqueConstraintManager();
    
    // Singleton instance
    static KVTUniqueConstraintManager* instance;
    
    // Registered unique indexes per table
    std::unordered_map<uint64_t, std::vector<UniqueIndexInfo>> table_unique_indexes;
    
    // Mutex for thread safety
    mysql_mutex_t mutex;
};

/**
 * Build unique constraint key
 * 
 * @param table_id     Table identifier
 * @param index_id     Index identifier
 * @param key_info     Index metadata
 * @param record       Record data
 * @return Encoded unique key
 */
std::string build_unique_key(
    uint64_t table_id,
    uint index_id,
    KEY* key_info,
    const uchar* record);

/**
 * Check if unique key contains any NULL values
 * 
 * @param key_info     Index metadata
 * @param record       Record data
 * @return true if any indexed column is NULL
 */
bool contains_null_in_unique_key(
    KEY* key_info,
    const uchar* record);

/**
 * Extract key values for error message
 * 
 * @param key_info     Index metadata
 * @param record       Record data
 * @return Vector of string representations of key values
 */
std::vector<std::string> extract_key_values_for_error(
    KEY* key_info,
    const uchar* record);

/**
 * Check if two records have the same unique key
 * 
 * @param key_info     Index metadata
 * @param record1      First record
 * @param record2      Second record
 * @return true if unique keys are identical
 */
bool same_unique_key(
    KEY* key_info,
    const uchar* record1,
    const uchar* record2);

/**
 * Batch unique constraint checker for bulk operations
 */
class BatchUniqueChecker {
public:
    BatchUniqueChecker(uint64_t table_id, uint64_t kvt_tx_id);
    ~BatchUniqueChecker();
    
    /**
     * Add a key to pending checks
     * 
     * @param index_id     Index identifier
     * @param key          Unique key to check
     * @return false if duplicate found in pending set
     */
    bool add_pending(uint index_id, const std::string& key);
    
    /**
     * Check all pending keys against storage
     * 
     * @return Result with first violation found (if any)
     */
    UniqueCheckResult check_all_pending();
    
    /**
     * Clear all pending checks
     */
    void clear();
    
    /**
     * Get number of pending checks
     */
    size_t pending_count() const { return pending_keys.size(); }

private:
    uint64_t table_id;
    uint64_t kvt_tx_id;
    
    // Pending unique keys to check (index_id -> set of keys)
    std::unordered_map<uint, std::unordered_set<std::string>> pending_keys;
    
    // Track which key belongs to which index for error reporting
    std::unordered_map<std::string, uint> key_to_index;
};

// Statistics for unique constraint checks
struct UniqueConstraintStats {
    uint64_t total_checks = 0;
    uint64_t violations = 0;
    uint64_t null_keys = 0;
    uint64_t same_row_updates = 0;
    std::unordered_map<uint, uint64_t> violations_per_index;
    
    void record_check(bool has_null, bool violated, uint index_id = 0);
    void record_same_row_update();
    void reset();
};

/**
 * Get global unique constraint statistics
 */
UniqueConstraintStats* get_unique_stats();

// Helper functions

/**
 * Check if index should enforce uniqueness
 * 
 * @param key_info     Index metadata
 * @return true if this is a unique index
 */
inline bool is_unique_index(KEY* key_info) {
    return (key_info->flags & HA_NOSAME) != 0;
}

/**
 * Check if this is a primary key index
 * 
 * @param key_info     Index metadata
 * @param table        Table definition
 * @return true if this is the primary key
 */
inline bool is_primary_key(KEY* key_info, TABLE* table) {
    return key_info == &table->key_info[table->s->primary_key];
}

/**
 * Create a string representation of field value for error messages
 * 
 * @param field        Field to extract value from
 * @param record       Record containing field data
 * @return String representation of field value
 */
std::string field_value_to_string(Field* field, const uchar* record);

/**
 * Options for handling unique constraint violations
 */
enum class UniqueConflictAction {
    ERROR,           // Return error (default)
    IGNORE,          // Skip the operation
    REPLACE,         // Delete existing row and insert new
    UPDATE_EXISTING  // Update the existing row
};

/**
 * Get conflict action based on SQL command
 * 
 * @param thd          Thread context
 * @return Configured conflict action
 */
UniqueConflictAction get_conflict_action(THD* thd);

} // namespace kvt_unique

#endif // KVT_UNIQUE_CONSTRAINT_H