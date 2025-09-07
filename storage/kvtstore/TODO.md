# KVT Storage Engine - TODO List

## Critical Issues (Priority 1)
*These issues affect core functionality and must be fixed for production use*

### Index Operations - Core Functionality Missing
- [ ] Implement `index_read()` - Currently TODO based on find_flag
  - File: `ha_kvt.cc:1031-1033`
  - Impact: Falls back to table scan
  
- [ ] Implement `index_next()` - TODO index traversal
  - File: `ha_kvt.cc` 
  - Impact: Sequential index reads not working
  
- [ ] Implement `index_prev()` - TODO reverse index traversal
  - File: `ha_kvt.cc`
  - Impact: Reverse index scans not working
  
- [ ] Implement `index_last()` - TODO position at end
  - File: `ha_kvt.cc`
  - Impact: Cannot position at end of index
  
- [ ] Implement `delete_all_rows()` - TODO requires scan
  - File: `kvt_index_manager.cc`
  - Impact: TRUNCATE TABLE performance

### Query Optimizer - Hardcoded Values
- [ ] Fix memory pressure calculation - Currently returns fixed 0.5
  - File: `kvt_query_optimizer.cc`
  - Current: `return 0.5; // TODO: Calculate actual memory pressure`
  
- [ ] Fix selectivity estimation - Currently fixed 10% for ranges
  - File: `kvt_query_optimizer.cc`
  - Current: `return 0.1; // TODO: Calculate actual selectivity`
  
- [ ] Fix memory usage calculation - Currently returns 100MB
  - File: `kvt_query_optimizer.cc`
  - Current: `return 100 * 1024 * 1024; // TODO: Get actual memory usage`
  
- [ ] Fix sample size multiplier - Currently uses placeholder 10x
  - File: `kvt_statistics.cc:669`
  - Current: `stats.row_count = sample_size * 10; // Placeholder multiplier`

## High Priority Issues (Priority 2)
*These issues significantly impact functionality*

### Foreign Key Enforcement
- [ ] Enable foreign key constraint checking
  - File: `kvt_foreign_key.cc`
  - Current: Returns false to allow all operations
  - Need: Proper constraint validation on INSERT/UPDATE/DELETE

### Spatial Index - R-tree Operations
- [ ] Implement higher level tree splits
  - File: `kvt_spatial_adapter.cc`
  - Comment: "TODO: Handle splits at higher levels"
  
- [ ] Implement tree condensation after deletion
  - File: `kvt_spatial_adapter.cc`
  - Comment: "TODO: Implement tree condensation"
  
- [ ] Implement STR bulk loading
  - File: `kvt_spatial_adapter.cc`
  - Comment: "TODO: Implement STR (Sort-Tile-Recursive) bulk loading"
  
- [ ] Implement index validation
  - File: `kvt_spatial_adapter.cc`
  - Comment: "TODO: Implement index validation"

### Transaction Management
- [ ] Implement actual savepoint API calls
  - File: `kvt_transaction_manager.cc:244-245`
  - Current: `// TODO: Call KVT savepoint API when available`
  - Functions: `savepoint_set()`, `savepoint_rollback()`, `savepoint_release()`
  
- [ ] Implement name-based savepoint lookup
  - File: `kvt_transaction_manager.cc:323`
  - Current: `// TODO: Implement proper name-based savepoint lookup`

## Medium Priority Issues (Priority 3)
*These issues affect correctness and completeness*

### API Implementations
- [ ] Implement row ID extraction from key
  - File: `kvt_row_codec.cc`
  - Current: `// TODO: Extract actual row_id from the key`
  
- [ ] Enable cardinality updates
  - File: `kvt_statistics.cc`
  - Current: Commented out with TODO
  
- [ ] Implement collation support for string fields
  - File: `kvt_row_codec.cc`
  - Current: `// TODO: Handle collation for string comparison`

### Error Handling
- [ ] Determine rollback policy for index errors
  - File: `ha_kvt.cc`
  - Current: `// TODO: Should we rollback the data row write here?`
  
- [ ] Establish consistent error handling patterns
  - Multiple files affected
  - Need: Document and implement error recovery strategy

### Index Manager
- [ ] Implement batch index operations
  - File: `kvt_index_manager.cc`
  - Current: Individual operations in loops
  
- [ ] Add index statistics collection
  - File: `kvt_index_manager.cc`
  - Current: No statistics gathered during index operations

## Low Priority Issues (Priority 4)
*Code quality and cleanup tasks*

### Remove Development Artifacts
- [ ] Replace field indexes used as "temporary IDs"
  - File: `kvt_alter_table.cc:206`
  - Current: `op.column_id = i; // Use field index as temporary ID`
  
- [ ] Remove placeholder comments
  - Search for: `// TODO`, `// FIXME`, `// XXX`, `// HACK`
  - Count: 29 occurrences across 9 files
  
- [ ] Remove unused variables and functions
  - Multiple files need cleanup

### Build System Improvements
- [ ] Fix KVT memory implementation compilation
  - File: `CMakeLists.txt:47-48`
  - Current: Hardcoded `g++ -c -fPIC -g -O0`
  - Need: Use CMake's compiler detection and flags
  
- [ ] Add proper dependency management
  - Current: Manual dependency on kvt_memory.o
  - Need: Integrate with CMake target system

### Documentation
- [ ] Document ALTER TABLE limitations
  - File: `kvt_alter_table.cc:581`
  - Current: `// For now, use altered_table as placeholder`
  
- [ ] Complete API documentation
  - Multiple header files lack proper documentation
  
- [ ] Add performance tuning guide
  - Document optimizer hints and configuration

## Completed Items ✓
*Recently completed fixes*

- [x] Add singleton cleanup functions - Prevents memory leaks
- [x] Document savepoint limitations - Added KNOWN_LIMITATIONS.md
- [x] Remove full-text search - Removed problematic implementation

## Notes

### File References
- Main handler: `ha_kvt.cc`
- Transaction manager: `kvt_transaction_manager.cc`
- Index manager: `kvt_index_manager.cc`
- Query optimizer: `kvt_query_optimizer.cc`
- Foreign keys: `kvt_foreign_key.cc`
- Spatial index: `kvt_spatial_adapter.cc`
- Statistics: `kvt_statistics.cc`
- ALTER TABLE: `kvt_alter_table.cc`

### Testing Requirements
Each fix should include:
1. Unit tests where applicable
2. MTR (MySQL Test Run) test cases
3. Performance benchmarks for optimizer changes
4. Memory leak detection with Valgrind

### Estimated Effort
- Priority 1: 2-3 weeks (critical for functionality)
- Priority 2: 2-3 weeks (important features)
- Priority 3: 1-2 weeks (correctness)
- Priority 4: 1 week (cleanup)

Total estimated effort: 6-9 weeks for complete resolution

## Contributors
Add your name when taking on a task:
- Task: [Your Name] - [Date Started]

---
Last Updated: 2025-01-07
Version: 1.0.0