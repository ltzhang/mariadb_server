# KVT Storage Engine Catalog Design

## Design Rationale

### Core Principles

1. **No Control Over KVT Table IDs**: KVT assigns table IDs internally when tables are created. We cannot specify or control these IDs, so we must always look them up by table name using `kvt_get_table_id()`. Never hardcode or assume table IDs as they may change across restarts.

2. **Prefix-Based Organization for Efficient Range Scans**: KVT supports efficient range scans within a table. By using prefixes in our key design, we can leverage this for operations like:
   - Finding all tables in a database: Range scan `mydb:` to `mydb;`
   - Finding all rows in a table: Range scan `table_name:` to `table_name;`
   - Deleting all data for a table: Range delete with same bounds

3. **Security Through Prefixing**: Always use prefixes (not suffixes) to prevent namespace collision and malicious manipulation:
   - **Problem with suffixes**: If user creates `mytable_data` and we suffix with `_data`, it becomes `mytable_data_data`. Another user creating `mytable` would get `mytable_data`, causing namespace collision.
   - **Solution**: Use special prefixes like `__` for system tables that users cannot create.

4. **Separator Character Selection**: Use non-printable characters like NULL (`\x00`) as separators because:
   - Users cannot include these in table/database names
   - Prevents parsing ambiguity
   - Clean range boundaries (e.g., `\x00` + 1 = `\x01`)
   - Avoids collision if user creates names like `mytable:` when using `:` as separator

5. **One KVT Table Per Database**: Each MariaDB database maps to one KVT table containing all its tables' data. This provides:
   - Natural isolation between databases
   - Efficient database-level operations
   - Simplified DROP DATABASE implementation

## Architecture Overview

### KVT Table Structure

```
MariaDB Databases          KVT Tables
-----------------          -----------
[System Catalog]    →      __CATALOG__ (table_id: dynamic)
database1           →      __DATA_database1 (table_id: dynamic)
database2           →      __DATA_database2 (table_id: dynamic)
...
```

### System Tables

1. **`__CATALOG__`**: Global catalog table (created first)
   - Stores all metadata about databases and tables
   - Stores auto-increment sequences
   - Stores system configuration

2. **`__DATA_<database>`**: Per-database data tables
   - One KVT table per MariaDB database
   - Contains all row data for all tables in that database
   - Tables are differentiated by key prefixes

## Key Space Design

### Catalog Table (`__CATALOG__`)

| Key Pattern | Value | Description |
|------------|-------|-------------|
| `DB\x00<database>` | Database metadata | Information about a database including its KVT data table ID |
| `TABLE\x00<database>\x00<table>` | Table metadata | Schema, columns, indexes, options |
| `SEQ\x00<database>\x00<table>\x00<column>` | Counter value | Auto-increment sequences |
| `SYS\x00version` | Version string | Schema version for upgrades |
| `SYS\x00initialized` | Timestamp | When catalog was initialized |

**Example entries:**
```
DB\x00test → {kvt_data_table_id: 42, created_at: 1234567890}
TABLE\x00test\x00users → {columns: [...], indexes: [...], row_count: 1000}
SEQ\x00test\x00users\x00id → 1001
```

### Data Tables (`__DATA_<database>`)

| Key Pattern | Value | Description |
|------------|-------|-------------|
| `<table>\x00<row_key>` | Serialized row | Actual row data |

**Example entries in `__DATA_test`:**
```
users\x00\x00\x00\x00\x01 → [row data for user id=1]
users\x00\x00\x00\x00\x02 → [row data for user id=2]
posts\x00\x00\x00\x00\x01 → [row data for post id=1]
```

## Operations Mapping

### Database Operations

#### CREATE DATABASE
```
1. Create KVT table "__DATA_<database>"
2. Get its table_id via kvt_get_table_id()
3. Store in catalog: "DB\x00<database>" → metadata
```

#### DROP DATABASE
```
1. Get catalog table_id
2. Range scan "TABLE\x00<database>\x00" to find all tables
3. Drop KVT table "__DATA_<database>"
4. Range delete all catalog entries for this database
```

### Table Operations

#### CREATE TABLE
```
1. Get catalog table_id (always lookup, never assume)
2. Store "TABLE\x00<database>\x00<table>" → schema metadata
3. Initialize sequences if needed
4. Data will be stored in "__DATA_<database>" table
```

#### DROP TABLE
```
1. Get data table_id for "__DATA_<database>"
2. Range delete from "<table>\x00" to "<table>\x01" in data table
3. Delete catalog entry "TABLE\x00<database>\x00<table>"
4. Delete related sequences
```

### Row Operations

#### INSERT
```
1. Get data table_id for "__DATA_<database>"
2. Generate key: "<table>\x00<primary_key_or_rowid>"
3. Store serialized row at that key
```

#### SELECT (Full Table Scan)
```
1. Get data table_id for "__DATA_<database>"
2. Range scan from "<table>\x00" to "<table>\x01"
3. Deserialize each row
```

#### DELETE
```
1. Get data table_id for "__DATA_<database>"
2. Generate key: "<table>\x00<primary_key_or_rowid>"
3. Delete the key
```

## Range Scan Boundaries

The key design enables efficient range operations:

| Operation | Start Key | End Key | Notes |
|-----------|-----------|---------|-------|
| All tables in database | `TABLE\x00mydb\x00` | `TABLE\x00mydb\x01` | `\x00` + 1 = `\x01` |
| All rows in table | `users\x00` | `users\x01` | Gets all users data |
| All sequences for table | `SEQ\x00mydb\x00users\x00` | `SEQ\x00mydb\x00users\x01` | All auto-increment counters |

## Security Considerations

1. **Reserved Prefixes**: Tables starting with `__` are system tables and cannot be created by users
2. **Name Validation**: Reject any database/table names containing `\x00` 
3. **Access Control**: Never expose internal KVT table names (`__CATALOG__`, `__DATA_*`) to SQL layer
4. **Isolation**: Each database's data is completely isolated in its own KVT table

## Implementation Guidelines

### Always Look Up Table IDs
```cpp
// WRONG - Never assume table IDs
uint64_t catalog_id = 1;  // DON'T DO THIS!

// CORRECT - Always look up by name
uint64_t catalog_id;
kvt_get_table_id("__CATALOG__", catalog_id, error);
```

### Use Proper Separators
```cpp
// WRONG - Using printable separator that could appear in names
string key = "TABLE:" + database + ":" + table;

// CORRECT - Using NULL separator
string key = string("TABLE\x00") + database + "\x00" + table;
```

### Prefix for Namespacing
```cpp
// WRONG - Suffixing (can cause collisions)
string kvt_table = database + "_data";

// CORRECT - Prefixing with reserved prefix
string kvt_table = "__DATA_" + database;
```

## Benefits of This Design

1. **Efficient Operations**: Range scans make bulk operations fast
2. **Clean Namespace Separation**: No possibility of key collision
3. **Security**: Protected against name manipulation attacks
4. **Scalability**: Each database isolated in its own KVT table
5. **Simplicity**: Consistent prefix-based organization throughout
6. **Recovery**: No hardcoded IDs means robust across restarts

## Future Considerations

1. **Partitioning**: Could extend to partition large tables across multiple KVT tables
2. **Indexes**: Secondary indexes could use separate KVT tables or key prefixes
3. **Optimization**: Cache frequently accessed catalog entries
4. **Monitoring**: Add statistics collection in catalog