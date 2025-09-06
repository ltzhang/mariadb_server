# Phase 3: Basic DML Operations - Implementation Plan

## Current State Assessment

### Already Implemented in Phase 2:
- ✅ Basic INSERT (write_row) - works with tx_id=0 (one-shot)
- ✅ Full table scan (rnd_init/rnd_next/rnd_end) - retrieves all rows
- ✅ Point query (rnd_pos) - direct key access
- ✅ UPDATE (update_row) - handles primary key changes
- ✅ DELETE (delete_row) - removes records
- ✅ Row serialization/deserialization (kvt_row_codec)

### Currently Missing/Incomplete:
- ❌ No real transaction support (tx_id always 0)
- ❌ No index-based access (only full table scans)
- ❌ No WHERE clause optimization
- ❌ No proper error recovery
- ❌ Limited testing of DML operations

## Problems to Solve in Phase 3

### 1. Transaction Consistency Problem
- Currently using tx_id=0 (one-shot operations)
- No isolation between concurrent operations
- Risk of partial updates in multi-row operations

### 2. Performance Problems
- Full table scans for all SELECT operations
- No index utilization for WHERE clauses
- Inefficient for large tables

### 3. Correctness Problems
- No validation of data types during insert/update
- No constraint checking (unique, foreign key)
- Limited NULL handling validation

## Implementation Approach

### Step 1: Enhance DML Operations (Without Transactions)

#### 1.1 Improve Row Key Generation
- Support composite primary keys
- Handle tables without primary keys (use internal rowid)
- Optimize key format for range scans

#### 1.2 Add WHERE Clause Support (Table Scans)
- Implement ha_kvt::cond_push() for condition pushdown
- Use KVT's update functions for server-side filtering
- Optimize common WHERE patterns

#### 1.3 Enhance Data Type Handling
- Proper NULL bitmap management
- Handle all MariaDB data types correctly
- Validate data on insert/update

#### 1.4 Add Bulk Operations
- Implement start_bulk_insert/end_bulk_insert
- Use kvt_batch_execute for multi-row operations
- Optimize for LOAD DATA INFILE

### Step 2: Add Basic Index Support (Preparation for Phase 5)

#### 2.1 Primary Key Index
- Map primary key to KVT key structure
- Implement index_read_map for key lookups
- Support unique constraint checking

#### 2.2 Index Statistics
- Implement ha_kvt::info() for optimizer
- Track basic statistics (row count, data size)
- Provide cardinality estimates

### Step 3: Comprehensive Testing

#### 3.1 Functional Tests
- All data types (INT, VARCHAR, TEXT, BLOB, DATE, etc.)
- NULL handling and default values
- Large data operations
- Edge cases (empty tables, single row, max values)

#### 3.2 Correctness Tests
- Data integrity after operations
- Concurrent access (even without transactions)
- Server restart persistence

#### 3.3 Performance Tests
- Bulk insert performance
- Table scan optimization
- Memory usage profiling

## Implementation Timeline

### Phase 3A: Core DML Enhancements (Week 1)
- Fix and optimize existing DML operations
- Add proper error handling and recovery
- Implement bulk operations
- Add comprehensive data type support

### Phase 3B: Query Optimization (Week 2)
- Implement condition pushdown
- Add basic WHERE clause handling
- Optimize table scans with filtering
- Add index infrastructure (prepare for Phase 5)

### Phase 3C: Testing and Validation (Week 3)
- Create comprehensive MTR test suite
- Add stress tests for concurrent operations
- Performance benchmarking
- Bug fixes and optimization

## Test Plan

### MTR Tests to Create:
1. `kvt_dml_basic.test` - Basic INSERT/SELECT/UPDATE/DELETE
2. `kvt_datatypes.test` - All MariaDB data types
3. `kvt_null_defaults.test` - NULL and default value handling
4. `kvt_bulk_ops.test` - Bulk insert and batch operations
5. `kvt_large_data.test` - BLOB/TEXT operations
6. `kvt_where_clause.test` - WHERE clause filtering
7. `kvt_concurrent.test` - Concurrent DML operations

### Performance Tests:
1. Bulk insert of 100K rows
2. Full table scan performance
3. Filtered scan performance
4. Memory usage under load

## Success Criteria

### Functional:
- All basic DML operations work correctly
- Support for all common data types
- Proper NULL and default value handling

### Performance:
- Bulk insert > 10K rows/second
- Table scan comparable to other engines
- Memory usage remains bounded

### Quality:
- Pass all MTR tests
- No data corruption under concurrent access
- Graceful error handling

## Implementation Tasks

### Priority 1: Core DML Operations
- [ ] Add composite primary key support
- [ ] Implement auto-increment handling
- [ ] Add proper NULL bitmap handling
- [ ] Implement bulk insert operations
- [ ] Add error recovery mechanisms

### Priority 2: Query Optimization
- [ ] Implement cond_push for WHERE clause
- [ ] Add basic filtering in table scans
- [ ] Implement index_read_map for primary keys
- [ ] Add statistics collection

### Priority 3: Testing
- [ ] Create MTR test suite
- [ ] Add data type tests
- [ ] Add concurrent operation tests
- [ ] Performance benchmarking

## Next Steps After Phase 3

After completing Phase 3, we'll move to Phase 4 (Transaction Support) which will:
- Replace tx_id=0 with proper transaction management
- Map THD to KVT transaction IDs
- Implement commit/rollback
- Add isolation levels

This sets up the foundation for full ACID compliance and prepares for advanced features in subsequent phases.