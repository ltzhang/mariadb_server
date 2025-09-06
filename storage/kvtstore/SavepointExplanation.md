# How Savepoints Currently Work in KVT Storage Engine

## Current Implementation Status: **NOT FUNCTIONAL**

The savepoint implementation in KVT is currently just a **stub/placeholder**. It doesn't actually work because the KVT backend doesn't have savepoint support yet.

## What the Code Does Now

### 1. **Savepoint Set** (`kvt_savepoint_set`)
```cpp
// What happens when you create a savepoint:
SAVEPOINT sp1;
```

- Creates a `kvt_savepoint_data` structure with a unique name
- Calls `KVTTransactionManager::savepoint_set()` which:
  - Generates a simple numeric ID (1, 2, 3...)
  - Adds this ID to a vector in `TransactionState.savepoints`
  - **TODO comment shows**: `// TODO: Call KVT savepoint API when available`
  - **Does NOT actually create a savepoint in the storage layer**

### 2. **Savepoint Rollback** (`kvt_savepoint_rollback`)
```cpp
// What happens when you rollback to a savepoint:
ROLLBACK TO SAVEPOINT sp1;
```

- Calls `KVTTransactionManager::savepoint_rollback()` which:
  - Finds the savepoint ID in the vector
  - Removes all savepoint IDs after this one from the vector
  - **TODO comment shows**: `// TODO: Call KVT savepoint rollback API when available`
  - **Does NOT actually rollback any data changes**

### 3. **Savepoint Release** (`kvt_savepoint_release`)
```cpp
// What happens when you release a savepoint:
RELEASE SAVEPOINT sp1;
```

- Calls `KVTTransactionManager::savepoint_release()` which:
  - Finds the savepoint ID
  - Removes it from the vector
  - **TODO comment shows**: `// TODO: Call KVT savepoint release API when available`
  - **Does NOT actually release anything in storage**

## The Problem

### What's Missing:
1. **No KVT Backend Support**: The `kvt_inc.h` interface has no savepoint functions
2. **No Actual Data Tracking**: Changes aren't tracked per savepoint
3. **No Rollback Capability**: Can't actually undo changes to a savepoint
4. **No Name Mapping**: The `find_savepoint()` function just returns the last savepoint, ignoring the name

### Current Code Structure:
```cpp
struct TransactionState {
  uint64_t tx_id;                    // KVT transaction ID
  std::vector<uint64_t> savepoints;  // Just a list of IDs - no actual data
  ...
};

// This function pretends to find a savepoint by name but doesn't
uint64_t find_savepoint(const TransactionState* state, const char* name) {
  // TODO: Implement proper name-based lookup
  return state->savepoints.back();  // Just returns the last one!
}
```

## What Would Be Needed for Real Implementation

### 1. **KVT Backend Support**
The KVT backend (`kvt_inc.h`) would need functions like:
```cpp
KVTError kvt_savepoint_create(uint64_t tx_id, uint64_t* savepoint_id);
KVTError kvt_savepoint_rollback(uint64_t tx_id, uint64_t savepoint_id);
KVTError kvt_savepoint_release(uint64_t tx_id, uint64_t savepoint_id);
```

### 2. **Change Tracking**
The backend would need to:
- Track all changes (inserts, updates, deletes) after each savepoint
- Maintain undo logs for each savepoint
- Be able to reverse changes back to a specific point

### 3. **Proper Name Mapping**
```cpp
struct TransactionState {
  std::map<std::string, uint64_t> savepoint_names;  // Name to ID mapping
  std::vector<SavepointData> savepoints;            // Actual savepoint data
};
```

## Why It Appears to "Work"

The code compiles and doesn't error because:
1. MariaDB's handler interface expects these functions
2. The functions return success (0) even though they don't do anything
3. No actual rollback happens, so data stays as-is
4. Tests might pass if they don't verify actual rollback behavior

## Example of Current Behavior

```sql
BEGIN;
  INSERT INTO t1 VALUES (1);  -- Row is inserted
  SAVEPOINT sp1;              -- ID 1 added to vector, nothing else
  INSERT INTO t1 VALUES (2);  -- Row is inserted  
  ROLLBACK TO SAVEPOINT sp1;  -- ID 1 found, nothing rolled back
  -- Both rows 1 and 2 are still in the table!
COMMIT;
```

**Result**: Both rows are committed even though we "rolled back" to sp1!

## Conclusion

The current savepoint implementation is essentially **non-functional placeholder code**. It maintains a list of savepoint IDs but has no ability to actually:
- Save transaction state at a point
- Rollback changes to that point
- Properly map savepoint names to IDs

This is why it's marked as "Partially implemented - needs testing" in the RemainingIssues.md. In reality, it's more accurately "Not implemented - just has stub functions".