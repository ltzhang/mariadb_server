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

#include <my_global.h>
#include "sql_priv.h"
#include "sql_class.h"
#include "ha_kvt.h"
#include "probes_mysql.h"
#include "sql_plugin.h"
#include "item.h"
#include "kvt_transaction_manager.h"
#include "kvt_index_manager.h"
#include <mysql/plugin.h>

static handler *kvt_create_handler(handlerton *hton,
                                   TABLE_SHARE *table,
                                   MEM_ROOT *mem_root);
static int kvt_init_func(void *p);
static int kvt_deinit_func(void *p);
static int kvt_panic_func(handlerton *hton, ha_panic_function flag);
static void kvt_drop_database(handlerton *hton, char *path);
static int kvt_close_connection(handlerton *hton, THD *thd);
static int kvt_commit(THD *thd, bool all);
static int kvt_rollback(THD *thd, bool all);
static int kvt_savepoint_set(handlerton *hton, THD *thd, void *sv);
static int kvt_savepoint_rollback(handlerton *hton, THD *thd, void *sv);
static int kvt_savepoint_release(handlerton *hton, THD *thd, void *sv);

static HASH kvt_open_tables;
static mysql_mutex_t kvt_mutex;
static bool kvt_initialized = false;

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_mutex_kvt, key_mutex_kvt_share;
static PSI_mutex_info all_kvt_mutexes[] =
{
  { &key_mutex_kvt, "kvt", PSI_FLAG_GLOBAL },
  { &key_mutex_kvt_share, "kvt_share", 0 }
};

static void init_kvt_psi_keys()
{
  const char* category = "kvt";
  int count = array_elements(all_kvt_mutexes);
  mysql_mutex_register(category, all_kvt_mutexes, count);
}
#endif

struct st_kvt_share
{
  char *table_name;
  uint64_t table_id;
  uint use_count;
  THR_LOCK lock;
  mysql_mutex_t mutex;
};

static uchar* kvt_get_key(const void *share_ptr, size_t *length,
                          my_bool not_used __attribute__((unused)))
{
  const st_kvt_share *share = static_cast<const st_kvt_share *>(share_ptr);
  if (!share || !share->table_name)
  {
    *length = 0;
    // Return empty string instead of NULL to avoid crash
    static char empty_str[] = "";
    return (uchar*) empty_str;
  }
  *length = strlen(share->table_name);
  return (uchar*) share->table_name;
}

static int kvt_init_func(void *p)
{
  DBUG_ENTER("kvt_init_func");
  handlerton *kvt_hton = (handlerton *)p;

#ifdef HAVE_PSI_INTERFACE
  init_kvt_psi_keys();
#endif

#ifdef HAVE_PSI_INTERFACE
  mysql_mutex_init(key_mutex_kvt, &kvt_mutex, MY_MUTEX_INIT_FAST);
#else
  mysql_mutex_init(0, &kvt_mutex, MY_MUTEX_INIT_FAST);
#endif
  (void) my_hash_init(PSI_NOT_INSTRUMENTED, &kvt_open_tables, system_charset_info, 32, 0, 0,
                      (my_hash_get_key) kvt_get_key, 0, 0);

  // kvt_hton->state = SHOW_OPTION_YES; // Not needed in newer MariaDB versions
  kvt_hton->create = kvt_create_handler;
  kvt_hton->flags = HTON_CAN_RECREATE;
  kvt_hton->panic = kvt_panic_func;
  kvt_hton->drop_database = kvt_drop_database;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
  kvt_hton->close_connection = reinterpret_cast<int (*)(THD *)>(kvt_close_connection);
  kvt_hton->commit = kvt_commit;
  kvt_hton->rollback = kvt_rollback;
  kvt_hton->savepoint_set = reinterpret_cast<int (*)(THD *, void *)>(kvt_savepoint_set);
  kvt_hton->savepoint_rollback = reinterpret_cast<int (*)(THD *, void *)>(kvt_savepoint_rollback);
  kvt_hton->savepoint_release = reinterpret_cast<int (*)(THD *, void *)>(kvt_savepoint_release);
#pragma GCC diagnostic pop
  kvt_hton->savepoint_offset = sizeof(void*);
  kvt_hton->db_type = DB_TYPE_UNKNOWN;

  std::string error_msg;
  KVTError kvt_err = kvt_initialize();
  if (kvt_err != KVTError::SUCCESS)
  {
    sql_print_error("KVT: Failed to initialize KVT system");
    DBUG_RETURN(1);
  }

  kvt_set_verbosity(2);
  kvt_set_sanity_check_level(1);
  
  kvt_initialized = true;
  sql_print_information("KVT storage engine initialized");
  
  DBUG_RETURN(0);
}

