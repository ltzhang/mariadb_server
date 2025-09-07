#!/bin/bash

# MariaDB with KVT Storage Engine Deployment Script
# This script deploys MariaDB with the KVT storage engine backend

set -e  # Exit on error

# Configuration
PROJECT_ROOT="/home/lintaoz/work/mariadb"
BUILD_DIR="${PROJECT_ROOT}/build_kvt"
DEPLOY_DIR="${PROJECT_ROOT}/deploy_kvt"
TEST_DIR="/tmp/mariadb_kvt_test"
CONFIG_FILE="${DEPLOY_DIR}/my.cnf"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== MariaDB KVT Deployment Script ===${NC}"

# Step 1: Check if MariaDB is already running
echo -e "${YELLOW}Checking for existing MariaDB processes...${NC}"
if pgrep -f "mariadbd.*mariadb_kvt_test" > /dev/null; then
    echo -e "${RED}MariaDB is already running. Please run clean.sh first.${NC}"
    exit 1
fi

# Step 2: Clean and create test directory (KVT is in-memory only)
echo -e "${YELLOW}Creating clean test directory...${NC}"
rm -rf ${TEST_DIR}
mkdir -p ${TEST_DIR}/data

# Step 3: Build KVT plugin if needed
echo -e "${YELLOW}Building KVT storage engine...${NC}"
cd ${BUILD_DIR}

# Compile kvt_memory.o with debug flags to match MariaDB
echo "  Compiling KVT memory implementation..."
g++ -c -fPIC -g -O0 -D_GLIBCXX_DEBUG \
    -I${PROJECT_ROOT}/storage/kvtstore/kvt \
    ${PROJECT_ROOT}/storage/kvtstore/kvt/kvt_mem.cpp \
    -o ${BUILD_DIR}/storage/kvtstore/kvt_memory.o

# Build the KVT plugin
echo "  Building KVT plugin..."
make -j10 kvt > /dev/null 2>&1
if [ $? -eq 0 ]; then
    echo -e "${GREEN}  ✓ KVT plugin built successfully${NC}"
else
    echo -e "${RED}  ✗ Failed to build KVT plugin${NC}"
    exit 1
fi

# Step 4: Initialize MariaDB data directory
echo -e "${YELLOW}Initializing MariaDB data directory...${NC}"
cd ${TEST_DIR}
${BUILD_DIR}/scripts/mariadb-install-db \
    --defaults-file=${CONFIG_FILE} \
    --srcdir=${PROJECT_ROOT} \
    --datadir=${TEST_DIR}/data \
    > /dev/null 2>&1

if [ $? -eq 0 ]; then
    echo -e "${GREEN}  ✓ Data directory initialized${NC}"
else
    echo -e "${RED}  ✗ Failed to initialize data directory${NC}"
    echo "  Check ${TEST_DIR}/mariadb.log for details"
    exit 1
fi

# Step 5: Start MariaDB server
echo -e "${YELLOW}Starting MariaDB server...${NC}"
${BUILD_DIR}/sql/mariadbd --defaults-file=${CONFIG_FILE} &
MARIADB_PID=$!

# Wait for server to be ready
echo "  Waiting for server to start..."
for i in {1..30}; do
    if ${BUILD_DIR}/client/mariadb --socket=${TEST_DIR}/mysql.sock -e "SELECT 1" > /dev/null 2>&1; then
        echo -e "${GREEN}  ✓ MariaDB server started (PID: ${MARIADB_PID})${NC}"
        break
    fi
    sleep 1
done

# Check if server started successfully
if ! ${BUILD_DIR}/client/mariadb --socket=${TEST_DIR}/mysql.sock -e "SELECT 1" > /dev/null 2>&1; then
    echo -e "${RED}  ✗ Failed to start MariaDB server${NC}"
    echo "  Check ${TEST_DIR}/mariadb.log for details"
    exit 1
fi

# Step 6: Load KVT plugin dynamically
echo -e "${YELLOW}Loading KVT storage engine plugin...${NC}"
${BUILD_DIR}/client/mariadb --socket=${TEST_DIR}/mysql.sock -e "INSTALL SONAME 'ha_kvt';" 2>/dev/null
if [ $? -eq 0 ]; then
    echo -e "${GREEN}  ✓ KVT plugin loaded${NC}"
    
    # Set KVT as default storage engine
    ${BUILD_DIR}/client/mariadb --socket=${TEST_DIR}/mysql.sock -e "SET GLOBAL default_storage_engine = 'KVT';" 2>/dev/null
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}  ✓ KVT set as default storage engine${NC}"
    fi
else
    echo -e "${YELLOW}  ! KVT plugin loading failed (may need system tables)${NC}"
fi

# Step 7: Display connection information
echo -e "${GREEN}=== MariaDB with KVT Backend is Running ===${NC}"
echo "Connection details:"
echo "  Socket: ${TEST_DIR}/mysql.sock"
echo "  Port: 13306"
echo "  Log file: ${TEST_DIR}/mariadb.log"
echo "  General log: ${TEST_DIR}/general.log"
echo ""
echo "Connect using:"
echo "  ${BUILD_DIR}/client/mariadb --socket=${TEST_DIR}/mysql.sock"
echo ""
echo "To stop, run: ${DEPLOY_DIR}/clean.sh"