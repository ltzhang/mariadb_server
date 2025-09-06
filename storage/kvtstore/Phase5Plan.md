# Phase 5: Index Support - Implementation Plan

## Overview
Phase 5 focuses on implementing index support for the KVT storage engine. This includes primary key indexes, secondary indexes, and index-based access paths to improve query performance beyond full table scans.

## Problems to Solve

### 1. Index Storage Architecture
**Problem**: How to store and organize indexes in KVT's key-value model
- Primary keys need to map directly to row data
- Secondary indexes need to reference primary keys
- Must support unique and non-unique indexes
- Need efficient range scans on indexes

**Solution Approach**:
- Use separate KVT key prefixes for different index types
- Primary key: Direct mapping in main data table
- Secondary indexes: Separate key space with index_name prefix
- Store primary key reference in secondary index values

### 2. Index Maintenance
**Problem**: Keep indexes synchronized with data changes
- INSERT must update all indexes
- UPDATE may require index entry moves
- DELETE must remove from all indexes
- Handle unique constraint violations

**Solution Approach**:
- Update all indexes atomically within transaction
- Check uniqueness before insert/update
- Maintain index statistics for optimizer

### 3. Index-Based Access Paths
**Problem**: Optimize queries using indexes
- Point queries on indexed columns
- Range scans on indexed columns
- ORDER BY optimization
- Index-only scans (covering indexes)

**Solution Approach**:
- Implement handler index methods (index_read, index_next)
- Support index condition pushdown (ICP)
- Provide accurate cost estimates to optimizer

## Technical Design

### Key Space Organization

```
Primary Key Index (in data table):
__DATA_<database>\x00<table>\x00PK\x00<pk_value> -> serialized_row

Secondary Index (in catalog table):
__CATALOG__\x00<database>\x00<table>\x00IDX\x00<index_name>\x00<index_value>\x00<pk_value> -> empty

Index Metadata:
__CATALOG__\x00<database>\x00<table>\x00IDXMETA\x00<index_name> -> index_definition
```

### Implementation Components

#### 1. Index Metadata Management
- Store index definitions in catalog
- Track index columns, uniqueness, type
- Maintain index statistics

#### 2. Primary Key Implementation
- Already partially implemented in Phase 2-3
- Need to add index_read methods
- Support ordered access

#### 3. Secondary Index Implementation
- New index storage layer
- Index maintenance triggers
- Uniqueness enforcement

#### 4. Index Access Methods
- `index_init()` - Initialize index scan
- `index_read()` - Read by index key
- `index_read_last()` - Read last matching key
- `index_next()` - Read next in index order
- `index_prev()` - Read previous in index order
- `index_first()` - Position at first index entry
- `index_last()` - Position at last index entry

#### 5. Index Statistics
- Cardinality estimation
- Range selectivity
- Cost calculation for optimizer

## Implementation Plan

### Step 1: Index Metadata Infrastructure (Day 1-2)
1. Extend catalog to store index definitions
2. Add index creation/deletion to CREATE/DROP TABLE
3. Implement index discovery on table open
4. Add index flags to handler capabilities

### Step 2: Primary Key Index (Day 3-5)
1. Refactor existing primary key handling
2. Implement index_read for point queries
3. Add index_next/prev for ordered access
4. Support index range scans
5. Handle composite primary keys

### Step 3: Secondary Index Storage (Day 6-8)
1. Design secondary index key format
2. Implement index entry creation
3. Add index maintenance to write_row
4. Add index maintenance to update_row
5. Add index maintenance to delete_row

### Step 4: Secondary Index Access (Day 9-11)
1. Implement secondary index read methods
2. Add index-to-primary-key lookup
3. Support non-unique index duplicates
4. Handle covering index optimization
5. Implement index-only scans

### Step 5: Index Optimization (Day 12-14)
1. Implement index condition pushdown
2. Add index statistics collection
3. Provide cost estimates to optimizer
4. Support ORDER BY optimization
5. Handle index hints

### Step 6: Testing and Validation (Day 15-16)
1. Create comprehensive test suite
2. Validate unique constraints
3. Test concurrent index updates
4. Benchmark index performance
5. Compare with table scan performance

## Test Plan

### Functional Tests

#### kvt_index_primary.test
- Primary key point queries
- Primary key range scans
- Composite primary keys
- Primary key updates
- NULL handling in primary keys

#### kvt_index_secondary.test
- Secondary index creation
- Index-based queries
- Multi-column indexes
- Index updates with data changes
- Index deletion

#### kvt_index_unique.test
- Unique index constraints
- Duplicate key errors
- NULL in unique indexes
- Unique index updates

#### kvt_index_covering.test
- Index-only scans
- Covering index optimization
- Partial index coverage

#### kvt_index_orderby.test
- ORDER BY with indexes
- DESC index scans
- Multi-column ORDER BY
- LIMIT with indexes

### Performance Tests

#### kvt_index_performance.test
- Index vs table scan comparison
- Large table index performance
- Index creation time
- Index maintenance overhead

### Stress Tests

#### kvt_index_concurrent.test
- Concurrent index updates
- Index consistency under load
- Deadlock scenarios
- Transaction isolation with indexes

## Success Criteria

### Functional Requirements
- ✅ All index types work correctly
- ✅ Unique constraints enforced
- ✅ Index-based queries faster than table scans
- ✅ ORDER BY uses indexes when available
- ✅ Index statistics accurate

### Performance Requirements
- Index point queries < 1ms
- Index range scans linear with result size
- Index maintenance overhead < 20% of write time
- Index creation time reasonable for large tables

### Quality Requirements
- All tests pass
- No index corruption under stress
- Proper error handling
- Clear documentation

## Risk Mitigation

### Technical Risks
1. **Index corruption**: Implement consistency checks
2. **Performance regression**: Benchmark continuously
3. **Memory usage**: Monitor index cache size
4. **Deadlocks**: Test concurrent scenarios

### Implementation Risks
1. **Complexity**: Start with primary keys, iterate
2. **Integration issues**: Test with existing queries
3. **Optimizer confusion**: Provide accurate statistics

## Timeline

- **Days 1-2**: Index metadata infrastructure
- **Days 3-5**: Primary key index implementation
- **Days 6-8**: Secondary index storage
- **Days 9-11**: Secondary index access
- **Days 12-14**: Index optimization
- **Days 15-16**: Testing and validation

**Total**: 16 days (approximately 3 weeks)

## Dependencies

- Phase 4 transaction support (COMPLETED ✅)
- KVT scan operations working
- Catalog system functional
- Row codec operational

## Deliverables

1. **Code**:
   - kvt_index_manager.h/cc - Index management
   - Updated ha_kvt.cc with index methods
   - Enhanced catalog for index metadata

2. **Tests**:
   - 7+ new test files
   - Performance benchmarks
   - Stress test scenarios

3. **Documentation**:
   - Index design document
   - Performance tuning guide
   - Phase 5 summary report

## Next Steps After Phase 5

- Phase 6: Advanced Query Optimization
- Phase 7: Advanced Features
- Phase 8: Performance and Reliability

## Notes

- Primary key support partially exists from Phase 2-3
- Focus on making indexes actually improve performance
- Ensure compatibility with MariaDB optimizer
- Consider index statistics accuracy for good query plans