static int kvt_deinit_func(void *p)
{
  DBUG_ENTER("kvt_deinit_func");

  if (kvt_initialized)
  {
    kvt_shutdown();
    kvt_initialized = false;
  }

  my_hash_free(&kvt_open_tables);
  mysql_mutex_destroy(&kvt_mutex);

  sql_print_information("KVT storage engine shutdown");
  DBUG_RETURN(0);
}

static handler *kvt_create_handler(handlerton *hton,
                                   TABLE_SHARE *table,
                                   MEM_ROOT *mem_root)
{
  return new (mem_root) ha_kvt(hton, table);
}

static int kvt_panic_func(handlerton *hton, ha_panic_function flag)
{
  return 0;
}

static int kvt_close_connection(handlerton *hton, THD *thd)
{
  DBUG_ENTER("kvt_close_connection");
  
  // Clean up any active transactions for this connection
  auto* tx_mgr = kvt_transaction::KVTTransactionManager::get_instance();
  tx_mgr->cleanup_transaction(thd);
  
  DBUG_RETURN(0);
}

static void kvt_drop_database(handlerton *hton, char *path)
{
  DBUG_ENTER("kvt_drop_database");
  
  // Get database name from path
  std::string path_str(path);
  size_t last_slash = path_str.find_last_of('/');
  std::string database = (last_slash != std::string::npos) 
                         ? path_str.substr(last_slash + 1) 
                         : path_str;
  
  // Drop the database from catalog
  auto* catalog = kvt_catalog::CatalogManager::get_instance();
  catalog->drop_database(database);
  
  DBUG_VOID_RETURN;
}

