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

#ifndef HA_KVT_H
#define HA_KVT_H

#include "my_global.h"
#include "thr_lock.h"
#include "handler.h"
#include "my_base.h"
#include "kvt/kvt_inc.h"
#include "kvt_constants.h"
#include "kvt_catalog.h"
#include "kvt_row_codec.h"
#include <string>
#include <memory>

class ha_kvt: public handler
{
  THR_LOCK_DATA lock;
  uint64_t kvt_data_table_id;  // KVT table ID for the database's data
  uint64_t kvt_tx_id;          // Current transaction ID
  bool is_delayed_insert;
  bool doing_bulk_insert;
  ha_rows bulk_insert_rows;
  KVTBatchOps batch_operations;  // KVTBatchOps is already std::vector<KVTOp>
  
  // Table metadata
  std::string database_name;
  std::string table_name;
  std::unique_ptr<kvt_catalog::TableMetadata> table_metadata;
  std::unique_ptr<kvt_row_codec::RowCodec> row_codec;
  
  // For auto-increment
  uint64_t next_rowid;
  
  // Full-text search
  FT_INFO *ft_handler;
  
  // Spatial index support
  class SpatialSearchHandle;
  std::unique_ptr<SpatialSearchHandle> spatial_search;
  
  struct kvt_table_share {
    THR_LOCK lock;
    uint64_t data_table_id;  // KVT table ID for database data
    char *table_name;
    char *database_name;
    uint use_count;
    mysql_mutex_t mutex;
  };
  
  kvt_table_share *share;

public:
  ha_kvt(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_kvt() override = default;

  const char *table_type() const override { return "KVT"; }
  const char *index_type(uint inx) override { return "BTREE"; }
  const char **bas_ext() const;

  ulonglong table_flags() const override
  {
    return (HA_BINLOG_STMT_CAPABLE |
            HA_HAS_RECORDS |
            HA_STATS_RECORDS_IS_EXACT |
            HA_CAN_GEOMETRY |
            HA_CAN_FULLTEXT |
            HA_CAN_SQL_HANDLER |
            HA_PRIMARY_KEY_REQUIRED_FOR_POSITION);
  }

  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return (HA_READ_NEXT | 
            HA_READ_PREV | 
            HA_READ_ORDER | 
            HA_READ_RANGE |
            HA_KEYREAD_ONLY);
  }

  uint max_supported_record_length() const override { return HA_MAX_REC_LENGTH; }
  uint max_supported_keys() const override { return 1; }
  uint max_supported_key_parts() const override { return 16; }
  uint max_supported_key_length() const override { return 3500; }
  uint max_supported_key_part_length() const override { return 3500; }

  int open(const char *name, int mode, uint test_if_locked) override;
  int close() override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *info) override;
  int delete_table(const char *name) override;
  int rename_table(const char* from, const char* to) override;

  int write_row(const uchar *buf) override;
  int update_row(const uchar *old_data, const uchar *new_data) override;
  int delete_row(const uchar *buf) override;

  int rnd_init(bool scan) override;
  int rnd_end() override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  void position(const uchar *record) override;

  // Index operations
  int index_init(uint idx, bool sorted) override;
  int index_end() override;
  int index_read_map(uchar *buf, const uchar *key,
                     key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_next(uchar *buf) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;
  int index_read_last_map(uchar *buf, const uchar *key,
                          key_part_map keypart_map) override;
  
  // Range scan operations
  int read_range_first(const key_range *start_key,
                       const key_range *end_key,
                       bool eq_range, bool sorted) override;
  int read_range_next() override;

  int info(uint) override;
  int extra(enum ha_extra_function operation) override;
  int external_lock(THD *thd, int lock_type) override;
  int start_stmt(THD *thd, thr_lock_type lock_type) override;
  int delete_all_rows() override;
  void start_bulk_insert(ha_rows rows, uint flags = 0) override;
  int end_bulk_insert() override;
  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key, page_range *pages) override;
  int analyze(THD* thd, HA_CHECK_OPT* check_opt) override;
  int optimize(THD* thd, HA_CHECK_OPT* check_opt) override;
  int check(THD* thd, HA_CHECK_OPT* check_opt) override;
  
  // Condition pushdown
  const COND *cond_push(const COND *cond) override;
  void cond_pop() override;
  
  // Index-only scan support
  int extra(enum ha_extra_function operation) override;
  ulong index_flags(uint idx, uint part, bool all_parts) const override;
  
  // Full-text search support
  FT_INFO *ft_init_ext(uint flags, uint inx, String *key) override;
  int ft_read(uchar *buf) override;
  
  // Spatial index support
  int create_spatial_index(KEY* key_info, uint key_nr);
  int drop_spatial_index(uint key_nr);
  int index_read_spatial(uchar* buf, uint index, const uchar* mbr_key, uint mbr_len);
  int spatial_search_init(uint index, const uchar* mbr_key, uint mbr_len, uint flags);
  int spatial_search_next(uchar* buf);

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;

  bool get_error_message(int error, String *buf) override;

private:
  kvt_table_share *get_share(const char *path);
  void free_share(kvt_table_share *share);
  
  // Catalog operations
  int load_table_metadata();
  int store_table_metadata();
  
  // Row operations
  std::string generate_row_key(const uchar *buf);
  std::string generate_data_key(const std::string& row_key);
  int encode_row(const uchar *buf, std::string &value);
  int decode_row(const std::string &value, uchar *buf);
  
  // Error handling
  int map_kvt_error_to_mysql(KVTError kvt_err, const std::string &error_msg);
  
  // Batch operations
  int flush_batch_operations();
  
  // Bulk operations
  void start_bulk_insert_if_needed();
  void end_bulk_insert_if_needed();
  
  // Scan state
  std::vector<std::pair<KVTKey, std::string>> scan_results;
  size_t scan_position;
  std::string current_position_key;
  
  // Pushed conditions
  const COND *pushed_cond;
  KVTProcessFunc pushed_filter_func;  // Pushdown filter function
  bool check_pushed_condition(const uchar *buf);
  
  // Index support
  uint active_index;
  bool index_sorted;
  std::string index_scan_start_key;
  std::string index_scan_end_key;
  std::vector<std::pair<std::string, std::string>> index_scan_results;
  size_t index_scan_position;
  
  // Index-only scan state
  bool index_only_scan_active;
  uint covering_index_id;
  MY_BITMAP* covered_columns_bitmap;
  void* covered_columns_buf;
  
  // Error tracking
  uint last_error_key;
  uint last_dup_key;  // Last duplicate key index for unique constraint violations
  
  // Range scan state
  bool in_range_scan;
  key_range saved_start_key;
  key_range saved_end_key;
  bool range_eq_flag;
  bool range_sorted;
  std::vector<std::pair<KVTKey, std::string>> range_scan_results;
  size_t range_scan_position;
};

#endif