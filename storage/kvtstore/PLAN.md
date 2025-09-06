# KVT Storage Engine Implementation Plan for MariaDB

## Project Overview
Integrate KVT (Key-Value Transaction) storage engine into MariaDB as a pluggable storage engine. The KVT backend handles all transaction management, concurrency control, WAL, caching, and ACID properties. MariaDB serves as the SQL frontend for query processing.

## Phase 1: Foundation and Basic Infrastructure
### Objectives
- Establish the basic storage engine plugin structure
- Initialize KVT system integration
- Create handler class skeleton

### Tasks
1. **Create basic plugin structure** (storage/kvtstore/ha_kvt.cc, ha_kvt.h)
   - Implement handlerton initialization
   - Register storage engine with MariaDB
   - Set up basic table flags and capabilities
   
2. **KVT system initialization**
   - Initialize KVT on plugin load
   - Shutdown KVT on plugin unload
   - Handle verbosity and sanity check configuration
   
3. **Basic handler class implementation**
   - Create ha_kvt class inheriting from handler
   - Implement constructor/destructor
   - Add basic metadata methods (table_flags, index_flags)

### Tests
- Unit test: Plugin loads and unloads correctly
- MTR test: CREATE TABLE ... ENGINE=KVT (should fail gracefully)
- Verify KVT initialization/shutdown in server logs

### Documentation Update
- Create storage/kvtstore/README.md with basic structure
- Document plugin architecture decisions
- Update phase completion status

---

## Phase 2: Table Management
### Objectives
- Implement table creation, opening, and deletion
- Map MariaDB tables to KVT key spaces
- Handle table metadata

### Tasks
1. **Table creation (CREATE TABLE)**
   - Map MariaDB table to KVT table
   - Store table metadata (columns, types, indexes)
   - Choose appropriate KVT partition method (hash/range)
   
2. **Table opening/closing**
   - Implement ha_kvt::open() and ha_kvt::close()
   - Manage KVT table handles
   - Cache table metadata
   
3. **Table deletion (DROP TABLE)**
   - Implement ha_kvt::delete_table()
   - Clean up KVT tables and metadata
   
4. **Table discovery**
   - Implement table existence checks
   - Support SHOW TABLES

### Tests
- MTR test: CREATE/DROP TABLE operations
- MTR test: Multiple tables with different schemas
- MTR test: Table persistence across server restarts
- Stress test: Concurrent table operations

### Documentation Update
- Document table metadata storage format
- Document KVT table naming conventions
- Update implementation progress

---

## Phase 3: Basic DML Operations (No Transactions)
### Objectives
- Implement basic INSERT, SELECT, UPDATE, DELETE
- Use KVT one-shot operations (tx_id = 0)
- Handle data serialization/deserialization

### Tasks
1. **Row format and serialization**
   - Design key format (table_id + primary key or internal rowid)
   - Implement value serialization (encode all columns)
   - Handle NULL values and default values
   
2. **INSERT operations**
   - Implement ha_kvt::write_row()
   - Generate keys (primary key or auto-increment rowid)
   - Serialize row data to KVT value
   
3. **Table scans (SELECT without WHERE)**
   - Implement ha_kvt::rnd_init(), rnd_next(), rnd_end()
   - Use kvt_scan() for full table scans
   - Deserialize KVT values to MariaDB rows
   
4. **Point queries (SELECT with primary key)**
   - Implement ha_kvt::rnd_pos()
   - Use kvt_get() for direct key access
   
5. **UPDATE operations**
   - Implement ha_kvt::update_row()
   - Handle primary key changes
   
6. **DELETE operations**
   - Implement ha_kvt::delete_row()
   - Use kvt_del() to remove records

### Tests
- MTR test: Basic INSERT/SELECT/UPDATE/DELETE
- MTR test: NULL handling and default values
- MTR test: Large data operations (BLOBs, TEXT)
- Performance test: Bulk insert operations
- Correctness test: Data integrity verification