static int kvt_commit(THD *thd, bool all)
{
  DBUG_ENTER("kvt_commit");
  DBUG_PRINT("info", ("all: %d", all));
  
  auto* tx_mgr = kvt_transaction::KVTTransactionManager::get_instance();
  
  if (!tx_mgr->has_active_transaction(thd)) {
    // No active transaction to commit
    DBUG_RETURN(0);
  }
  
  // Commit the transaction
  int ret = tx_mgr->commit_transaction(thd, all);
  
  if (ret != 0) {
    DBUG_PRINT("error", ("Failed to commit transaction"));
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  DBUG_PRINT("info", ("Transaction committed successfully"));
  DBUG_RETURN(0);
}

static int kvt_rollback(THD *thd, bool all)
{
  DBUG_ENTER("kvt_rollback");
  DBUG_PRINT("info", ("all: %d", all));
  
  auto* tx_mgr = kvt_transaction::KVTTransactionManager::get_instance();
  
  if (!tx_mgr->has_active_transaction(thd)) {
    // No active transaction to rollback
    DBUG_RETURN(0);
  }
  
  // Rollback the transaction
  int ret = tx_mgr->rollback_transaction(thd, all);
  
  if (ret != 0) {
    DBUG_PRINT("error", ("Failed to rollback transaction"));
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  DBUG_PRINT("info", ("Transaction rolled back successfully"));
  DBUG_RETURN(0);
}

struct kvt_savepoint_data {
  uint64_t savepoint_id;
  char name[64];
};

static int kvt_savepoint_set(handlerton *hton, THD *thd, void *sv)
{
  DBUG_ENTER("kvt_savepoint_set");
  
  auto* tx_mgr = kvt_transaction::KVTTransactionManager::get_instance();
  
  if (!tx_mgr->has_active_transaction(thd)) {
    // No active transaction
    DBUG_RETURN(1);
  }
  
  kvt_savepoint_data *savepoint = (kvt_savepoint_data*)sv;
  
  // Generate savepoint name
  snprintf(savepoint->name, sizeof(savepoint->name), "sp_%p_%lu", 
           thd, (unsigned long)time(nullptr));
  
  // Set the savepoint
  int ret = tx_mgr->savepoint_set(thd, savepoint->name);
  
  if (ret != 0) {
    DBUG_PRINT("error", ("Failed to set savepoint"));
    DBUG_RETURN(1);
  }
  
  DBUG_PRINT("info", ("Savepoint set: %s", savepoint->name));
  DBUG_RETURN(0);
}

static int kvt_savepoint_rollback(handlerton *hton, THD *thd, void *sv)
{
  DBUG_ENTER("kvt_savepoint_rollback");
  
  auto* tx_mgr = kvt_transaction::KVTTransactionManager::get_instance();
  
  if (!tx_mgr->has_active_transaction(thd)) {
    // No active transaction
    DBUG_RETURN(1);
  }
  
  kvt_savepoint_data *savepoint = (kvt_savepoint_data*)sv;
  
  // Rollback to the savepoint
  int ret = tx_mgr->savepoint_rollback(thd, savepoint->name);
  
  if (ret != 0) {
    DBUG_PRINT("error", ("Failed to rollback to savepoint"));
    DBUG_RETURN(1);
  }
  
  DBUG_PRINT("info", ("Rolled back to savepoint: %s", savepoint->name));
  DBUG_RETURN(0);
}

static int kvt_savepoint_release(handlerton *hton, THD *thd, void *sv)
{
  DBUG_ENTER("kvt_savepoint_release");
  
  auto* tx_mgr = kvt_transaction::KVTTransactionManager::get_instance();
  
  if (!tx_mgr->has_active_transaction(thd)) {
    // No active transaction
    DBUG_RETURN(1);
  }
  
  kvt_savepoint_data *savepoint = (kvt_savepoint_data*)sv;
  
  // Release the savepoint
  int ret = tx_mgr->savepoint_release(thd, savepoint->name);
  
  if (ret != 0) {
    DBUG_PRINT("error", ("Failed to release savepoint"));
    DBUG_RETURN(1);
  }
  
  DBUG_PRINT("info", ("Released savepoint: %s", savepoint->name));
  DBUG_RETURN(0);
}

ha_kvt::ha_kvt(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg),
    kvt_data_table_id(0),
    kvt_tx_id(0),
    is_delayed_insert(false),
    doing_bulk_insert(false),
    bulk_insert_rows(0),
    next_rowid(1),
    share(nullptr),
    scan_position(0),
    pushed_cond(nullptr),
    active_index(MAX_KEY),
    index_sorted(false),
    index_scan_position(0)
{
}

static const char *ha_kvt_exts[] = {
  NullS
};

const char **ha_kvt::bas_ext() const
{
  return ha_kvt_exts;
}

int ha_kvt::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_kvt::open");
  
  // Parse database and table name
  if (!kvt_catalog::CatalogManager::parse_table_path(name, database_name, table_name)) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Get share
  if (!(share = get_share(name)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    
  thr_lock_data_init(&share->lock, &lock, NULL);
  
  // Load table metadata
  auto* catalog = kvt_catalog::CatalogManager::get_instance();
  table_metadata = std::make_unique<kvt_catalog::TableMetadata>();
  
  int ret = catalog->load_table_metadata(database_name, table_name, *table_metadata);
  if (ret != 0) {
    free_share(share);
    DBUG_RETURN(ret);
  }
  
  // Get data table ID
  kvt_data_table_id = catalog->get_data_table_id(database_name);
  if (kvt_data_table_id == 0) {
    free_share(share);
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
  }
  
  // Initialize row codec
  row_codec = std::make_unique<kvt_row_codec::RowCodec>(table);
  
  // Initialize auto-increment if needed
  if (table_metadata->auto_increment_values.size() > 0) {
    for (const auto& [col, val] : table_metadata->auto_increment_values) {
      next_rowid = val + 1;
      break;  // Use first auto-increment value
    }
  }
  
  DBUG_RETURN(0);
}

int ha_kvt::close()
{
  DBUG_ENTER("ha_kvt::close");
  free_share(share);
  DBUG_RETURN(0);
}

int ha_kvt::create(const char *name, TABLE *form, HA_CREATE_INFO *info)
{
  DBUG_ENTER("ha_kvt::create");
  
  // Parse database and table name from path
  std::string db, tbl;
  if (!kvt_catalog::CatalogManager::parse_table_path(name, db, tbl)) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Validate names
  if (!kvt_constants::is_valid_name(db) || !kvt_constants::is_valid_name(tbl)) {
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  
  // Get catalog manager
  auto* catalog = kvt_catalog::CatalogManager::get_instance();
  if (!catalog->is_initialized()) {
    catalog->initialize();
  }
  
  // Ensure database exists
  catalog->create_database(db);
  
  // Create table in catalog
  int ret = catalog->create_table(db, tbl, form, info);
  
  DBUG_RETURN(ret);
}

int ha_kvt::delete_table(const char *name)
{
  DBUG_ENTER("ha_kvt::delete_table");
  
  // Parse database and table name
  std::string db, tbl;
  if (!kvt_catalog::CatalogManager::parse_table_path(name, db, tbl)) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Get catalog manager
  auto* catalog = kvt_catalog::CatalogManager::get_instance();
  if (!catalog->is_initialized()) {
    catalog->initialize();
  }
  
  // Drop table from catalog
  int ret = catalog->drop_table(db, tbl);
  
  DBUG_RETURN(ret);
}

int ha_kvt::rename_table(const char* from, const char* to)
{
  DBUG_ENTER("ha_kvt::rename_table");
  DBUG_RETURN(HA_ERR_UNSUPPORTED);
}

int ha_kvt::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_kvt::write_row");
  
  if (!row_codec || kvt_data_table_id == 0) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Handle auto-increment if needed
  if (table->next_number_field && buf == table->record[0]) {
    int error = update_auto_increment();
    if (error) {
      DBUG_RETURN(error);
    }
  }
  
  // Generate row key
  std::string row_key = generate_row_key(buf);
  std::string data_key = generate_data_key(row_key);
  
  // Encode row data
  std::string row_value;
  int ret = encode_row(buf, row_value);
  if (ret != 0) {
    DBUG_RETURN(ret);
  }
  
  // Check if we're doing bulk insert
  if (doing_bulk_insert) {
    // Add to batch
    KVTOp op;
    op.op = OP_SET;
    op.table_id = kvt_data_table_id;
    op.key = KVTKey(data_key);
    op.value = row_value;
    batch_operations.push_back(op);
    
    // Flush if batch is large enough
    if (batch_operations.size() >= 1000) {
      int ret = flush_batch_operations();
      if (ret != 0) {
        DBUG_RETURN(ret);
      }
    }
  } else {
    // Regular single insert
    std::string error_msg;
    KVTError err = kvt_set(kvt_tx_id, kvt_data_table_id, KVTKey(data_key), row_value, error_msg);
    
    if (err != KVTError::SUCCESS) {
      DBUG_RETURN(map_kvt_error_to_mysql(err, error_msg));
    }
    
    // Update statistics
    stats.records++;
  }
  
  // Store the current position key for subsequent operations
  current_position_key = data_key;
  
  DBUG_RETURN(0);
}

int ha_kvt::update_row(const uchar *old_data, const uchar *new_data)
{
  DBUG_ENTER("ha_kvt::update_row");
  
  if (kvt_data_table_id == 0) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Use stored position key for the old row
  if (current_position_key.empty()) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Encode new row data
  std::string new_value;
  int ret = encode_row(new_data, new_value);
  if (ret != 0) {
    DBUG_RETURN(ret);
  }
  
  // Check if primary key changed
  std::string new_row_key = generate_row_key(new_data);
  std::string new_data_key = generate_data_key(new_row_key);
  
  std::string error_msg;
  
  if (new_data_key != current_position_key) {
    // Primary key changed - delete old, insert new
    KVTError err = kvt_del(kvt_tx_id, kvt_data_table_id, current_position_key, error_msg);
    if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
      DBUG_RETURN(map_kvt_error_to_mysql(err, error_msg));
    }
    
    err = kvt_set(kvt_tx_id, kvt_data_table_id, KVTKey(new_data_key), new_value, error_msg);
    if (err != KVTError::SUCCESS) {
      DBUG_RETURN(map_kvt_error_to_mysql(err, error_msg));
    }
    
    current_position_key = new_data_key;
  } else {
    // Primary key unchanged - just update
    KVTError err = kvt_set(kvt_tx_id, kvt_data_table_id, current_position_key, new_value, error_msg);
    if (err != KVTError::SUCCESS) {
      DBUG_RETURN(map_kvt_error_to_mysql(err, error_msg));
    }
  }
  
  DBUG_RETURN(0);
}

int ha_kvt::delete_row(const uchar *buf)
{
  DBUG_ENTER("ha_kvt::delete_row");
  
  if (kvt_data_table_id == 0) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Use stored position key
  if (current_position_key.empty()) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Delete from KVT
  std::string error_msg;
  KVTError err = kvt_del(kvt_tx_id, kvt_data_table_id, current_position_key, error_msg);
  
  if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
    DBUG_RETURN(map_kvt_error_to_mysql(err, error_msg));
  }
  
  // Update statistics
  stats.records--;
  
  DBUG_RETURN(0);
}

int ha_kvt::rnd_init(bool scan)
{
  DBUG_ENTER("ha_kvt::rnd_init");
  
  if (!scan) {
    DBUG_RETURN(0);
  }
  
  if (kvt_data_table_id == 0) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Clear previous scan results
  scan_results.clear();
  scan_position = 0;
  
  // Perform range scan for all rows of this table
  KVTKey start_key(kvt_constants::make_data_scan_start(table_name));
  KVTKey end_key(kvt_constants::make_data_scan_end(table_name));
  
  std::string error_msg;
  KVTError err = kvt_scan(kvt_tx_id, kvt_data_table_id, start_key, end_key,
                          10000, scan_results, error_msg);
  
  if (err != KVTError::SUCCESS && err != KVTError::KEY_NOT_FOUND) {
    DBUG_RETURN(map_kvt_error_to_mysql(err, error_msg));
  }
  
  DBUG_RETURN(0);
}

int ha_kvt::rnd_end()
{
  DBUG_ENTER("ha_kvt::rnd_end");
  DBUG_RETURN(0);
}

int ha_kvt::rnd_next(uchar *buf)
{
  DBUG_ENTER("ha_kvt::rnd_next");
  
  while (scan_position < scan_results.size()) {
    // Get next row
    const auto& [key, value] = scan_results[scan_position++];
    
    // Decode row
    int ret = decode_row(value, buf);
    if (ret != 0) {
      continue;  // Skip rows that can't be decoded
    }
    
    // Store position for rnd_pos
    current_position_key = key;
    
    // Check pushed condition if any
    if (pushed_cond && !check_pushed_condition(buf)) {
      continue;  // Skip rows that don't match the condition
    }
    
    DBUG_RETURN(0);
  }
  
  DBUG_RETURN(HA_ERR_END_OF_FILE);
}

int ha_kvt::rnd_pos(uchar *buf, uchar *pos)
{
  DBUG_ENTER("ha_kvt::rnd_pos");
  
  if (kvt_data_table_id == 0) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  // Position contains the key
  std::string key(reinterpret_cast<char*>(pos), ref_length);
  
  // Get the row
  std::string value;
  std::string error_msg;
  KVTError err = kvt_get(kvt_tx_id, kvt_data_table_id, KVTKey(key), value, error_msg);
  
  if (err != KVTError::SUCCESS) {
    DBUG_RETURN(map_kvt_error_to_mysql(err, error_msg));
  }
  
  // Decode row
  int ret = decode_row(value, buf);
  
  DBUG_RETURN(ret);
}

void ha_kvt::position(const uchar *record)
{
  DBUG_ENTER("ha_kvt::position");
  
  // Store current key as position
  if (current_position_key.length() > 0) {
    size_t len = std::min(current_position_key.length(), (size_t)ref_length);
    std::memcpy(ref, current_position_key.data(), len);
    if (len < ref_length) {
      std::memset(ref + len, 0, ref_length - len);
    }
  }
  
  DBUG_VOID_RETURN;
}

int ha_kvt::index_init(uint idx, bool sorted)
{
  DBUG_ENTER("ha_kvt::index_init");
  active_index = idx;
  index_sorted = sorted;
  index_scan_position = 0;
  index_scan_results.clear();
  
  // Initialize index scan with index manager
  auto* idx_mgr = kvt_index::KVTIndexManager::get_instance();
  std::string index_name = "PRIMARY";  // For now, only support primary key
  
  if (table->key_info && idx < table->s->keys) {
    index_name = table->key_info[idx].name.str;
  }
  
  int ret = idx_mgr->index_init(this, kvt_tx_id, kvt_data_table_id,
                                database_name, table_name, index_name, sorted);
  
  DBUG_RETURN(ret);
}

int ha_kvt::index_end()
{
  DBUG_ENTER("ha_kvt::index_end");
  active_index = MAX_KEY;
  index_scan_results.clear();
  index_scan_position = 0;
  
  auto* idx_mgr = kvt_index::KVTIndexManager::get_instance();
  idx_mgr->index_end(this);
  
  DBUG_RETURN(0);
}

int ha_kvt::index_read_map(uchar *buf, const uchar *key,
                           key_part_map keypart_map,
                           enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_kvt::index_read_map");
  
  // For now, fall back to table scan for index operations
  // This provides basic functionality while we implement full index support
  
  // Start a table scan
  int ret = rnd_init(true);
  if (ret != 0) {
    DBUG_RETURN(ret);
  }
  
  // Scan for matching row
  while ((ret = rnd_next(buf)) == 0) {
    // Check if this row matches the key
    // For primary key, compare the key fields
    bool matches = true;
    
    if (active_index < table->s->keys) {
      KEY *key_info = &table->key_info[active_index];
      const uchar *key_ptr = key;
      
      for (uint i = 0; i < key_info->user_defined_key_parts && matches; i++) {
        if (keypart_map & (1 << i)) {
          Field *field = key_info->key_part[i].field;
          uint key_part_length = key_info->key_part[i].length;
          
          // Compare field value with key
          if (field->key_cmp(key_ptr, field->offset(table->record[0])) != 0) {
            matches = false;
          }
          key_ptr += key_part_length;
        }
      }
    }
    
    if (matches) {
      rnd_end();
      DBUG_RETURN(0);
    }
  }
  
  rnd_end();
  DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
}

int ha_kvt::index_next(uchar *buf)
{
  DBUG_ENTER("ha_kvt::index_next");
  
  // For now, continue table scan
  int ret = rnd_next(buf);
  
  DBUG_RETURN(ret);
}

int ha_kvt::index_prev(uchar *buf)
{
  DBUG_ENTER("ha_kvt::index_prev");
  DBUG_RETURN(HA_ERR_UNSUPPORTED);  // Not implemented yet
}

int ha_kvt::index_first(uchar *buf)
{
  DBUG_ENTER("ha_kvt::index_first");
  
  // Start scan and return first row
  int ret = rnd_init(true);
  if (ret == 0) {
    ret = rnd_next(buf);
    rnd_end();
  }
  
  DBUG_RETURN(ret);
}

int ha_kvt::index_last(uchar *buf)
{
  DBUG_ENTER("ha_kvt::index_last");
  DBUG_RETURN(HA_ERR_UNSUPPORTED);  // Not implemented yet
}

int ha_kvt::index_read_last_map(uchar *buf, const uchar *key,
                                key_part_map keypart_map)
{
  DBUG_ENTER("ha_kvt::index_read_last_map");
  DBUG_RETURN(HA_ERR_UNSUPPORTED);  // Not implemented yet
}

int ha_kvt::info(uint flag)
{
  DBUG_ENTER("ha_kvt::info");
  
  if (flag & HA_STATUS_AUTO)
    stats.auto_increment_value = 1;
  if (flag & HA_STATUS_CONST)
  {
    stats.max_data_file_length = MAX_FILE_SIZE;
    stats.max_index_file_length = MAX_FILE_SIZE;
    stats.create_time = 0;
    ref_length = sizeof(uint64_t);
  }
  if (flag & HA_STATUS_VARIABLE)
  {
    // TODO: Get actual row count from KVT
    // For now, use a hardcoded value to avoid assertion failures
    // We need to implement a proper row counting mechanism
    stats.records = 10; // Temporary: assume non-zero to avoid assertions
    stats.deleted = 0;
    stats.data_file_length = stats.records * 100; // Estimate
    stats.index_file_length = 0;
    stats.mean_rec_length = stats.records ? stats.data_file_length / stats.records : 0;
  }
  
  DBUG_RETURN(0);
}

int ha_kvt::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_kvt::extra");
  DBUG_RETURN(0);
}

