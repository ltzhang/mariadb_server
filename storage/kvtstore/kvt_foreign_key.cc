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

#include "kvt_foreign_key.h"
#include "kvt_catalog.h"
#include "kvt_row_codec.h"
#include "sql_class.h"
#include "table.h"
#include "field.h"
#include <sstream>
#include <algorithm>
#include <set>

using namespace kvt_constants;

namespace kvt_fk {

// Static member initialization
ForeignKeyManager* ForeignKeyManager::instance = nullptr;
std::mutex ForeignKeyManager::instance_mutex;

// ============================================================================
// ForeignKeyConstraint Implementation
// ============================================================================

bool ForeignKeyConstraint::involves_columns(const std::vector<uint>& column_indices) const {
  for (uint idx : column_indices) {
    if (std::find(child_column_indices.begin(), 
                  child_column_indices.end(), idx) != child_column_indices.end()) {
      return true;
    }
  }
  return false;
}

std::string ForeignKeyConstraint::generate_parent_key(const uchar* child_row, 
                                                      TABLE* child_table) const {
  std::string key;
  
  // Build key from child FK column values
  for (size_t i = 0; i < child_column_indices.size(); i++) {
    uint col_idx = child_column_indices[i];
    Field* field = child_table->field[col_idx];
    
    if (field->is_null()) {
      // NULL in FK means no constraint check needed
      return "";
    }
    
    // Append field value to key
    char buffer[256];
    String str(buffer, sizeof(buffer), field->charset());
    field->val_str(&str);
    
    if (i > 0) key += NULL_SEPARATOR;
    key.append(str.ptr(), str.length());
  }
  
  return key;
}

std::string ForeignKeyConstraint::generate_child_lookup_key(const uchar* parent_row,
                                                           TABLE* parent_table) const {
  std::string key;
  
  // Build key from parent column values that are referenced
  for (size_t i = 0; i < parent_column_indices.size(); i++) {
    uint col_idx = parent_column_indices[i];
    Field* field = parent_table->field[col_idx];
    
    // Append field value to key
    char buffer[256];
    String str(buffer, sizeof(buffer), field->charset());
    field->val_str(&str);
    
    if (i > 0) key += NULL_SEPARATOR;
    key.append(str.ptr(), str.length());
  }
  
  return key;
}

bool ForeignKeyConstraint::columns_changed(const uchar* old_row, const uchar* new_row,
                                          TABLE* table) const {
  for (uint col_idx : child_column_indices) {
    Field* field = table->field[col_idx];
    
    // Compare old and new values
    uint offset = field->offset(table->record[0]);
    if (memcmp(old_row + offset, new_row + offset, field->pack_length()) != 0) {
      return true;
    }
  }
  return false;
}

std::string ForeignKeyConstraint::serialize() const {
  std::stringstream ss;
  
  ss << "name=" << constraint_name << ";";
  ss << "parent_db=" << parent_database << ";";
  ss << "parent_table=" << parent_table << ";";
  
  ss << "parent_cols=";
  for (size_t i = 0; i < parent_columns.size(); i++) {
    if (i > 0) ss << ",";
    ss << parent_columns[i];
  }
  ss << ";";
  
  ss << "child_cols=";
  for (size_t i = 0; i < child_columns.size(); i++) {
    if (i > 0) ss << ",";
    ss << child_columns[i];
  }
  ss << ";";
  
  ss << "on_delete=" << static_cast<int>(on_delete_action) << ";";
  ss << "on_update=" << static_cast<int>(on_update_action);
  
  return ss.str();
}

ForeignKeyConstraint ForeignKeyConstraint::deserialize(const std::string& data) {
  ForeignKeyConstraint fk;
  std::istringstream ss(data);
  std::string token;
  
  while (std::getline(ss, token, ';')) {
    size_t eq_pos = token.find('=');
    if (eq_pos == std::string::npos) continue;
    
    std::string key = token.substr(0, eq_pos);
    std::string value = token.substr(eq_pos + 1);
    
    if (key == "name") {
      fk.constraint_name = value;
    } else if (key == "parent_db") {
      fk.parent_database = value;
    } else if (key == "parent_table") {
      fk.parent_table = value;
    } else if (key == "parent_cols") {
      std::istringstream cols(value);
      std::string col;
      while (std::getline(cols, col, ',')) {
        fk.parent_columns.push_back(col);
      }
    } else if (key == "child_cols") {
      std::istringstream cols(value);
      std::string col;
      while (std::getline(cols, col, ',')) {
        fk.child_columns.push_back(col);
      }
    } else if (key == "on_delete") {
      fk.on_delete_action = static_cast<FKAction>(std::stoi(value));
    } else if (key == "on_update") {
      fk.on_update_action = static_cast<FKAction>(std::stoi(value));
    }
  }
  
  return fk;
}

// ============================================================================
// ForeignKeyManager Implementation
// ============================================================================

ForeignKeyManager::ForeignKeyManager() {
  stats = FKStatistics();
}

ForeignKeyManager::~ForeignKeyManager() {
  clear_cache();
}

ForeignKeyManager* ForeignKeyManager::get_instance() {
  if (instance == nullptr) {
    std::lock_guard<std::mutex> lock(instance_mutex);
    if (instance == nullptr) {
      instance = new ForeignKeyManager();
    }
  }
  return instance;
}

std::string ForeignKeyManager::make_table_key(const std::string& database,
                                             const std::string& table) const {
  return database + "." + table;
}

int ForeignKeyManager::add_constraint(const std::string& database,
                                     const std::string& table,
                                     const ForeignKeyConstraint& constraint) {
  // Check for circular references
  if (would_create_cycle(database, table, 
                        constraint.parent_database, constraint.parent_table)) {
    return HA_ERR_FK_DEPTH_EXCEEDED;
  }
  
  // Save to catalog
  int ret = save_constraint_to_catalog(database, table, constraint);
  if (ret != 0) {
    return ret;
  }
  
  // Update cache
  std::lock_guard<std::mutex> lock(cache_mutex);
  std::string table_key = make_table_key(database, table);
  table_constraints_cache[table_key].push_back(constraint);
  
  // Update reverse index
  std::string parent_key = make_table_key(constraint.parent_database, 
                                         constraint.parent_table);
  parent_to_children[parent_key].push_back(std::make_pair(database, table));
  
  return 0;
}

int ForeignKeyManager::drop_constraint(const std::string& database,
                                      const std::string& table,
                                      const std::string& constraint_name) {
  // Remove from catalog
  int ret = remove_constraint_from_catalog(database, table, constraint_name);
  if (ret != 0) {
    return ret;
  }
  
  // Update cache
  std::lock_guard<std::mutex> lock(cache_mutex);
  std::string table_key = make_table_key(database, table);
  
  auto& constraints = table_constraints_cache[table_key];
  constraints.erase(
    std::remove_if(constraints.begin(), constraints.end(),
                   [&](const ForeignKeyConstraint& fk) {
                     return fk.constraint_name == constraint_name;
                   }),
    constraints.end()
  );
  
  return 0;
}

FKValidationResult ForeignKeyManager::validate_insert(THD* thd, TABLE* table,
                                                     const uchar* new_row) {
  FKValidationResult result;
  
  // Check if FK checks are enabled
  if (!FKUtils::are_fk_checks_enabled(thd)) {
    return result;
  }
  
  std::string database = table->s->db.str;
  std::string table_name = table->s->table_name.str;
  
  // Get constraints for this table
  std::vector<ForeignKeyConstraint> constraints;
  if (get_constraints(database, table_name, constraints) != 0) {
    return result;  // No constraints or error loading
  }
  
  stats.validations_performed++;
  
  // Check each FK constraint
  for (const auto& constraint : constraints) {
    // Check if parent row exists
    int check_result = check_parent_row_exists(thd, constraint, new_row, table);
    
    if (check_result != 0) {
      stats.violations_detected++;
      result.set_error(constraint.constraint_name,
                      FKUtils::build_fk_error_message(constraint.constraint_name,
                                                     constraint.parent_table,
                                                     "INSERT"));
      return result;
    }
  }
  
  return result;
}

FKValidationResult ForeignKeyManager::validate_update(THD* thd, TABLE* table,
                                                     const uchar* old_row,
                                                     const uchar* new_row) {
  FKValidationResult result;
  
  // Check if FK checks are enabled
  if (!FKUtils::are_fk_checks_enabled(thd)) {
    return result;
  }
  
  std::string database = table->s->db.str;
  std::string table_name = table->s->table_name.str;
  
  // Get constraints for this table
  std::vector<ForeignKeyConstraint> constraints;
  if (get_constraints(database, table_name, constraints) != 0) {
    return result;  // No constraints or error loading
  }
  
  stats.validations_performed++;
  
  // Check each FK constraint
  for (const auto& constraint : constraints) {
    // Only check if FK columns changed
    if (constraint.columns_changed(old_row, new_row, table)) {
      // Check if new parent row exists
      int check_result = check_parent_row_exists(thd, constraint, new_row, table);
      
      if (check_result != 0) {
        stats.violations_detected++;
        result.set_error(constraint.constraint_name,
                        FKUtils::build_fk_error_message(constraint.constraint_name,
                                                       constraint.parent_table,
                                                       "UPDATE"));
        return result;
      }
    }
  }
  
  // Also check if this row is referenced by children (for parent table updates)
  std::string table_key = make_table_key(database, table_name);
  auto it = parent_to_children.find(table_key);
  
  if (it != parent_to_children.end()) {
    // This table is referenced by other tables
    // Check cascade/restrict rules
    for (const auto& child_ref : it->second) {
      // Load child table constraints
      std::vector<ForeignKeyConstraint> child_constraints;
      if (get_constraints(child_ref.first, child_ref.second, child_constraints) == 0) {
        for (const auto& child_constraint : child_constraints) {
          if (child_constraint.parent_database == database &&
              child_constraint.parent_table == table_name) {
            
            // Check update action
            if (child_constraint.on_update_action == FK_ACTION_RESTRICT) {
              // Check if children exist
              if (has_child_references(thd, child_ref.first, child_ref.second, old_row)) {
                stats.violations_detected++;
                result.set_error(child_constraint.constraint_name,
                               "Cannot update parent row: child rows exist");
                return result;
              }
            }
            // CASCADE and SET NULL handled separately
          }
        }
      }
    }
  }
  
  return result;
}

FKValidationResult ForeignKeyManager::validate_delete(THD* thd, TABLE* table,
                                                     const uchar* old_row) {
  FKValidationResult result;
  
  // Check if FK checks are enabled
  if (!FKUtils::are_fk_checks_enabled(thd)) {
    return result;
  }
  
  std::string database = table->s->db.str;
  std::string table_name = table->s->table_name.str;
  
  stats.validations_performed++;
  
  // Check if this row is referenced by children
  std::string table_key = make_table_key(database, table_name);
  auto it = parent_to_children.find(table_key);
  
  if (it != parent_to_children.end()) {
    // This table is referenced by other tables
    for (const auto& child_ref : it->second) {
      // Load child table constraints
      std::vector<ForeignKeyConstraint> child_constraints;
      if (get_constraints(child_ref.first, child_ref.second, child_constraints) == 0) {
        for (const auto& child_constraint : child_constraints) {
          if (child_constraint.parent_database == database &&
              child_constraint.parent_table == table_name) {
            
            // Check delete action
            if (child_constraint.on_delete_action == FK_ACTION_RESTRICT ||
                child_constraint.on_delete_action == FK_ACTION_NO_ACTION) {
              // Check if children exist
              if (has_child_references(thd, child_ref.first, child_ref.second, old_row)) {
                stats.violations_detected++;
                result.set_error(child_constraint.constraint_name,
                               "Cannot delete parent row: child rows exist");
                return result;
              }
            }
            // CASCADE and SET NULL handled separately
          }
        }
      }
    }
  }
  
  return result;
}

int ForeignKeyManager::check_parent_row_exists(THD* thd,
                                              const ForeignKeyConstraint& constraint,
                                              const uchar* child_row,
                                              TABLE* child_table) {
  // Generate parent lookup key
  std::string parent_key = constraint.generate_parent_key(child_row, child_table);
  
  if (parent_key.empty()) {
    // NULL in FK columns - no constraint check needed
    return 0;
  }
  
  // Look up parent row in KVT
  // This is simplified - actual implementation would open parent table
  uint64_t parent_table_id = 0;
  std::string error_msg;
  
  // Get parent table's data table
  std::string data_table_name = std::string(DATA_TABLE_PREFIX) + constraint.parent_database;
  KVTError err = kvt_get_table_id(data_table_name.c_str(), parent_table_id, error_msg);
  
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  // Build full key for parent row
  std::string full_key = constraint.parent_table + NULL_SEPARATOR + parent_key;
  std::string value;
  
  // Check if parent exists
  err = kvt_get(0, parent_table_id, full_key, value, error_msg);
  
  if (err == KVTError::KEY_NOT_FOUND) {
    return HA_ERR_NO_REFERENCED_ROW;  // Parent doesn't exist
  }
  
  return (err == KVTError::SUCCESS) ? 0 : -1;
}

int ForeignKeyManager::get_constraints(const std::string& database,
                                      const std::string& table,
                                      std::vector<ForeignKeyConstraint>& constraints) {
  std::lock_guard<std::mutex> lock(cache_mutex);
  
  std::string table_key = make_table_key(database, table);
  
  // Check cache first
  auto it = table_constraints_cache.find(table_key);
  if (it != table_constraints_cache.end()) {
    constraints = it->second;
    stats.cache_hits++;
    return 0;
  }
  
  stats.cache_misses++;
  
  // Load from catalog
  int ret = load_all_constraints(database, table);
  if (ret != 0) {
    return ret;
  }
  
  // Try cache again after loading
  it = table_constraints_cache.find(table_key);
  if (it != table_constraints_cache.end()) {
    constraints = it->second;
    return 0;
  }
  
  return -1;  // No constraints found
}

int ForeignKeyManager::load_all_constraints(const std::string& database,
                                           const std::string& table) {
  // Load all FK constraints for this table from catalog
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  // Scan for FK constraints
  std::string prefix = std::string(CATALOG_TABLE_NAME) + NULL_SEPARATOR +
                      database + NULL_SEPARATOR + table + NULL_SEPARATOR + "FK" + NULL_SEPARATOR;
  std::string end_key = prefix;
  end_key[end_key.length() - 1]++;
  
  std::vector<std::pair<KVTKey, std::string>> results;
  err = kvt_scan(0, catalog_table_id, KVTKey(prefix), KVTKey(end_key), 100, results, error_msg);
  
  if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
    return -1;
  }
  
