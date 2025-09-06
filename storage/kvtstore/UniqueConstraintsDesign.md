# Unique Constraints Design for KVT Storage Engine

## Overview
Implement support for UNIQUE constraints in the KVT storage engine to ensure data integrity by preventing duplicate values in specified columns or column combinations.

## Motivation

### Business Requirements
1. **Data Integrity**: Prevent duplicate entries in critical fields (email, SSN, etc.)
2. **Business Rules**: Enforce uniqueness for natural keys
3. **SQL Compliance**: Support standard UNIQUE constraint syntax
4. **Performance**: Efficient duplicate detection without full table scans

### Technical Requirements
1. **Multi-Column Support**: UNIQUE(col1, col2, ...)
2. **NULL Handling**: Multiple NULLs allowed per SQL standard
3. **Transaction Safety**: Atomic constraint checking
4. **Error Reporting**: Clear duplicate key error messages

## Current State Analysis

### What We Have
1. **Composite Index Support**: Can build multi-column index keys
2. **Index Operations**: index_read_map() for key lookups
3. **Transaction Manager**: KVT transaction support
4. **Error Handling**: MySQL error mapping infrastructure

### What We Need
1. **Duplicate Detection**: Check before insert/update
2. **Unique Index Metadata**: Track which indexes are unique
3. **NULL Value Handling**: Special logic for NULL in unique constraints
4. **Conflict Resolution**: Handle duplicate key errors properly

## Design Approach

### 1. Unique Constraint Storage

#### Unique Index Identification
```cpp
struct UniqueIndexInfo {
    uint index_id;
    bool is_primary;
    bool allows_nulls;
    std::vector<uint> column_indices;
    std::string constraint_name;
};
```

#### Unique Key Format
Same as composite index but with special handling:
```
[table_id:8][index_id:4][column_values][unique_suffix]
```

Where unique_suffix is:
- For non-NULL values: empty (key must be unique)
- For NULL-containing keys: [null_row_id:8] (allows multiple NULLs)

### 2. Duplicate Detection Strategy

#### Check Before Write
```cpp
bool check_unique_constraint(
    const uchar* record,
    KEY* key_info,
    uint index_id,
    bool is_update = false,
    const uchar* old_record = nullptr)
{
    // Build unique key from record
    std::string unique_key = build_unique_key(record, key_info);
    
    // Check if key contains any NULL
    if (has_null_in_unique_key(record, key_info)) {
        // NULL values don't violate uniqueness
        return true;
    }
    
    // Check if key already exists
    std::string existing_value;
    if (kvt_get(unique_key, existing_value) == SUCCESS) {
        // Key exists - check if it's the same row (for updates)
        if (is_update && is_same_row(existing_value, old_record)) {
            return true;  // Updating same row is OK
        }
        return false;  // Duplicate found
    }
    
    return true;  // No duplicate
}
```

### 3. Write Operation Flow

#### INSERT Flow
```
1. Validate NOT NULL constraints
2. For each UNIQUE index:
   a. Build unique key from new record
   b. Check if key already exists in KVT
   c. If exists and not all-NULL, return duplicate error
3. If all unique checks pass:
   a. Insert row data
   b. Insert all index entries
   c. Commit transaction
```

#### UPDATE Flow
```
1. For each UNIQUE index affected by update:
   a. Build old unique key from old record
   b. Build new unique key from new record
   c. If keys differ:
      - Check if new key already exists
      - If exists and not same row, return duplicate error
2. If all unique checks pass:
   a. Update row data
   b. Update affected index entries
   c. Commit transaction
```

### 4. NULL Handling Strategy

SQL Standard: Multiple NULL values are allowed in UNIQUE constraints

```cpp
bool has_null_in_unique_key(const uchar* record, KEY* key_info) {
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        Field* field = key_info->key_part[i].field;
        if (field->is_null_in_record(record)) {
            return true;
        }
    }
    return false;
}
```

For partial NULL keys (some columns NULL, some not):
- Still allow duplicates per SQL standard
- Only all non-NULL combinations must be unique

## Implementation Plan

### Phase 1: Unique Constraint Infrastructure

1. **Create Unique Constraint Manager**
```cpp
class KVTUniqueConstraintManager {
public:
    // Check if a record violates unique constraints
    int check_unique_constraints(
        TABLE* table,
        const uchar* new_record,
        const uchar* old_record = nullptr);
    
    // Register unique index
    void register_unique_index(
        uint64_t table_id,
        uint index_id,
        KEY* key_info);
    
    // Get duplicate key error info
    std::string get_duplicate_key_error(
        KEY* key_info,
        const uchar* record);
        
private:
    // Check single unique constraint
    bool check_single_unique_constraint(
        uint64_t table_id,
        KEY* key_info,
        const uchar* new_record,
        const uchar* old_record);
        
    // Build error message
    std::string format_duplicate_error(
        const std::string& index_name,
        const std::vector<std::string>& values);
};
```

