/*
   Copyright (c) 2025 KVT Storage Engine

   ALTER TABLE implementation for KVT storage engine
*/

#ifndef KVT_ALTER_TABLE_H
#define KVT_ALTER_TABLE_H

#include "my_global.h"
#include "handler.h"
#include "sql_class.h"
#include "sql_table.h"
#include "kvt/kvt_inc.h"
#include <vector>
#include <map>
#include <memory>
#include <atomic>

namespace kvt_alter {

/**
 * Column definition with versioning for instant ALTER
 */
struct KVTColumnDef {
    uint32_t column_id;           // Permanent column ID
    std::string name;              // Column name
    enum_field_types type;        // MySQL field type
    uint32_t length;               // Field length
    bool is_nullable;              // NULL allowed
    std::string default_value;     // Default value (serialized)
    uint64_t added_version;        // Schema version when added
    uint64_t dropped_version;      // 0 if active, >0 if dropped
    uint32_t display_order;        // Column position in table
    
    bool is_active(uint64_t version) const {
        return added_version <= version && 
               (dropped_version == 0 || dropped_version > version);
    }
};

/**
 * Index definition with versioning
 */
struct KVTIndexDef {
    uint32_t index_id;             // Permanent index ID
    std::string name;              // Index name
    bool is_unique;                // Unique constraint
    bool is_primary;               // Primary key
    std::vector<uint32_t> column_ids;  // Columns in index
    uint64_t added_version;        // Schema version when added
    uint64_t dropped_version;      // 0 if active, >0 if dropped
    
    bool is_active(uint64_t version) const {
        return added_version <= version && 
               (dropped_version == 0 || dropped_version > version);
    }
};

/**
 * Table schema with version tracking
 */
class KVTTableSchema {
public:
    uint64_t table_id;
    uint64_t schema_version;
    std::string table_name;
    std::string database_name;
    
    // All columns (including dropped)
    std::map<uint32_t, KVTColumnDef> columns;
    
    // All indexes (including dropped)
    std::map<uint32_t, KVTIndexDef> indexes;
    
    // Next IDs for new columns/indexes
    std::atomic<uint32_t> next_column_id;
    std::atomic<uint32_t> next_index_id;
    
    KVTTableSchema() : schema_version(1), next_column_id(1), next_index_id(1) {}
    
    /**
     * Get active columns for a schema version
     */
    std::vector<KVTColumnDef> get_active_columns(uint64_t version = 0) const;
    
    /**
     * Get active indexes for a schema version
     */
    std::vector<KVTIndexDef> get_active_indexes(uint64_t version = 0) const;
    
    /**
     * Add a new column (instant operation)
     */
    uint32_t add_column(const KVTColumnDef& col);
    
    /**
     * Drop a column (instant operation)
     */
    bool drop_column(uint32_t column_id);
    
    /**
     * Rename a column (instant operation)
     */
    bool rename_column(uint32_t column_id, const std::string& new_name);
    
    /**
     * Add an index
     */
    uint32_t add_index(const KVTIndexDef& idx);
    
    /**
     * Drop an index
     */
    bool drop_index(uint32_t index_id);
    
    /**
     * Increment schema version
     */
    void bump_version() { schema_version++; }
    
    /**
     * Serialize schema to storage
     */
    std::string serialize() const;
    
    /**
     * Deserialize schema from storage
     */
    static std::unique_ptr<KVTTableSchema> deserialize(const std::string& data);
};

/**
 * ALTER operation types
 */
enum class AlterOperationType {
    ADD_COLUMN,
    DROP_COLUMN,
    MODIFY_COLUMN,
    RENAME_COLUMN,
    ADD_INDEX,
    DROP_INDEX,
    ADD_FOREIGN_KEY,
    DROP_FOREIGN_KEY,
    RENAME_TABLE,
    CHANGE_DEFAULT,
    ADD_CONSTRAINT,
    DROP_CONSTRAINT
};

/**
 * Single ALTER operation
 */
struct AlterOperation {
    AlterOperationType type;
    uint32_t column_id;           // For column operations
    uint32_t index_id;            // For index operations
    KVTColumnDef new_column;      // For ADD/MODIFY COLUMN
    KVTIndexDef new_index;        // For ADD INDEX
    std::string new_name;         // For RENAME operations
    std::string new_default;      // For CHANGE DEFAULT
};

/**
 * ALTER execution context
 */
class AlterContext {
public:
    // Original and new table structures
    TABLE* old_table;
    TABLE* new_table;
    Alter_inplace_info* alter_info;
    
    // Schema versions
    std::unique_ptr<KVTTableSchema> original_schema;
    std::unique_ptr<KVTTableSchema> new_schema;
    
    // Operations to perform
    std::vector<AlterOperation> operations;
    
    // Change tracking for online ALTER
    bool is_online;
    std::atomic<bool> tracking_changes;
    
    /**
     * Analyze ALTER and determine operations
     */
    bool analyze_alter(TABLE* old_table, TABLE* new_table, 
                       Alter_inplace_info* alter_info);
    
