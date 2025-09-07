# ALTER TABLE Implementation Design for KVT Storage Engine

## Overview
ALTER TABLE is critical functionality that allows schema evolution without recreating tables. We'll implement both instant (metadata-only) and inplace (online) ALTER operations for the KVT storage engine.

## ALTER TABLE Operation Types

### 1. Instant Operations (Metadata Only)
- **ADD COLUMN** with DEFAULT value
- **DROP COLUMN** (mark as hidden)
- **RENAME COLUMN**
- **MODIFY DEFAULT value**
- **ADD/DROP INDEX** (metadata update)

### 2. Inplace Operations (Online)
- **MODIFY COLUMN** type/size
- **ADD PRIMARY KEY**
- **CHANGE column order**
- **ADD UNIQUE constraint**

### 3. Copy Operations (Table Rebuild)
- **CHANGE storage engine**
- **CHANGE ROW_FORMAT**
- **Convert character set**
- **Partition changes**

## Implementation Strategy

### Phase 1: Instant ALTER Support
Modify metadata without touching data rows:
```cpp
// Example: ADD COLUMN with DEFAULT
ALTER TABLE t1 ADD COLUMN new_col INT DEFAULT 0;
// Only updates table metadata, not existing rows
```

### Phase 2: Online ALTER Support
Allow concurrent DML during ALTER:
```cpp
// Example: ADD INDEX online
ALTER TABLE t1 ADD INDEX idx_col (col), ALGORITHM=INPLACE, LOCK=NONE;
// Table remains accessible during index creation
```

## Handler Methods to Implement

### 1. check_if_supported_inplace_alter()
```cpp
enum_alter_inplace_result ha_kvt::check_if_supported_inplace_alter(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info)
{
    // Determine if ALTER can be done inplace
    // Return:
    //   HA_ALTER_INPLACE_NOT_SUPPORTED - Requires table copy
    //   HA_ALTER_INPLACE_EXCLUSIVE_LOCK - Needs exclusive lock
    //   HA_ALTER_INPLACE_SHARED_LOCK - Needs shared lock  
    //   HA_ALTER_INPLACE_NO_LOCK - Can be done online
}
```

### 2. prepare_inplace_alter_table()
```cpp
bool ha_kvt::prepare_inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info)
{
    // Prepare for ALTER operation
    // - Validate changes
    // - Allocate resources
    // - Create temporary structures
    // Return true on error, false on success
}
```

### 3. inplace_alter_table()
```cpp
bool ha_kvt::inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info)
{
    // Perform the actual ALTER
    // - Modify metadata
    // - Update indexes
    // - Convert data if needed
    // Return true on error, false on success
}
```

### 4. commit_inplace_alter_table()
```cpp
bool ha_kvt::commit_inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info,
    bool commit)
{
    // Finalize ALTER operation
    // - Make changes permanent
    // - Update catalog
    // - Clean up temporary structures
    // Return true on error, false on success
}
```

### 5. rollback_inplace_alter_table()
```cpp
bool ha_kvt::rollback_inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info)
{
    // Rollback ALTER operation
    // - Restore original metadata
    // - Clean up temporary structures
    // - Release resources
    // Return true on error, false on success
}
```

## Alter_inplace_info Structure

```cpp
class Alter_inplace_info {
    // Flags indicating what is being altered
    HA_ALTER_FLAGS handler_flags;
    
    // Original table structure
    TABLE *table;
    
    // New table structure
    TABLE *altered_table;
    
    // Key (index) information
    KEY *key_info_buffer;
    uint key_count;
    
    // Column changes
    Create_field *create_list;
    
    // Index changes
    Alter_index_add *index_add_buffer;
    Alter_index_drop *index_drop_buffer;
    
    // Rename information
    const char *new_db_name;
    const char *new_table_name;
};
```

## ALTER Operations Support Matrix

| Operation | Support Level | Algorithm | Lock Required |
|-----------|--------------|-----------|---------------|
| ADD COLUMN (end) | ✅ Instant | INSTANT | None |
| ADD COLUMN (middle) | ✅ Inplace | INPLACE | Shared |
| DROP COLUMN | ✅ Instant | INSTANT | None |
| RENAME COLUMN | ✅ Instant | INSTANT | None |
| MODIFY COLUMN type | ✅ Inplace | INPLACE | Shared |
| ADD INDEX | ✅ Online | INPLACE | None |
| DROP INDEX | ✅ Instant | INSTANT | None |
| ADD PRIMARY KEY | ⚠️ Copy | COPY | Exclusive |
| ADD FOREIGN KEY | ✅ Instant | INSTANT | None |
| ADD UNIQUE | ✅ Inplace | INPLACE | Shared |
| RENAME TABLE | ✅ Instant | INSTANT | None |

## Column Metadata Management

### Schema Version Tracking
```cpp
struct KVTTableSchema {
    uint64_t schema_version;
    std::vector<KVTColumnDef> columns;
    std::map<uint32_t, KVTColumnDef> dropped_columns;  // Hidden columns
    std::map<std::string, uint32_t> column_name_map;
};

struct KVTColumnDef {
    uint32_t column_id;        // Permanent ID
    std::string name;
    KVTColumnType type;
    bool is_nullable;
    std::optional<std::string> default_value;
    uint64_t added_version;    // Schema version when added
    uint64_t dropped_version;  // 0 if active, >0 if dropped
};
```

