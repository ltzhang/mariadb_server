# Composite Index Design for KVT Storage Engine

## Overview
Implement support for multi-column (composite) indexes in the KVT storage engine to enable efficient queries on multiple columns and support multi-column PRIMARY KEYs and UNIQUE constraints.

## Key Challenges

### 1. Key Encoding
- Combine multiple column values into a single KVT key
- Preserve sort order across all columns
- Handle NULL values correctly
- Support variable-length columns (VARCHAR, TEXT)

### 2. Partial Key Searches
- Support queries that specify only prefix columns
- Example: INDEX(a,b,c) should work for WHERE a=1 or WHERE a=1 AND b=2

### 3. Comparison Operations
- Support all comparison operators (<, <=, =, >=, >, !=)
- Handle range scans on composite keys
- Maintain correct sort order

## Design Approach

### Key Format for Composite Indexes

```
[table_id:8][index_id:4][col1_value][separator][col2_value][separator]...[colN_value][row_id:8]
```

#### Separators and NULL Handling
- Use 0x00 as column separator (works because we encode lengths for variable data)
- NULL values encoded as 0xFF (sorts after all non-NULL values per SQL standard)
- For DESC columns, invert bytes to reverse sort order

#### Variable-Length Column Encoding
```
For VARCHAR/TEXT:
  [length:2][data:variable][padding:0x00]
  
For fixed-length (INT, DATE, etc.):
  [data:fixed_size]
```

### Key Building Algorithm

```cpp
std::string build_composite_index_key(
    uint64_t table_id,
    uint32_t index_id,
    KEY* key_info,
    const uchar* key_data,
    key_part_map keypart_map,
    uint64_t row_id)
{
    std::string result;
    
    // Add table_id and index_id
    append_bigendian(result, table_id);
    append_bigendian(result, index_id);
    
    // Process each key part
    for (uint i = 0; i < key_info->user_defined_key_parts; i++) {
        if (!(keypart_map & (1 << i))) {
            break;  // No more key parts specified
        }
        
        KEY_PART_INFO* key_part = &key_info->key_part[i];
        Field* field = key_part->field;
        
        // Handle NULL
        if (field->is_null()) {
            result.append(1, 0xFF);  // NULL marker
            continue;
        }
        
        // Encode based on field type
        switch (field->type()) {
            case MYSQL_TYPE_LONG:
                append_int32(result, field->val_int(), key_part->key_part_flag & HA_REVERSE_SORT);
                break;
            case MYSQL_TYPE_VARCHAR:
                append_varchar(result, field, key_part->length);
                break;
            // ... other types
        }
        
        // Add separator (except for last column)
        if (i < key_info->user_defined_key_parts - 1) {
            result.append(1, 0x00);
        }
    }
    
    // Add row_id at the end
    append_bigendian(result, row_id);
    
    return result;
}
```

### Partial Key Search Support

```cpp
int index_read_map(uchar *buf, const uchar *key,
                  key_part_map keypart_map,
                  enum ha_rkey_function find_flag)
{
    // Build partial key for search
    std::string search_key = build_composite_index_key(
        table_id, index_id, key_info, key, keypart_map, 0);
    
    // Determine scan range based on find_flag
    std::string start_key, end_key;
    
    switch (find_flag) {
        case HA_READ_KEY_EXACT:
            // Exact match on provided columns
            start_key = search_key;
            end_key = search_key + std::string(8, 0xFF);  // Include all row_ids
            break;
            
        case HA_READ_KEY_OR_NEXT:
            // Greater than or equal
            start_key = search_key;
            end_key = build_index_end_key(table_id, index_id + 1);
            break;
            
        case HA_READ_PREFIX:
            // Prefix match (for partial keys)
            start_key = search_key;
            end_key = increment_key(search_key);
            break;
    }
    
    // Perform scan
    return scan_index_range(start_key, end_key, buf);
}
```

### Comparison Functions

