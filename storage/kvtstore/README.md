# KVT Storage Engine for MariaDB

## Overview
KVT (Key-Value Transaction) is a pluggable storage engine for MariaDB that leverages a powerful transactional key-value store backend. The KVT backend handles all complex operations including concurrency control, WAL, caching, visibility, and ACID transactions, while MariaDB provides the SQL query processing frontend.

## Current Status
**Phase 2: Table Management** - COMPLETED (2025-01-06)

### Completed Features (Phase 2)
- ✅ Complete catalog system with secure namespacing
- ✅ Multi-database support with prefix-based organization
- ✅ Table metadata storage and retrieval
- ✅ Row serialization/deserialization (kvt_row_codec)
- ✅ Basic DML operations (INSERT/SELECT/UPDATE/DELETE)
- ✅ Full table scans with range queries
- ✅ Auto-increment support
- ✅ NULL value handling
- ✅ Position-based row access

### Completed Features (Phase 1)
- ✅ Basic plugin structure (ha_kvt.cc, ha_kvt.h)
- ✅ Handlerton initialization and registration
- ✅ KVT system initialization and shutdown
- ✅ Basic handler class skeleton
- ✅ CMakeLists.txt for building
- ✅ Basic MTR tests
- ✅ Plugin loads and unloads correctly

### Known Limitations (Current)
- No transaction support (uses one-shot operations)
- No index operations beyond table scans
- No JOIN optimizations
- No partitioning support

## Building

### Prerequisites
- MariaDB source code
- C++ compiler with C++11 support
- CMake 3.0+
- KVT library (kvt_inc.h interface)

### Build Instructions
```bash
# From MariaDB source root
cmake . -DCMAKE_BUILD_TYPE=Debug -DWITH_DEBUG=1
make kvt

# Or build everything
make -j$(nproc)
```

## Installation

### Loading the Plugin
```sql
INSTALL SONAME 'ha_kvt';

-- Verify installation
SHOW ENGINES LIKE 'KVT';
SELECT * FROM INFORMATION_SCHEMA.ENGINES WHERE ENGINE='KVT';
```

### Unloading the Plugin
```sql
UNINSTALL SONAME 'ha_kvt';
```

## Usage (Phase 1)

### Create a Table
```sql
CREATE TABLE test_table (
  id INT PRIMARY KEY,
  name VARCHAR(100),
  value INT
) ENGINE=KVT;
```

### Drop a Table
```sql
DROP TABLE test_table;
```

## Testing

### Run KVT Test Suite
```bash
cd mysql-test
./mariadb-test-run.pl --suite=kvtstore
```

### Run Specific Test
```bash
./mariadb-test-run.pl kvtstore.kvt_basic
./mariadb-test-run.pl kvtstore.kvt_plugin_load
```

## Architecture

### Components
- **ha_kvt.h/cc**: Main storage engine handler implementation
- **kvt/kvt_inc.h**: KVT backend interface
- **kvt/kvt_mem.cpp**: In-memory KVT implementation for testing

### Key Design Decisions
1. **Transaction Management**: Delegated entirely to KVT backend
2. **Key Format**: table_id + primary_key (or internal rowid)
3. **Partition Method**: Hash for non-indexed tables, Range for indexed tables
4. **Serialization**: Custom row format for storing MariaDB rows as KVT values

## Development Roadmap

### Phase 2: Table Management (Next)
- Full table metadata handling
- Table discovery and persistence
- Schema management

### Phase 3: Basic DML Operations
- INSERT/SELECT/UPDATE/DELETE
- Row serialization/deserialization
- Table scans

### Phase 4: Transaction Support
- Full ACID transactions
- Isolation levels
- Commit/Rollback

### Phase 5: Index Support
- Primary key indexes
- Secondary indexes
- Index scans

See [PLAN.md](PLAN.md) for complete roadmap.

## Debugging

### Enable Debug Output
```sql
-- Set KVT verbosity (0-3)
-- 0: none, 1: warnings, 2: info, 3: detailed
SET GLOBAL kvt_verbosity = 3;

-- Set sanity check level (0-3)
SET GLOBAL kvt_sanity_check = 2;
```

### Debug Build
```bash
cmake . -DCMAKE_BUILD_TYPE=Debug -DWITH_DEBUG=1
make kvt
```

### GDB Debugging
```bash
gdb sql/mariadbd
(gdb) run --defaults-file=/path/to/my.cnf
```

## Contributing
Please refer to [PLAN.md](PLAN.md) for the implementation plan and current development status.

## License
GPL v2 (same as MariaDB)

## Contact
KVT Development Team