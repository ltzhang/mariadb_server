# Index-Only Scan (Covering Index) Design for KVT Storage Engine

## Overview
Index-only scans allow queries to be satisfied entirely from index data without accessing the actual row data. This optimization significantly improves query performance when all required columns are included in the index.

## Motivation

### Performance Benefits
1. **Reduced I/O**: No need to fetch row data from separate storage
2. **Better Cache Utilization**: Index pages stay in cache, row pages not loaded
3. **Faster Aggregations**: COUNT(*), MIN(), MAX() can use index directly
4. **Improved Concurrency**: Less contention on data pages

### Use Cases
```sql
-- Query can be satisfied from index alone
CREATE INDEX idx_covering ON orders (customer_id, order_date, total_amount);
SELECT customer_id, order_date, total_amount 
FROM orders 
WHERE customer_id = 123;

-- Aggregation using index
SELECT COUNT(*), MAX(order_date), SUM(total_amount)
FROM orders
WHERE customer_id = 123;
```

## Current Architecture Analysis

### What We Have
1. **Composite Index Support**: Already store multiple columns in index keys
2. **Index Structure**: Currently stores `[index_key] -> [row_id]`
3. **Row Fetch**: Always fetches full row data after index lookup

### What We Need
1. **Extended Index Storage**: Store column values in index entries
2. **Column Detection**: Identify when query needs only index columns
3. **Direct Return**: Return data from index without row fetch
4. **Handler Support**: Implement `HA_KEYREAD_ONLY` flag handling

## Design Approach

### 1. Extended Index Value Format

Current format:
```
Index Key: [table_id][index_id][column_values][row_id]
Index Value: [row_id]
```

New format for covering indexes:
```
Index Key: [table_id][index_id][indexed_columns][row_id]
Index Value: [row_id][covered_column_values]
```

### 2. Covered Column Storage

For each index, store additional columns that are frequently accessed together:
```cpp
struct IndexValueEntry {
    uint64_t row_id;
    std::vector<FieldValue> covered_columns;
};
```

### 3. Handler Integration

#### Key Methods to Update

```cpp
class ha_kvt : public handler {
    // Check if current query can use index-only scan
    bool can_use_index_only_scan(uint index, MY_BITMAP *read_set);
    
    // Enable/disable index-only scan mode
    int extra(enum ha_extra_function operation) override;
    
    // Modified index read to return covered columns
    int index_read_map(uchar *buf, const uchar *key,
                      key_part_map keypart_map,
                      enum ha_rkey_function find_flag) override;
    
    // Track index-only scan state
    bool index_only_scan_active;
    uint covering_index_id;
};
```

### 4. Column Extraction from Index

```cpp
int extract_covered_columns_from_index_value(
    const std::string& index_value,
    KEY* key_info,
    MY_BITMAP* read_set,
    uchar* buf)
{
    // Parse index value to get covered columns
    // Fill only requested columns into buffer
    // Mark other columns as NULL or default
}
```

## Implementation Plan

### Phase 1: Infrastructure Setup
1. **Define Covered Column Configuration**
   - Identify which columns to include in index values
   - Add metadata to track covered columns per index

2. **Extend Index Value Storage**
   - Modify index write operations to include covered columns
   - Update `build_composite_index_key()` to handle covered columns

### Phase 2: Query Detection
1. **Implement `can_use_index_only_scan()`**
   - Check if all columns in read_set are in index
   - Verify index is suitable for query conditions

2. **Handle `HA_EXTRA_KEYREAD` Flag**
   - Detect when optimizer requests index-only scan
   - Set internal state for index-only mode

### Phase 3: Data Retrieval
1. **Modify `index_read_map()`**
   - When in index-only mode, extract data from index value
   - Skip row data fetch
   - Fill buffer with covered column values

2. **Update `index_next()`**
   - Continue index-only scan for subsequent rows
   - Maintain performance benefits throughout scan

### Phase 4: Optimization
1. **Smart Column Selection**
   - Automatically include frequently accessed columns
   - Balance index size vs. coverage benefits

