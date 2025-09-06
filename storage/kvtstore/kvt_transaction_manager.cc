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

#include "kvt_transaction_manager.h"
#include "sql_class.h"
#include <time.h>

namespace kvt_transaction {

// Static member initialization
KVTTransactionManager* KVTTransactionManager::instance = nullptr;
std::mutex KVTTransactionManager::instance_mutex;

KVTTransactionManager::KVTTransactionManager()
  : total_transactions_started(0),
    total_transactions_committed(0),
    total_transactions_rolled_back(0) {
}

KVTTransactionManager::~KVTTransactionManager() {
  cleanup_all_transactions();
}

KVTTransactionManager* KVTTransactionManager::get_instance() {
  // Double-checked locking pattern
  if (instance == nullptr) {
    std::lock_guard<std::mutex> lock(instance_mutex);
    if (instance == nullptr) {
      instance = new KVTTransactionManager();
    }
  }
  return instance;
}

uint64_t KVTTransactionManager::begin_transaction(THD* thd, int isolation_level) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  // Check if transaction already exists
  auto it = transactions.find(thd);
  if (it != transactions.end() && it->second.is_active) {
    // Already have active transaction
    return it->second.tx_id;
  }
  
  // Start new KVT transaction
  uint64_t tx_id = 0;
  std::string error_msg;
  KVTError err = kvt_start_transaction(tx_id, error_msg);
  
  if (err != KVTError::SUCCESS || tx_id == 0) {
    // Failed to start transaction
    return 0;
  }
  
  // Create or update transaction state
  TransactionState& state = transactions[thd];
  state.tx_id = tx_id;
  state.is_active = true;
  state.is_autocommit = (thd->variables.option_bits & OPTION_AUTOCOMMIT) != 0;
  state.isolation_level = isolation_level;
  state.statement_count = 0;
  state.savepoints.clear();
  state.start_time = time(nullptr);
  
  total_transactions_started++;
  
  return tx_id;
}

int KVTTransactionManager::commit_transaction(THD* thd, bool all) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it == transactions.end() || !it->second.is_active) {
    // No active transaction
    return 0;
  }
  
  TransactionState& state = it->second;
  
  // Commit the KVT transaction
  std::string error_msg;
  KVTError err = kvt_commit_transaction(state.tx_id, error_msg);
  
  if (err != KVTError::SUCCESS) {
    // Commit failed
    return -1;
  }
  
  // Clean up transaction state
  state.is_active = false;
  state.tx_id = 0;
  state.statement_count = 0;
  state.savepoints.clear();
  
  total_transactions_committed++;
  
  // Remove state if we're done with it
  if (all) {
    transactions.erase(it);
  }
  
  return 0;
}

int KVTTransactionManager::rollback_transaction(THD* thd, bool all) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it == transactions.end() || !it->second.is_active) {
    // No active transaction
    return 0;
  }
  
  TransactionState& state = it->second;
  
  // Rollback the KVT transaction
  std::string error_msg;
  KVTError err = kvt_rollback_transaction(state.tx_id, error_msg);
  
  if (err != KVTError::SUCCESS && err != KVTError::TRANSACTION_NOT_FOUND) {
    // Rollback failed (but ignore if transaction doesn't exist)
    return -1;
  }
  
  // Clean up transaction state
  state.is_active = false;
  state.tx_id = 0;
  state.statement_count = 0;
  state.savepoints.clear();
  
  total_transactions_rolled_back++;
  
  // Remove state if we're done with it
  if (all) {
    transactions.erase(it);
  }
  
  return 0;
}

uint64_t KVTTransactionManager::get_transaction_id(THD* thd) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it != transactions.end() && it->second.is_active) {
    return it->second.tx_id;
  }
  
  return 0;  // No active transaction
}

bool KVTTransactionManager::has_active_transaction(THD* thd) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  return (it != transactions.end() && it->second.is_active);
}

bool KVTTransactionManager::is_autocommit(THD* thd) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it != transactions.end()) {
    return it->second.is_autocommit;
  }
  
  // Default to checking THD directly
  return (thd->variables.option_bits & OPTION_AUTOCOMMIT) != 0;
}

TransactionState* KVTTransactionManager::get_transaction_state(THD* thd) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it != transactions.end()) {
    return &it->second;
  }
  
  return nullptr;
}

