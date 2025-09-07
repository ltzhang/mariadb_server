# KVT Storage Engine - Known Limitations

## Critical Limitations

### 1. Savepoints Not Functional
**Status**: Non-functional placeholder implementation
**Impact**: HIGH - Savepoint operations (SAVEPOINT, ROLLBACK TO SAVEPOINT, RELEASE SAVEPOINT) do not work
**Details**: 
- The savepoint methods in `kvt_transaction_manager.cc` are stubs that maintain internal state but don't interact with the KVT backend
- Files affected: `kvt_transaction_manager.cc` (lines 231-301)
- TODO comments indicate waiting for KVT savepoint API

**Workaround**: Applications should not rely on savepoints when using KVT storage engine. Use full transaction rollback instead.

### 2. Full-text Search (Restored but Incomplete)
**Status**: Restored with header fixes, but implementation is incomplete
**Impact**: MEDIUM - Full-text search API present but not fully functional
**Details**:
- Full-text adapter restored and fixed header dependency issues
- Files affected: `kvt_fulltext_adapter.cc/h`
- Fixed by reordering includes: my_global.h first, then SQL headers with pragma warnings
- Implementation is mostly stubs - actual full-text indexing/search not implemented

**Workaround**: Full-text indexes can be created but won't provide actual search functionality yet.

## Functional Limitations

### 3. Index Operations Incomplete
**Status**: Placeholder implementations
**Impact**: MEDIUM - Falls back to table scans
**Methods with placeholder implementations**:
- `index_read()` - TODO based on find_flag
- `index_next()` - TODO index traversal
- `index_prev()` - TODO reverse traversal
- `index_last()` - TODO position at end
- `delete_all_rows()` - TODO requires scan

**Workaround**: Performance may be degraded due to table scans instead of index lookups.

### 4. Foreign Key Enforcement Disabled
**Status**: Returns false to allow all operations
**Impact**: MEDIUM - Referential integrity not enforced
**Details**: Foreign key constraints are parsed and stored but not enforced during DML operations

**Workaround**: Application must handle referential integrity.

### 5. Query Optimizer Uses Hardcoded Values
**Status**: Placeholder calculations
**Impact**: LOW-MEDIUM - Suboptimal query plans
**Issues**:
- Memory pressure always returns 0.5
- Range selectivity fixed at 10%
- Memory usage hardcoded to 100MB
- Sample size multiplier is placeholder (10x)

**Workaround**: Manual query optimization may be needed for complex queries.

## Incomplete Features

### 6. Spatial Index R-tree Operations
**Missing functionality**:
- Higher level tree splits
- Tree condensation after deletion
- STR bulk loading
- Index validation

### 7. API Implementations
**Incomplete**:
- Row ID extraction uses placeholder
- Cardinality updates commented out
- Collation support marked TODO

## Development Artifacts

### 8. Temporary Solutions
- Field indexes used as "temporary IDs" in ALTER TABLE operations
- Hardcoded g++ compilation for KVT memory implementation
- Debug/placeholder comments throughout codebase

## Recommendations

1. **For Production Use**: 
   - Avoid savepoints
   - Don't rely on full-text search
   - Monitor foreign key integrity at application level
   - Test query performance thoroughly

2. **For Development**:
   - Priority fixes: Savepoints, Index operations, Foreign keys
   - Consider removing full-text support entirely if not feasible
   - Replace all hardcoded optimizer values with real calculations

## Version
Last Updated: 2025-01-07
KVT Storage Engine Version: 0.1.0-alpha
MariaDB Version: 12.2.0