  // Parse and cache constraints
  std::string table_key = make_table_key(database, table);
  std::vector<ForeignKeyConstraint> constraints;
  
  for (const auto& kv : results) {
    ForeignKeyConstraint fk = ForeignKeyConstraint::deserialize(kv.second);
    constraints.push_back(fk);
    
    // Update reverse index
    std::string parent_key = make_table_key(fk.parent_database, fk.parent_table);
    parent_to_children[parent_key].push_back(std::make_pair(database, table));
  }
  
  table_constraints_cache[table_key] = constraints;
  return 0;
}

int ForeignKeyManager::save_constraint_to_catalog(const std::string& database,
                                                 const std::string& table,
                                                 const ForeignKeyConstraint& constraint) {
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  // Build key for constraint
  std::string key = std::string(CATALOG_TABLE_NAME) + NULL_SEPARATOR +
                   database + NULL_SEPARATOR + table + NULL_SEPARATOR + 
                   "FK" + NULL_SEPARATOR + constraint.constraint_name;
  
  // Serialize constraint
  std::string value = constraint.serialize();
  
  // Store in catalog
  err = kvt_set(0, catalog_table_id, key, value, error_msg);
  
  return (err == KVTError::SUCCESS) ? 0 : -1;
}

int ForeignKeyManager::remove_constraint_from_catalog(const std::string& database,
                                                     const std::string& table,
                                                     const std::string& constraint_name) {
  uint64_t catalog_table_id = 0;
  std::string error_msg;
  
  KVTError err = kvt_get_table_id(CATALOG_TABLE_NAME, catalog_table_id, error_msg);
  if (err != KVTError::SUCCESS) {
    return -1;
  }
  
  // Build key for constraint
  std::string key = std::string(CATALOG_TABLE_NAME) + NULL_SEPARATOR +
                   database + NULL_SEPARATOR + table + NULL_SEPARATOR + 
                   "FK" + NULL_SEPARATOR + constraint_name;
  
  // Delete from catalog
  err = kvt_del(0, catalog_table_id, key, error_msg);
  
  return (err == KVTError::SUCCESS || err == KVTError::KEY_NOT_FOUND) ? 0 : -1;
}