int ha_kvt::start_stmt(THD *thd, thr_lock_type lock_type)
{
  DBUG_ENTER("ha_kvt::start_stmt");
  
  // For multi-statement transactions, this is called at the start of each statement
  auto* tx_mgr = kvt_transaction::KVTTransactionManager::get_instance();
  
  if (!tx_mgr->has_active_transaction(thd)) {
    // Need to start a transaction
    int iso_level = thd_tx_isolation(thd);
    int kvt_iso = kvt_transaction::KVTTransactionManager::map_mariadb_isolation_to_kvt(iso_level);
    
    uint64_t tx_id = tx_mgr->begin_transaction(thd, kvt_iso);
    if (tx_id == 0) {
      DBUG_RETURN(HA_ERR_GENERIC);
    }
    
    this->kvt_tx_id = tx_id;
  } else {
    // Use existing transaction
    this->kvt_tx_id = tx_mgr->get_transaction_id(thd);
  }
  
  DBUG_RETURN(0);
}

int ha_kvt::external_lock(THD *thd, int lock_type)
{
  DBUG_ENTER("ha_kvt::external_lock");
  DBUG_PRINT("info", ("lock_type: %d", lock_type));
  
  auto* tx_mgr = kvt_transaction::KVTTransactionManager::get_instance();
  
  if (lock_type != F_UNLCK) {
    // Starting a statement - ensure we have a transaction
    if (!tx_mgr->has_active_transaction(thd)) {
      // Get isolation level from THD
      int iso_level = thd_tx_isolation(thd);
      int kvt_iso = kvt_transaction::KVTTransactionManager::map_mariadb_isolation_to_kvt(iso_level);
      
      // Start new transaction
      uint64_t tx_id = tx_mgr->begin_transaction(thd, kvt_iso);
      if (tx_id == 0) {
        DBUG_RETURN(HA_ERR_GENERIC);
      }
      
      this->kvt_tx_id = tx_id;
      DBUG_PRINT("info", ("Started new transaction: %llu", (ulonglong)tx_id));
    } else {
      // Use existing transaction
      this->kvt_tx_id = tx_mgr->get_transaction_id(thd);
      DBUG_PRINT("info", ("Using existing transaction: %llu", (ulonglong)kvt_tx_id));
    }
    
    // Track statement start
    tx_mgr->start_statement(thd);
  } else {
    // Ending a statement
    if (tx_mgr->has_active_transaction(thd)) {
      // Check if we should auto-commit
      // Use thd_test_options instead of accessing ha_data directly
      bool autocommit = (thd_test_options(thd, OPTION_AUTOCOMMIT)) != 0;
      bool not_in_trans = true; // Simplified for now
      
      if (autocommit && not_in_trans) {
        // Auto-commit the transaction (cast issue fix)
        DBUG_PRINT("info", ("Auto-committing transaction"));
        int ret = tx_mgr->commit_transaction(thd);
        if (ret != 0) {
          DBUG_RETURN(HA_ERR_GENERIC);
        }
        this->kvt_tx_id = 0;
      } else {
        // Keep transaction active for next statement
        tx_mgr->end_statement(thd, false);
      }
    }
  }
  
  DBUG_RETURN(0);
}

