/*
   Copyright (c) 2025 KVT Storage Engine

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#ifndef KVT_FULLTEXT_ADAPTER_H
#define KVT_FULLTEXT_ADAPTER_H

#include "my_global.h"
#include "kvt/kvt_inc.h"
#include "kvt_constants.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

// Avoid C++ template errors with C headers by ensuring proper order
#ifdef __cplusplus
extern "C" {
#endif
#include "ft_global.h"
#include "mysql/plugin_ftparser.h"
#ifdef __cplusplus
}
#endif

// Forward declarations
class THD;
class TABLE;
class String;

namespace kvt_fts {

// FTS configuration for an index
struct FTSConfig {
  std::string parser_name;
  uint32_t min_word_len;
  uint32_t max_word_len;
  bool with_positions;
  
  FTSConfig() : 
    parser_name("default"),
    min_word_len(4),
    max_word_len(84),
    with_positions(true) {}
};

// Posting list entry for a document
struct PostingEntry {
  uint64_t doc_id;
  float term_frequency;
  std::vector<uint32_t> positions;
  
  PostingEntry() : doc_id(0), term_frequency(0.0f) {}
  PostingEntry(uint64_t id) : doc_id(id), term_frequency(0.0f) {}
};

// Document metadata for scoring
struct DocumentMetadata {
  uint64_t doc_id;
  uint32_t doc_length;
  uint32_t num_unique_terms;
  float norm_factor;
  
  DocumentMetadata() : doc_id(0), doc_length(0), num_unique_terms(0), norm_factor(1.0f) {}
};

// Index statistics for scoring
struct IndexStatistics {
  uint64_t num_docs;
  uint64_t total_terms;
  float avg_doc_length;
  
  IndexStatistics() : num_docs(0), total_terms(0), avg_doc_length(0.0f) {}
};

// Forward declaration
class KVTFulltextAdapter;

// KVT implementation of FT_INFO interface
class KVTFulltextInfo : public FT_INFO {
private:
  KVTFulltextAdapter* adapter_;
  uint64_t table_id_;
  uint32_t index_id_;
  uint flags_;
  std::string query_;
  
  // Search state
  struct SearchState {
    std::vector<std::string> query_terms;
    std::map<uint64_t, float> doc_scores;  // doc_id -> relevance score
    std::map<uint64_t, float>::iterator current_doc;
    bool initialized;
    
    SearchState() : initialized(false) {}
  } search_state_;
  
  // Boolean mode info
  struct BooleanQuery {
    std::vector<std::string> must_have;     // + operator
    std::vector<std::string> must_not_have; // - operator
    std::vector<std::string> optional;      // no operator
    std::map<std::string, float> weights;   // term weights
  } boolean_query_;
  
public:
  KVTFulltextInfo(KVTFulltextAdapter* adapter, uint64_t table_id, 
                  uint32_t index_id, uint flags, const char* query, uint query_len);
  ~KVTFulltextInfo();
  
  // FT_INFO interface methods (via please pointer)
  int read_next(char* record);
  float find_relevance(uchar* record, uint length);
  void close_search();
  float get_relevance();
  void reinit_search();
  
  // Extended interface methods
  ulonglong get_docid();
  
private:
  // Search execution
  int execute_natural_search();
  int execute_boolean_search();
  float calculate_bm25_score(const std::string& term, const PostingEntry& entry,
                            uint64_t df, const DocumentMetadata& doc_meta);
  
  // Query parsing
  int parse_boolean_query(const char* query, uint query_len);
  
  friend class KVTFulltextAdapter;
};

// Parser context for indexing
struct IndexContext {
  KVTFulltextAdapter* adapter;
  uint64_t table_id;
  uint32_t index_id;
  uint64_t doc_id;
  uint32_t current_position;
  std::map<std::string, std::vector<uint32_t>> term_positions;
  std::string error_message;
  
  IndexContext(KVTFulltextAdapter* a, uint64_t tid, uint32_t iid, uint64_t did) :
    adapter(a), table_id(tid), index_id(iid), doc_id(did), current_position(0) {}
};

// Main FTS adapter class
class KVTFulltextAdapter {
private:
  // Singleton instance
  static KVTFulltextAdapter* instance_;
  static std::mutex instance_mutex_;
  
  // Cache for index configurations
  std::map<std::pair<uint64_t, uint32_t>, FTSConfig> config_cache_;
  mutable std::mutex cache_mutex_;
  
  // Default parser
  st_mysql_ftparser* default_parser_;
  
  // Private constructor for singleton
  KVTFulltextAdapter();
  
public:
  ~KVTFulltextAdapter();
  
  // Singleton access
  static KVTFulltextAdapter* get_instance();
  
  // Index management
  int create_fulltext_index(uint64_t table_id, uint32_t index_id,
                          const FTSConfig& config);
  int drop_fulltext_index(uint64_t table_id, uint32_t index_id);
  
  // Document operations
  int index_document(uint64_t table_id, uint32_t index_id,
                    uint64_t doc_id, const char* text, uint text_len);
  int remove_document(uint64_t table_id, uint32_t index_id, uint64_t doc_id);
  int update_document(uint64_t table_id, uint32_t index_id,
                     uint64_t doc_id, const char* old_text, uint old_len,
                     const char* new_text, uint new_len);
  
  // Search operations
  FT_INFO* init_search(uint64_t table_id, uint32_t index_id,
                      uint flags, const char* query, uint query_len,
                      CHARSET_INFO* cs);
  
  // Storage operations (public for KVTFulltextInfo access)
  int load_posting_list(uint64_t table_id, uint32_t index_id,
                       const std::string& term, std::vector<PostingEntry>& entries);
  int load_document_metadata(uint64_t table_id, uint32_t index_id,
                            uint64_t doc_id, DocumentMetadata& meta);
  int load_index_statistics(uint64_t table_id, uint32_t index_id,
                           IndexStatistics& stats);
  
private:
  // Parser callbacks
  static int add_word_to_index(MYSQL_FTPARSER_PARAM* param,
                              const char* word, int word_len,
                              MYSQL_FTPARSER_BOOLEAN_INFO* info);
  static int parse_query_callback(MYSQL_FTPARSER_PARAM* param,
                                 const char* word, int word_len,
                                 MYSQL_FTPARSER_BOOLEAN_INFO* info);
  
  // Storage helpers
  int store_posting_entry(uint64_t table_id, uint32_t index_id,
                         const std::string& term, uint64_t doc_id,
                         const std::vector<uint32_t>& positions);
  int store_document_metadata(uint64_t table_id, uint32_t index_id,
                             uint64_t doc_id, const DocumentMetadata& meta);
  int update_index_statistics(uint64_t table_id, uint32_t index_id,
                             int64_t doc_delta, int64_t term_delta);
  
  // Configuration management
  int load_config(uint64_t table_id, uint32_t index_id, FTSConfig& config);
  int save_config(uint64_t table_id, uint32_t index_id, const FTSConfig& config);
  
  // Key generation
  std::string make_term_key(uint64_t table_id, uint32_t index_id, const std::string& term);
  std::string make_doc_key(uint64_t table_id, uint32_t index_id, uint64_t doc_id);
  std::string make_stats_key(uint64_t table_id, uint32_t index_id);
  std::string make_config_key(uint64_t table_id, uint32_t index_id);
  
  // Serialization helpers
  std::string serialize_posting_list(const std::vector<PostingEntry>& entries);
  int deserialize_posting_list(const std::string& data, std::vector<PostingEntry>& entries);
  std::string serialize_metadata(const DocumentMetadata& meta);
  int deserialize_metadata(const std::string& data, DocumentMetadata& meta);
  std::string serialize_statistics(const IndexStatistics& stats);
  int deserialize_statistics(const std::string& data, IndexStatistics& stats);
};

// FT_INFO virtual function table implementation
extern struct _ft_vft kvt_ft_vft;

// Helper functions
bool is_stopword_check(const char* word, size_t len);
std::string normalize_term(const char* word, size_t len, CHARSET_INFO* cs);

} // namespace kvt_fts

#endif // KVT_FULLTEXT_ADAPTER_H