2. **Statistics Integration**
   - Track index-only scan usage
   - Provide hints to query optimizer

## Detailed Implementation

### 1. Index Value Encoding

```cpp
std::string encode_index_value_with_covered_columns(
    uint64_t row_id,
    KEY* key_info,
    const uchar* record,
    MY_BITMAP* covered_columns)
{
    std::string value;
    
    // Add row_id first (8 bytes)
    append_bigendian(value, row_id);
    
    // Add covered column values
    for (uint i = 0; i < table->s->fields; i++) {
        if (bitmap_is_set(covered_columns, i)) {
            Field* field = table->field[i];
            
            // Add field presence flag
            if (field->is_null()) {
                value.append(1, 0x00);  // NULL marker
            } else {
                value.append(1, 0x01);  // Non-NULL marker
                
                // Encode field value
                std::string field_value = encode_field_for_index(field);
                
                // Add length prefix for variable-length fields
                if (field->is_variable_length()) {
                    uint16_t len = field_value.length();
                    append_bigendian_16(value, len);
                }
                
                value.append(field_value);
            }
        }
    }
    
    return value;
}
```

### 2. Covered Column Extraction

```cpp
int decode_covered_columns_from_index_value(
    const std::string& value,
    uint64_t& row_id,
    KEY* key_info,
    MY_BITMAP* covered_columns,
    uchar* buf)
{
    size_t offset = 0;
    
    // Extract row_id
    row_id = read_bigendian_64(value, offset);
    offset += 8;
    
    // Extract covered columns
    for (uint i = 0; i < table->s->fields; i++) {
        if (bitmap_is_set(covered_columns, i)) {
            Field* field = table->field[i];
            
            // Read NULL flag
            if (value[offset++] == 0x00) {
                field->set_null();
            } else {
                field->set_notnull();
                
                // Read field value
                size_t field_len;
                if (field->is_variable_length()) {
                    field_len = read_bigendian_16(value, offset);
                    offset += 2;
                } else {
                    field_len = field->pack_length();
                }
                
                // Decode into field
                field->unpack(value.data() + offset, field_len);
                offset += field_len;
            }
        }
    }
    
    return 0;
}
```

### 3. Handler Method Updates

```cpp
bool ha_kvt::can_use_index_only_scan(uint index, MY_BITMAP* read_set)
{
    KEY* key_info = &table->key_info[index];
    
    // Check if all requested columns are in the index
    for (uint i = 0; i < table->s->fields; i++) {
        if (bitmap_is_set(read_set, i)) {
            bool found_in_index = false;
            
            // Check if field is part of index key
            for (uint j = 0; j < key_info->user_defined_key_parts; j++) {
                if (key_info->key_part[j].field->field_index == i) {
                    found_in_index = true;
                    break;
                }
            }
            
            // Check if field is in covered columns
            if (!found_in_index && 
                !is_field_covered_by_index(index, i)) {
                return false;  // Field not available in index
            }
        }
    }
    
    return true;
}

int ha_kvt::extra(enum ha_extra_function operation)
{
    switch (operation) {
        case HA_EXTRA_KEYREAD:
            // Enable index-only scan
            if (active_index != MAX_KEY &&
                can_use_index_only_scan(active_index, table->read_set)) {
                index_only_scan_active = true;
                covering_index_id = active_index;
            }
            break;
            
        case HA_EXTRA_NO_KEYREAD:
            // Disable index-only scan
            index_only_scan_active = false;
            break;
            
        default:
            break;
    }
    
    return 0;
}
```

## Test Plan

### 1. Basic Covering Index Tests
```sql
-- Create table with covering index
CREATE TABLE t1 (
    id INT PRIMARY KEY,
    customer_id INT,
    order_date DATE,
    amount DECIMAL(10,2),
    status VARCHAR(20),
    INDEX idx_cover (customer_id, order_date, amount)
) ENGINE=KVT;

-- Query using only indexed columns
SELECT customer_id, order_date, amount 
FROM t1 
WHERE customer_id = 100;

-- Verify no row fetch occurred (check handler status)
SHOW STATUS LIKE 'Handler_read%';
```

