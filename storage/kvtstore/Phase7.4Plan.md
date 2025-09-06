# Phase 7.4: Spatial Index Support - Implementation Plan

## Overview
Add spatial index support to KVT storage engine by creating an adapter layer that leverages MariaDB's existing spatial functionality while using KVT for R-tree index storage.

## Research Summary
Based on investigation of MariaDB's codebase:
1. **Spatial Infrastructure**: MariaDB has robust spatial support in `sql/spatial.h` with MBR (Minimum Bounding Rectangle) structures
2. **R-tree Implementation**: InnoDB implements R-trees in `storage/innobase/include/gis0rtree.h`
3. **Spatial Functions**: SQL functions in `sql/item_geofunc.h` handle ST_Contains, ST_Within, ST_Distance, etc.
4. **Index Flag**: `HA_SPATIAL` flag (0x400) in `include/my_base.h` marks spatial indexes

## Detailed Design

### Architecture Overview
```
┌─────────────────────────────────────┐
│     MariaDB SQL Layer               │
│  (Spatial functions & predicates)   │
└─────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────┐
│        ha_kvt Handler               │
│   (Spatial index operations)        │
└─────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────┐
│    KVT Spatial Adapter              │
│  (R-tree implementation)            │
└─────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────┐
│      KVT Storage Backend            │
│   (Key-value transactions)          │
└─────────────────────────────────────┘
```

### Key Components

#### 1. KVT Spatial Index Structure

##### Key Space Design
```cpp
enum KVTKeySpace {
    // ... existing key spaces ...
    SPATIAL_INDEX = 0x05,  // R-tree nodes
    SPATIAL_META  = 0x06   // R-tree metadata
};
```

##### Key-Value Format
```cpp
// Node Key Format:
// [table_id:8][index_id:4][node_id:8]
struct SpatialNodeKey {
    uint64_t table_id;
    uint32_t index_id;
    uint64_t node_id;
};

// Node Value Format:
struct SpatialNodeValue {
    uint8_t  level;          // 0 = leaf, >0 = internal
    uint16_t entry_count;    // Number of entries
    MBR      node_mbr;       // Node's overall MBR
    struct Entry {
        MBR mbr;             // Entry's MBR
        union {
            uint64_t row_id;     // For leaf nodes
            uint64_t child_id;   // For internal nodes
        };
    } entries[MAX_ENTRIES];
};

// Metadata Key Format:
// [table_id:8][index_id:4][meta_type:1]
struct SpatialMetaKey {
    uint64_t table_id;
    uint32_t index_id;
    uint8_t  meta_type;  // ROOT_NODE, NEXT_NODE_ID, etc.
};
```

#### 2. R-tree Algorithm Details

##### Insert Algorithm
```
1. Start from root node
2. For each level until leaf:
   - Choose child with minimum MBR enlargement
   - Update path for later MBR adjustments
3. At leaf level:
   - Insert entry
   - If overflow, split node (quadratic split)
4. Propagate changes upward:
   - Update MBRs along path
   - Handle splits recursively
   - Create new root if root splits
```

##### Search Algorithm
```
1. Start from root with search MBR
2. For each node:
   - Check predicate against node MBR
   - If match, recurse into children/return rows
3. Optimization: Use priority queue for nearest neighbor
```

##### Quadratic Split Algorithm
```
1. Pick two seeds (maximum separation)
2. Distribute remaining entries:
   - Assign to group with minimum enlargement
   - Balance group sizes
3. Create two new nodes with computed MBRs
```

#### 3. Spatial Predicates Integration
Reuse MariaDB's existing predicates from `sql/spatial.h`:
- MBR_CONTAIN_CMP - For ST_Contains
- MBR_INTERSECT_CMP - For ST_Intersects  
- MBR_WITHIN_CMP - For ST_Within
- MBR_DISJOINT_CMP - For ST_Disjoint
- MBR_EQUAL_CMP - For exact match

## Implementation Phases

### Day 1-2: Core Adapter Infrastructure
- Create `kvt_spatial_adapter.h` with interface definitions
- Implement singleton pattern for global instance
- Define KVT key/value formats for R-tree nodes
- Set up MBR serialization/deserialization

### Day 3-4: R-tree Operations
- Implement R-tree insertion algorithm
  - Choose subtree based on minimum MBR enlargement
  - Handle node splitting (quadratic split)
  - Update parent MBRs recursively
- Implement R-tree search
  - Top-down traversal with MBR filtering
  - Support for different spatial predicates
- Basic deletion support

### Day 5: Handler Integration
- Modify `ha_kvt.h` to add spatial methods
  - `create_spatial_index()`
  - `drop_spatial_index()`
  - `spatial_search()`