2. **Unique Key Building Functions**
```cpp
namespace kvt_unique {
    // Build unique constraint key
    std::string build_unique_key(
        uint64_t table_id,
        uint index_id,
        KEY* key_info,
        const uchar* record);
    
    // Extract values for error message
    std::vector<std::string> extract_key_values(
        KEY* key_info,
        const uchar* record);
    
    // Check if unique key contains NULL
    bool contains_null(
        KEY* key_info,
        const uchar* record);
}
```

### Phase 2: Handler Integration

1. **Update write_row()**
```cpp
int ha_kvt::write_row(const uchar *buf) {
    // Check unique constraints first
    auto* unique_mgr = KVTUniqueConstraintManager::get_instance();
    int check_result = unique_mgr->check_unique_constraints(
        table, buf, nullptr);
    
    if (check_result != 0) {
        // Set duplicate key error
        return HA_ERR_FOUND_DUPP_KEY;
    }
    
    // Proceed with normal insert
    ...
}
```

2. **Update update_row()**
```cpp
int ha_kvt::update_row(const uchar *old_data, const uchar *new_data) {
    // Check if update violates unique constraints
    auto* unique_mgr = KVTUniqueConstraintManager::get_instance();
    int check_result = unique_mgr->check_unique_constraints(
        table, new_data, old_data);
    
    if (check_result != 0) {
        return HA_ERR_FOUND_DUPP_KEY;
    }
    
    // Proceed with normal update
    ...
}
```

3. **Error Information**
```cpp
void ha_kvt::info(uint flag) {
    if (flag & HA_STATUS_ERRKEY) {
        // Set duplicate key information
        errkey = last_dup_key;
        my_error(ER_DUP_ENTRY, MYF(0), 
                dup_key_message.c_str(),
                table->s->key_info[errkey].name.str);
    }
}
```

### Phase 3: Optimization

1. **Batch Unique Checks**
For bulk inserts, batch unique constraint checks:
```cpp
class UniqueBatchChecker {
    std::unordered_set<std::string> pending_keys;
    
    bool add_pending(const std::string& key) {
        if (pending_keys.count(key)) {
            return false;  // Duplicate in batch
        }
        pending_keys.insert(key);
        return true;
    }
    
    bool check_against_storage() {
        // Batch check all pending keys
        std::vector<std::string> keys(pending_keys.begin(), 
                                     pending_keys.end());
        return kvt_batch_exists(keys);
    }
};
```

2. **Unique Index Statistics**
Track unique constraint violations:
```cpp
struct UniqueConstraintStats {
    uint64_t total_checks;
    uint64_t violations;
    uint64_t null_keys;
    std::map<uint, uint64_t> violations_per_index;
};
```

### Phase 4: Advanced Features

1. **Deferred Constraint Checking**
```cpp
class DeferredUniqueChecker {
    std::vector<UniqueCheck> deferred_checks;
    
    void defer_check(const UniqueCheck& check) {
        deferred_checks.push_back(check);
    }
    
    bool validate_all_deferred() {
        // Check all deferred constraints at commit time
        for (const auto& check : deferred_checks) {
            if (!validate_unique(check)) {
                return false;
            }
        }
        return true;
    }
};
```

2. **Conflict Resolution Options**
```cpp
enum UniqueConflictAction {
    ERROR,           // Default: return error
    IGNORE,          // Skip insert/update
    REPLACE,         // Delete existing, insert new
    UPDATE_EXISTING  // Update existing row
};
```

## Test Plan

### 1. Basic Unique Constraint Tests
```sql
-- Single column unique
CREATE TABLE t1 (
    id INT PRIMARY KEY,
    email VARCHAR(100) UNIQUE
) ENGINE=KVT;

-- Should succeed
INSERT INTO t1 VALUES (1, 'test@example.com');

-- Should fail (duplicate)
INSERT INTO t1 VALUES (2, 'test@example.com');

-- NULL handling - should succeed
INSERT INTO t1 VALUES (3, NULL);
INSERT INTO t1 VALUES (4, NULL);
```

### 2. Multi-Column Unique Tests
```sql
-- Composite unique constraint
CREATE TABLE t2 (
    id INT PRIMARY KEY,
    first_name VARCHAR(50),
    last_name VARCHAR(50),
    UNIQUE KEY uk_name (first_name, last_name)
) ENGINE=KVT;

-- Test various combinations
INSERT INTO t2 VALUES (1, 'John', 'Doe');     -- OK
INSERT INTO t2 VALUES (2, 'Jane', 'Doe');     -- OK
INSERT INTO t2 VALUES (3, 'John', 'Smith');   -- OK
INSERT INTO t2 VALUES (4, 'John', 'Doe');     -- FAIL
```