### 2. Partial Coverage Tests
```sql
-- Query needs some non-indexed columns
SELECT customer_id, order_date, amount, status
FROM t1 
WHERE customer_id = 100;
-- Should still use index but fetch rows for 'status'

-- Query uses only key columns
SELECT customer_id, order_date 
FROM t1 
WHERE customer_id BETWEEN 100 AND 200;
-- Should use index-only scan
```

### 3. Aggregation Tests
```sql
-- COUNT using covering index
SELECT COUNT(*) 
FROM t1 
WHERE customer_id = 100;

-- MIN/MAX using covering index
SELECT MIN(order_date), MAX(amount)
FROM t1
WHERE customer_id = 100;

-- GROUP BY with covering index
SELECT customer_id, COUNT(*), SUM(amount)
FROM t1
GROUP BY customer_id;
```

### 4. Performance Benchmarks
```sql
-- Create large dataset
INSERT INTO t1 SELECT ...;  -- 1M rows

-- Measure performance difference
-- Without index-only scan
SET optimizer_switch='index_condition_pushdown=off';
SELECT customer_id, order_date, amount FROM t1 WHERE customer_id < 1000;

-- With index-only scan
SET optimizer_switch='index_condition_pushdown=on';
SELECT customer_id, order_date, amount FROM t1 WHERE customer_id < 1000;
```

## Configuration Options

### 1. Index Definition Extensions
```sql
-- Explicitly define covered columns
CREATE INDEX idx_name ON table_name (key_col1, key_col2) 
INCLUDE (covered_col1, covered_col2);

-- Auto-cover primary key columns
CREATE INDEX idx_name ON table_name (col1, col2) 
WITH (COVER_PRIMARY_KEY=true);
```

### 2. Runtime Configuration
```cpp
// System variables
kvt_index_only_scan_enabled = true;
kvt_max_covered_columns = 5;
kvt_covered_column_size_limit = 1024;
```

## Success Criteria

1. **Functional Requirements**
   - ✓ Queries using only indexed columns don't fetch row data
   - ✓ Correct results for all query types
   - ✓ Proper NULL handling in covered columns
   - ✓ Statistics accurately reflect index-only scans

2. **Performance Requirements**
   - ✓ 50%+ reduction in I/O for covered queries
   - ✓ 30%+ improvement in query response time
   - ✓ Minimal overhead for index maintenance

3. **Compatibility Requirements**
   - ✓ Works with existing indexes (backward compatible)
   - ✓ Optimizer correctly chooses index-only scans
   - ✓ EXPLAIN shows "Using index" when applicable

## Risks and Mitigation

### 1. Index Size Growth
**Risk**: Storing additional columns increases index size
**Mitigation**: 
- Selective column inclusion based on query patterns
- Compression for covered column values
- Size limits and warnings

### 2. Update Performance Impact
**Risk**: More data to update when rows change
**Mitigation**:
- Lazy update for non-key covered columns
- Batch updates where possible
- Option to disable covering for write-heavy tables

### 3. Memory Usage
**Risk**: Larger index entries consume more buffer pool
**Mitigation**:
- Smart eviction policies
- Separate cache for covered vs. non-covered indexes
- Memory usage monitoring

## Future Enhancements

1. **Automatic Covering Detection**
   - Analyze query workload
   - Automatically suggest/create covering indexes
   - Dynamic covered column selection

2. **Partial Column Coverage**
   - Store only prefix of long VARCHAR columns
   - Compress redundant data in covered columns

3. **Multi-Version Coverage**
   - Store multiple versions for MVCC
   - Enable index-only scans in more isolation levels

4. **Join Optimization**
   - Use covering indexes for join operations
   - Index-only hash joins

## Conclusion

Index-only scans provide significant performance benefits for read-heavy workloads. By storing additional column data in indexes, we can eliminate unnecessary row fetches and improve query response times. The implementation maintains backward compatibility while providing flexible configuration options for different use cases.