### Instant ADD COLUMN
```cpp
// Add column to metadata without modifying rows
void instant_add_column(KVTTableSchema& schema, const KVTColumnDef& new_col) {
    schema.schema_version++;
    new_col.added_version = schema.schema_version;
    schema.columns.push_back(new_col);
    
    // Existing rows will use default value when reading
}
```

### Instant DROP COLUMN
```cpp
// Mark column as dropped without removing data
void instant_drop_column(KVTTableSchema& schema, uint32_t column_id) {
    schema.schema_version++;
    auto it = find_column(schema.columns, column_id);
    if (it != schema.columns.end()) {
        it->dropped_version = schema.schema_version;
        schema.dropped_columns[column_id] = *it;
        schema.columns.erase(it);
    }
}
```

## Index Management During ALTER

### Online Index Creation
```cpp
class OnlineIndexBuilder {
    // Build index while allowing concurrent DML
    void build_index_online(uint64_t table_id, const KEY& key_info) {
        // Phase 1: Scan existing data
        scan_and_build_initial_index();
        
        // Phase 2: Track concurrent changes
        start_change_tracking();
        
        // Phase 3: Apply tracked changes
        apply_tracked_changes();
        
        // Phase 4: Finalize index
        finalize_index();
    }
    
private:
    std::vector<DMLOperation> tracked_changes;
};
```

### Index Drop
```cpp
void drop_index_instant(uint64_t table_id, uint32_t index_id) {
    // Mark index as dropped in metadata
    // Actual cleanup happens asynchronously
    mark_index_dropped(index_id);
    schedule_background_cleanup(index_id);
}
```

## Data Conversion for Type Changes

### Safe Type Conversions (Online)
```cpp
bool is_safe_type_conversion(const Field* from, const Field* to) {
    // INT -> BIGINT (widening)
    // VARCHAR(50) -> VARCHAR(100) (lengthening)
    // NULL -> NOT NULL with DEFAULT
    return can_convert_without_data_loss(from, to);
}
```

### Type Conversion Process
```cpp
void convert_column_type_online(
    uint64_t table_id,
    uint32_t column_id,
    const Field* old_type,
    const Field* new_type)
{
    if (is_safe_type_conversion(old_type, new_type)) {
        // Just update metadata
        update_column_metadata(column_id, new_type);
    } else {
        // Need to convert each row
        convert_rows_in_background(table_id, column_id, new_type);
    }
}
```

## Concurrent DML Handling

### Change Tracking During ALTER
```cpp
class AlterChangeLog {
    // Track DML operations during ALTER
    void log_insert(const KVTRow& row);
    void log_update(const KVTKey& key, const KVTRow& old_row, const KVTRow& new_row);
    void log_delete(const KVTKey& key);
    
    // Apply logged changes after ALTER
    void apply_to_new_structure();
    
private:
    std::vector<DMLOperation> operations;
    std::mutex log_mutex;
};
```

## Error Handling and Rollback

### Rollback Strategy
```cpp
class AlterRollbackManager {
    // Save original state before ALTER
    void save_checkpoint() {
        original_schema = current_schema;
        original_indexes = current_indexes;
    }
    
    // Restore on failure
    void rollback() {
        current_schema = original_schema;
        current_indexes = original_indexes;
        cleanup_temporary_structures();
    }
    
private:
    KVTTableSchema original_schema;
    std::vector<KVTIndexDef> original_indexes;
};
```

## Testing Plan

### Functional Tests
1. **Basic ALTER operations**
   - ADD/DROP/MODIFY COLUMN
   - ADD/DROP INDEX
   - RENAME TABLE/COLUMN

2. **Online ALTER**
   - Concurrent INSERT during ALTER
   - Concurrent UPDATE during ALTER
   - Concurrent SELECT during ALTER

3. **Rollback scenarios**
   - Failure during ALTER
   - Explicit ROLLBACK
   - Crash recovery

4. **Edge cases**
   - Large tables (millions of rows)
   - Many columns (100+)
   - Long column names/values

### Performance Tests
- ALTER on 1M row table
- Online index creation time
- Impact on concurrent operations
- Memory usage during ALTER

## Implementation Priority

### Phase 1: Basic ALTER (Week 1)
1. Implement check_if_supported_inplace_alter()
2. Simple ADD/DROP COLUMN
3. RENAME operations
4. Basic testing

### Phase 2: Online ALTER (Week 2)
1. Online index operations
2. Column type changes
3. Change tracking
4. Concurrent DML handling

### Phase 3: Advanced Features (Week 3)
1. Complex type conversions
2. Multi-column operations
3. Performance optimization
4. Comprehensive testing

## Success Criteria

1. **Functionality**
   - All common ALTER operations supported
   - Online operations don't block DML
   - Correct rollback on failure

2. **Performance**
   - Instant operations < 100ms
   - Online operations with minimal impact
   - Memory usage proportional to change size

3. **Reliability**
   - No data loss on failure
   - Atomic operation guarantee
   - Crash-safe implementation