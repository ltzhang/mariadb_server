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

#include "kvt_fulltext_adapter.h"
#include "sql_class.h"
#include "table.h"
#include <cmath>
#include <algorithm>
#include <sstream>

namespace kvt_fts {

// BM25 parameters
const float BM25_K1 = 1.2f;
const float BM25_B = 0.75f;

// Singleton instance
KVTFulltextAdapter* KVTFulltextAdapter::instance_ = nullptr;
std::mutex KVTFulltextAdapter::instance_mutex_;

// FT_INFO virtual function table callbacks
static int kvt_ft_read_next(FT_INFO* ftinfo, char* record) {
  return ((KVTFulltextInfo*)ftinfo)->read_next(record);
}

static float kvt_ft_find_relevance(FT_INFO* ftinfo, uchar* record, uint length) {
  return ((KVTFulltextInfo*)ftinfo)->find_relevance(record, length);
}

static void kvt_ft_close_search(FT_INFO* ftinfo) {
  ((KVTFulltextInfo*)ftinfo)->close_search();
}

static float kvt_ft_get_relevance(FT_INFO* ftinfo) {
  return ((KVTFulltextInfo*)ftinfo)->get_relevance();
}

static void kvt_ft_reinit_search(FT_INFO* ftinfo) {
  ((KVTFulltextInfo*)ftinfo)->reinit_search();
}

// Virtual function table for FT_INFO
struct _ft_vft kvt_ft_vft = {
  kvt_ft_read_next,
  kvt_ft_find_relevance,
  kvt_ft_close_search,
  kvt_ft_get_relevance,
  kvt_ft_reinit_search
};

// KVTFulltextInfo implementation
KVTFulltextInfo::KVTFulltextInfo(KVTFulltextAdapter* adapter, uint64_t table_id,
                                 uint32_t index_id, uint flags, 
                                 const char* query, uint query_len)
  : adapter_(adapter), table_id_(table_id), index_id_(index_id), flags_(flags),
    query_(query, query_len) {
  please = &kvt_ft_vft;
}

KVTFulltextInfo::~KVTFulltextInfo() {
}

int KVTFulltextInfo::read_next(char* record) {
  if (!search_state_.initialized) {
    // Execute search on first read
    int error = (flags_ & FT_BOOL) ? execute_boolean_search() : execute_natural_search();
    if (error) return error;
    
    search_state_.current_doc = search_state_.doc_scores.begin();
    search_state_.initialized = true;
  }
  
  if (search_state_.current_doc == search_state_.doc_scores.end()) {
    return HA_ERR_END_OF_FILE;
  }
  
  // Store doc_id in record (implementation depends on table structure)
  // For now, just advance to next document
  ++search_state_.current_doc;
  return 0;
}

float KVTFulltextInfo::find_relevance(uchar* record, uint length) {
  // Find relevance for a specific record
  // This would need to extract doc_id from record
  return 0.0f;
}

void KVTFulltextInfo::close_search() {
  search_state_.doc_scores.clear();
  search_state_.query_terms.clear();
  search_state_.initialized = false;
}

float KVTFulltextInfo::get_relevance() {
  if (search_state_.current_doc != search_state_.doc_scores.end()) {
    return search_state_.current_doc->second;
  }
  return 0.0f;
}

void KVTFulltextInfo::reinit_search() {
  close_search();
}

ulonglong KVTFulltextInfo::get_docid() {
  if (search_state_.current_doc != search_state_.doc_scores.end()) {
    return search_state_.current_doc->first;
  }
  return 0;
}

int KVTFulltextInfo::execute_natural_search() {
  // Parse query into terms (simple tokenization for now)
  std::istringstream iss(query_);
  std::string term;
  while (iss >> term) {
    search_state_.query_terms.push_back(term);
  }
  
  // Load index statistics
  IndexStatistics stats;
  adapter_->load_index_statistics(table_id_, index_id_, stats);
  
  // For each query term, load posting list and calculate scores
  for (const auto& term : search_state_.query_terms) {
    std::vector<PostingEntry> posting_list;
    if (adapter_->load_posting_list(table_id_, index_id_, term, posting_list) != 0) {
      continue; // Term not found
    }
    
    uint64_t df = posting_list.size(); // Document frequency
    
    for (const auto& entry : posting_list) {
      DocumentMetadata doc_meta;
      adapter_->load_document_metadata(table_id_, index_id_, entry.doc_id, doc_meta);
      
      float score = calculate_bm25_score(term, entry, df, doc_meta);
      search_state_.doc_scores[entry.doc_id] += score;
    }
  }
  
  return 0;
}

int KVTFulltextInfo::execute_boolean_search() {
  // Parse boolean query
  if (parse_boolean_query(query_.c_str(), query_.length()) != 0) {
    return HA_ERR_FTS_INVALID_QUERY;
  }
  
  IndexStatistics stats;
  adapter_->load_index_statistics(table_id_, index_id_, stats);
  
  std::set<uint64_t> must_have_docs;
  std::set<uint64_t> must_not_have_docs;
  bool first_must = true;
  
  // Process MUST HAVE terms (+)
  for (const auto& term : boolean_query_.must_have) {
    std::vector<PostingEntry> posting_list;
    if (adapter_->load_posting_list(table_id_, index_id_, term, posting_list) != 0) {
      // Required term not found, no results
      return 0;
    }
    
    std::set<uint64_t> term_docs;
    for (const auto& entry : posting_list) {
      term_docs.insert(entry.doc_id);
    }
    
    if (first_must) {
      must_have_docs = term_docs;
      first_must = false;
    } else {
      // Intersection with existing must_have docs
      std::set<uint64_t> intersection;
      std::set_intersection(must_have_docs.begin(), must_have_docs.end(),
                           term_docs.begin(), term_docs.end(),
                           std::inserter(intersection, intersection.begin()));
      must_have_docs = intersection;
    }
  }
  
  // Process MUST NOT HAVE terms (-)
  for (const auto& term : boolean_query_.must_not_have) {
    std::vector<PostingEntry> posting_list;
    if (adapter_->load_posting_list(table_id_, index_id_, term, posting_list) == 0) {
      for (const auto& entry : posting_list) {
        must_not_have_docs.insert(entry.doc_id);
      }
    }
  }
  
  // Process OPTIONAL terms and calculate scores
  std::set<uint64_t> candidate_docs = must_have_docs;
  
  // If no must_have terms, consider all docs with optional terms
  if (boolean_query_.must_have.empty()) {
    for (const auto& term : boolean_query_.optional) {
      std::vector<PostingEntry> posting_list;
      if (adapter_->load_posting_list(table_id_, index_id_, term, posting_list) == 0) {
        for (const auto& entry : posting_list) {
          candidate_docs.insert(entry.doc_id);
        }
      }
    }
  }
  
  // Calculate scores for candidate docs, excluding must_not_have
  for (uint64_t doc_id : candidate_docs) {
    if (must_not_have_docs.count(doc_id) > 0) {
      continue; // Exclude this document
    }
    
    float score = 0.0f;
    
    // Score based on all query terms (must_have and optional)
    std::vector<std::string> all_terms = boolean_query_.must_have;
    all_terms.insert(all_terms.end(), 
                     boolean_query_.optional.begin(), 
                     boolean_query_.optional.end());
    
    for (const auto& term : all_terms) {
      std::vector<PostingEntry> posting_list;
      if (adapter_->load_posting_list(table_id_, index_id_, term, posting_list) != 0) {
        continue;
      }
      
      // Find entry for this doc
      auto it = std::find_if(posting_list.begin(), posting_list.end(),
                            [doc_id](const PostingEntry& e) { return e.doc_id == doc_id; });
      if (it != posting_list.end()) {
        DocumentMetadata doc_meta;
        adapter_->load_document_metadata(table_id_, index_id_, doc_id, doc_meta);
        score += calculate_bm25_score(term, *it, posting_list.size(), doc_meta);
      }
    }
    
    if (score > 0) {
      search_state_.doc_scores[doc_id] = score;
    }
  }
  
  return 0;
}

float KVTFulltextInfo::calculate_bm25_score(const std::string& term, 
                                           const PostingEntry& entry,
                                           uint64_t df, 
                                           const DocumentMetadata& doc_meta) {
  IndexStatistics stats;
  adapter_->load_index_statistics(table_id_, index_id_, stats);
  
  // IDF calculation
  float idf = log((stats.num_docs - df + 0.5f) / (df + 0.5f));
  
  // BM25 score
  float tf = entry.term_frequency;
  float doc_len = doc_meta.doc_length;
  float avg_doc_len = stats.avg_doc_length;
  
  float score = idf * (tf * (BM25_K1 + 1)) / 
                (tf + BM25_K1 * (1 - BM25_B + BM25_B * doc_len / avg_doc_len));
  
  return score;
}

int KVTFulltextInfo::parse_boolean_query(const char* query, uint query_len) {
  // Simple boolean query parsing
  // This should ideally use the MariaDB parser, but for now we do simple parsing
  std::string query_str(query, query_len);
  std::istringstream iss(query_str);
  std::string token;
  
  while (iss >> token) {
    if (token.empty()) continue;
    
    if (token[0] == '+') {
      // Must have
      boolean_query_.must_have.push_back(token.substr(1));
    } else if (token[0] == '-') {
      // Must not have
      boolean_query_.must_not_have.push_back(token.substr(1));
    } else {
      // Optional
      boolean_query_.optional.push_back(token);
    }
  }
  
  return 0;
}

// KVTFulltextAdapter implementation
KVTFulltextAdapter::KVTFulltextAdapter() {
  default_parser_ = &ft_default_parser;
}

KVTFulltextAdapter::~KVTFulltextAdapter() {
}

KVTFulltextAdapter* KVTFulltextAdapter::get_instance() {
  std::lock_guard<std::mutex> lock(instance_mutex_);
  if (!instance_) {
    instance_ = new KVTFulltextAdapter();
  }
  return instance_;
}

int KVTFulltextAdapter::create_fulltext_index(uint64_t table_id, uint32_t index_id,
                                             const FTSConfig& config) {
  // Save configuration
  if (save_config(table_id, index_id, config) != 0) {
    return -1;
  }
  
  // Initialize index statistics
  IndexStatistics stats;
  std::string stats_key = make_stats_key(table_id, index_id);
  std::string stats_value = serialize_statistics(stats);
  
  int ret = kvt_set(KVT_DEFAULT_TXN, stats_key.c_str(), stats_key.length(),
                   stats_value.c_str(), stats_value.length());
  
  // Cache the configuration
  std::lock_guard<std::mutex> lock(cache_mutex_);
  config_cache_[std::make_pair(table_id, index_id)] = config;
  
  return ret;
}

int KVTFulltextAdapter::drop_fulltext_index(uint64_t table_id, uint32_t index_id) {
  // Remove from cache
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    config_cache_.erase(std::make_pair(table_id, index_id));
  }
  