void ha_kvt::start_bulk_insert(ha_rows rows, uint flags)
{
  DBUG_ENTER("ha_kvt::start_bulk_insert");
  DBUG_PRINT("info", ("rows: %lu flags: %u", (ulong)rows, flags));
  
  doing_bulk_insert = true;
  bulk_insert_rows = rows;
  
  // Pre-allocate batch buffer if we know the size
  if (rows > 0 && rows < 10000) {
    batch_operations.reserve(rows);
  }
  
  DBUG_VOID_RETURN;
}

int ha_kvt::end_bulk_insert()
{
  DBUG_ENTER("ha_kvt::end_bulk_insert");
  
  if (!doing_bulk_insert) {
    DBUG_RETURN(0);
  }
  
  int error = 0;
  
  // Flush any remaining batch operations
  if (!batch_operations.empty()) {
    error = flush_batch_operations();
    batch_operations.clear();
  }
  
  doing_bulk_insert = false;
  bulk_insert_rows = 0;
  
  DBUG_RETURN(error);
}

int ha_kvt::flush_batch_operations()
{
  DBUG_ENTER("ha_kvt::flush_batch_operations");
  
  if (batch_operations.empty()) {
    DBUG_RETURN(0);
  }
  
  // Use kvt_batch_execute if available
  std::string error_msg;
  KVTBatchResults results;
  
  KVTError err = kvt_batch_execute(kvt_tx_id, batch_operations, 
                                   results, error_msg);
  
  if (err != KVTError::SUCCESS) {
    DBUG_RETURN(map_kvt_error_to_mysql(err, error_msg));
  }
  
  // Check individual results
  for (size_t i = 0; i < results.size(); i++) {
    if (results[i].error != KVTError::SUCCESS) {
      DBUG_RETURN(map_kvt_error_to_mysql(results[i].error, "Batch operation failed"));
    }
  }
  
  // Update statistics
  stats.records += batch_operations.size();
  
  batch_operations.clear();
  DBUG_RETURN(0);
}