```cpp
int compare_composite_keys(
    const std::string& key1,
    const std::string& key2,
    KEY* key_info,
    uint num_parts)
{
    size_t offset1 = 12;  // Skip table_id and index_id
    size_t offset2 = 12;
    
    for (uint i = 0; i < num_parts; i++) {
        KEY_PART_INFO* key_part = &key_info->key_part[i];
        
        // Extract and compare this column
        int cmp = compare_column_value(
            key1, offset1,
            key2, offset2,
            key_part);
            
        if (cmp != 0) {
            return cmp;
        }
        
        // Move to next column
        advance_to_next_column(key1, offset1, key_part);
        advance_to_next_column(key2, offset2, key_part);
    }
    
    return 0;  // Keys are equal
}
```

## Implementation Plan

### Phase 1: Basic Composite Key Support
1. Implement `build_composite_index_key()` function
2. Add composite key encoding for common data types (INT, VARCHAR, DATE)
3. Update `index_write()` to use composite keys
4. Update `index_read_map()` for exact matches

### Phase 2: Partial Key and Range Support
1. Support partial key searches (prefix columns only)
2. Implement range scans on composite keys
3. Handle all ha_rkey_function types
4. Add proper NULL handling

### Phase 3: Advanced Features
1. Support DESC columns in composite indexes
2. Handle all MariaDB data types
3. Optimize for covering indexes
4. Add statistics for composite indexes

### Phase 4: Testing and Optimization
1. Create comprehensive test suite
2. Performance benchmarking
3. Memory usage optimization
4. Edge case handling

## Test Cases

### Basic Functionality
```sql
-- Multi-column PRIMARY KEY
CREATE TABLE t1 (
    a INT,
    b VARCHAR(50),
    c DATE,
    PRIMARY KEY (a, b)
) ENGINE=KVT;

-- Multi-column INDEX
CREATE INDEX idx_abc ON t1(a, b, c);

-- Partial key search
SELECT * FROM t1 WHERE a = 1;  -- Uses index
SELECT * FROM t1 WHERE a = 1 AND b = 'test';  -- Uses index
SELECT * FROM t1 WHERE a = 1 AND b = 'test' AND c = '2025-01-06';  -- Uses index
SELECT * FROM t1 WHERE b = 'test';  -- Cannot use index (not leftmost prefix)
```

### Range Queries
```sql
-- Range on first column
SELECT * FROM t1 WHERE a BETWEEN 1 AND 10;

-- Range on second column with first fixed
SELECT * FROM t1 WHERE a = 5 AND b > 'abc';

-- Complex ranges
SELECT * FROM t1 WHERE a >= 5 AND b <= 'xyz';
```

### NULL Handling
```sql
-- NULL values in composite index
INSERT INTO t1 VALUES (1, NULL, '2025-01-06');
INSERT INTO t1 VALUES (NULL, 'test', '2025-01-06');

-- Searching for NULL
SELECT * FROM t1 WHERE a IS NULL;
SELECT * FROM t1 WHERE a = 1 AND b IS NULL;
```

### ORDER BY Optimization
```sql
-- Should use index for sorting
SELECT * FROM t1 ORDER BY a, b;
SELECT * FROM t1 WHERE a = 5 ORDER BY b;
```

## Expected Benefits

1. **Better Query Performance**: Queries on multiple columns can use a single index
2. **Reduced Storage**: One composite index vs multiple single-column indexes
3. **ORDER BY Optimization**: Natural sorting on multiple columns
4. **Unique Constraints**: Support for multi-column unique constraints
5. **Foreign Keys**: Multi-column foreign key support

## Potential Issues

1. **Key Size**: Composite keys can become large with many columns
2. **Selectivity**: Later columns in the index may have poor selectivity
3. **Maintenance Overhead**: More complex index updates
4. **Memory Usage**: Larger keys consume more memory

## Success Criteria

- [ ] Support for 2+ column composite indexes
- [ ] All comparison operators work correctly
- [ ] Partial key searches function properly
- [ ] NULL values handled correctly
- [ ] Performance improvement for multi-column queries
- [ ] Pass all MariaDB composite index tests