bool ForeignKeyManager::has_child_references(THD* thd, const std::string& database,
                                            const std::string& table,
                                            const uchar* row) {
  // Simplified check - actual implementation would scan child table
  // For now, return false to allow operations
  return false;
}

bool ForeignKeyManager::would_create_cycle(const std::string& child_db,
                                          const std::string& child_table,
                                          const std::string& parent_db,
                                          const std::string& parent_table) {
  std::set<std::string> visited;
  std::string from = make_table_key(parent_db, parent_table);
  std::string to = make_table_key(child_db, child_table);
  
  return has_path(from, to, visited);
}

bool ForeignKeyManager::has_path(const std::string& from_table,
                                const std::string& to_table,
                                std::set<std::string>& visited) {
  if (from_table == to_table) {
    return true;  // Cycle detected
  }
  
  if (visited.find(from_table) != visited.end()) {
    return false;  // Already visited
  }
  
  visited.insert(from_table);
  
  // Check all children of from_table
  auto it = parent_to_children.find(from_table);
  if (it != parent_to_children.end()) {
    for (const auto& child : it->second) {
      std::string child_key = make_table_key(child.first, child.second);
      if (has_path(child_key, to_table, visited)) {
        return true;
      }
    }
  }
  
  return false;
}

void ForeignKeyManager::invalidate_cache(const std::string& database,
                                        const std::string& table) {
  std::lock_guard<std::mutex> lock(cache_mutex);
  std::string table_key = make_table_key(database, table);
  table_constraints_cache.erase(table_key);
}

