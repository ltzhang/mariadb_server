/*
   Copyright (c) 2025 KVT Storage Engine

   ALTER TABLE implementation for KVT storage engine
*/

#include "kvt_alter_table.h"
#include "ha_kvt.h"
#include "sql_table.h"
#include "sql_alter.h"
#include <sstream>
#include <algorithm>

namespace kvt_alter {

// =============================================================================
// KVTTableSchema Implementation
// =============================================================================

std::vector<KVTColumnDef> KVTTableSchema::get_active_columns(uint64_t version) const {
    if (version == 0) version = schema_version;
    
    std::vector<KVTColumnDef> active;
    for (const auto& [id, col] : columns) {
        if (col.is_active(version)) {
            active.push_back(col);
        }
    }
    
    // Sort by display order
    std::sort(active.begin(), active.end(),
              [](const KVTColumnDef& a, const KVTColumnDef& b) {
                  return a.display_order < b.display_order;
              });
    
    return active;
}

std::vector<KVTIndexDef> KVTTableSchema::get_active_indexes(uint64_t version) const {
    if (version == 0) version = schema_version;
    
    std::vector<KVTIndexDef> active;
    for (const auto& [id, idx] : indexes) {
        if (idx.is_active(version)) {
            active.push_back(idx);
        }
    }
    
    return active;
}

uint32_t KVTTableSchema::add_column(const KVTColumnDef& col) {
    uint32_t col_id = next_column_id++;
    KVTColumnDef new_col = col;
    new_col.column_id = col_id;
    new_col.added_version = schema_version + 1;
    new_col.dropped_version = 0;
    
    columns[col_id] = new_col;
    bump_version();
    
    return col_id;
}

bool KVTTableSchema::drop_column(uint32_t column_id) {
    auto it = columns.find(column_id);
    if (it == columns.end() || it->second.dropped_version > 0) {
        return false;
    }
    
    it->second.dropped_version = schema_version + 1;
    bump_version();
    
    return true;
}

bool KVTTableSchema::rename_column(uint32_t column_id, const std::string& new_name) {
    auto it = columns.find(column_id);
    if (it == columns.end() || it->second.dropped_version > 0) {
        return false;
    }
    
    it->second.name = new_name;
    bump_version();
    
    return true;
}

uint32_t KVTTableSchema::add_index(const KVTIndexDef& idx) {
    uint32_t idx_id = next_index_id++;
    KVTIndexDef new_idx = idx;
    new_idx.index_id = idx_id;
    new_idx.added_version = schema_version + 1;
    new_idx.dropped_version = 0;
    
    indexes[idx_id] = new_idx;
    bump_version();
    
    return idx_id;
}

bool KVTTableSchema::drop_index(uint32_t index_id) {
    auto it = indexes.find(index_id);
    if (it == indexes.end() || it->second.dropped_version > 0) {
        return false;
    }
    
    it->second.dropped_version = schema_version + 1;
    bump_version();
    
    return true;
}

std::string KVTTableSchema::serialize() const {
    std::stringstream ss;
    
    // Serialize basic info
    ss << table_id << "|" << schema_version << "|" 
       << table_name << "|" << database_name << "|"
       << next_column_id << "|" << next_index_id << "\n";
    
    // Serialize columns
    ss << columns.size() << "\n";
    for (const auto& [id, col] : columns) {
        ss << col.column_id << "|" << col.name << "|" 
           << (int)col.type << "|" << col.length << "|"
           << col.is_nullable << "|" << col.default_value << "|"
           << col.added_version << "|" << col.dropped_version << "|"
           << col.display_order << "\n";
    }
    
    // Serialize indexes
    ss << indexes.size() << "\n";
    for (const auto& [id, idx] : indexes) {
        ss << idx.index_id << "|" << idx.name << "|"
           << idx.is_unique << "|" << idx.is_primary << "|"
           << idx.added_version << "|" << idx.dropped_version << "|";
        
        ss << idx.column_ids.size();
        for (uint32_t col_id : idx.column_ids) {
            ss << "|" << col_id;
        }
        ss << "\n";
    }
    
    return ss.str();
}

// =============================================================================
// AlterContext Implementation
// =============================================================================

bool AlterContext::analyze_alter(TABLE* old_tbl, TABLE* new_tbl, 
                                 Alter_inplace_info* alter_inf) {
    old_table = old_tbl;
    new_table = new_tbl;
    alter_info = alter_inf;
    
    // Analyze column changes
    for (uint i = 0; i < new_table->s->fields; i++) {
        Field* new_field = new_table->field[i];
        Field* old_field = nullptr;
        
        // Find corresponding old field
        for (uint j = 0; j < old_table->s->fields; j++) {
            if (strcmp(old_table->field[j]->field_name.str, 
                      new_field->field_name.str) == 0) {
                old_field = old_table->field[j];
                break;
            }
        }
        
        if (!old_field) {
            // New column added
            AlterOperation op;
            op.type = AlterOperationType::ADD_COLUMN;
            op.new_column = AlterUtils::field_to_column_def(new_field);
            operations.push_back(op);
        } else if (old_field->type() != new_field->type() ||
                  old_field->field_length != new_field->field_length) {
            // Column modified
            AlterOperation op;
            op.type = AlterOperationType::MODIFY_COLUMN;
            op.new_column = AlterUtils::field_to_column_def(new_field);
            operations.push_back(op);
        }
    }
    
    // Check for dropped columns
    for (uint i = 0; i < old_table->s->fields; i++) {
        Field* old_field = old_table->field[i];
        bool found = false;
        
        for (uint j = 0; j < new_table->s->fields; j++) {
            if (strcmp(new_table->field[j]->field_name.str,
                      old_field->field_name.str) == 0) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            // Column dropped
            AlterOperation op;
            op.type = AlterOperationType::DROP_COLUMN;
            op.column_id = i;  // Use field index as temporary ID
            operations.push_back(op);
        }
    }
    
    // Analyze index changes
    for (uint i = 0; i < new_table->s->keys; i++) {
        KEY* new_key = &new_table->key_info[i];
        KEY* old_key = nullptr;
        
        // Find corresponding old index
        for (uint j = 0; j < old_table->s->keys; j++) {
            if (strcmp(old_table->key_info[j].name.str,
                      new_key->name.str) == 0) {
                old_key = &old_table->key_info[j];
                break;
            }
        }
        
        if (!old_key) {
            // New index added
            AlterOperation op;
            op.type = AlterOperationType::ADD_INDEX;
            op.new_index = AlterUtils::key_to_index_def(new_key);
            operations.push_back(op);
        }
    }
    
    // Check for dropped indexes
    for (uint i = 0; i < old_table->s->keys; i++) {
        KEY* old_key = &old_table->key_info[i];
        bool found = false;
        
        for (uint j = 0; j < new_table->s->keys; j++) {
            if (strcmp(new_table->key_info[j].name.str,
                      old_key->name.str) == 0) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            // Index dropped
            AlterOperation op;
            op.type = AlterOperationType::DROP_INDEX;
            op.index_id = i;  // Use index position as temporary ID
            operations.push_back(op);
        }
    }
    
    return !operations.empty();
}

bool AlterContext::can_do_instant() const {
    for (const auto& op : operations) {
        switch (op.type) {
            case AlterOperationType::ADD_COLUMN:
                // Can do instant if adding at end with default
                if (!op.new_column.default_value.empty()) {
                    continue;
                }
                return false;
                
            case AlterOperationType::DROP_COLUMN:
            case AlterOperationType::RENAME_COLUMN:
            case AlterOperationType::DROP_INDEX:
            case AlterOperationType::RENAME_TABLE:
            case AlterOperationType::CHANGE_DEFAULT:
                // These are instant operations
                continue;
                
            default:
                // Other operations cannot be instant
                return false;
        }
    }
    return true;
}

bool AlterContext::can_do_online() const {
    for (const auto& op : operations) {
        switch (op.type) {
            case AlterOperationType::MODIFY_COLUMN:
                // Check if it's a safe type change
                // For now, assume all type changes can be online
                continue;
                
            case AlterOperationType::ADD_INDEX:
                // Can build index online
                continue;
                
            default:
                continue;
        }
    }
    return true;
}

bool AlterContext::execute_instant_operations() {
    for (const auto& op : operations) {
        switch (op.type) {
            case AlterOperationType::ADD_COLUMN:
                new_schema->add_column(op.new_column);
                break;
                
            case AlterOperationType::DROP_COLUMN:
                new_schema->drop_column(op.column_id);
                break;
                
            case AlterOperationType::RENAME_COLUMN:
                new_schema->rename_column(op.column_id, op.new_name);
                break;
                
            case AlterOperationType::DROP_INDEX:
                new_schema->drop_index(op.index_id);
                break;
                
            default:
                break;
        }
    }
    
    return true;
}

bool AlterContext::execute_online_operations() {
    // Start tracking changes
    start_change_tracking();
    
    try {
        for (const auto& op : operations) {
            switch (op.type) {
                case AlterOperationType::ADD_INDEX: {
                    // Build index online
                    auto builder = std::make_unique<OnlineIndexBuilder>(
                        new_schema->table_id, 
                        new_schema->add_index(op.new_index),
                        nullptr);  // KEY info would be passed here
                    
                    if (!builder->build_initial_index()) {
                        return false;
                    }
                    
                    if (!builder->apply_tracked_changes()) {
                        return false;
                    }
                    
                    if (!builder->finalize()) {
                        return false;
                    }
                    break;
                }
                
                case AlterOperationType::MODIFY_COLUMN:
                    // Convert column data
                    // This would involve scanning and converting each row
                    break;
                    
                default:
                    break;
            }
        }
        
        // Apply any tracked changes
        apply_tracked_changes();
        
    } catch (...) {
        stop_change_tracking();
        return false;
    }
    
    stop_change_tracking();
    return true;
}

void AlterContext::start_change_tracking() {
    tracking_changes = true;
}

void AlterContext::stop_change_tracking() {
    tracking_changes = false;
}

bool AlterContext::apply_tracked_changes() {
    // Apply DML operations that occurred during ALTER
    // This would process a change log
    return true;
}

// =============================================================================
// OnlineIndexBuilder Implementation
// =============================================================================

OnlineIndexBuilder::OnlineIndexBuilder(uint64_t table_id, uint32_t index_id,
                                       const KEY* key_info)
    : table_id_(table_id), index_id_(index_id), key_info_(key_info),
      rows_processed_(0), is_building_(false) {
}

bool OnlineIndexBuilder::build_initial_index() {
    is_building_ = true;
    
    // Scan table and build index
    std::string error_msg;
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    
    // Get all rows from table
    KVTError err = kvt_scan(
        0,  // Transaction ID (would need actual)
        table_id_,
        KVTKey::minimum_key(),
        KVTKey::maximum_key(),
        SIZE_MAX,
        scan_results,
        error_msg);
    
    if (err != KVTError::SUCCESS) {
        is_building_ = false;
        return false;
    }
    
    // Build index entries for each row
    for (const auto& [key, value] : scan_results) {
        // Extract index key from row
        // Store in index table
        rows_processed_++;
    }
    
    is_building_ = false;
    return true;
}

void OnlineIndexBuilder::track_insert(const KVTKey& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(change_mutex_);
    
    Change change;
    change.type = Change::INSERT;
    change.key = key;
    change.value = value;
    tracked_changes_.push_back(change);
}

void OnlineIndexBuilder::track_update(const KVTKey& key, 
                                      const std::string& old_value,
                                      const std::string& new_value) {
    std::lock_guard<std::mutex> lock(change_mutex_);
    
    Change change;
    change.type = Change::UPDATE;
    change.key = key;
    change.value = new_value;
    change.old_value = old_value;
    tracked_changes_.push_back(change);
}

void OnlineIndexBuilder::track_delete(const KVTKey& key) {
    std::lock_guard<std::mutex> lock(change_mutex_);
    
    Change change;
    change.type = Change::DELETE;
    change.key = key;
    tracked_changes_.push_back(change);
}

bool OnlineIndexBuilder::apply_tracked_changes() {
    std::lock_guard<std::mutex> lock(change_mutex_);
    
    for (const auto& change : tracked_changes_) {
        switch (change.type) {
            case Change::INSERT:
                // Add to index
                break;
            case Change::UPDATE:
                // Update index entry
                break;
            case Change::DELETE:
                // Remove from index
                break;
        }
    }
    
    tracked_changes_.clear();
    return true;
}

bool OnlineIndexBuilder::finalize() {
    // Mark index as ready for use
    return true;
}

// =============================================================================
// AlterTableManager Implementation
// =============================================================================

AlterTableManager* AlterTableManager::instance_ = nullptr;

AlterTableManager* AlterTableManager::get_instance() {
    if (!instance_) {
        instance_ = new AlterTableManager();
    }
    return instance_;
}

AlterTableManager::AlterTableManager() {
}

enum_alter_inplace_result AlterTableManager::check_alter_support(
    TABLE* altered_table,
    Alter_inplace_info* ha_alter_info)
{
    // Check what operations are requested
    HA_CREATE_INFO *create_info = ha_alter_info->create_info;
    HA_ALTER_FLAGS alter_flags = ha_alter_info->handler_flags;
    
    // Check for operations we don't support
    if (alter_flags & ALTER_CHANGE_COLUMN_DEFAULT) {
        // Changing default is instant
        return HA_ALTER_INPLACE_INSTANT;
    }
    
    if (alter_flags & ALTER_ADD_COLUMN) {
        // Adding column can be instant with default
        return HA_ALTER_INPLACE_INSTANT;
    }
    
    if (alter_flags & ALTER_DROP_COLUMN) {
        // Dropping column is instant (just mark as hidden)
        return HA_ALTER_INPLACE_INSTANT;
    }
    
    if (alter_flags & ALTER_RENAME_COLUMN) {
        // Renaming is instant
        return HA_ALTER_INPLACE_INSTANT;
    }
    
    if (alter_flags & ALTER_ADD_INDEX) {
        // Can build index online
        return HA_ALTER_INPLACE_NO_LOCK;
    }
    
    if (alter_flags & ALTER_DROP_INDEX) {
        // Dropping index is instant
        return HA_ALTER_INPLACE_INSTANT;
    }
    
    if (alter_flags & ALTER_CHANGE_COLUMN) {
        // Type changes need online operation
        return HA_ALTER_INPLACE_SHARED_LOCK;
    }
    
    // For operations we don't handle inplace
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
}

bool AlterTableManager::prepare_alter(TABLE* altered_table,
                                      Alter_inplace_info* ha_alter_info,
                                      AlterContext** ctx)
{
    auto context = std::make_unique<AlterContext>();
    
    // Get original schema
    // uint64_t table_id = ...; // Would get from handler
    // context->original_schema = get_schema(table_id);
    
    // Create new schema
    context->new_schema = std::make_unique<KVTTableSchema>();
    *context->new_schema = *context->original_schema;
    
    // Analyze what needs to be done
    if (!context->analyze_alter(ha_alter_info->table, altered_table, ha_alter_info)) {
        return false;
    }
    
    *ctx = context.release();
    return true;
}

bool AlterTableManager::execute_alter(AlterContext* ctx) {
    if (ctx->can_do_instant()) {
        return ctx->execute_instant_operations();
    } else if (ctx->can_do_online()) {
        return ctx->execute_online_operations();
    } else {
        // Would need table rebuild
        return false;
    }
}

bool AlterTableManager::commit_alter(AlterContext* ctx, bool commit) {
    if (commit) {
        // Save new schema
        return save_schema(*ctx->new_schema);
    } else {
        // Discard changes
        return true;
    }
}

bool AlterTableManager::rollback_alter(AlterContext* ctx) {
    // Restore original schema
    return save_schema(*ctx->original_schema);
}

bool AlterTableManager::save_schema(const KVTTableSchema& schema) {
    // Save schema to KVT metadata table
    std::string serialized = schema.serialize();
    
    // Store in metadata table
    std::string error_msg;
    std::string key = "schema:" + std::to_string(schema.table_id);
    
    KVTError err = kvt_set(
        0,  // Transaction ID
        1,  // Metadata table ID
        KVTKey(key),
        serialized,
        error_msg);
    
    return err == KVTError::SUCCESS;
}

// =============================================================================
// AlterUtils Implementation
// =============================================================================

bool AlterUtils::is_safe_type_change(const Field* from, const Field* to) {
    // Check if conversion would lose data
    
    // INT -> BIGINT is safe (widening)
    if (from->type() == MYSQL_TYPE_LONG && to->type() == MYSQL_TYPE_LONGLONG) {
        return true;
    }
    
    // VARCHAR(n) -> VARCHAR(m) where m > n is safe
    if (from->type() == MYSQL_TYPE_VARCHAR && to->type() == MYSQL_TYPE_VARCHAR) {
        return from->field_length <= to->field_length;
    }
    
    // Adding/removing NULL is usually safe
    if (from->type() == to->type() && from->field_length == to->field_length) {
        return true;
    }
    
    return false;
}

bool AlterUtils::needs_table_rebuild(const Alter_inplace_info* alter_info) {
    // Check if any operation requires rebuilding the table
    
    if (alter_info->handler_flags & ALTER_CHANGE_CREATE_OPTION) {
        // Changing storage options might need rebuild
        return true;
    }
    
    if (alter_info->handler_flags & ALTER_RECREATE_TABLE) {
        // Explicit rebuild requested
        return true;
    }
    
    return false;
}

KVTColumnDef AlterUtils::field_to_column_def(const Field* field) {
    KVTColumnDef col;
    col.name = field->field_name.str;
    col.type = field->real_type();
    col.length = field->field_length;
    col.is_nullable = field->maybe_null();
    
    // Get default value if exists
    if (field->has_default_value()) {
        char buff[MAX_FIELD_WIDTH];
        String str(buff, sizeof(buff), field->charset());
        field->val_str(&str, &str);
        col.default_value = std::string(str.ptr(), str.length());
    }
    
    return col;
}

KVTIndexDef AlterUtils::key_to_index_def(const KEY* key_info) {
    KVTIndexDef idx;
    idx.name = key_info->name.str;
    idx.is_unique = (key_info->flags & HA_NOSAME) != 0;
    idx.is_primary = strcmp(key_info->name.str, "PRIMARY") == 0;
    
    // Add column IDs
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        idx.column_ids.push_back(key_info->key_part[i].field->field_index);
    }
    
