/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "encoding.h"

#include <float.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <utility>

void EncodeFixed8(char *buf, uint8_t value) { buf[0] = static_cast<char>(value & 0xff); }

void EncodeFixed16(char *buf, uint16_t value) {
  if constexpr (IsLittleEndian()) {
    value = __builtin_bswap16(value);
  }
  __builtin_memcpy(buf, &value, sizeof(value));
}

void EncodeFixed32(char *buf, uint32_t value) {
  if constexpr (IsLittleEndian()) {
    value = __builtin_bswap32(value);
  }
  __builtin_memcpy(buf, &value, sizeof(value));
}

void EncodeFixed64(char *buf, uint64_t value) {
  if constexpr (IsLittleEndian()) {
    value = __builtin_bswap64(value);
  }
  __builtin_memcpy(buf, &value, sizeof(value));
}

void PutFixed8(std::string *dst, uint8_t value) {
  char buf[1];
  EncodeFixed8(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed16(std::string *dst, uint16_t value) {
  char buf[sizeof(value)];
  EncodeFixed16(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed32(std::string *dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string *dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutDouble(std::string *dst, double value) {
  uint64_t u64 = 0;

  __builtin_memcpy(&u64, &value, sizeof(value));

  if ((u64 >> 63) == 1) {
    // signed bit would be zero
    u64 ^= 0xffffffffffffffff;
  } else {
    // signed bit would be one
    u64 |= 0x8000000000000000;
  }

  PutFixed64(dst, u64);
}

bool GetFixed8(rocksdb::Slice *input, uint8_t *value) {
  const char *data = nullptr;
  if (input->size() < sizeof(uint8_t)) {
    return false;
  }
  data = input->data();
  *value = static_cast<uint8_t>(data[0] & 0xff);
  input->remove_prefix(sizeof(uint8_t));
  return true;
}

bool GetFixed64(rocksdb::Slice *input, uint64_t *value) {
  if (input->size() < sizeof(uint64_t)) {
    return false;
  }
  *value = DecodeFixed64(input->data());
  input->remove_prefix(sizeof(uint64_t));
  return true;
}

bool GetFixed32(rocksdb::Slice *input, uint32_t *value) {
  if (input->size() < sizeof(uint32_t)) {
    return false;
  }
  *value = DecodeFixed32(input->data());
  input->remove_prefix(sizeof(uint32_t));
  return true;
}

bool GetFixed16(rocksdb::Slice *input, uint16_t *value) {
  if (input->size() < sizeof(uint16_t)) {
    return false;
  }
  *value = DecodeFixed16(input->data());
  input->remove_prefix(sizeof(uint16_t));
  return true;
}

bool GetDouble(rocksdb::Slice *input, double *value) {
  if (input->size() < sizeof(double)) return false;
  *value = DecodeDouble(input->data());
  input->remove_prefix(sizeof(double));
  return true;
}

uint16_t DecodeFixed16(const char *ptr) {
  uint16_t value = 0;

  __builtin_memcpy(&value, ptr, sizeof(value));

  return IsLittleEndian() ? __builtin_bswap16(value) : value;
}

uint32_t DecodeFixed32(const char *ptr) {
  uint32_t value = 0;

  __builtin_memcpy(&value, ptr, sizeof(value));

  return IsLittleEndian() ? __builtin_bswap32(value) : value;
}

uint64_t DecodeFixed64(const char *ptr) {
  uint64_t value = 0;

  __builtin_memcpy(&value, ptr, sizeof(value));

  return IsLittleEndian() ? __builtin_bswap64(value) : value;
}

double DecodeDouble(const char *ptr) {
  uint64_t decoded = DecodeFixed64(ptr);

  if ((decoded >> 63) == 0) {
    decoded ^= 0xffffffffffffffff;
  } else {
    decoded &= 0x7fffffffffffffff;
  }

  double value = 0;
  __builtin_memcpy(&value, &decoded, sizeof(value));
  return value;
}

char *EncodeVarint32(char *dst, uint32_t v) {
  // Operate on characters as unsigneds
  auto *ptr = reinterpret_cast<unsigned char *>(dst);
  do {
    *ptr = 0x80 | v;
    v >>= 7, ++ptr;
  } while (v != 0);
  *(ptr - 1) &= 0x7F;
  return reinterpret_cast<char *>(ptr);
}

void PutVarint32(std::string *dst, uint32_t v) {
  char buf[5];
  char *ptr = EncodeVarint32(buf, v);
  dst->append(buf, static_cast<size_t>(ptr - buf));
}

const char *GetVarint32PtrFallback(const char *p, const char *limit, uint32_t *value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = static_cast<unsigned char>(*p);
    p++;
    if (byte & 0x80) {
      // More bytes are present
      result |= ((byte & 0x7F) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}

const char *GetVarint32Ptr(const char *p, const char *limit, uint32_t *value) {
  if (p < limit) {
    uint32_t result = static_cast<unsigned char>(*p);
    if ((result & 0x80) == 0) {
      *value = result;
      return p + 1;
    }
  }
  return GetVarint32PtrFallback(p, limit, value);
}

bool GetVarint32(rocksdb::Slice *input, uint32_t *value) {
  const char *p = input->data();
  const char *limit = p + input->size();
  const char *q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  } else {
    *input = rocksdb::Slice(q, static_cast<size_t>(limit - q));
    return true;
  }
}