  // Delete all index data (would need to scan and delete all keys with prefix)
  // For now, just delete config and stats
  std::string config_key = make_config_key(table_id, index_id);
  std::string stats_key = make_stats_key(table_id, index_id);
  
  kvt_delete(KVT_DEFAULT_TXN, config_key.c_str(), config_key.length());
  kvt_delete(KVT_DEFAULT_TXN, stats_key.c_str(), stats_key.length());
  
  return 0;
}

int KVTFulltextAdapter::index_document(uint64_t table_id, uint32_t index_id,
                                      uint64_t doc_id, const char* text, uint text_len) {
  // Load configuration
  FTSConfig config;
  if (load_config(table_id, index_id, config) != 0) {
    return -1;
  }
  
  // Set up parser context
  IndexContext ctx(this, table_id, index_id, doc_id);
  
  // Set up parser parameters
  MYSQL_FTPARSER_PARAM param;
  memset(&param, 0, sizeof(param));
  param.mysql_add_word = add_word_to_index;
  param.ftparser_state = &ctx;
  param.cs = &my_charset_utf8mb3_general_ci; // Default charset
  param.doc = text;
  param.length = text_len;
  param.mode = MYSQL_FTPARSER_SIMPLE_MODE;
  
  // Parse the document
  if (default_parser_->parse(&param) != 0) {
    return -1;
  }
  
  // Store posting entries for each term
  for (const auto& term_entry : ctx.term_positions) {
    PostingEntry posting;
    posting.doc_id = doc_id;
    posting.positions = term_entry.second;
    posting.term_frequency = term_entry.second.size();
    
    if (store_posting_entry(table_id, index_id, term_entry.first, 
                          doc_id, term_entry.second) != 0) {
      return -1;
    }
  }
  
  // Store document metadata
  DocumentMetadata meta;
  meta.doc_id = doc_id;
  meta.doc_length = ctx.current_position;
  meta.num_unique_terms = ctx.term_positions.size();
  meta.norm_factor = 1.0f / sqrt(meta.doc_length);
  
  if (store_document_metadata(table_id, index_id, doc_id, meta) != 0) {
    return -1;
  }
  
  // Update index statistics
  update_index_statistics(table_id, index_id, 1, ctx.term_positions.size());
  
  return 0;
}

