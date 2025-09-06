# Phase 7.3: Full-Text Search Support - Design and Implementation Plan

## Motivation

Full-text search is essential for modern applications that need to search through text content efficiently. The KVT storage engine needs to support FULLTEXT indexes to:
1. Enable MATCH...AGAINST queries
2. Provide relevance-based ranking
3. Support boolean and natural language search modes
4. Maintain compatibility with existing MariaDB FTS syntax

## Key Discovery - Reusing MariaDB Components

After researching the codebase, we discovered MariaDB already provides robust FTS infrastructure:
- **Parser API** (`plugin_ftparser.h`) - Handles tokenization, stopwords, boolean operators
- **FT_INFO Interface** (`ft_global.h`) - Standard interface for FTS operations
- **Scoring Algorithms** - Proven implementations of BM25 and other ranking algorithms
- **Query Processing** - Boolean mode operators, phrase matching, wildcard support

## Design - Thin Adapter Pattern

Instead of reimplementing FTS from scratch, we'll create a thin adapter layer that:
1. **Leverages MariaDB's existing parsers** for text processing
2. **Stores the inverted index in KVT** instead of traditional B-trees
3. **Reuses scoring algorithms** from existing implementations
4. **Implements required handler methods** to integrate with the SQL layer

### Architecture Overview

```
SQL Layer (MATCH...AGAINST)
    ↓
Handler Interface (ft_init_ext, ft_read)
    ↓
KVT FTS Adapter (kvt_fulltext_adapter)
    ↓
├── MariaDB Parser (tokenization, stopwords)
└── KVT Storage (inverted index storage)
```

### Key Space Design for FTS in KVT

```
Inverted Index:
__fts.<table_id>.<index_id>:term:<term> → [doc_id, positions, tf]...
__fts.<table_id>.<index_id>:doc:<doc_id> → {doc_length, norm_factor}
__fts.<table_id>.<index_id>:stats → {num_docs, avg_doc_len, terms_count}
__fts.<table_id>.<index_id>:config → {parser_name, min_word_len, max_word_len}
```

## Implementation Plan (7 Days)

### Day 1-2: FTS Storage Adapter
Create the core adapter that bridges MariaDB FTS with KVT storage:

**Files to create:**
- `kvt_fulltext_adapter.h` - Interface definitions
- `kvt_fulltext_adapter.cc` - Implementation

**Key Components:**
```cpp
class KVTFulltextInfo : public FT_INFO {
  // Implements FT_INFO interface
  // Reads posting lists from KVT
  // Calculates relevance scores
};

class KVTFulltextAdapter {
public:
  // Document indexing
  int index_document(uint64_t table_id, uint32_t index_id,
                    uint64_t doc_id, const char* text, uint text_len);
  
  // Search initialization  
  FT_INFO* init_search(uint64_t table_id, uint32_t index_id,
                      uint flags, const char* query, uint query_len);
  
  // Index management
  int create_fulltext_index(uint64_t table_id, uint32_t index_id,
                          const FTSConfig& config);
  int drop_fulltext_index(uint64_t table_id, uint32_t index_id);
  
private:
  // Callbacks for parser
  static int add_word_to_index(MYSQL_FTPARSER_PARAM* param,
                              const char* word, int word_len,
                              MYSQL_FTPARSER_BOOLEAN_INFO* info);
  
  // Storage helpers
  int store_posting_entry(uint64_t table_id, uint32_t index_id,
                         const char* term, uint64_t doc_id,
                         const std::vector<uint32_t>& positions);
  int load_posting_list(uint64_t table_id, uint32_t index_id,
                       const char* term, PostingList& list);
};
```

### Day 3-4: Parser Integration

Integrate with MariaDB's existing parser infrastructure:

**Tasks:**
1. Use `ft_default_parser` for text tokenization
2. Implement `mysql_add_word` callback to store terms in KVT
3. Handle different parser modes (SIMPLE, BOOLEAN, WITH_STOPWORDS)
4. Reuse stopword filtering from `ft_stopword_file`

**Code Structure:**
```cpp
// Parser callback context
struct IndexContext {
  KVTFulltextAdapter* adapter;
  uint64_t table_id;
  uint32_t index_id;
  uint64_t doc_id;
  std::map<std::string, std::vector<uint32_t>> term_positions;
};

// Implementation of mysql_add_word callback
int add_word_callback(MYSQL_FTPARSER_PARAM* param,
                     const char* word, int word_len,
                     MYSQL_FTPARSER_BOOLEAN_INFO* boolean_info) {
  IndexContext* ctx = (IndexContext*)param->ftparser_state;
  // Store word and position in context
  // Will be batch-written to KVT after parsing completes
}
```