- Update `ha_kvt.cc`
  - Handle HA_SPATIAL flag in create()
  - Implement spatial index operations
  - Add spatial predicate pushdown

### Day 6: Query Processing
- Implement spatial range scans
- Support for common spatial queries:
  - ST_Contains/ST_Within
  - ST_Intersects
  - ST_Distance (with optimization)
- Index hint processing

### Day 7: Testing and Optimization
- Create test suite `kvt_spatial_basic.test`
  - Point/Polygon insertions
  - Spatial predicates
  - Index usage verification
  - Updates and deletes
- Performance optimizations:
  - Bulk loading
  - Node caching
  - MBR compression

## File Structure
```
storage/kvtstore/
├── kvt_spatial_adapter.h    # Spatial adapter interface
├── kvt_spatial_adapter.cc   # R-tree implementation
├── ha_kvt.h                 # Add spatial methods
├── ha_kvt.cc                # Integrate spatial support
└── mysql-test/suite/kvtstore/
    ├── t/kvt_spatial_basic.test
    └── r/kvt_spatial_basic.result
```

## Technical Decisions

### 1. R-tree Split Algorithm
Use **Quadratic Split** (Guttman's algorithm):
- Good balance between quality and performance
- Simpler than R*-tree but effective
- Well-understood algorithm

### 2. Node Size
- Target: 64-128 entries per node
- Optimize for KVT's key-value operations
- Consider KVT's internal page size

### 3. Spatial Reference System
- Support SRID=0 (Cartesian) initially
- Geographic (SRID=4326) as future enhancement
- Leverage MariaDB's SRID handling

### 4. Concurrency
- Rely on KVT's transaction isolation
- No additional locking needed
- Phantom prevention via KVT's MVCC

## Risk Mitigation

1. **Performance Risk**: R-tree operations can be slow
   - Mitigation: Cache frequently accessed nodes
   - Implement lazy deletion
   - Bulk loading optimization

2. **Complexity Risk**: Spatial algorithms are complex
   - Mitigation: Reuse MariaDB's MBR calculations
   - Start with 2D support only
   - Extensive testing

3. **Integration Risk**: Handler interface complexities
   - Mitigation: Study InnoDB's implementation
   - Start with basic operations
   - Incremental enhancement

## Success Metrics
- All spatial tests pass
- Performance within 2x of InnoDB spatial
- Support for common spatial queries
- Clean integration with query optimizer

## Dependencies
- MariaDB spatial infrastructure (`sql/spatial.h`)
- KVT transaction support
- Existing MBR calculation routines

## Timeline Summary
- **Days 1-2**: Core adapter infrastructure
- **Days 3-4**: R-tree algorithms
- **Day 5**: Handler integration
- **Day 6**: Query processing
- **Day 7**: Testing and optimization

## API Interface Design

### KVTSpatialAdapter Class
```cpp
class KVTSpatialAdapter {
public:
    // Singleton access
    static KVTSpatialAdapter* get_instance();
    
    // Index management
    int create_spatial_index(uint64_t table_id, uint32_t index_id);
    int drop_spatial_index(uint64_t table_id, uint32_t index_id);
    
    // Data operations
    int insert_spatial(uint64_t table_id, uint32_t index_id,
                      uint64_t row_id, const MBR& mbr);
    int delete_spatial(uint64_t table_id, uint32_t index_id,
                      uint64_t row_id, const MBR& mbr);
    int update_spatial(uint64_t table_id, uint32_t index_id,
                      uint64_t row_id, 
                      const MBR& old_mbr, const MBR& new_mbr);
    
    // Search operations
    class SpatialSearch {
    public:
        bool get_next(uint64_t& row_id);
        void reset();
    };
    
    std::unique_ptr<SpatialSearch> search(
        uint64_t table_id, uint32_t index_id,
        const MBR& search_mbr, 
        SpatialPredicate predicate);
    
    // Statistics
    int get_index_stats(uint64_t table_id, uint32_t index_id,
                       SpatialIndexStats* stats);
};
```

### Handler Integration Points
```cpp
class ha_kvt : public handler {
    // Spatial index support
    int create_spatial_index(KEY* key_info);
    int prepare_spatial_index(uint key_nr);
    ha_rows records_in_range_spatial(uint inx, 
                                    const MBR& min_mbr,
                                    const MBR& max_mbr);
    int index_read_spatial(uchar* buf, uint index,
                          const MBR& mbr, 
                          SpatialPredicate predicate);
};
```

## Notes
This approach maximizes reuse of MariaDB's spatial infrastructure while providing efficient R-tree storage via KVT. The thin adapter pattern has proven successful in Phase 7.3 (FTS) and should work equally well for spatial indexes.