### Documentation Update
- Document row serialization format
- Document key generation strategy
- Update feature matrix

---

## Phase 4: Transaction Support
### Objectives
- Implement full ACID transaction support
- Integrate with MariaDB transaction coordinator
- Handle commit/rollback

### Tasks
1. **Transaction initialization**
   - Implement ha_kvt::start_stmt()
   - Map THD to KVT transaction ID
   - Implement external_lock() for transaction boundaries
   
2. **Transaction commit/rollback**
   - Implement ha_kvt::commit()
   - Implement ha_kvt::rollback()
   - Handle savepoints
   
3. **Transaction isolation**
   - Map MariaDB isolation levels to KVT behavior
   - Handle read consistency
   
4. **Multi-statement transactions**
   - Track transaction state across statements
   - Handle autocommit mode

### Tests
- MTR test: Basic transaction commit/rollback
- MTR test: Concurrent transactions (isolation)
- MTR test: Deadlock detection and resolution
- MTR test: Long-running transactions
- Stress test: High concurrency transaction workload

### Documentation Update
- Document transaction mapping strategy
- Document isolation level handling
- Update compatibility matrix

---

## Phase 5: Index Support
### Objectives
- Implement primary key indexes
- Support secondary indexes
- Enable index-based access paths

### Tasks
1. **Primary key indexes**
   - Map primary keys to KVT key structure
   - Implement index read methods (index_read, index_next)
   - Support unique constraint enforcement
   
2. **Secondary indexes**
   - Design secondary index storage in KVT
   - Implement index maintenance on DML
   - Handle non-unique indexes
   
3. **Index scans**
   - Implement index range scans
   - Support ORDER BY optimization
   - Handle index condition pushdown (ICP)
   
4. **Index statistics**
   - Collect and maintain index statistics
   - Implement ha_kvt::info() for optimizer

### Tests
- MTR test: Primary key operations
- MTR test: Secondary index CRUD
- MTR test: Index-based ORDER BY
- MTR test: Composite indexes
- Performance test: Index scan vs table scan

### Documentation Update
- Document index storage format
- Document index maintenance strategy
- Update performance characteristics

---

## Phase 6: Advanced Query Optimization
### Objectives
- Leverage KVT's computation pushdown capabilities
- Optimize complex queries
- Implement advanced scan features

### Tasks
1. **Condition pushdown**
   - Use KVT's update functions for filtering
   - Implement WHERE clause pushdown
   - Optimize range queries with kvt_range_update()
   
2. **Column projection**
   - Implement partial column reads
   - Use KVT functions for field extraction
   - Reduce deserialization overhead
   
3. **Batch operations**
   - Implement multi-row operations using kvt_batch_execute()
   - Optimize bulk inserts/updates
   - Implement read_multi_range optimization
   
4. **Join optimization**
   - Implement Block Nested Loop join support
   - Support Index Nested Loop joins
   - Optimize join buffer usage

### Tests
- MTR test: Complex WHERE conditions
- MTR test: Partial column reads
- MTR test: Batch operation correctness
- Performance test: Query optimization effectiveness
- Benchmark: TPC-H query performance

### Documentation Update
- Document pushdown capabilities
- Document optimization strategies
- Update performance tuning guide

---

## Phase 7: Advanced Features
### Objectives
- Support advanced MariaDB features
- Implement engine-specific optimizations
- Handle special table types

### Tasks
1. **Partitioning support**
   - Map MariaDB partitions to KVT tables
   - Implement partition pruning
   - Handle partition maintenance operations
   
2. **Foreign keys**
   - Implement foreign key constraints
   - Handle cascade operations
   - Ensure referential integrity
   
3. **Full-text search**
   - Design FTS index in KVT
   - Implement MATCH...AGAINST queries
   - Handle FTS index maintenance
   