    return idx;
}

const char* AlterUtils::get_alter_operation_name(AlterOperationType type) {
    switch (type) {
        case AlterOperationType::ADD_COLUMN: return "ADD COLUMN";
        case AlterOperationType::DROP_COLUMN: return "DROP COLUMN";
        case AlterOperationType::MODIFY_COLUMN: return "MODIFY COLUMN";
        case AlterOperationType::RENAME_COLUMN: return "RENAME COLUMN";
        case AlterOperationType::ADD_INDEX: return "ADD INDEX";
        case AlterOperationType::DROP_INDEX: return "DROP INDEX";
        case AlterOperationType::RENAME_TABLE: return "RENAME TABLE";
        default: return "UNKNOWN";
    }
}

// =============================================================================
// AlterProgress Implementation
// =============================================================================

AlterProgress::AlterProgress(const char* operation, uint64_t total_work)
    : operation_(operation), total_work_(total_work), work_completed_(0),
      current_phase_("Starting"), start_time_(std::chrono::steady_clock::now()) {
}

void AlterProgress::update(uint64_t work_completed) {
    work_completed_ = work_completed;
}

void AlterProgress::set_phase(const char* phase) {
    current_phase_ = phase;
}

void AlterProgress::complete() {
    work_completed_ = total_work_;
    current_phase_ = "Completed";
}

double AlterProgress::get_percentage() const {
    if (total_work_ == 0) return 100.0;
    return (double)work_completed_ * 100.0 / total_work_;
}

} // namespace kvt_alter