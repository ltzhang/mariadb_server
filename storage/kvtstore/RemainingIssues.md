# KVT Storage Engine - Remaining Issues and Tasks

## Status Legend
- 🔴 **Critical** - Blocking/Breaking functionality
- 🟡 **High** - Important for production use
- 🟢 **Medium** - Performance/Features
- 🔵 **Low** - Nice to have
- ⚫ **KVT-Dependent** - Requires KVT backend support

## KVT-Dependent Features (Requires Backend Support)

These features depend on KVT backend implementation and cannot be completed without KVT API support:

### Transaction & Storage Management ⚫
- [ ] **Savepoint Implementation** - Requires KVT savepoint API
  - `kvt_savepoint_create()`, `kvt_savepoint_rollback()`, `kvt_savepoint_release()`
  - Current code is placeholder only
- [ ] **Two-phase Commit (XA)** - Requires KVT XA support
- [ ] **Transaction Isolation Levels** - KVT must support different isolation levels
- [ ] **Deadlock Detection** - KVT must detect and report deadlocks
- [ ] **WAL (Write-Ahead Logging)** - KVT responsibility
- [ ] **Crash Recovery** - KVT must handle recovery
- [ ] **Checkpointing** - KVT must manage checkpoints
- [ ] **MVCC (Multi-Version Concurrency Control)** - KVT implementation
- [ ] **Vacuum/Garbage Collection** - KVT must reclaim space
- [ ] **Backup/Restore** - KVT must provide backup mechanisms
- [ ] **Point-in-Time Recovery** - KVT feature
- [ ] **Replication Support** - KVT must support replication logs
- [ ] **Durability Guarantees** - KVT responsibility
- [ ] **Page/Block Management** - KVT internal
- [ ] **Buffer Pool/Caching** - KVT should manage its own cache
- [ ] **Lock Management** - KVT handles locking
- [ ] **Compression** - KVT can implement transparent compression

## MariaDB Handler Implementation (Our Responsibility)

### High Priority - Core Functionality

#### 1. 🟡 Index Enhancements
- [ ] **Composite Index Support** - Multi-column indexes
  - Build composite keys from multiple columns
  - Handle partial key searches
  - Support all comparison operators
- [ ] **Index-only Scans** - Covering indexes
- [ ] **Index Statistics** - Cardinality estimation
- [ ] **Unique Constraints** - Enforce uniqueness

#### 2. 🟡 Query Optimization
- [x] ~~`records_in_range()`~~ - ✅ Completed
- [x] ~~`handler::info()` complete implementation~~ - ✅ Completed
- [x] ~~`read_range_first()/next()`~~ - ✅ Completed
- [ ] **Join optimization** - Better join order selection
- [ ] **Condition pushdown improvements** - More complex expressions

#### 3. 🟡 Handler Operations
- [ ] **Batch operations** - Multi-row inserts/updates
- [ ] **Handler pushdown** - Push more operations to KVT
- [ ] **Parallel scan support** - Multiple threads scanning

### Medium Priority - Performance & Features

#### 4. 🟢 Advanced Index Features
- [ ] **Descending indexes** - Support DESC in index definition
- [ ] **Functional indexes** - Index on expressions
- [ ] **Partial indexes** - Index with WHERE clause
- [ ] **Index hints** - USE/FORCE/IGNORE INDEX

#### 5. 🟢 Full-Text Search Enhancements
- [ ] **Phrase search** - Exact phrase matching
- [ ] **Proximity search** - NEAR operator
- [ ] **Custom stopwords** - User-defined lists
- [ ] **Language-specific stemming** - Multiple languages
- [ ] **Field boosting** - Weight different columns

#### 6. 🟢 Spatial Enhancements
- [ ] **3D/4D geometries** - Z and M coordinates
- [ ] **k-NN queries** - Nearest neighbor search
- [ ] **Spatial joins** - Optimize spatial predicates
- [ ] **Geography types** - Spherical calculations

#### 7. 🟢 Data Types & Features
- [ ] **JSON support** - Native JSON type
- [ ] **Generated columns** - Virtual/stored
- [ ] **DEFAULT expressions** - Complex defaults
- [ ] **CHECK constraints** - Row validation

### Low Priority - Advanced Features

#### 8. 🔵 Partitioning
- [ ] **Range partitioning** - By date/number ranges
- [ ] **List partitioning** - By discrete values
- [ ] **Hash partitioning** - Even distribution
- [ ] **Partition pruning** - Skip irrelevant partitions

#### 9. 🔵 Monitoring & Diagnostics
- [ ] **Performance schema tables** - Engine statistics
- [ ] **INFORMATION_SCHEMA views** - Metadata
- [ ] **EXPLAIN enhancements** - Better query plans
- [ ] **Slow query logging** - Track slow operations

#### 10. 🔵 Security Features
- [ ] **Column-level encryption** - Encrypt specific columns
- [ ] **Row-level security** - Access policies
- [ ] **Audit logging** - Track changes
- [ ] **Data masking** - Hide sensitive data

## Current Sprint Focus

### Immediate Task: Composite Index Implementation 🚧

1. **Design composite key encoding**
   - Combine multiple column values into single key
   - Handle NULL values properly
   - Support partial key lookups

2. **Implement key building functions**
   - `build_composite_index_key()`
   - `extract_composite_key_parts()`
   - `compare_composite_keys()`

3. **Update index operations**
   - Modify `index_read_map()` for composite keys
   - Update `index_next()` for partial matches
   - Handle key_part_map correctly

4. **Test composite indexes**
   - Multi-column PRIMARY KEY
   - Multi-column UNIQUE index
   - Partial key searches
   - ORDER BY optimization

## Testing & Quality

### Test Coverage Needed
- [ ] Composite index tests
- [ ] Stress tests with concurrent access
- [ ] Large dataset performance tests
- [ ] Edge case handling

### Documentation Needed
- [ ] Composite index design document
- [ ] Performance tuning guide
- [ ] API documentation updates

## Progress Tracking

Last Updated: 2025-01-06

### Recently Completed
- ✅ Query Optimization (`records_in_range`, `info()`, range scans)
- ✅ Full-Text Search Adapter
- ✅ Spatial Index Support
- ✅ Foreign Key Constraints

### Currently Working On
- 🚧 Composite Index Implementation

### Next Up
- Index-only scans
- Unique constraints
- Batch operations

## Notes

- Features marked with ⚫ are KVT-dependent and cannot be implemented without backend support
- We focus on MariaDB handler interface implementation that works with existing KVT API
- Composite indexes are our immediate priority as they don't require KVT changes