4. **Spatial indexes**
   - Support geometry types
   - Implement R-tree indexes in KVT
   - Handle spatial queries

### Tests
- MTR test: Partitioned table operations
- MTR test: Foreign key constraints
- MTR test: Full-text search queries
- MTR test: Spatial data operations
- Integration test: Mixed feature usage

### Documentation Update
- Document advanced feature support
- Document limitations and workarounds
- Update feature compatibility matrix

---

## Phase 8: Performance and Reliability
### Objectives
- Optimize performance-critical paths
- Ensure reliability and error handling
- Implement monitoring and diagnostics

### Tasks
1. **Performance optimization**
   - Profile and optimize hot paths
   - Implement caching strategies
   - Optimize memory usage
   
2. **Error handling**
   - Map KVT errors to MySQL errors
   - Implement retry logic for transient failures
   - Add comprehensive error logging
   
3. **Monitoring and diagnostics**
   - Implement INFORMATION_SCHEMA tables
   - Add performance_schema instrumentation
   - Create status variables
   
4. **Crash recovery**
   - Ensure proper cleanup on crash
   - Verify transaction consistency
   - Test with kill -9 scenarios

### Tests
- Performance test: Sysbench OLTP workload
- Stress test: Concurrent workload under memory pressure
- Reliability test: Random kill testing
- Long-running test: 24-hour stress test
- Benchmark: Compare with InnoDB/RocksDB

### Documentation Update
- Document performance characteristics
- Document monitoring best practices
- Create troubleshooting guide

---

## Phase 9: Integration and Compatibility
### Objectives
- Ensure full MariaDB ecosystem compatibility
- Support backup/restore operations
- Enable replication

### Tasks
1. **Backup and restore**
   - Implement mariabackup support
   - Support mysqldump
   - Handle hot backup scenarios
   
2. **Replication support**
   - Implement binlog integration
   - Support row-based replication
   - Handle parallel replication
   
3. **Migration tools**
   - Create InnoDB to KVT migration tool
   - Support online schema changes
   - Implement ALTER TABLE operations
   
4. **Ecosystem integration**
   - MaxScale compatibility
   - Galera cluster support (if applicable)
   - Third-party tool compatibility

### Tests
- Integration test: Backup and restore
- Integration test: Master-slave replication
- Integration test: Schema migration
- Compatibility test: Major MariaDB tools
- Upgrade test: Version compatibility

### Documentation Update
- Document backup procedures
- Document replication setup
- Create migration guide
- Update compatibility matrix

---

## Phase 10: Production Readiness
### Objectives
- Complete production hardening
- Comprehensive testing
- Final documentation

### Tasks
1. **Security hardening**
   - Security audit
   - Implement encryption at rest (if needed)
   - Handle sensitive data properly
   
2. **Final testing**
   - Full regression test suite
   - Customer workload testing
   - Edge case testing
   
3. **Documentation completion**
   - User manual
   - Administrator guide
   - API documentation
   - Performance tuning guide
   
4. **Release preparation**
   - Code review completion
   - Performance benchmarks
   - Known issues documentation
   - Release notes

### Tests
- Security test: Penetration testing
- Regression test: Full test suite
- Acceptance test: Customer scenarios
- Load test: Production workload simulation
- Certification: MariaDB compatibility

### Documentation Update
- Finalize all documentation
- Create quick start guide
- Publish benchmarks
- Release announcement

---

## Testing Strategy

### Unit Tests
- Located in storage/kvtstore/tests/
- Test individual components in isolation
- Run with: make test

### MTR Tests
- Located in mysql-test/suite/kvtstore/
- Test end-to-end functionality
- Run with: ./mariadb-test-run.pl --suite=kvtstore

### Performance Tests
- Sysbench benchmarks
- TPC-H/TPC-C workloads
- Custom KVT-specific benchmarks

### Stress Tests
- Concurrent operations
- Memory pressure scenarios
- Long-running stability tests

