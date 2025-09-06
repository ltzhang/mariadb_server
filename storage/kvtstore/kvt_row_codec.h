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

#ifndef KVT_ROW_CODEC_H
#define KVT_ROW_CODEC_H

#include "kvt_constants.h"
#include "kvt/kvt_inc.h"
#include "handler.h"
#include <string>
#include <vector>

// Forward declarations
struct TABLE;
class Field;

namespace kvt_row_codec {

// Row format header structure
struct RowHeader {
  uint8_t version;      // Format version
  uint8_t flags;        // Flags (reserved)
  uint16_t field_count; // Number of fields
  
  static const size_t SIZE = 4;
  
  void serialize(uchar* buffer) const;
  void deserialize(const uchar* buffer);
};

// Main row codec class
class RowCodec {
private:
  TABLE* table;
  std::vector<Field*> fields;
  size_t null_bytes;
  
public:
  explicit RowCodec(TABLE* table);
  ~RowCodec() = default;
  
  // Main encoding/decoding functions
  int encode_row(const uchar* record, std::string& encoded);
  int decode_row(const std::string& encoded, uchar* record);
  
  // Key generation functions
  std::string encode_primary_key(const uchar* record);
  std::string encode_index_key(uint key_num, const uchar* record);
  std::string encode_rowid(uint64_t rowid);
  
  // Helper functions for specific field types
  static int encode_field(Field* field, std::string& output);
  static int decode_field(Field* field, const uchar* data, size_t& offset);
  
  // Variable length encoding for integers
  static void encode_varint(uint64_t value, std::string& output);
  static uint64_t decode_varint(const uchar* data, size_t& offset);
  
  // NULL bitmap handling
  static size_t calc_null_bytes(uint field_count);
  static bool is_null(const uchar* null_bitmap, uint field_index);
  static void set_null(uchar* null_bitmap, uint field_index, bool is_null);
  
private:
  // Internal helpers
  int encode_field_data(Field* field, std::string& output);
  int decode_field_data(Field* field, const uchar* data, size_t length);
  
  // Type-specific encoders
  int encode_integer_field(Field* field, std::string& output);
  int encode_string_field(Field* field, std::string& output);
  int encode_decimal_field(Field* field, std::string& output);
  int encode_temporal_field(Field* field, std::string& output);
  int encode_blob_field(Field* field, std::string& output);
  
  // Type-specific decoders
  int decode_integer_field(Field* field, const uchar* data, size_t length);
  int decode_string_field(Field* field, const uchar* data, size_t length);
  int decode_decimal_field(Field* field, const uchar* data, size_t length);
  int decode_temporal_field(Field* field, const uchar* data, size_t length);
  int decode_blob_field(Field* field, const uchar* data, size_t length);
};

// Utility functions for binary-comparable encoding
namespace encoding_utils {
  
  // Encode integers in big-endian format for sorting
  std::string encode_uint8(uint8_t value);
  std::string encode_uint16(uint16_t value);
  std::string encode_uint32(uint32_t value);
  std::string encode_uint64(uint64_t value);
  
  std::string encode_int8(int8_t value);
  std::string encode_int16(int16_t value);
  std::string encode_int32(int32_t value);
  std::string encode_int64(int64_t value);
  
  // Decode from big-endian format
  uint8_t decode_uint8(const uchar* data);
  uint16_t decode_uint16(const uchar* data);
  uint32_t decode_uint32(const uchar* data);
  uint64_t decode_uint64(const uchar* data);
  
  int8_t decode_int8(const uchar* data);
  int16_t decode_int16(const uchar* data);
  int32_t decode_int32(const uchar* data);
  int64_t decode_int64(const uchar* data);
  
  // String encoding with proper sorting
  std::string encode_string_key(const char* str, size_t length);
  std::string decode_string_key(const uchar* data, size_t& offset);
  
  // Float/double encoding (IEEE 754 with sign bit flip for sorting)
  std::string encode_float(float value);
  std::string encode_double(double value);
  float decode_float(const uchar* data);
  double decode_double(const uchar* data);
}

} // namespace kvt_row_codec

#endif // KVT_ROW_CODEC_H