int KVTFulltextAdapter::remove_document(uint64_t table_id, uint32_t index_id, 
                                       uint64_t doc_id) {
  // Load document metadata to get term count
  DocumentMetadata meta;
  if (load_document_metadata(table_id, index_id, doc_id, meta) != 0) {
    return -1;
  }
  
  // Remove document metadata
  std::string doc_key = make_doc_key(table_id, index_id, doc_id);
  kvt_delete(KVT_DEFAULT_TXN, doc_key.c_str(), doc_key.length());
  
  // Update statistics
  update_index_statistics(table_id, index_id, -1, -meta.num_unique_terms);
  
  // Note: We should also remove the document from all posting lists
  // This requires scanning all terms, which is expensive
  // In practice, we might mark documents as deleted and clean up periodically
  
  return 0;
}

int KVTFulltextAdapter::update_document(uint64_t table_id, uint32_t index_id,
                                       uint64_t doc_id, const char* old_text, uint old_len,
                                       const char* new_text, uint new_len) {
  // Simple implementation: remove old and index new
  remove_document(table_id, index_id, doc_id);
  return index_document(table_id, index_id, doc_id, new_text, new_len);
}

FT_INFO* KVTFulltextAdapter::init_search(uint64_t table_id, uint32_t index_id,
                                        uint flags, const char* query, uint query_len,
                                        CHARSET_INFO* cs) {
  return new KVTFulltextInfo(this, table_id, index_id, flags, query, query_len);
}

