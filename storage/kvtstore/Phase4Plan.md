# Phase 4: Transaction Support - Implementation Plan

## Overview
Phase 4 will implement full ACID transaction support for the KVT storage engine, integrating with MariaDB's transaction coordinator. This will replace the current one-shot operations (tx_id=0) with proper transaction management, enabling multi-statement transactions, isolation levels, and rollback capabilities.

## Current State Assessment

### What We Have:
- ✅ Basic DML operations (INSERT/SELECT/UPDATE/DELETE)
- ✅ All operations use tx_id=0 (one-shot/auto-commit)
- ✅ No isolation between concurrent operations
- ✅ No rollback capability
- ✅ Basic external_lock() stub implementation

### What We Need:
- ❌ Transaction lifecycle management (BEGIN/COMMIT/ROLLBACK)
- ❌ Mapping THD to KVT transaction IDs
- ❌ Isolation level support (READ UNCOMMITTED/COMMITTED/REPEATABLE READ/SERIALIZABLE)
- ❌ Savepoint support
- ❌ Two-phase commit for XA transactions
- ❌ Deadlock detection and resolution
- ❌ Transaction state tracking

## Architecture Design

### 1. Transaction ID Management

#### Problem:
- Each THD (Thread Descriptor) represents a client connection
- Multiple statements can run within a transaction
- Need to map THD to KVT transaction ID consistently

#### Solution:
```cpp
class TransactionManager {
    // Thread-local storage for transaction mapping
    std::unordered_map<THD*, uint64_t> thd_to_tx_map;
    std::mutex tx_map_mutex;
    
    // Transaction state tracking
    struct TxState {
        uint64_t tx_id;
        bool is_active;
        bool is_autocommit;
        int isolation_level;
        std::vector<uint64_t> savepoints;
    };
    std::unordered_map<THD*, TxState> tx_states;
};
```

### 2. Transaction Lifecycle

#### Transaction Start:
1. **external_lock(F_RDLCK/F_WRLCK)** - Called at statement start
   - Check if transaction exists for THD
   - If not, call `kvt_start_transaction()`
   - Store tx_id in TransactionManager

2. **start_stmt()** - Called for each statement
   - Verify transaction is active
   - Set up statement-level resources

#### Transaction End:
1. **external_lock(F_UNLCK)** - Called at statement end
   - For autocommit: commit immediately
   - For explicit transaction: keep active

2. **commit()/rollback()** - Explicit transaction control
   - Call `kvt_commit_transaction()` or `kvt_rollback_transaction()`
   - Clean up transaction mapping

### 3. Isolation Level Mapping

MariaDB isolation levels → KVT behavior:

| MariaDB Level | KVT Implementation | Description |
|--------------|-------------------|-------------|
| READ UNCOMMITTED | tx_id with no locks | Dirty reads allowed |
| READ COMMITTED | tx_id with statement-level consistency | Each statement sees committed data |
| REPEATABLE READ | tx_id with transaction-level snapshot | Consistent reads within transaction |
| SERIALIZABLE | tx_id with full locking | No concurrent modifications |

### 4. Handler Method Implementation

#### Key Methods to Implement:

```cpp
// Transaction control
int ha_kvt::start_stmt(THD *thd, thr_lock_type lock_type);
int ha_kvt::external_lock(THD *thd, int lock_type);
int ha_kvt::commit(handlerton *hton, THD *thd, bool all);
int ha_kvt::rollback(handlerton *hton, THD *thd, bool all);

// Savepoints
int ha_kvt::savepoint_set(handlerton *hton, THD *thd, void *sv);
int ha_kvt::savepoint_rollback(handlerton *hton, THD *thd, void *sv);
int ha_kvt::savepoint_release(handlerton *hton, THD *thd, void *sv);

// XA transactions (optional for Phase 4)
int ha_kvt::xa_prepare(handlerton *hton, THD *thd, bool all);
int ha_kvt::xa_commit(handlerton *hton, XID *xid);
int ha_kvt::xa_rollback(handlerton *hton, XID *xid);
```

## Implementation Plan

### Step 1: Transaction Manager (Week 1, Days 1-2)