### Integration Tests
- Backup/restore operations
- Replication scenarios
- Tool compatibility

---

## Documentation Structure

```
storage/kvtstore/
├── README.md                 # Overview and quick start
├── IMPLEMENTATION_PLAN.md    # This document
├── docs/
│   ├── architecture.md      # Technical architecture
│   ├── api.md               # API documentation
│   ├── performance.md       # Performance guide
│   ├── troubleshooting.md  # Problem resolution
│   └── migration.md         # Migration guide
├── tests/                   # Test files
└── benchmarks/             # Benchmark results
```

---

## Risk Management

### Technical Risks
1. **Performance gaps vs. InnoDB**
   - Mitigation: Early benchmarking and optimization
   - Contingency: Focus on specific workload advantages

2. **Transaction semantics mismatch**
   - Mitigation: Thorough testing of isolation levels
   - Contingency: Document limitations clearly

3. **Memory management issues**
   - Mitigation: Careful resource tracking
   - Contingency: Implement memory limits

### Schedule Risks
1. **KVT API limitations discovered**
   - Mitigation: Early prototype validation
   - Contingency: Request KVT enhancements

2. **MariaDB version compatibility**
   - Mitigation: Test on multiple versions
   - Contingency: Target specific version initially

---

## Success Criteria

### Functional
- All basic SQL operations work correctly
- Transaction ACID properties maintained
- Compatible with major MariaDB features

### Performance
- Within 2x of InnoDB for OLTP workloads
- Better than InnoDB for specific KVT-optimized workloads
- Acceptable memory and disk usage

### Quality
- Pass all regression tests
- No critical bugs in production workloads
- Comprehensive documentation

### Adoption
- Easy migration from existing engines
- Good tool ecosystem support
- Positive user feedback

---

## Timeline Estimate

- Phase 1: 1 week
- Phase 2: 2 weeks
- Phase 3: 3 weeks
- Phase 4: 3 weeks
- Phase 5: 4 weeks
- Phase 6: 3 weeks
- Phase 7: 4 weeks
- Phase 8: 3 weeks
- Phase 9: 3 weeks
- Phase 10: 2 weeks

**Total: ~28 weeks (7 months)**

Note: Timeline assumes one developer. Can be parallelized with multiple developers working on different phases.

---

## Current Status

**Phase**: Phase 2 COMPLETED ✅
**Last Updated**: 2025-01-06
**Next Steps**: Begin Phase 3 (Basic DML Operations) or Phase 4 (Transaction Support)

### Phase 2 Accomplishments
- ✅ Implemented complete catalog system (kvt_catalog.h/cc)
- ✅ Created secure key namespace design with NULL separators
- ✅ Multi-database support with __DATA_ prefix convention
- ✅ Row serialization/deserialization codec (kvt_row_codec.h/cc)
- ✅ Table metadata persistence and discovery
- ✅ Basic CRUD operations (INSERT/SELECT/UPDATE/DELETE)
- ✅ Full table scans using KVT range operations
- ✅ Auto-increment sequence management
- ✅ Proper error handling and KVT error mapping

### Phase 2 Technical Notes
- Used prefix-based key organization for efficient range scans
- Implemented security through __ prefix for system tables
- NULL character (0x00) as separator prevents injection attacks
- Simple key=value serialization instead of JSON dependency
- Each database gets its own KVT table (__DATA_<dbname>)
- Catalog stored in __CATALOG__ KVT table

### Phase 1 Accomplishments
- ✅ Created plugin structure (ha_kvt.cc, ha_kvt.h)
- ✅ Implemented handlerton initialization
- ✅ KVT system initialization/shutdown working
- ✅ Basic handler class with table flags
- ✅ CMakeLists.txt configured and building
- ✅ Plugin loads/unloads successfully
- ✅ Basic CREATE/DROP TABLE operations
- ✅ MTR test suite created
- ✅ Documentation updated