int KVTFulltextAdapter::add_word_to_index(MYSQL_FTPARSER_PARAM* param,
                                         const char* word, int word_len,
                                         MYSQL_FTPARSER_BOOLEAN_INFO* info) {
  IndexContext* ctx = (IndexContext*)param->ftparser_state;
  
  // Check word length
  if (word_len < (int)ft_min_word_len || word_len > (int)ft_max_word_len) {
    return 0; // Skip this word
  }
  
  // Check if stopword
  if (is_stopword_check(word, word_len)) {
    return 0; // Skip stopword
  }
  
  // Normalize term (lowercase, etc.)
  std::string term = normalize_term(word, word_len, param->cs);
  
  // Add to term positions
  ctx->term_positions[term].push_back(ctx->current_position++);
  
  return 0;
}

int KVTFulltextAdapter::load_posting_list(uint64_t table_id, uint32_t index_id,
                                         const std::string& term, 
                                         std::vector<PostingEntry>& entries) {
  std::string key = make_term_key(table_id, index_id, term);
  
  char value[65536]; // Max value size
  size_t value_len = sizeof(value);
  
  int ret = kvt_get(KVT_DEFAULT_TXN, key.c_str(), key.length(), value, &value_len);
  if (ret == KVT_KEY_NOT_FOUND) {
    return 0; // Term not found, empty posting list
  }
  if (ret != 0) {
    return ret;
  }
  
  std::string data(value, value_len);
  return deserialize_posting_list(data, entries);
}