#### 1.1 Create Transaction Manager Class
```cpp
// kvt_transaction_manager.h
class KVTTransactionManager {
public:
    static KVTTransactionManager* get_instance();
    
    // Transaction lifecycle
    uint64_t begin_transaction(THD* thd, int isolation_level);
    int commit_transaction(THD* thd);
    int rollback_transaction(THD* thd);
    
    // Transaction state
    uint64_t get_transaction_id(THD* thd);
    bool has_active_transaction(THD* thd);
    
    // Cleanup
    void cleanup_transaction(THD* thd);
    
private:
    std::unordered_map<THD*, TxState> transactions;
    std::mutex mutex;
};
```

#### 1.2 Integrate with ha_kvt
- Update all DML operations to use transaction IDs
- Replace tx_id=0 with proper transaction IDs
- Add transaction state tracking

### Step 2: Basic Transaction Support (Week 1, Days 3-5)

#### 2.1 Implement external_lock()
```cpp
int ha_kvt::external_lock(THD *thd, int lock_type) {
    if (lock_type != F_UNLCK) {
        // Start or join transaction
        auto* tx_mgr = KVTTransactionManager::get_instance();
        if (!tx_mgr->has_active_transaction(thd)) {
            uint64_t tx_id = tx_mgr->begin_transaction(thd, 
                thd_tx_isolation(thd));
            this->kvt_tx_id = tx_id;
        } else {
            this->kvt_tx_id = tx_mgr->get_transaction_id(thd);
        }
    } else {
        // Statement end - check autocommit
        if (thd_test_options(thd, OPTION_AUTOCOMMIT)) {
            // Commit if autocommit is on
            auto* tx_mgr = KVTTransactionManager::get_instance();
            tx_mgr->commit_transaction(thd);
        }
    }
    return 0;
}
```

#### 2.2 Implement commit/rollback handlers
- Update handlerton commit/rollback functions
- Handle both statement and transaction level
- Clean up resources properly

### Step 3: Isolation Levels (Week 2, Days 1-3)

#### 3.1 Map Isolation Levels
```cpp
int get_kvt_isolation_level(THD* thd) {
    switch(thd_tx_isolation(thd)) {
        case ISO_READ_UNCOMMITTED:
            return KVT_ISO_READ_UNCOMMITTED;
        case ISO_READ_COMMITTED:
            return KVT_ISO_READ_COMMITTED;
        case ISO_REPEATABLE_READ:
            return KVT_ISO_REPEATABLE_READ;
        case ISO_SERIALIZABLE:
            return KVT_ISO_SERIALIZABLE;
    }
}
```

#### 3.2 Test Isolation Behaviors
- Dirty reads
- Non-repeatable reads
- Phantom reads
- Serialization anomalies

### Step 4: Savepoints (Week 2, Days 4-5)

#### 4.1 Implement Savepoint Support
```cpp
struct kvt_savepoint {
    uint64_t savepoint_id;
    char name[64];
};

int ha_kvt::savepoint_set(handlerton *hton, THD *thd, void *sv) {
    kvt_savepoint *savepoint = (kvt_savepoint*)sv;
    // Call KVT savepoint API if available
    // Store savepoint info
}
```

#### 4.2 Test Savepoint Operations
- Nested savepoints
- Rollback to savepoint
- Release savepoint

### Step 5: Advanced Features (Week 3, Days 1-3)

#### 5.1 Deadlock Detection
- Implement timeout handling
- Add deadlock detection if KVT supports it
- Proper error reporting

#### 5.2 Lock Wait Timeout
```cpp
int ha_kvt::lock_wait_timeout_handler(THD *thd) {
    // Handle lock wait timeout
    // Rollback if necessary
    return HA_ERR_LOCK_WAIT_TIMEOUT;
}
```

#### 5.3 Transaction Monitoring
- Add INFORMATION_SCHEMA tables
- Performance schema integration
- Transaction statistics

### Step 6: Testing and Validation (Week 3, Days 4-5)

#### 6.1 Comprehensive Testing
- Multi-statement transactions
- Concurrent transactions
- Isolation level validation
- Crash recovery

#### 6.2 Performance Testing
- Transaction overhead
- Concurrency scalability
- Lock contention

## Test Plan

### 1. Basic Transaction Tests (`kvt_transaction_basic.test`)
```sql
-- Test BEGIN/COMMIT/ROLLBACK
BEGIN;
INSERT INTO t1 VALUES (1, 'test');
COMMIT;

BEGIN;
INSERT INTO t1 VALUES (2, 'rollback');
ROLLBACK;
SELECT * FROM t1;  -- Should not see rolled back row

-- Test autocommit
SET autocommit=0;
INSERT INTO t1 VALUES (3, 'manual');
COMMIT;

SET autocommit=1;
INSERT INTO t1 VALUES (4, 'auto');  -- Auto-committed
```

