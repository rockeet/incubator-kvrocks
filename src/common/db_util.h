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

#pragma once

#include <memory>

#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "storage/storage.h"

namespace util {

struct UniqueIterator : std::unique_ptr<rocksdb::Iterator> {
  using BaseType = std::unique_ptr<rocksdb::Iterator>;

  explicit UniqueIterator(rocksdb::Iterator* iter) : BaseType(iter) {}
  UniqueIterator(engine::Storage* storage, const rocksdb::ReadOptions& options,
                 rocksdb::ColumnFamilyHandle* column_family)
      : BaseType(storage->NewIterator(options, column_family)) {}
  UniqueIterator(engine::Storage* storage, const rocksdb::ReadOptions& options)
      : BaseType(storage->NewIterator(options)) {}
};

}  // namespace util