### Day 5: Handler Method Implementation

Implement FTS methods in `ha_kvt.cc`:

**Methods to implement:**
```cpp
class ha_kvt : public handler {
  // Initialize FTS search
  FT_INFO* ft_init_ext(uint flags, uint inx, String* key) override;
  
  // Read next matching document
  int ft_read(uchar* buf) override;
  
  // Support for FTS in table operations
  int create_index(...) {
    if (key_info->flags & HA_FULLTEXT) {
      // Create FTS index via adapter
    }
  }
  
  int write_row(uchar* buf) {
    // Index document for FTS columns
  }
  
  int update_row(const uchar* old_data, uchar* new_data) {
    // Re-index document if FTS columns changed
  }
  
  int delete_row(const uchar* buf) {
    // Remove document from FTS index
  }
};
```

### Day 6: Testing

Create comprehensive test suite:

**Test files:**
- `kvt_fts_basic.test` - Basic FULLTEXT index and MATCH...AGAINST
- `kvt_fts_boolean.test` - Boolean mode searches
- `kvt_fts_ranking.test` - Relevance ranking tests

**Test scenarios:**
1. Create FULLTEXT index on single/multiple columns
2. Natural language searches
3. Boolean mode with operators (+, -, *, etc.)
4. Relevance scoring and ORDER BY
5. Update/delete with FTS index maintenance

### Day 7: Documentation and Performance Tuning

**Tasks:**
1. Document the adapter pattern and integration points
2. Add inline code documentation
3. Performance profiling and optimization
4. Create usage examples

## Technical Details

### Posting List Format in KVT

```
Key: __fts.<table_id>.<index_id>:term:<term>
Value: {
  num_docs: varint,
  posting_entries: [
    {
      doc_id: varint,
      term_frequency: float,
      num_positions: varint,
      positions: [varint...]
    }...
  ]
}
```

### Document Metadata Format

```
Key: __fts.<table_id>.<index_id>:doc:<doc_id>
Value: {
  doc_length: varint,
  num_unique_terms: varint,
  norm_factor: float
}
```

### Integration with Existing Components

1. **Transaction Manager**: FTS updates are transactional
2. **Index Manager**: FTS indexes registered in index catalog
3. **Query Optimizer**: Cost estimation for FTS operations
4. **Catalog System**: FTS metadata storage

## Benefits of This Approach

1. **Minimal Code**: ~1000 lines vs ~5000 lines for full implementation
2. **Proven Algorithms**: Leverage battle-tested FTS logic
3. **Compatibility**: Works with existing SQL syntax without changes
4. **Maintainability**: Updates to MariaDB FTS benefit KVT automatically
5. **Time Efficient**: 7 days vs 14 days implementation time

## What We're NOT Implementing

- Text tokenization logic (use ft_default_parser)
- Stopword management (use ft_stopword_file)
- Boolean query parsing (use existing parser)
- Scoring algorithms (use existing implementations)
- Query operator syntax (reuse ft_boolean_syntax)

## What We ARE Implementing

- Storage adapter for inverted index in KVT
- FT_INFO implementation backed by KVT
- Handler method implementations
- Integration with KVT transactions
- Test suite for KVT-specific FTS

## Success Criteria

1. Support for FULLTEXT index creation
2. Functional MATCH...AGAINST queries (natural and boolean modes)
3. Correct relevance scoring compatible with MyISAM/InnoDB
4. Transactional consistency for FTS operations
5. All tests passing
6. Performance within 20% of MyISAM FTS

## Risks and Mitigations

1. **Risk**: Parser API changes in future MariaDB versions
   - **Mitigation**: Version checking and compatibility layer

2. **Risk**: Performance overhead from KVT storage
   - **Mitigation**: Caching and batch operations

3. **Risk**: Large posting lists affecting memory
   - **Mitigation**: Streaming and pagination support

## Deliverables

1. **Core Files**:
   - `kvt_fulltext_adapter.h/cc` - FTS adapter implementation
   - Updates to `ha_kvt.cc` - Handler method implementations
   - Updates to `CMakeLists.txt` - Build configuration

2. **Tests**:
   - `kvt_fts_basic.test` - Basic functionality
   - `kvt_fts_boolean.test` - Boolean mode
   - `kvt_fts_ranking.test` - Relevance ranking

3. **Documentation**:
   - API documentation in header files
   - Usage examples in tests
   - Phase summary document