int KVTTransactionManager::start_statement(THD* thd) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it != transactions.end() && it->second.is_active) {
    it->second.statement_count++;
    return 0;
  }
  
  // No active transaction - might need to start one
  return -1;
}

int KVTTransactionManager::end_statement(THD* thd, bool commit_statement) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it == transactions.end() || !it->second.is_active) {
    return 0;
  }
  
  // In autocommit mode, commit after each statement
  if (commit_statement && it->second.is_autocommit) {
    // Need to unlock before calling commit to avoid deadlock
    transactions_mutex.unlock();
    int ret = commit_transaction(thd, true);
    transactions_mutex.lock();
    return ret;
  }
  
  return 0;
}

int KVTTransactionManager::savepoint_set(THD* thd, const char* name) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it == transactions.end() || !it->second.is_active) {
    return -1;  // No active transaction
  }
  
  // For now, we'll use a simple counter for savepoint IDs
  // In a real implementation, we'd call a KVT savepoint API
  uint64_t savepoint_id = it->second.savepoints.size() + 1;
  it->second.savepoints.push_back(savepoint_id);
  
  // TODO: Call KVT savepoint API when available
  // kvt_savepoint_set(it->second.tx_id, savepoint_id, name);
  
  return 0;
}

int KVTTransactionManager::savepoint_rollback(THD* thd, const char* name) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it == transactions.end() || !it->second.is_active) {
    return -1;  // No active transaction
  }
  
  // Find the savepoint
  uint64_t savepoint_id = find_savepoint(&it->second, name);
  if (savepoint_id == 0) {
    return -1;  // Savepoint not found
  }
  
  // TODO: Call KVT savepoint rollback API when available
  // kvt_savepoint_rollback(it->second.tx_id, savepoint_id);
  
  // Remove savepoints after this one
  while (!it->second.savepoints.empty() && 
         it->second.savepoints.back() > savepoint_id) {
    it->second.savepoints.pop_back();
  }
  
  return 0;
}

int KVTTransactionManager::savepoint_release(THD* thd, const char* name) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it == transactions.end() || !it->second.is_active) {
    return -1;  // No active transaction
  }
  
  // Find the savepoint
  uint64_t savepoint_id = find_savepoint(&it->second, name);
  if (savepoint_id == 0) {
    return -1;  // Savepoint not found
  }
  
  // TODO: Call KVT savepoint release API when available
  // kvt_savepoint_release(it->second.tx_id, savepoint_id);
  
  // Remove this savepoint from the list
  auto& savepoints = it->second.savepoints;
  savepoints.erase(
    std::remove(savepoints.begin(), savepoints.end(), savepoint_id),
    savepoints.end()
  );
  
  return 0;
}

void KVTTransactionManager::cleanup_transaction(THD* thd) {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it != transactions.end()) {
    if (it->second.is_active) {
      // Rollback any active transaction
      std::string error_msg;
      kvt_rollback_transaction(it->second.tx_id, error_msg);
      total_transactions_rolled_back++;
    }
    transactions.erase(it);
  }
}

void KVTTransactionManager::cleanup_all_transactions() {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  for (auto& pair : transactions) {
    if (pair.second.is_active) {
      std::string error_msg;
      kvt_rollback_transaction(pair.second.tx_id, error_msg);
      total_transactions_rolled_back++;
    }
  }
  
  transactions.clear();
}

size_t KVTTransactionManager::get_active_transaction_count() const {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  size_t count = 0;
  for (const auto& pair : transactions) {
    if (pair.second.is_active) {
      count++;
    }
  }
  
  return count;
}

void KVTTransactionManager::get_transaction_info(std::vector<TransactionState>& info) const {
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  info.clear();
  for (const auto& pair : transactions) {
    if (pair.second.is_active) {
      info.push_back(pair.second);
    }
  }
}

int KVTTransactionManager::map_mariadb_isolation_to_kvt(int mariadb_level) {
  // Map MariaDB isolation levels to KVT levels
  // These constants would be defined in the KVT API
  switch (mariadb_level) {
    case ISO_READ_UNCOMMITTED:
      return 0;  // KVT_ISO_READ_UNCOMMITTED
    case ISO_READ_COMMITTED:
      return 1;  // KVT_ISO_READ_COMMITTED
    case ISO_REPEATABLE_READ:
      return 2;  // KVT_ISO_REPEATABLE_READ
    case ISO_SERIALIZABLE:
      return 3;  // KVT_ISO_SERIALIZABLE
    default:
      return 2;  // Default to REPEATABLE READ
  }
}

