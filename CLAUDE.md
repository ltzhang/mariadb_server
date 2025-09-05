# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Standard build (out-of-source recommended)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DWITH_DEBUG=1
make -j$(nproc)

# In-source build
cmake . -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

## Test Commands

```bash
# Run MySQL Test Run (MTR) framework
cd mysql-test
./mariadb-test-run.pl                      # Run default test suite
./mariadb-test-run.pl test_name            # Run specific test
./mariadb-test-run.pl suite_name.test_name # Run test from specific suite
./mariadb-test-run.pl --record test_name   # Create/update result file
./mariadb-test-run.pl --debug test_name    # Run with debug server
./mariadb-test-run.pl --parallel=4         # Run tests in parallel

# Unit tests
cd build
make test                                   # Run unit tests
ctest -V                                    # Verbose unit test output
```

## Code Architecture

### Core Directory Structure
- **sql/** - Main SQL server implementation (parser, optimizer, executor)
  - `sql_parse.cc` - SQL statement parsing entry point
  - `sql_select.cc` - SELECT statement execution
  - `handler.cc/h` - Storage engine interface
  - `item_*.cc` - Expression and function implementations
  
- **storage/** - Pluggable storage engines
  - `innobase/` - InnoDB (default transactional engine)
  - `maria/` - Aria engine
  - `rocksdb/` - RocksDB integration
  - Each engine implements the `handler` interface defined in sql/handler.h

- **mysys/** - System utilities and OS abstraction layer
- **include/** - Public header files
- **plugin/** - Server plugins (authentication, audit, etc.)

### Key Architectural Concepts

**Storage Engine Interface**: All storage engines implement the `handler` class interface. Key methods include:
- `ha_open()`, `ha_close()` - Table open/close
- `ha_rnd_next()`, `ha_index_read()` - Row retrieval
- `ha_write_row()`, `ha_update_row()`, `ha_delete_row()` - DML operations

**SQL Execution Flow**:
1. Parser (`sql_yacc.yy`) → Parse tree
2. Optimizer (`sql_optimizer.cc`) → Execution plan
3. Executor → Calls storage engine handlers
4. Results returned to client

**Thread Handling**: Each client connection gets a THD (Thread Descriptor) object that maintains connection state throughout query execution.

## Coding Standards

- **Style**: Allman braces, 2-space indentation (no tabs)
- **Line length**: Maximum 80 characters
- **Files**: `.cc` for C++, `.c` for C, `.h` for headers
- **Naming**: snake_case for functions/variables, CapitalCase for classes
- **Comments**: `//` for single-line, `/* */` for multi-line

## Debugging

```bash
# Run server under debugger
gdb sql/mariadbd
(gdb) run --defaults-file=/path/to/my.cnf

# Debug MTR test
cd mysql-test
./mariadb-test-run.pl --debug --gdb test_name
```

### How to Add Storage Engine Feature
1. Extend `handler` class interface if needed
2. Implement in specific engine (e.g., `storage/innobase/`)
3. Add tests in `mysql-test/suite/engines/`

## Project Overview

This project is to integrate KVT (Key-Value Transaction) storage to MariaDB under storage/kvtstore directory. The project aims to add a backend storage with KVT, which handles concurrnecy control, WAL, caching, visibility, and transactions. MariaDB acts as a frontend to the KVT storage backend. The KVT engine's interface is defined by storage/kvtstore/kvt/kvt_inc.h. We will put all relevant files under the storage/kvtstore directory, including documents, plans, records, and code files. 

- In this effort, you only need to care about src/backend/kvt_am/kvt/kvt_inc.h file, which is the interface for the C++ transactional key-value store.

- You should assume kvt_inc.h can interface with very powerful, transactional key value stores. Do not assume any limitations (such as durability, scalability, ACID, and so on). Assume the store has all the capabilities. 

- To link a sample kvt store, you just link storage/kvtstore/kvt/kvt_memory.o, kvt_memory.o (which is produced with "g++ -c -fPIC -g -O0 kvt_memory.cpp") implements a in-memory version for test purposes. But you should not concern with its limitations, since other implementations have different capabilities. Stick to the interface.

- In this project, we will leave the complexities of concurrency control, caching, locking, WAL, free space management, vacuum, ACID to KVT, assume KVT take care of them. We only use MySQL for query processing, and assume kvt handles all transaction related complexities. Again, kvt_inc.h is the definitive interface and kvt_mem.h/kvt_mem.cpp is just for testing purposes. 

## Working with KVT

The KVT library interface is defined in `src/backend/access/kvt_am/kvt/kvt_inc.h`. Key functions include:
- kvt_init() - Initialize KVT system
- kvt_set() - Store key-value pair
- kvt_get() - Retrieve value by key
- kvt_scan() - Scan range of keys
- kvt_delete() - Delete key-value pair
- Advanced update functions for computation push down. These can be used for, e.g. conditioanl scan and filtering, field/column access within a tuple without fully deserialize, and so on to optimize performance. 
- Transaction management functions
- Table management functions (i.e. key spaces)



