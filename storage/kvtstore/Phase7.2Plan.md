# Phase 7.2: Foreign Key Constraints - Implementation Plan

## Overview
This document outlines the implementation of foreign key constraint support for the KVT storage engine. Foreign keys are essential for maintaining referential integrity between related tables and supporting cascade operations.

## Motivation

### Business Requirements
1. **Data Integrity**: Prevent orphaned records and ensure referential consistency
2. **Application Compatibility**: Support existing applications that rely on FK constraints
3. **Migration Support**: Enable migration from InnoDB and other engines
4. **Compliance**: Meet enterprise requirements for data integrity guarantees

### Technical Benefits
1. **Automatic Validation**: Database-level enforcement reduces application complexity
2. **Cascade Operations**: Automatic propagation of changes through relationships
3. **Performance**: Centralized validation more efficient than application-level checks
4. **Documentation**: FK constraints serve as schema documentation

## Design

### Architecture Components

```
┌─────────────────────────────────────────┐
│         MariaDB SQL Layer               │
├─────────────────────────────────────────┤
│         ha_kvt Handler                  │
│  ┌────────────────────────────────┐     │
│  │   Foreign Key Manager          │     │
│  ├────────────────────────────────┤     │
│  │ - Constraint Definitions       │     │
│  │ - Validation Engine            │     │
│  │ - Cascade Handler              │     │
│  │ - Metadata Cache               │     │
│  └────────────────────────────────┘     │
├─────────────────────────────────────────┤
│         KVT Transaction Layer           │
└─────────────────────────────────────────┘
```

### Key Design Decisions

#### 1. Metadata Storage Schema

**FK Constraint Definition**:
```
Key: __CATALOG__\x00<database>\x00<table>\x00FK\x00<constraint_name>
Value: {
  "parent_db": "database_name",
  "parent_table": "table_name",
  "parent_columns": ["col1", "col2"],
  "child_columns": ["col1", "col2"],
  "on_delete": "CASCADE|RESTRICT|SET_NULL|NO_ACTION",
  "on_update": "CASCADE|RESTRICT|SET_NULL|NO_ACTION"
}
```

**Reverse Index for Cascade Operations**:
```
Key: __CATALOG__\x00<parent_db>\x00<parent_table>\x00FK_REF\x00<child_db>\x00<child_table>
Value: ["constraint_name1", "constraint_name2"]
```

#### 2. Validation Flow

**INSERT Validation**:
```cpp
1. Extract FK column values from new row
2. For each FK constraint:
   a. Build parent table lookup key
   b. Check parent row exists (kvt_get)
   c. If not found, return FK violation error
3. Proceed with insert if all constraints satisfied
```

**DELETE Validation**:
```cpp
1. Check for dependent child rows
2. Based on ON DELETE action:
   - RESTRICT: Fail if children exist
   - CASCADE: Delete children recursively
   - SET NULL: Update children FK to NULL
   - NO ACTION: Check at statement end
3. Execute appropriate action
```

**UPDATE Validation**:
```cpp
1. If FK columns changed:
   a. Validate new values against parent
   b. Check for dependent children
2. Based on ON UPDATE action:
   - Handle similar to DELETE
3. Execute update if valid
```

#### 3. Cascade Operation Strategy

**Batch Processing**:
- Collect all affected rows first
- Execute cascade operations in batches
- Use KVT transactions for atomicity

**Deadlock Prevention**:
- Consistent lock ordering (parent → child)
- Timeout detection and retry
- Rollback on deadlock

### Implementation Classes

#### ForeignKeyConstraint Structure
```cpp
struct ForeignKeyConstraint {
  std::string constraint_name;
  std::string parent_database;
  std::string parent_table;
  std::vector<std::string> parent_columns;
  std::vector<std::string> child_columns;
  
  enum Action {
    ACTION_RESTRICT,
    ACTION_CASCADE,
    ACTION_SET_NULL,
    ACTION_NO_ACTION,
    ACTION_SET_DEFAULT
  };
  
  Action on_delete_action;
  Action on_update_action;
  
  // Helper methods
  bool matches_columns(const std::vector<std::string>& cols) const;
  std::string generate_parent_key(const uchar* row, TABLE* table) const;
};
```

#### ForeignKeyManager Class
```cpp
class ForeignKeyManager {
private:
  // Singleton instance
  static ForeignKeyManager* instance;
  
  // Cache of FK constraints per table
  std::map<std::string, std::vector<ForeignKeyConstraint>> table_constraints;
  
  // Mutex for thread safety
  mutable std::mutex constraints_mutex;
  
public:
  // Singleton access
  static ForeignKeyManager* get_instance();
  
  // Constraint management
  int add_constraint(const std::string& database,
                    const std::string& table,
                    const ForeignKeyConstraint& constraint);
  
  int drop_constraint(const std::string& database,
                     const std::string& table,
                     const std::string& constraint_name);
  
  // Validation methods
  int validate_insert(THD* thd, TABLE* table, const uchar* new_row);
  int validate_update(THD* thd, TABLE* table, 
                     const uchar* old_row, const uchar* new_row);
  int validate_delete(THD* thd, TABLE* table, const uchar* old_row);
  
  // Cascade operations
  int execute_cascade_delete(THD* thd, TABLE* parent_table,
                            const uchar* parent_row);
  int execute_cascade_update(THD* thd, TABLE* parent_table,
                            const uchar* old_row, const uchar* new_row);
  
  // Metadata operations
  int load_constraints(const std::string& database, const std::string& table);
  int save_constraint_to_catalog(const ForeignKeyConstraint& constraint);
  
  // Utility methods
  bool has_child_references(THD* thd, TABLE* table, const uchar* row);
  int check_parent_exists(THD* thd, const ForeignKeyConstraint& constraint,
                         const uchar* child_row, TABLE* child_table);
};
```