int KVTFulltextAdapter::store_posting_entry(uint64_t table_id, uint32_t index_id,
                                           const std::string& term, uint64_t doc_id,
                                           const std::vector<uint32_t>& positions) {
  // Load existing posting list
  std::vector<PostingEntry> entries;
  load_posting_list(table_id, index_id, term, entries);
  
  // Add or update entry for this document
  bool found = false;
  for (auto& entry : entries) {
    if (entry.doc_id == doc_id) {
      entry.positions = positions;
      entry.term_frequency = positions.size();
      found = true;
      break;
    }
  }
  
  if (!found) {
    PostingEntry new_entry;
    new_entry.doc_id = doc_id;
    new_entry.positions = positions;
    new_entry.term_frequency = positions.size();
    entries.push_back(new_entry);
  }
  
  // Sort by doc_id for efficient access
  std::sort(entries.begin(), entries.end(), 
           [](const PostingEntry& a, const PostingEntry& b) {
             return a.doc_id < b.doc_id;
           });
  
  // Serialize and store
  std::string key = make_term_key(table_id, index_id, term);
  std::string value = serialize_posting_list(entries);
  
  return kvt_set(KVT_DEFAULT_TXN, key.c_str(), key.length(),
                value.c_str(), value.length());
}

int KVTFulltextAdapter::load_document_metadata(uint64_t table_id, uint32_t index_id,
                                              uint64_t doc_id, DocumentMetadata& meta) {
  std::string key = make_doc_key(table_id, index_id, doc_id);
  
  char value[1024];
  size_t value_len = sizeof(value);
  
  int ret = kvt_get(KVT_DEFAULT_TXN, key.c_str(), key.length(), value, &value_len);
  if (ret != 0) {
    return ret;
  }
  
  std::string data(value, value_len);
  return deserialize_metadata(data, meta);
}

int KVTFulltextAdapter::store_document_metadata(uint64_t table_id, uint32_t index_id,
                                               uint64_t doc_id, const DocumentMetadata& meta) {
  std::string key = make_doc_key(table_id, index_id, doc_id);
  std::string value = serialize_metadata(meta);
  
  return kvt_set(KVT_DEFAULT_TXN, key.c_str(), key.length(),
                value.c_str(), value.length());
}

int KVTFulltextAdapter::load_index_statistics(uint64_t table_id, uint32_t index_id,
                                             IndexStatistics& stats) {
  std::string key = make_stats_key(table_id, index_id);
  
  char value[256];
  size_t value_len = sizeof(value);
  
  int ret = kvt_get(KVT_DEFAULT_TXN, key.c_str(), key.length(), value, &value_len);
  if (ret == KVT_KEY_NOT_FOUND) {
    // Initialize empty stats
    stats = IndexStatistics();
    return 0;
  }
  if (ret != 0) {
    return ret;
  }
  
  std::string data(value, value_len);
  return deserialize_statistics(data, stats);
}

int KVTFulltextAdapter::update_index_statistics(uint64_t table_id, uint32_t index_id,
                                               int64_t doc_delta, int64_t term_delta) {
  IndexStatistics stats;
  load_index_statistics(table_id, index_id, stats);
  
  stats.num_docs += doc_delta;
  stats.total_terms += term_delta;
  if (stats.num_docs > 0) {
    stats.avg_doc_length = (float)stats.total_terms / stats.num_docs;
  }
  
  std::string key = make_stats_key(table_id, index_id);
  std::string value = serialize_statistics(stats);
  
  return kvt_set(KVT_DEFAULT_TXN, key.c_str(), key.length(),
                value.c_str(), value.length());
}

int KVTFulltextAdapter::load_config(uint64_t table_id, uint32_t index_id, FTSConfig& config) {
  // Check cache first
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = config_cache_.find(std::make_pair(table_id, index_id));
    if (it != config_cache_.end()) {
      config = it->second;
      return 0;
    }
  }
  
  // Load from KVT
  std::string key = make_config_key(table_id, index_id);
  char value[512];
  size_t value_len = sizeof(value);
  
  int ret = kvt_get(KVT_DEFAULT_TXN, key.c_str(), key.length(), value, &value_len);
  if (ret != 0) {
    return ret;
  }
  
  // Simple deserialization
  std::string data(value, value_len);
  std::istringstream iss(data);
  iss >> config.parser_name >> config.min_word_len >> config.max_word_len >> config.with_positions;
  
  // Cache it
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    config_cache_[std::make_pair(table_id, index_id)] = config;
  }
  
  return 0;
}

