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

#ifndef KVT_TRANSACTION_MANAGER_H
#define KVT_TRANSACTION_MANAGER_H

#include "my_global.h"
#include <unordered_map>
#include <mutex>
#include <vector>
#include "kvt/kvt_inc.h"

// Forward declaration
class THD;

namespace kvt_transaction {

/**
 * Transaction state for a given connection (THD)
 */
struct TransactionState {
  uint64_t tx_id;                     // KVT transaction ID
  bool is_active;                      // Is transaction currently active
  bool is_autocommit;                  // Is autocommit mode enabled
  int isolation_level;                 // Transaction isolation level
  uint statement_count;                // Number of statements in transaction
  std::vector<uint64_t> savepoints;    // Stack of savepoint IDs
  time_t start_time;                   // Transaction start timestamp
  
  TransactionState() 
    : tx_id(0), is_active(false), is_autocommit(true), 
      isolation_level(0), statement_count(0), start_time(0) {}
};

/**
 * Singleton manager for all KVT transactions
 * Manages THD to transaction ID mapping and transaction lifecycle
 */
class KVTTransactionManager {
public:
  // Singleton access
  static KVTTransactionManager* get_instance();
  
  // Transaction lifecycle management
  uint64_t begin_transaction(THD* thd, int isolation_level);
  int commit_transaction(THD* thd, bool all = true);
  int rollback_transaction(THD* thd, bool all = true);
  
  // Transaction state queries
  uint64_t get_transaction_id(THD* thd);
  bool has_active_transaction(THD* thd);
  bool is_autocommit(THD* thd);
  TransactionState* get_transaction_state(THD* thd);
  
  // Statement management
  int start_statement(THD* thd);
  int end_statement(THD* thd, bool commit_statement = false);
  
  // Savepoint management
  int savepoint_set(THD* thd, const char* name);
  int savepoint_rollback(THD* thd, const char* name);
  int savepoint_release(THD* thd, const char* name);
  
  // Connection cleanup
  void cleanup_transaction(THD* thd);
  void cleanup_all_transactions();
  
  // Statistics and monitoring
  size_t get_active_transaction_count() const;
  void get_transaction_info(std::vector<TransactionState>& info) const;
  
  // Deadlock detection and timeout handling
  bool check_for_deadlock(THD* thd);
  int handle_lock_wait_timeout(THD* thd);
  void set_lock_wait_timeout(THD* thd, ulong timeout_sec);
  
  // Isolation level helpers
  static int map_mariadb_isolation_to_kvt(int mariadb_level);
  static const char* get_isolation_level_name(int level);
  
private:
  KVTTransactionManager();
  ~KVTTransactionManager();
  
  // Prevent copying
  KVTTransactionManager(const KVTTransactionManager&) = delete;
  KVTTransactionManager& operator=(const KVTTransactionManager&) = delete;
  
  // Internal helpers
  TransactionState* get_or_create_state(THD* thd);
  void remove_transaction_state(THD* thd);
  uint64_t find_savepoint(const TransactionState* state, const char* name);
  
private:
  static KVTTransactionManager* instance;
  static std::mutex instance_mutex;
  
  std::unordered_map<THD*, TransactionState> transactions;
  mutable std::mutex transactions_mutex;
  
  // Statistics
  uint64_t total_transactions_started;
  uint64_t total_transactions_committed;
  uint64_t total_transactions_rolled_back;
};

/**
 * RAII helper for transaction scope
 */
class TransactionScope {
public:
  TransactionScope(THD* thd, bool auto_commit = true);
  ~TransactionScope();
  
  void commit();
  void rollback();
  
private:
  THD* thd;
  bool committed;
  bool should_auto_commit;
};

} // namespace kvt_transaction

#endif // KVT_TRANSACTION_MANAGER_H