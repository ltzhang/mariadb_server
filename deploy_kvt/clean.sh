#!/bin/bash

# MariaDB with KVT Storage Engine Cleanup Script
# This script stops MariaDB and cleans up all test files

# Configuration
TEST_DIR="/tmp/mariadb_kvt_test"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}=== MariaDB KVT Cleanup Script ===${NC}"

# Step 1: Kill MariaDB processes
echo -e "${YELLOW}Stopping MariaDB processes...${NC}"
MARIADB_PIDS=$(pgrep -f "mariadbd.*mariadb_kvt_test" 2>/dev/null)

if [ -n "$MARIADB_PIDS" ]; then
    for PID in $MARIADB_PIDS; do
        echo "  Killing MariaDB process (PID: $PID)..."
        kill $PID 2>/dev/null
    done
    
    # Wait for processes to terminate
    sleep 2
    
    # Force kill if still running
    REMAINING_PIDS=$(pgrep -f "mariadbd.*mariadb_kvt_test" 2>/dev/null)
    if [ -n "$REMAINING_PIDS" ]; then
        echo "  Force killing remaining processes..."
        for PID in $REMAINING_PIDS; do
            kill -9 $PID 2>/dev/null
        done
    fi
    
    echo -e "${GREEN}  ✓ MariaDB processes stopped${NC}"
else
    echo "  No MariaDB processes found"
fi

# Step 2: Remove test directory
echo -e "${YELLOW}Removing test directory...${NC}"
if [ -d "${TEST_DIR}" ]; then
    rm -rf ${TEST_DIR}
    echo -e "${GREEN}  ✓ Test directory removed: ${TEST_DIR}${NC}"
else
    echo "  Test directory not found: ${TEST_DIR}"
fi

# Step 3: Clean up any socket files that might be left
echo -e "${YELLOW}Cleaning up socket files...${NC}"
if [ -S "/tmp/mysql.sock" ]; then
    rm -f /tmp/mysql.sock
    echo "  Removed /tmp/mysql.sock"
fi

# Step 4: Summary
echo -e "${GREEN}=== Cleanup Complete ===${NC}"
echo "All MariaDB KVT test resources have been cleaned up."
echo "You can now run deploy.sh to start fresh."