int KVTFulltextAdapter::save_config(uint64_t table_id, uint32_t index_id, const FTSConfig& config) {
  std::string key = make_config_key(table_id, index_id);
  
  // Simple serialization
  std::ostringstream oss;
  oss << config.parser_name << " " << config.min_word_len << " " 
      << config.max_word_len << " " << config.with_positions;
  std::string value = oss.str();
  
  return kvt_set(KVT_DEFAULT_TXN, key.c_str(), key.length(),
                value.c_str(), value.length());
}

// Key generation methods
std::string KVTFulltextAdapter::make_term_key(uint64_t table_id, uint32_t index_id, 
                                             const std::string& term) {
  std::ostringstream oss;
  oss << "__fts." << table_id << "." << index_id << ":term:" << term;
  return oss.str();
}

std::string KVTFulltextAdapter::make_doc_key(uint64_t table_id, uint32_t index_id, 
                                            uint64_t doc_id) {
  std::ostringstream oss;
  oss << "__fts." << table_id << "." << index_id << ":doc:" << doc_id;
  return oss.str();
}

std::string KVTFulltextAdapter::make_stats_key(uint64_t table_id, uint32_t index_id) {
  std::ostringstream oss;
  oss << "__fts." << table_id << "." << index_id << ":stats";
  return oss.str();
}

std::string KVTFulltextAdapter::make_config_key(uint64_t table_id, uint32_t index_id) {
  std::ostringstream oss;
  oss << "__fts." << table_id << "." << index_id << ":config";
  return oss.str();
}

// Serialization methods
std::string KVTFulltextAdapter::serialize_posting_list(const std::vector<PostingEntry>& entries) {
  std::ostringstream oss;
  oss << entries.size() << " ";
  for (const auto& entry : entries) {
    oss << entry.doc_id << " " << entry.term_frequency << " " << entry.positions.size() << " ";
    for (uint32_t pos : entry.positions) {
      oss << pos << " ";
    }
  }
  return oss.str();
}

int KVTFulltextAdapter::deserialize_posting_list(const std::string& data, 
                                                std::vector<PostingEntry>& entries) {
  std::istringstream iss(data);
  size_t num_entries;
  iss >> num_entries;
  
  entries.clear();
  entries.reserve(num_entries);
  
  for (size_t i = 0; i < num_entries; i++) {
    PostingEntry entry;
    size_t num_positions;
    iss >> entry.doc_id >> entry.term_frequency >> num_positions;
    
    entry.positions.reserve(num_positions);
    for (size_t j = 0; j < num_positions; j++) {
      uint32_t pos;
      iss >> pos;
      entry.positions.push_back(pos);
    }
    
    entries.push_back(entry);
  }
  
  return 0;
}

std::string KVTFulltextAdapter::serialize_metadata(const DocumentMetadata& meta) {
  std::ostringstream oss;
  oss << meta.doc_id << " " << meta.doc_length << " " 
      << meta.num_unique_terms << " " << meta.norm_factor;
  return oss.str();
}

int KVTFulltextAdapter::deserialize_metadata(const std::string& data, DocumentMetadata& meta) {
  std::istringstream iss(data);
  iss >> meta.doc_id >> meta.doc_length >> meta.num_unique_terms >> meta.norm_factor;
  return 0;
}

std::string KVTFulltextAdapter::serialize_statistics(const IndexStatistics& stats) {
  std::ostringstream oss;
  oss << stats.num_docs << " " << stats.total_terms << " " << stats.avg_doc_length;
  return oss.str();
}

int KVTFulltextAdapter::deserialize_statistics(const std::string& data, IndexStatistics& stats) {
  std::istringstream iss(data);
  iss >> stats.num_docs >> stats.total_terms >> stats.avg_doc_length;
  return 0;
}

// Helper functions
bool is_stopword_check(const char* word, size_t len) {
  // Use MariaDB's stopword checking
  extern int is_stopword(const char*, size_t);
  return is_stopword(word, len) != 0;
}

std::string normalize_term(const char* word, size_t len, CHARSET_INFO* cs) {
  // Simple normalization: convert to lowercase
  std::string term(word, len);
  std::transform(term.begin(), term.end(), term.begin(), ::tolower);
  return term;
}

} // namespace kvt_fts