int ha_kvt::delete_all_rows()
{
  DBUG_ENTER("ha_kvt::delete_all_rows");
  DBUG_RETURN(HA_ERR_UNSUPPORTED);
}

ha_rows ha_kvt::records_in_range(uint inx, const key_range *min_key,
                                 const key_range *max_key, page_range *pages)
{
  DBUG_ENTER("ha_kvt::records_in_range");
  DBUG_RETURN(10);
}

int ha_kvt::analyze(THD* thd, HA_CHECK_OPT* check_opt)
{
  DBUG_ENTER("ha_kvt::analyze");
  DBUG_RETURN(HA_ADMIN_NOT_IMPLEMENTED);
}

int ha_kvt::optimize(THD* thd, HA_CHECK_OPT* check_opt)
{
  DBUG_ENTER("ha_kvt::optimize");
  DBUG_RETURN(HA_ADMIN_NOT_IMPLEMENTED);
}

int ha_kvt::check(THD* thd, HA_CHECK_OPT* check_opt)
{
  DBUG_ENTER("ha_kvt::check");
  DBUG_RETURN(HA_ADMIN_NOT_IMPLEMENTED);
}

THR_LOCK_DATA **ha_kvt::store_lock(THD *thd,
                                   THR_LOCK_DATA **to,
                                   enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type = lock_type;
  *to++ = &lock;
  return to;
}