## Implementation Plan

### Step 1: Create Foreign Key Manager (Day 1-2)
1. Create kvt_foreign_key.h with class definitions
2. Implement kvt_foreign_key.cc with core logic
3. Add constraint structure and enums
4. Implement singleton pattern

### Step 2: Metadata Storage (Day 3)
1. Extend catalog to store FK constraints
2. Implement save/load operations
3. Create reverse index for parent tables
4. Add constraint cache management

### Step 3: Validation Logic (Day 4-5)
1. Implement validate_insert()
2. Implement validate_update()
3. Implement validate_delete()
4. Add parent existence checking
5. Add child reference checking

### Step 4: Cascade Operations (Day 6-7)
1. Implement CASCADE delete
2. Implement CASCADE update
3. Implement SET NULL operations
4. Implement RESTRICT logic
5. Add batch processing for efficiency

### Step 5: Integration with Handler (Day 8)
1. Modify ha_kvt::write_row() for INSERT
2. Modify ha_kvt::update_row() for UPDATE
3. Modify ha_kvt::delete_row() for DELETE
4. Parse FK constraints in CREATE TABLE
5. Handle ALTER TABLE for FK operations

### Step 6: Testing (Day 9-10)
1. Create basic FK tests
2. Create cascade operation tests
3. Create transaction tests
4. Create performance tests
5. Create error handling tests

## Test Plan

### Test Coverage Areas

#### 1. Basic Foreign Key Tests (kvt_fk_basic.test)
- Single column FK
- Multi-column FK
- Self-referencing tables
- Multiple FKs on same table
- FK with different data types

#### 2. Cascade Tests (kvt_fk_cascade.test)
- ON DELETE CASCADE
- ON DELETE RESTRICT
- ON DELETE SET NULL
- ON UPDATE CASCADE
- ON UPDATE RESTRICT
- Multi-level cascades

#### 3. Transaction Tests (kvt_fk_transaction.test)
- FK violations cause rollback
- Cascade within transaction
- Concurrent FK operations
- Deadlock scenarios

#### 4. Performance Tests (kvt_fk_performance.test)
- Large cascade operations
- FK validation overhead
- Bulk insert with FK checks
- Impact on DML operations

#### 5. Error Tests (kvt_fk_errors.test)
- Invalid FK definitions
- Circular references
- Missing parent tables
- Type mismatches

## Success Criteria

### Functional Requirements
- ✅ All FK constraint types supported
- ✅ Cascade operations work correctly
- ✅ No referential integrity violations
- ✅ Transaction safety maintained
- ✅ Error messages clear and helpful

### Performance Requirements
- FK validation < 5ms per operation
- Cascade operations scale linearly
- Minimal impact on non-FK operations
- Efficient metadata caching

### Quality Requirements
- 100% test coverage for FK paths
- No memory leaks
- Thread-safe operations
- Proper error handling

## Risk Mitigation

### Technical Risks
1. **Performance Impact**: Mitigate with caching and indexing
2. **Deadlocks**: Use consistent lock ordering
3. **Cascade Loops**: Detect and prevent circular references
4. **Memory Usage**: Limit cascade batch sizes

### Implementation Risks
1. **Complexity**: Start with basic validation, add cascades later
2. **Testing**: Comprehensive test suite from start
3. **Integration**: Careful integration with existing code

## Future Enhancements

1. **Deferred Constraint Checking**: Check at transaction end
2. **Match Types**: MATCH FULL, MATCH PARTIAL support
3. **Performance Optimizations**: FK-aware query optimization
4. **Monitoring**: FK violation statistics and monitoring

## Timeline

- **Days 1-2**: Core FK Manager implementation
- **Day 3**: Metadata storage
- **Days 4-5**: Validation logic
- **Days 6-7**: Cascade operations
- **Day 8**: Handler integration
- **Days 9-10**: Testing and validation

**Total**: 10 days

## Deliverables

1. **Code Files**:
   - kvt_foreign_key.h/cc - FK management system
   - Updates to kvt_catalog.cc
   - Updates to ha_kvt.cc

2. **Test Files**:
   - kvt_fk_basic.test
   - kvt_fk_cascade.test
   - kvt_fk_transaction.test
   - kvt_fk_performance.test
   - kvt_fk_errors.test

3. **Documentation**:
   - This plan document
   - Implementation summary
   - User documentation for FK usage