### 2. Isolation Level Tests (`kvt_isolation_levels.test`)
```sql
-- Test READ UNCOMMITTED (dirty reads)
-- Connection 1:
BEGIN;
UPDATE t1 SET value = 'dirty' WHERE id = 1;
-- Connection 2:
SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED;
SELECT * FROM t1 WHERE id = 1;  -- Should see 'dirty'

-- Test READ COMMITTED
-- Test REPEATABLE READ
-- Test SERIALIZABLE
```

### 3. Concurrent Transaction Tests (`kvt_concurrent_tx.test`)
```sql
-- Test concurrent inserts
-- Test concurrent updates on same row
-- Test deadlock scenarios
-- Test lock wait timeout
```

### 4. Savepoint Tests (`kvt_savepoints.test`)
```sql
BEGIN;
INSERT INTO t1 VALUES (1, 'first');
SAVEPOINT sp1;
INSERT INTO t1 VALUES (2, 'second');
SAVEPOINT sp2;
INSERT INTO t1 VALUES (3, 'third');
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT * FROM t1;  -- Should only see 'first'
```

### 5. Crash Recovery Tests (`kvt_crash_recovery.test`)
```sql
-- Test transaction recovery after crash
-- Test incomplete transaction rollback
-- Test prepared transaction recovery
```

### 6. Performance Tests (`kvt_tx_performance.test`)
```sql
-- Measure transaction overhead
-- Test high concurrency scenarios
-- Benchmark commit/rollback performance
```

## Success Criteria

### Functional Requirements:
1. ✓ Multi-statement transactions work correctly
2. ✓ COMMIT persists changes, ROLLBACK discards them
3. ✓ Autocommit mode functions properly
4. ✓ All isolation levels behave according to SQL standard
5. ✓ Savepoints work correctly
6. ✓ Concurrent transactions don't corrupt data

### Performance Requirements:
1. ✓ Transaction overhead < 10% for single-row operations
2. ✓ Support > 100 concurrent transactions
3. ✓ Commit latency < 1ms for small transactions
4. ✓ Rollback completes in O(1) time

### Quality Requirements:
1. ✓ No transaction leaks
2. ✓ Proper cleanup on connection close
3. ✓ Correct error handling and reporting
4. ✓ All tests pass consistently

## Risk Analysis

### Technical Risks:
1. **KVT Transaction Limitations**
   - Risk: KVT may not support all required features
   - Mitigation: Document limitations, implement workarounds

2. **THD Lifecycle Complexity**
   - Risk: Complex interaction with MariaDB internals
   - Mitigation: Thorough testing, defensive coding

3. **Performance Degradation**
   - Risk: Transaction overhead too high
   - Mitigation: Optimize hot paths, batch operations

### Implementation Risks:
1. **Deadlock Handling**
   - Risk: Difficult to detect and resolve
   - Mitigation: Implement timeout-based resolution first

2. **Crash Recovery**
   - Risk: Data corruption on crash
   - Mitigation: Rely on KVT's recovery mechanisms

## Dependencies

### Required KVT APIs:
- `kvt_start_transaction()`
- `kvt_commit_transaction()`
- `kvt_rollback_transaction()`
- Savepoint APIs (if available)

### MariaDB Internal APIs:
- THD transaction state
- Handlerton callbacks
- Lock management

## Timeline

### Week 1: Foundation
- Days 1-2: Transaction Manager implementation
- Days 3-5: Basic transaction support

### Week 2: Advanced Features
- Days 1-3: Isolation levels
- Days 4-5: Savepoints

### Week 3: Testing and Polish
- Days 1-3: Advanced features (deadlock, monitoring)
- Days 4-5: Comprehensive testing

**Total: 3 weeks**

## Next Steps After Phase 4

Once transaction support is complete, Phase 5 will focus on:
1. Index support for better query performance
2. Primary and secondary indexes
3. Index-based access paths
4. Query optimization with indexes

## Conclusion

Phase 4 will transform the KVT storage engine from a simple storage backend to a fully transactional engine capable of handling complex OLTP workloads. The implementation focuses on correctness first, then optimization, ensuring data integrity while maintaining good performance.