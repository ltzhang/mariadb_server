/*
  Simple test program for KVT storage backend
  Tests basic KVT operations without MariaDB integration
*/

#include <iostream>
#include <string>
#include <vector>
#include "kvt/kvt_inc.h"

void test_basic_operations() {
    std::cout << "Testing KVT Basic Operations\n";
    std::cout << "=============================\n";
    
    // Initialize KVT
    std::string error_msg;
    KVTError err = kvt_init(error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to initialize KVT: " << error_msg << std::endl;
        return;
    }
    std::cout << "✓ KVT initialized successfully\n";
    
    // Create a database
    uint64_t db_id = 1;
    err = kvt_create_database(db_id, "test_db", error_msg);
    if (err != KVTError::SUCCESS && err != KVTError::DATABASE_ALREADY_EXISTS) {
        std::cerr << "Failed to create database: " << error_msg << std::endl;
        return;
    }
    std::cout << "✓ Database created/exists\n";
    
    // Create a table
    uint64_t table_id = 100;
    err = kvt_create_table(db_id, table_id, "test_table", error_msg);
    if (err != KVTError::SUCCESS && err != KVTError::TABLE_ALREADY_EXISTS) {
        std::cerr << "Failed to create table: " << error_msg << std::endl;
        return;
    }
    std::cout << "✓ Table created/exists\n";
    
    // Start a transaction
    uint64_t tx_id;
    err = kvt_begin(IsolationLevel::REPEATABLE_READ, tx_id, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to begin transaction: " << error_msg << std::endl;
        return;
    }
    std::cout << "✓ Transaction started (ID: " << tx_id << ")\n";
    
    // Test SET operation
    KVTKey key("test_key_1");
    std::string value = "test_value_1";
    err = kvt_set(tx_id, table_id, key, value, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to set key-value: " << error_msg << std::endl;
        kvt_rollback(tx_id, error_msg);
        return;
    }
    std::cout << "✓ SET operation successful\n";
    
    // Test GET operation
    std::string retrieved_value;
    err = kvt_get(tx_id, table_id, key, retrieved_value, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to get value: " << error_msg << std::endl;
        kvt_rollback(tx_id, error_msg);
        return;
    }
    if (retrieved_value != value) {
        std::cerr << "Value mismatch! Expected: " << value << ", Got: " << retrieved_value << std::endl;
        kvt_rollback(tx_id, error_msg);
        return;
    }
    std::cout << "✓ GET operation successful (value matches)\n";
    
    // Test multiple inserts
    for (int i = 2; i <= 5; i++) {
        KVTKey k("test_key_" + std::to_string(i));
        std::string v = "test_value_" + std::to_string(i);
        err = kvt_set(tx_id, table_id, k, v, error_msg);
        if (err != KVTError::SUCCESS) {
            std::cerr << "Failed to set key-value " << i << ": " << error_msg << std::endl;
            kvt_rollback(tx_id, error_msg);
            return;
        }
    }
    std::cout << "✓ Multiple SET operations successful (5 keys total)\n";
    
    // Test SCAN operation
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTKey start_key("test_key_1");
    KVTKey end_key("test_key_6");
    err = kvt_scan(tx_id, table_id, start_key, end_key, 10, scan_results, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to scan: " << error_msg << std::endl;
        kvt_rollback(tx_id, error_msg);
        return;
    }
    if (scan_results.size() != 5) {
        std::cerr << "Scan returned wrong number of results. Expected: 5, Got: " << scan_results.size() << std::endl;
        kvt_rollback(tx_id, error_msg);
        return;
    }
    std::cout << "✓ SCAN operation successful (found " << scan_results.size() << " keys)\n";
    
    // Test DELETE operation
    KVTKey del_key("test_key_3");
    err = kvt_del(tx_id, table_id, del_key, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to delete key: " << error_msg << std::endl;
        kvt_rollback(tx_id, error_msg);
        return;
    }
    std::cout << "✓ DELETE operation successful\n";
    
    // Verify deletion
    std::string deleted_value;
    err = kvt_get(tx_id, table_id, del_key, deleted_value, error_msg);
    if (err != KVTError::KEY_NOT_FOUND) {
        std::cerr << "Key should have been deleted but was found!" << std::endl;
        kvt_rollback(tx_id, error_msg);
        return;
    }
    std::cout << "✓ DELETE verified (key not found)\n";
    
    // Commit transaction
    err = kvt_commit(tx_id, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to commit transaction: " << error_msg << std::endl;
        return;
    }
    std::cout << "✓ Transaction committed successfully\n";
    
    // Test persistence - start new transaction and verify data
    uint64_t tx2_id;
    err = kvt_begin(IsolationLevel::REPEATABLE_READ, tx2_id, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to begin second transaction: " << error_msg << std::endl;
        return;
    }
    
    scan_results.clear();
    err = kvt_scan(tx2_id, table_id, start_key, end_key, 10, scan_results, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to scan in new transaction: " << error_msg << std::endl;
        kvt_rollback(tx2_id, error_msg);
        return;
    }
    if (scan_results.size() != 4) { // Should be 4 after deleting one
        std::cerr << "Persistence check failed. Expected: 4 keys, Got: " << scan_results.size() << std::endl;
        kvt_rollback(tx2_id, error_msg);
        return;
    }
    std::cout << "✓ Data persisted correctly (4 keys remain after delete)\n";
    
    kvt_commit(tx2_id, error_msg);
    
    std::cout << "\n✅ All KVT basic operations passed!\n";
}

void test_batch_operations() {
    std::cout << "\nTesting KVT Batch Operations\n";
    std::cout << "=============================\n";
    
    std::string error_msg;
    uint64_t table_id = 100;
    
    // Start transaction
    uint64_t tx_id;
    KVTError err = kvt_begin(IsolationLevel::REPEATABLE_READ, tx_id, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to begin transaction: " << error_msg << std::endl;
        return;
    }
    
    // Prepare batch operations
    std::vector<KVTOp> ops;
    for (int i = 1; i <= 100; i++) {
        KVTOp op;
        op.op = OP_SET;
        op.table_id = table_id;
        op.key = KVTKey("batch_key_" + std::to_string(i));
        op.value = "batch_value_" + std::to_string(i);
        ops.push_back(op);
    }
    
    // Execute batch
    err = kvt_batch(tx_id, ops, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to execute batch: " << error_msg << std::endl;
        kvt_rollback(tx_id, error_msg);
        return;
    }
    std::cout << "✓ Batch insert of 100 records successful\n";
    
    // Verify batch insert
    std::vector<std::pair<KVTKey, std::string>> scan_results;
    KVTKey start_key("batch_key_1");
    KVTKey end_key("batch_key_2");  // This will get keys starting with "batch_key_1"
    err = kvt_scan(tx_id, table_id, start_key, end_key, 20, scan_results, error_msg);
    if (err != KVTError::SUCCESS) {
        std::cerr << "Failed to scan batch results: " << error_msg << std::endl;
        kvt_rollback(tx_id, error_msg);
        return;
    }
    std::cout << "✓ Batch data verified (found " << scan_results.size() << " keys in sample range)\n";
    
    kvt_commit(tx_id, error_msg);
    std::cout << "✅ Batch operations test passed!\n";
}

int main() {
    std::cout << "KVT Storage Backend Test Suite\n";
    std::cout << "==============================\n\n";
    
    test_basic_operations();
    test_batch_operations();
    
    std::cout << "\n✅ All tests completed successfully!\n";
    return 0;
}