# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Standard build (out-of-source recommended)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# In-source build
cmake . -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# Common build configurations
cmake . -DCMAKE_BUILD_TYPE=RelWithDebInfo  # Default, with debug symbols
cmake . -DCMAKE_BUILD_TYPE=Release         # Optimized release build
cmake . -DWITH_EMBEDDED_SERVER=1           # Include embedded server
cmake . -DWITH_WSREP=ON                    # Enable Galera/WSREP support
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

## Development Workflow

- **Branches**: Feature development on `main`, bug fixes on earliest affected branch
- **Commit messages**: Start with `MDEV-#####` JIRA ticket number
- **Testing**: Always run relevant MTR tests before committing
- **Code formatting**: Use `.clang-format` configuration in repository

## Debugging

```bash
# Build with debug symbols
cmake . -DCMAKE_BUILD_TYPE=Debug -DWITH_DEBUG=1

# Run server under debugger
gdb sql/mariadbd
(gdb) run --defaults-file=/path/to/my.cnf

# Debug MTR test
cd mysql-test
./mariadb-test-run.pl --debug --gdb test_name
```

## Common Development Tasks

### Adding a New System Variable
1. Define in `sql/sys_vars.cc`
2. Add to `sql/mysqld.h` or appropriate header
3. Document in `mysql-test/suite/sys_vars/`

### Modifying SQL Parser
1. Edit `sql/sql_yacc.yy` (Bison grammar)
2. Run `bison` to regenerate parser
3. Update `sql/lex.h` for new keywords

### Adding Storage Engine Feature
1. Extend `handler` class interface if needed
2. Implement in specific engine (e.g., `storage/innobase/`)
3. Add tests in `mysql-test/suite/engines/`