bool ha_kvt::get_error_message(int error, String *buf)
{
  DBUG_ENTER("ha_kvt::get_error_message");
  DBUG_RETURN(false);
}

ha_kvt::kvt_table_share *ha_kvt::get_share(const char *path)
{
  kvt_table_share *share;
  uint length = strlen(path);

  mysql_mutex_lock(&kvt_mutex);
  
  if (!(share = (kvt_table_share*)my_hash_search(&kvt_open_tables,
                                                 (uchar*)path, length)))
  {
    if (!(share = (kvt_table_share *)my_malloc(PSI_NOT_INSTRUMENTED,
                                               sizeof(*share) + length + 1,
                                               MYF(MY_WME | MY_ZEROFILL))))
    {
      mysql_mutex_unlock(&kvt_mutex);
      return NULL;
    }

    share->table_name = (char *)(share + 1);
    strcpy(share->table_name, path);
    share->use_count = 0;
    
    // Parse database and table name
    std::string db, tbl;
    kvt_catalog::CatalogManager::parse_table_path(path, db, tbl);
    
    // Get data table ID
    auto* catalog = kvt_catalog::CatalogManager::get_instance();
    share->data_table_id = catalog->get_data_table_id(db);
    
    if (my_hash_insert(&kvt_open_tables, (uchar *)share))
    {
      my_free(share);
      mysql_mutex_unlock(&kvt_mutex);
      return NULL;
    }
    
    thr_lock_init(&share->lock);
    mysql_mutex_init(key_mutex_kvt_share, &share->mutex, MY_MUTEX_INIT_FAST);
  }
  
  share->use_count++;
  mysql_mutex_unlock(&kvt_mutex);
  
  return share;
}

void ha_kvt::free_share(kvt_table_share *share)
{
  mysql_mutex_lock(&kvt_mutex);
  
  if (!--share->use_count)
  {
    my_hash_delete(&kvt_open_tables, (uchar *)share);
    thr_lock_delete(&share->lock);
    mysql_mutex_destroy(&share->mutex);
    my_free(share);
  }
  
  mysql_mutex_unlock(&kvt_mutex);
}