### 3. NULL Handling Tests
```sql
-- Partial NULL in composite unique
INSERT INTO t2 VALUES (5, 'Alice', NULL);     -- OK
INSERT INTO t2 VALUES (6, 'Alice', NULL);     -- OK (NULL allows duplicates)
INSERT INTO t2 VALUES (7, NULL, 'Brown');     -- OK
INSERT INTO t2 VALUES (8, NULL, NULL);        -- OK
```

### 4. Update Tests
```sql
-- Update to duplicate value
UPDATE t1 SET email = 'test@example.com' WHERE id = 3;  -- FAIL

-- Update to unique value
UPDATE t1 SET email = 'new@example.com' WHERE id = 3;   -- OK

-- Update same row (no-op for unique)
UPDATE t1 SET email = 'test@example.com' WHERE id = 1;  -- OK
```

### 5. Performance Tests
```sql
-- Bulk insert with unique checks
INSERT INTO t1 SELECT seq, CONCAT('user', seq, '@test.com') 
FROM seq_1_to_10000;

-- Measure unique check overhead
```

### 6. Edge Cases
```sql
-- Empty string vs NULL
INSERT INTO t1 VALUES (100, '');    -- OK
INSERT INTO t1 VALUES (101, '');    -- FAIL

-- Case sensitivity
INSERT INTO t1 VALUES (102, 'Test@Example.com');  -- Depends on collation

-- Long values
INSERT INTO t1 VALUES (103, REPEAT('a', 100));    -- OK
INSERT INTO t1 VALUES (104, REPEAT('a', 100));    -- FAIL
```

## Error Handling

### Error Messages
```cpp
std::string format_duplicate_entry_error(
    KEY* key_info,
    const std::vector<std::string>& values)
{
    std::string error = "Duplicate entry '";
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0) error += "-";
        error += values[i];
    }
    error += "' for key '";
    error += key_info->name.str;
    error += "'";
    return error;
}
```

### Error Codes
- `HA_ERR_FOUND_DUPP_KEY` - Duplicate key found
- `ER_DUP_ENTRY` - MySQL error for duplicate entry
- `ER_DUP_UNIQUE` - Duplicate unique constraint

## Performance Considerations

### 1. Index Lookup Cost
- Each unique constraint requires an index lookup
- Batch operations should minimize lookups
- Consider bloom filters for negative cache

### 2. Transaction Overhead
- Unique checks must be atomic with writes
- Consider optimistic vs pessimistic locking
- Batch validation at commit time

### 3. Memory Usage
- Cache recent unique checks
- Limit batch checker memory usage
- Clear caches on transaction boundaries

## Success Criteria

1. **Functional Requirements**
   - ✓ Single-column UNIQUE constraints work
   - ✓ Multi-column UNIQUE constraints work
   - ✓ NULL values handled per SQL standard
   - ✓ Clear error messages for violations

2. **Performance Requirements**
   - ✓ Unique check overhead < 10% for single row
   - ✓ Batch operations optimized
   - ✓ No memory leaks

3. **Compatibility Requirements**
   - ✓ Works with existing indexes
   - ✓ Compatible with MariaDB syntax
   - ✓ Proper error codes returned

## Future Enhancements

1. **Deferred Constraints**
   - Check at commit time instead of statement time
   - Useful for circular references

2. **ON DUPLICATE KEY UPDATE**
   - Support INSERT ... ON DUPLICATE KEY UPDATE
   - Automatic conflict resolution

3. **Partial Unique Indexes**
   - UNIQUE with WHERE clause
   - Conditional uniqueness

4. **Ignore Duplicates Option**
   - INSERT IGNORE support
   - Configurable conflict handling

## Risks and Mitigation

### 1. Performance Impact
**Risk**: Unique checks slow down writes
**Mitigation**: 
- Implement efficient index lookups
- Batch checking for bulk operations
- Optional async checking for non-critical constraints

### 2. Deadlock Potential
**Risk**: Unique checks may cause deadlocks
**Mitigation**:
- Consistent lock ordering
- Timeout and retry logic
- Optimistic concurrency control

### 3. Memory Overhead
**Risk**: Tracking unique constraints uses memory
**Mitigation**:
- Bounded cache sizes
- Efficient data structures
- Periodic cleanup

## Conclusion

The unique constraints implementation will provide essential data integrity features for the KVT storage engine. By leveraging existing composite index support and adding efficient duplicate detection, we can ensure data consistency while maintaining good performance. The design handles SQL-standard NULL semantics and provides clear error reporting for constraint violations.