void ForeignKeyManager::clear_cache() {
  std::lock_guard<std::mutex> lock(cache_mutex);
  table_constraints_cache.clear();
  parent_to_children.clear();
}

bool ForeignKeyManager::has_foreign_keys(const std::string& database,
                                        const std::string& table) {
  std::vector<ForeignKeyConstraint> constraints;
  return (get_constraints(database, table, constraints) == 0 && !constraints.empty());
}

// ============================================================================
// FKUtils Implementation
// ============================================================================

FKAction FKUtils::parse_action(const std::string& action_str) {
  if (action_str == "CASCADE") return FK_ACTION_CASCADE;
  if (action_str == "SET NULL") return FK_ACTION_SET_NULL;
  if (action_str == "NO ACTION") return FK_ACTION_NO_ACTION;
  if (action_str == "SET DEFAULT") return FK_ACTION_SET_DEFAULT;
  return FK_ACTION_RESTRICT;  // Default
}

std::string FKUtils::action_to_string(FKAction action) {
  switch (action) {
    case FK_ACTION_CASCADE: return "CASCADE";
    case FK_ACTION_SET_NULL: return "SET NULL";
    case FK_ACTION_NO_ACTION: return "NO ACTION";
    case FK_ACTION_SET_DEFAULT: return "SET DEFAULT";
    default: return "RESTRICT";
  }
}

std::string FKUtils::build_fk_error_message(const std::string& constraint_name,
                                           const std::string& parent_table,
                                           const std::string& operation) {
  std::stringstream ss;
  ss << "Foreign key constraint '" << constraint_name 
     << "' failed: Cannot " << operation 
     << " - parent row not found in '" << parent_table << "'";
  return ss.str();
}

bool FKUtils::are_fk_checks_enabled(THD* thd) {
  // Check system variable foreign_key_checks
  // Simplified - actual implementation would check THD variables
  return true;
}

} // namespace kvt_fk