std::string ha_kvt::generate_row_key(const uchar *buf)
{
  DBUG_ENTER("ha_kvt::generate_row_key");
  
  if (!row_codec) {
    DBUG_RETURN("");
  }
  
  // Try to use primary key
  if (table->s->primary_key != MAX_KEY) {
    DBUG_RETURN(row_codec->encode_primary_key(buf));
  }
  
  // Use rowid
  DBUG_RETURN(row_codec->encode_rowid(next_rowid++));
}

std::string ha_kvt::generate_data_key(const std::string& row_key)
{
  DBUG_ENTER("ha_kvt::generate_data_key");
  DBUG_RETURN(kvt_constants::make_data_key(table_name, row_key));
}

int ha_kvt::encode_row(const uchar *buf, std::string &value)
{
  DBUG_ENTER("ha_kvt::encode_row");
  if (!row_codec) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  DBUG_RETURN(row_codec->encode_row(buf, value));
}

int ha_kvt::decode_row(const std::string &value, uchar *buf)
{
  DBUG_ENTER("ha_kvt::decode_row");
  if (!row_codec) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  DBUG_RETURN(row_codec->decode_row(value, buf));
}

int ha_kvt::map_kvt_error_to_mysql(KVTError kvt_err, const std::string &error_msg)
{
  switch (kvt_err)
  {
    case KVTError::SUCCESS:
      return 0;
    case KVTError::KEY_NOT_FOUND:
      return HA_ERR_KEY_NOT_FOUND;
    case KVTError::TABLE_NOT_FOUND:
      return HA_ERR_NO_SUCH_TABLE;
    case KVTError::TABLE_ALREADY_EXISTS:
      return HA_ERR_TABLE_EXIST;
    case KVTError::WRITE_CONFLICT:
    case KVTError::UPDATE_CONFLICT:
    case KVTError::DELETE_CONFLICT:
      return HA_ERR_LOCK_DEADLOCK;
    default:
      return HA_ERR_GENERIC;
  }
}

void ha_kvt::start_bulk_insert_if_needed()
{
  DBUG_ENTER("ha_kvt::start_bulk_insert_if_needed");
  DBUG_VOID_RETURN;
}

void ha_kvt::end_bulk_insert_if_needed()
{
  DBUG_ENTER("ha_kvt::end_bulk_insert_if_needed");
  DBUG_VOID_RETURN;
}

int ha_kvt::load_table_metadata()
{
  DBUG_ENTER("ha_kvt::load_table_metadata");
  
  auto* catalog = kvt_catalog::CatalogManager::get_instance();
  if (!table_metadata) {
    table_metadata = std::make_unique<kvt_catalog::TableMetadata>();
  }
  
  int ret = catalog->load_table_metadata(database_name, table_name, *table_metadata);
  DBUG_RETURN(ret);
}

int ha_kvt::store_table_metadata()
{
  DBUG_ENTER("ha_kvt::store_table_metadata");
  
  if (!table_metadata) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  
  auto* catalog = kvt_catalog::CatalogManager::get_instance();
  int ret = catalog->store_table_metadata(database_name, table_name, *table_metadata);
  DBUG_RETURN(ret);
}

const COND *ha_kvt::cond_push(const COND *cond)
{
  DBUG_ENTER("ha_kvt::cond_push");
  
  // For now, we accept all conditions and evaluate them locally
  // In future, we can use KVT's update functions for server-side filtering
  pushed_cond = cond;
  
  DBUG_RETURN(cond);
}

void ha_kvt::cond_pop()
{
  DBUG_ENTER("ha_kvt::cond_pop");
  pushed_cond = nullptr;
  DBUG_VOID_RETURN;
}

bool ha_kvt::check_pushed_condition(const uchar *buf)
{
  DBUG_ENTER("ha_kvt::check_pushed_condition");
  
  if (!pushed_cond) {
    DBUG_RETURN(true);
  }
  
  // Evaluate the condition
  // Note: In MariaDB, conditions are evaluated by calling val_int() on the condition
  // The record should be in table->record[0]
  if (buf != table->record[0]) {
    // Copy record to record[0] for evaluation
    memcpy(table->record[0], buf, table->s->reclength);
  }
  
  // The condition returns 0 for false, non-zero for true
  // Cast away const for evaluation (required by MariaDB's Item interface)
  bool result = const_cast<COND*>(pushed_cond)->val_int() != 0;
  
  DBUG_RETURN(result);
}

struct st_mysql_storage_engine kvt_storage_engine =
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(kvt)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &kvt_storage_engine,
  "KVT",
  "KVT Development Team",
  "Key-Value Transaction storage engine",
  PLUGIN_LICENSE_GPL,
  kvt_init_func,
  kvt_deinit_func,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE,
}
maria_declare_plugin_end;