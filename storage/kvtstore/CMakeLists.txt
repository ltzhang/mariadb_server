# Copyright (c) 2025 KVT Storage Engine

SET(KVT_PLUGIN_DYNAMIC "ha_kvt")
SET(KVT_SOURCES 
    ha_kvt.cc 
    ha_kvt.h
    kvt_constants.h
    kvt_catalog.cc
    kvt_catalog.h
    kvt_row_codec.cc
    kvt_row_codec.h
    kvt_transaction_manager.cc
    kvt_transaction_manager.h
    kvt_index_manager.cc
    kvt_index_manager.h
    kvt_query_optimizer.cc
    kvt_query_optimizer.h
    kvt_foreign_key.cc
    kvt_foreign_key.h
    kvt_fulltext_adapter.cc
    kvt_fulltext_adapter.h
    kvt_spatial_adapter.cc
    kvt_spatial_adapter.h
    kvt_statistics.cc
    kvt_statistics.h
    kvt_composite_index.cc
    kvt_composite_index.h
    kvt_index_only_scan.cc
    kvt_index_only_scan.h
    kvt_unique_constraint.cc
    kvt_unique_constraint.h)

# Include directories
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/sql
                    ${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/storage/kvtstore
                    ${CMAKE_CURRENT_SOURCE_DIR})

# Compile the KVT memory implementation
ADD_CUSTOM_COMMAND(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kvt_memory.o
  COMMAND g++ -c -fPIC -g -O0 -I${CMAKE_CURRENT_SOURCE_DIR}/kvt 
          ${CMAKE_CURRENT_SOURCE_DIR}/kvt/kvt_mem.cpp
          -o ${CMAKE_CURRENT_BINARY_DIR}/kvt_memory.o
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kvt/kvt_mem.cpp
          ${CMAKE_CURRENT_SOURCE_DIR}/kvt/kvt_mem.h
          ${CMAKE_CURRENT_SOURCE_DIR}/kvt/kvt_inc.h
  COMMENT "Building KVT memory implementation"
)

# Create a custom target for the object file
ADD_CUSTOM_TARGET(kvt_memory_obj DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kvt_memory.o)

# Build the storage engine plugin
MYSQL_ADD_PLUGIN(kvt ${KVT_SOURCES}
  STORAGE_ENGINE
  MODULE_ONLY
  RECOMPILE_FOR_EMBEDDED
  LINK_LIBRARIES ${CMAKE_CURRENT_BINARY_DIR}/kvt_memory.o)

# Add dependency on the KVT memory object
IF(TARGET kvt)
  ADD_DEPENDENCIES(kvt kvt_memory_obj)
ENDIF()

# Install header files (optional)
IF(INSTALL_MYSQLTESTDIR)
  INSTALL(FILES ha_kvt.h DESTINATION ${INSTALL_MYSQLTESTDIR}/storage/kvt COMPONENT Test)
ENDIF()