const char* KVTTransactionManager::get_isolation_level_name(int level) {
  switch (level) {
    case 0: return "READ UNCOMMITTED";
    case 1: return "READ COMMITTED";
    case 2: return "REPEATABLE READ";
    case 3: return "SERIALIZABLE";
    default: return "UNKNOWN";
  }
}

TransactionState* KVTTransactionManager::get_or_create_state(THD* thd) {
  auto it = transactions.find(thd);
  if (it == transactions.end()) {
    TransactionState state;
    transactions[thd] = state;
    return &transactions[thd];
  }
  return &it->second;
}

void KVTTransactionManager::remove_transaction_state(THD* thd) {
  transactions.erase(thd);
}

uint64_t KVTTransactionManager::find_savepoint(const TransactionState* state, 
                                               const char* name) {
  // For now, we use a simple index-based approach
  // In a real implementation, we'd maintain a name->id mapping
  if (state->savepoints.empty()) {
    return 0;
  }
  
  // Return the most recent savepoint
  // TODO: Implement proper name-based lookup
  return state->savepoints.back();
}

bool KVTTransactionManager::check_for_deadlock(THD* thd) {
  // Simple deadlock detection
  // In a real implementation, we would:
  // 1. Build a wait-for graph of transactions
  // 2. Check for cycles in the graph
  // 3. Choose a victim to rollback
  
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it == transactions.end() || !it->second.is_active) {
    return false;
  }
  
  // For now, we use a simple timeout-based approach
  // If transaction has been running too long, consider it as potential deadlock
  time_t current_time = time(nullptr);
  time_t elapsed = current_time - it->second.start_time;
  
  // If transaction is older than 30 seconds and waiting, might be deadlocked
  if (elapsed > 30) {
    // In KVT, we would check if the transaction is actually waiting
    // For now, return false as we don't have real deadlock detection
    return false;
  }
  
  return false;
}

int KVTTransactionManager::handle_lock_wait_timeout(THD* thd) {
  // Handle lock wait timeout
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it == transactions.end() || !it->second.is_active) {
    return 0;
  }
  
  // Check if we've exceeded the lock wait timeout
  ulong timeout = thd->variables.lock_wait_timeout;
  time_t current_time = time(nullptr);
  time_t elapsed = current_time - it->second.start_time;
  
  if (elapsed > timeout) {
    // Timeout exceeded - rollback the transaction
    transactions_mutex.unlock();
    rollback_transaction(thd, true);
    transactions_mutex.lock();
    return HA_ERR_LOCK_WAIT_TIMEOUT;
  }
  
  return 0;
}

void KVTTransactionManager::set_lock_wait_timeout(THD* thd, ulong timeout_sec) {
  // This would be used to set KVT-specific timeout
  // For now, we rely on MariaDB's lock_wait_timeout variable
  std::lock_guard<std::mutex> lock(transactions_mutex);
  
  auto it = transactions.find(thd);
  if (it != transactions.end()) {
    // Could store timeout in TransactionState if needed
    // For now, we use THD's variable directly
  }
}

// TransactionScope implementation
TransactionScope::TransactionScope(THD* thd, bool auto_commit)
  : thd(thd), committed(false), should_auto_commit(auto_commit) {
  auto* mgr = KVTTransactionManager::get_instance();
  if (!mgr->has_active_transaction(thd)) {
    mgr->begin_transaction(thd, ISO_REPEATABLE_READ);
  }
}

TransactionScope::~TransactionScope() {
  if (!committed && should_auto_commit) {
    rollback();
  }
}

void TransactionScope::commit() {
  if (!committed) {
    auto* mgr = KVTTransactionManager::get_instance();
    mgr->commit_transaction(thd);
    committed = true;
  }
}

void TransactionScope::rollback() {
  if (!committed) {
    auto* mgr = KVTTransactionManager::get_instance();
    mgr->rollback_transaction(thd);
    committed = true;  // Prevent double rollback
  }
}

} // namespace kvt_transaction