    /**
     * Check if ALTER can be done instantly
     */
    bool can_do_instant() const;
    
    /**
     * Check if ALTER can be done online
     */
    bool can_do_online() const;
    
    /**
     * Execute instant ALTER operations
     */
    bool execute_instant_operations();
    
    /**
     * Execute online ALTER operations
     */
    bool execute_online_operations();
    
    /**
     * Start tracking DML changes
     */
    void start_change_tracking();
    
    /**
     * Stop tracking DML changes
     */
    void stop_change_tracking();
    
    /**
     * Apply tracked changes to new structure
     */
    bool apply_tracked_changes();
};

/**
 * Online index builder
 */
class OnlineIndexBuilder {
public:
    OnlineIndexBuilder(uint64_t table_id, uint32_t index_id, 
                       const KEY* key_info);
    
    /**
     * Build index from existing data
     */
    bool build_initial_index();
    
    /**
     * Track concurrent DML operations
     */
    void track_insert(const KVTKey& key, const std::string& value);
    void track_update(const KVTKey& key, const std::string& old_value, 
                     const std::string& new_value);
    void track_delete(const KVTKey& key);
    
    /**
     * Apply tracked changes to index
     */
    bool apply_tracked_changes();
    
    /**
     * Finalize index creation
     */
    bool finalize();
    
private:
    uint64_t table_id_;
    uint32_t index_id_;
    const KEY* key_info_;
    
    // Tracked changes during build
    struct Change {
        enum Type { INSERT, UPDATE, DELETE } type;
        KVTKey key;
        std::string value;
        std::string old_value;  // For UPDATE
    };
    std::vector<Change> tracked_changes_;
    std::mutex change_mutex_;
    
    // Progress tracking
    std::atomic<uint64_t> rows_processed_;
    std::atomic<bool> is_building_;
};

/**
 * ALTER TABLE manager
 */
class AlterTableManager {
public:
    static AlterTableManager* get_instance();
    static void cleanup_instance();
    
    /**
     * Check if ALTER can be done inplace
     */
    enum_alter_inplace_result check_alter_support(
        TABLE* altered_table,
        Alter_inplace_info* ha_alter_info);
    
    /**
     * Prepare for ALTER operation
     */
    bool prepare_alter(TABLE* altered_table,
                      Alter_inplace_info* ha_alter_info,
                      AlterContext** ctx);
    
    /**
     * Execute ALTER operation
     */
    bool execute_alter(AlterContext* ctx);
    
    /**
     * Commit ALTER operation
     */
    bool commit_alter(AlterContext* ctx, bool commit);
    
    /**
     * Rollback ALTER operation
     */
    bool rollback_alter(AlterContext* ctx);
    
    /**
     * Get schema for table
     */
    std::unique_ptr<KVTTableSchema> get_schema(uint64_t table_id);
    
    /**
     * Save schema for table
     */
    bool save_schema(const KVTTableSchema& schema);
    
    /**
     * Track DML during online ALTER
     */
    void track_dml_operation(uint64_t table_id, 
                            const AlterOperation& op);
    
private:
    AlterTableManager();
    static AlterTableManager* instance_;
    
    // Active ALTER operations
    std::map<uint64_t, std::unique_ptr<AlterContext>> active_alters_;
    std::mutex alter_mutex_;
    
    // Online index builders
    std::map<std::pair<uint64_t, uint32_t>, 
             std::unique_ptr<OnlineIndexBuilder>> index_builders_;
};

/**
 * Utility functions for ALTER operations
 */
namespace AlterUtils {
    /**
     * Check if column type change is safe (no data loss)
     */
    bool is_safe_type_change(const Field* from, const Field* to);
    
    /**
     * Check if ALTER needs table rebuild
     */
    bool needs_table_rebuild(const Alter_inplace_info* alter_info);
    
    /**
     * Convert field definition to KVTColumnDef
     */
    KVTColumnDef field_to_column_def(const Field* field);
    
    /**
     * Convert KEY definition to KVTIndexDef
     */
    KVTIndexDef key_to_index_def(const KEY* key_info);
    
    /**
     * Check if two indexes are equivalent
     */
    bool indexes_equal(const KEY* key1, const KEY* key2);
    
    /**
     * Get ALTER operation name for logging
     */
    const char* get_alter_operation_name(AlterOperationType type);
    
    /**
     * Estimate time for ALTER operation
     */
    uint64_t estimate_alter_time(const AlterContext* ctx, 
                                 uint64_t row_count);
}

/**
 * ALTER progress reporting
 */
class AlterProgress {
public:
    AlterProgress(const char* operation, uint64_t total_work);
    
    void update(uint64_t work_completed);
    void set_phase(const char* phase);
    void complete();
    
    double get_percentage() const;
    const char* get_phase() const { return current_phase_; }
    
private:
    const char* operation_;
    uint64_t total_work_;
    std::atomic<uint64_t> work_completed_;
    const char* current_phase_;
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace kvt_alter

#endif // KVT_ALTER_TABLE_H