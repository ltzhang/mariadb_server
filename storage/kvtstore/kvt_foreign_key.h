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

#ifndef KVT_FOREIGN_KEY_H
#define KVT_FOREIGN_KEY_H

#include "my_global.h"
#include "kvt/kvt_inc.h"
#include "kvt_constants.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <memory>

// Forward declarations
class THD;
class TABLE;
class Field;

namespace kvt_fk {

// Foreign key action types
enum FKAction {
  FK_ACTION_RESTRICT,
  FK_ACTION_CASCADE,
  FK_ACTION_SET_NULL,
  FK_ACTION_NO_ACTION,
  FK_ACTION_SET_DEFAULT
};

// Foreign key constraint definition
struct ForeignKeyConstraint {
  std::string constraint_name;
  std::string parent_database;
  std::string parent_table;
  std::vector<std::string> parent_columns;
  std::vector<std::string> child_columns;
  std::vector<uint> parent_column_indices;  // Cached column positions
  std::vector<uint> child_column_indices;   // Cached column positions
  
  FKAction on_delete_action;
  FKAction on_update_action;
  
  // Constructor
  ForeignKeyConstraint() : 
    on_delete_action(FK_ACTION_RESTRICT),
    on_update_action(FK_ACTION_RESTRICT) {}
  
  // Check if this constraint involves the given columns
  bool involves_columns(const std::vector<uint>& column_indices) const;
  
  // Generate parent table lookup key from child row
  std::string generate_parent_key(const uchar* child_row, TABLE* child_table) const;
  
  // Generate child table lookup key from parent row
  std::string generate_child_lookup_key(const uchar* parent_row, TABLE* parent_table) const;
  
  // Check if the FK columns have changed
  bool columns_changed(const uchar* old_row, const uchar* new_row, TABLE* table) const;
  
  // Serialize/deserialize for storage
  std::string serialize() const;
  static ForeignKeyConstraint deserialize(const std::string& data);
};

// Foreign key validation result
struct FKValidationResult {
  bool valid;
  std::string error_message;
  std::string failed_constraint;
  
  FKValidationResult() : valid(true) {}
  
  void set_error(const std::string& constraint, const std::string& message) {
    valid = false;
    failed_constraint = constraint;
    error_message = message;
  }
};

// Foreign Key Manager - Singleton class
class ForeignKeyManager {
private:
  // Singleton instance
  static ForeignKeyManager* instance;
  static std::mutex instance_mutex;
  
  // Cache of FK constraints per table (database.table -> constraints)
  std::map<std::string, std::vector<ForeignKeyConstraint>> table_constraints_cache;
  
  // Reverse index: parent table -> child tables with FKs
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> parent_to_children;
  
  // Mutex for thread safety
  mutable std::mutex cache_mutex;
  
  // Private constructor for singleton
  ForeignKeyManager();
  
  // Helper methods
  std::string make_table_key(const std::string& database, const std::string& table) const;
  int load_constraint_from_catalog(const std::string& database, 
                                  const std::string& table,
                                  const std::string& constraint_name);
  int check_parent_row_exists(THD* thd, const ForeignKeyConstraint& constraint,
                             const uchar* child_row, TABLE* child_table);
  int find_child_rows(THD* thd, const ForeignKeyConstraint& constraint,
                     const uchar* parent_row, TABLE* parent_table,
                     std::vector<std::string>& child_keys);
  
public:
  ~ForeignKeyManager();
  
  // Singleton access
  static ForeignKeyManager* get_instance();
  static void cleanup_instance();
  
  // Constraint management
  int add_constraint(const std::string& database,
                    const std::string& table,
                    const ForeignKeyConstraint& constraint);
  
  int drop_constraint(const std::string& database,
                     const std::string& table,
                     const std::string& constraint_name);
  
  int get_constraints(const std::string& database,
                     const std::string& table,
                     std::vector<ForeignKeyConstraint>& constraints);
  
  // Validation methods for DML operations
  FKValidationResult validate_insert(THD* thd, TABLE* table, 
                                    const uchar* new_row);
  
  FKValidationResult validate_update(THD* thd, TABLE* table,
                                    const uchar* old_row, 
                                    const uchar* new_row);
  
  FKValidationResult validate_delete(THD* thd, TABLE* table,
                                    const uchar* old_row);
  
  // Cascade operations
  int execute_cascade_delete(THD* thd, TABLE* parent_table,
                            const uchar* parent_row,
                            const std::string& database,
                            const std::string& table);
  
  int execute_cascade_update(THD* thd, TABLE* parent_table,
                            const uchar* old_row, 
                            const uchar* new_row,
                            const std::string& database,
                            const std::string& table);
  
  int execute_set_null(THD* thd, const ForeignKeyConstraint& constraint,
                      const uchar* parent_row, TABLE* parent_table);
  
  int execute_set_default(THD* thd, const ForeignKeyConstraint& constraint,
                         const uchar* parent_row, TABLE* parent_table);
  
  // Metadata operations
  int load_all_constraints(const std::string& database, const std::string& table);
  int save_constraint_to_catalog(const std::string& database,
                                const std::string& table,
                                const ForeignKeyConstraint& constraint);
  int remove_constraint_from_catalog(const std::string& database,
                                    const std::string& table,
                                    const std::string& constraint_name);
  
  // Cache management
  void invalidate_cache(const std::string& database, const std::string& table);
  void clear_cache();
  
  // Utility methods
  bool has_foreign_keys(const std::string& database, const std::string& table);
  bool has_child_references(THD* thd, const std::string& database,
                          const std::string& table, const uchar* row);
  
  // Check for circular references
  bool would_create_cycle(const std::string& child_db, const std::string& child_table,
                         const std::string& parent_db, const std::string& parent_table);
  
  // Statistics and monitoring
  struct FKStatistics {
    uint64_t validations_performed;
    uint64_t violations_detected;
    uint64_t cascades_executed;
    uint64_t cache_hits;
    uint64_t cache_misses;
    
    FKStatistics() : validations_performed(0), violations_detected(0),
                     cascades_executed(0), cache_hits(0), cache_misses(0) {}
  };
  
  const FKStatistics& get_statistics() const { return stats; }
  void reset_statistics() { stats = FKStatistics(); }
  
private:
  FKStatistics stats;
  
  // Helper for cycle detection
  bool has_path(const std::string& from_table, const std::string& to_table,
               std::set<std::string>& visited);
};

// Utility functions for FK operations
namespace FKUtils {
  // Parse FK action string
  FKAction parse_action(const std::string& action_str);
  std::string action_to_string(FKAction action);
  
  // Build FK error message
  std::string build_fk_error_message(const std::string& constraint_name,
                                    const std::string& parent_table,
                                    const std::string& operation);
  
  // Check if FK checks are enabled
  bool are_fk_checks_enabled(THD* thd);
  
  // Extract column values for FK
  std::vector<std::string> extract_fk_values(const uchar* row, TABLE* table,
                                            const std::vector<uint>& column_indices);
  
  // Compare FK column values
  bool fk_values_equal(const uchar* row1, const uchar* row2, TABLE* table,
                      const std::vector<uint>& column_indices);
}

} // namespace kvt_fk

#endif // KVT_FOREIGN_KEY_H