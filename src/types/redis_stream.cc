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

#include "redis_stream.h"

#include <rocksdb/status.h>

#include <memory>
#include <utility>
#include <vector>

#include "db_util.h"

namespace redis {

const char *errSetEntryIdSmallerThanLastGenerated =
    "The ID specified in XSETID is smaller than the target stream top item";
const char *errEntriesAddedSmallerThanStreamSize =
    "The entries_added specified in XSETID is smaller than the target stream length";
const char *errMaxDeletedIdGreaterThanLastGenerated =
    "The ID specified in XSETID is smaller than the provided max_deleted_entry_id";
const char *errEntriesAddedNotSpecifiedForEmptyStream = "an empty stream should have non-zero value of ENTRIESADDED";
const char *errMaxDeletedIdNotSpecifiedForEmptyStream = "an empty stream should have MAXDELETEDID";

rocksdb::Status Stream::GetMetadata(const Slice &stream_name, StreamMetadata *metadata) {
  return Database::GetMetadata(kRedisStream, stream_name, metadata);
}

rocksdb::Status Stream::GetLastGeneratedID(const Slice &stream_name, StreamEntryID *id) {
  std::string ns_key;
  AppendNamespacePrefix(stream_name, &ns_key);

  StreamMetadata metadata;
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }

  if (s.IsNotFound()) {
    id->ms = 0;
    id->seq = 0;
  } else {
    *id = metadata.last_generated_id;
  }

  return rocksdb::Status::OK();
}

StreamEntryID Stream::entryIDFromInternalKey(const rocksdb::Slice &key) const {
  InternalKey ikey(key, storage_->IsSlotIdEncoded());
  Slice entry_id = ikey.GetSubKey();
  StreamEntryID id;
  GetFixed64(&entry_id, &id.ms);
  GetFixed64(&entry_id, &id.seq);
  return id;
}

std::string Stream::internalKeyFromEntryID(const std::string &ns_key, const StreamMetadata &metadata,
                                           const StreamEntryID &id) const {
  std::string sub_key;
  PutFixed64(&sub_key, id.ms);
  PutFixed64(&sub_key, id.seq);
  std::string entry_key;
  InternalKey(ns_key, sub_key, metadata.version, storage_->IsSlotIdEncoded()).Encode(&entry_key);
  return entry_key;
}

rocksdb::Status Stream::Add(const Slice &stream_name, const StreamAddOptions &options,
                            const std::vector<std::string> &args, StreamEntryID *id) {
  for (auto const &v : args) {
    if (v.size() > INT32_MAX) {
      return rocksdb::Status::InvalidArgument("argument length is too high");
    }
  }

  std::string entry_value = EncodeStreamEntryValue(args);

  std::string ns_key;
  AppendNamespacePrefix(stream_name, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  StreamMetadata metadata;
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;

  if (s.IsNotFound() && options.nomkstream) {
    return s;
  }

  StreamEntryID next_entry_id;
  auto status = options.next_id_strategy->GenerateID(metadata.last_generated_id, &next_entry_id);
  if (!status.IsOK()) return rocksdb::Status::InvalidArgument(status.Msg());

  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisStream);
  batch->PutLogData(log_data.Encode());

  bool should_add = true;

  // trim the stream before adding a new entry to provide atomic XADD + XTRIM
  if (options.trim_options.strategy != StreamTrimStrategy::None) {
    StreamTrimOptions trim_options = options.trim_options;
    if (trim_options.strategy == StreamTrimStrategy::MaxLen) {
      // because one entry will be added, we can trim up to (MAXLEN-1) if MAXLEN was specified
      trim_options.max_len = options.trim_options.max_len > 0 ? options.trim_options.max_len - 1 : 0;
    }

    trim(ns_key, trim_options, &metadata, batch->GetWriteBatch());

    if (trim_options.strategy == StreamTrimStrategy::MinID && next_entry_id < trim_options.min_id) {
      // there is no sense to add this element because it would be removed, so just modify metadata and return it's ID
      should_add = false;
    }

    if (trim_options.strategy == StreamTrimStrategy::MaxLen && options.trim_options.max_len == 0) {
      // there is no sense to add this element because it would be removed, so just modify metadata and return it's ID
      should_add = false;
    }
  }

  if (should_add) {
    std::string entry_key = internalKeyFromEntryID(ns_key, metadata, next_entry_id);
    batch->Put(stream_cf_handle_, entry_key, entry_value);

    metadata.last_generated_id = next_entry_id;
    metadata.last_entry_id = next_entry_id;
    metadata.size += 1;

    if (metadata.size == 1) {
      metadata.first_entry_id = next_entry_id;
      metadata.recorded_first_entry_id = next_entry_id;
    }
  } else {
    metadata.last_generated_id = next_entry_id;
    metadata.max_deleted_entry_id = next_entry_id;
  }

  metadata.entries_added += 1;

  std::string metadata_bytes;
  metadata.Encode(&metadata_bytes);
  batch->Put(metadata_cf_handle_, ns_key, metadata_bytes);

  *id = next_entry_id;

  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

rocksdb::Status Stream::DeleteEntries(const rocksdb::Slice &stream_name, const std::vector<StreamEntryID> &ids,
                                      uint64_t *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(stream_name, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  StreamMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) {
    return s.IsNotFound() ? rocksdb::Status::OK() : s;
  }

  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisStream);
  batch->PutLogData(log_data.Encode());

  std::string next_version_prefix_key;
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix_key);
  std::string prefix_key;
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(storage_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix_key);
  read_options.iterate_upper_bound = &upper_bound;
  rocksdb::Slice lower_bound(prefix_key);
  read_options.iterate_lower_bound = &lower_bound;
  storage_->SetReadOptions(read_options);

  auto iter = util::UniqueIterator(storage_, read_options, stream_cf_handle_);

  for (const auto &id : ids) {
    std::string entry_key = internalKeyFromEntryID(ns_key, metadata, id);
    std::string value;
    s = storage_->Get(read_options, stream_cf_handle_, entry_key, &value);
    if (s.ok()) {
      *ret += 1;
      batch->Delete(stream_cf_handle_, entry_key);

      if (metadata.max_deleted_entry_id < id) {
        metadata.max_deleted_entry_id = id;
      }

      if (*ret == metadata.size) {
        metadata.first_entry_id.Clear();
        metadata.last_entry_id.Clear();
        metadata.recorded_first_entry_id.Clear();
        break;
      }

      if (id == metadata.first_entry_id) {
        iter->Seek(entry_key);
        iter->Next();
        if (iter->Valid()) {
          metadata.first_entry_id = entryIDFromInternalKey(iter->key());
          metadata.recorded_first_entry_id = metadata.first_entry_id;
        } else {
          metadata.first_entry_id.Clear();
          metadata.recorded_first_entry_id.Clear();
        }
      }

      if (id == metadata.last_entry_id) {
        iter->Seek(entry_key);
        iter->Prev();
        if (iter->Valid()) {
          metadata.last_entry_id = entryIDFromInternalKey(iter->key());
        } else {
          metadata.last_entry_id.Clear();
        }
      }
    }
  }

  if (*ret > 0) {
    metadata.size -= *ret;

    std::string bytes;
    metadata.Encode(&bytes);
    batch->Put(metadata_cf_handle_, ns_key, bytes);
  }

  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

// If `options` is StreamLenOptions{} the function just returns the number of entries in the stream.
// Additionally, if a specific entry ID is provided via `StreamLenOptions::entry_id`,
// the function starts counting entries from that ID. With only entry ID specified, the function counts elements
// between that ID and the last element in the stream.
// If `StreamLenOptions::to_first` is set to true, the function will count elements
// between specified ID and the first element in the stream.
// The entry with the ID `StreamLenOptions::entry_id` has not taken into account (it serves as exclusive boundary).
rocksdb::Status Stream::Len(const rocksdb::Slice &stream_name, const StreamLenOptions &options, uint64_t *ret) {
  *ret = 0;
  std::string ns_key;
  AppendNamespacePrefix(stream_name, &ns_key);

  StreamMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) {
    return s.IsNotFound() ? rocksdb::Status::OK() : s;
  }

  if (!options.with_entry_id) {
    *ret = metadata.size;
    return rocksdb::Status::OK();
  }

  if (options.entry_id > metadata.last_entry_id) {
    *ret = options.to_first ? metadata.size : 0;
    return rocksdb::Status::OK();
  }

  if (options.entry_id < metadata.first_entry_id) {
    *ret = options.to_first ? 0 : metadata.size;
    return rocksdb::Status::OK();
  }

  if ((!options.to_first && options.entry_id == metadata.first_entry_id) ||
      (options.to_first && options.entry_id == metadata.last_entry_id)) {
    *ret = metadata.size - 1;
    return rocksdb::Status::OK();
  }

  std::string prefix_key;
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);
  std::string next_version_prefix_key;
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix_key);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(storage_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice lower_bound(prefix_key);
  read_options.iterate_lower_bound = &lower_bound;
  rocksdb::Slice upper_bound(next_version_prefix_key);
  read_options.iterate_upper_bound = &upper_bound;
  storage_->SetReadOptions(read_options);

  auto iter = util::UniqueIterator(storage_, read_options, stream_cf_handle_);
  std::string start_key = internalKeyFromEntryID(ns_key, metadata, options.entry_id);

  iter->Seek(start_key);
  if (!iter->Valid()) {
    return rocksdb::Status::OK();
  }

  if (options.to_first) {
    iter->Prev();
  } else if (iter->key().ToString() == start_key) {
    iter->Next();
  }

  for (; iter->Valid(); options.to_first ? iter->Prev() : iter->Next()) {
    *ret += 1;
  }

  return rocksdb::Status::OK();
}

rocksdb::Status Stream::range(const std::string &ns_key, const StreamMetadata &metadata,
                              const StreamRangeOptions &options, std::vector<StreamEntry> *entries) const {
  std::string start_key = internalKeyFromEntryID(ns_key, metadata, options.start);
  std::string end_key = internalKeyFromEntryID(ns_key, metadata, options.end);

  if (start_key == end_key) {
    if (options.exclude_start || options.exclude_end) {
      return rocksdb::Status::OK();
    }

    std::string entry_value;
    auto s = storage_->Get(rocksdb::ReadOptions(), stream_cf_handle_, start_key, &entry_value);
    if (!s.ok()) {
      return s.IsNotFound() ? rocksdb::Status::OK() : s;
    }

    std::vector<std::string> values;
    auto rv = DecodeRawStreamEntryValue(entry_value, &values);
    if (!rv.IsOK()) {
      return rocksdb::Status::InvalidArgument(rv.Msg());
    }

    entries->emplace_back(options.start.ToString(), std::move(values));
    return rocksdb::Status::OK();
  }

  if ((!options.reverse && options.end < options.start) || (options.reverse && options.start < options.end)) {
    return rocksdb::Status::OK();
  }

  std::string next_version_prefix_key;
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix_key);
  std::string prefix_key;
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(storage_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix_key);
  read_options.iterate_upper_bound = &upper_bound;
  rocksdb::Slice lower_bound(prefix_key);
  read_options.iterate_lower_bound = &lower_bound;
  storage_->SetReadOptions(read_options);

  auto iter = util::UniqueIterator(storage_, read_options, stream_cf_handle_);
  iter->Seek(start_key);
  if (options.reverse && (!iter->Valid() || iter->key().ToString() != start_key)) {
    iter->SeekForPrev(start_key);
  }

  for (; iter->Valid() && (options.reverse ? iter->key().ToString() >= end_key : iter->key().ToString() <= end_key);
       options.reverse ? iter->Prev() : iter->Next()) {
    if (options.exclude_start && iter->key().ToString() == start_key) {
      continue;
    }

    if (options.exclude_end && iter->key().ToString() == end_key) {
      break;
    }

    std::vector<std::string> values;
    auto rv = DecodeRawStreamEntryValue(iter->value().ToString(), &values);
    if (!rv.IsOK()) {
      return rocksdb::Status::InvalidArgument(rv.Msg());
    }

    entries->emplace_back(entryIDFromInternalKey(iter->key()).ToString(), std::move(values));

    if (options.with_count && entries->size() == options.count) {
      break;
    }
  }

  return rocksdb::Status::OK();
}

rocksdb::Status Stream::getEntryRawValue(const std::string &ns_key, const StreamMetadata &metadata,
                                         const StreamEntryID &id, std::string *value) const {
  std::string entry_key = internalKeyFromEntryID(ns_key, metadata, id);
  return storage_->Get(rocksdb::ReadOptions(), stream_cf_handle_, entry_key, value);
}

rocksdb::Status Stream::GetStreamInfo(const rocksdb::Slice &stream_name, bool full, uint64_t count, StreamInfo *info) {
  std::string ns_key;
  AppendNamespacePrefix(stream_name, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  StreamMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  info->size = metadata.size;
  info->entries_added = metadata.entries_added;
  info->last_generated_id = metadata.last_generated_id;
  info->max_deleted_entry_id = metadata.max_deleted_entry_id;
  info->recorded_first_entry_id = metadata.recorded_first_entry_id;

  if (metadata.size == 0) {
    return rocksdb::Status::OK();
  }

  if (full) {
    uint64_t need_entries = metadata.size;
    if (count != 0 && count < metadata.size) {
      need_entries = count;
    }

    info->entries.reserve(need_entries);

    StreamRangeOptions options;
    options.start = metadata.first_entry_id;
    options.end = metadata.last_entry_id;
    options.with_count = true;
    options.count = need_entries;
    options.reverse = false;
    options.exclude_start = false;
    options.exclude_end = false;

    s = range(ns_key, metadata, options, &info->entries);
    if (!s.ok()) {
      return s;
    }
  } else {
    std::string first_value;
    s = getEntryRawValue(ns_key, metadata, metadata.first_entry_id, &first_value);
    if (!s.ok()) {
      return s;
    }

    std::vector<std::string> values;
    auto rv = DecodeRawStreamEntryValue(first_value, &values);
    if (!rv.IsOK()) {
      return rocksdb::Status::InvalidArgument(rv.Msg());
    }

    info->first_entry = std::make_unique<StreamEntry>(metadata.first_entry_id.ToString(), std::move(values));

    std::string last_value;
    s = getEntryRawValue(ns_key, metadata, metadata.last_entry_id, &last_value);
    if (!s.ok()) {
      return s;
    }

    rv = DecodeRawStreamEntryValue(last_value, &values);
    if (!rv.IsOK()) {
      return rocksdb::Status::InvalidArgument(rv.Msg());
    }

    info->last_entry = std::make_unique<StreamEntry>(metadata.last_entry_id.ToString(), std::move(values));
  }

  return rocksdb::Status::OK();
}

rocksdb::Status Stream::Range(const Slice &stream_name, const StreamRangeOptions &options,
                              std::vector<StreamEntry> *entries) {
  entries->clear();

  if (options.with_count && options.count == 0) {
    return rocksdb::Status::OK();
  }

  if (options.exclude_start && options.start.IsMaximum()) {
    return rocksdb::Status::InvalidArgument("invalid start ID for the interval");
  }

  if (options.exclude_end && options.end.IsMinimum()) {
    return rocksdb::Status::InvalidArgument("invalid end ID for the interval");
  }

  std::string ns_key;
  AppendNamespacePrefix(stream_name, &ns_key);

  StreamMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) {
    return s.IsNotFound() ? rocksdb::Status::OK() : s;
  }

  return range(ns_key, metadata, options, entries);
}

rocksdb::Status Stream::Trim(const rocksdb::Slice &stream_name, const StreamTrimOptions &options, uint64_t *ret) {
  *ret = 0;

  if (options.strategy == StreamTrimStrategy::None) {
    return rocksdb::Status::OK();
  }

  std::string ns_key;
  AppendNamespacePrefix(stream_name, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);

  StreamMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) {
    return s.IsNotFound() ? rocksdb::Status::OK() : s;
  }

  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisStream);
  batch->PutLogData(log_data.Encode());

  *ret = trim(ns_key, options, &metadata, batch->GetWriteBatch());

  if (*ret > 0) {
    std::string bytes;
    metadata.Encode(&bytes);
    batch->Put(metadata_cf_handle_, ns_key, bytes);

    return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
  }

  return rocksdb::Status::OK();
}

uint64_t Stream::trim(const std::string &ns_key, const StreamTrimOptions &options, StreamMetadata *metadata,
                      rocksdb::WriteBatch *batch) {
  if (metadata->size == 0) {
    return 0;
  }

  if (options.strategy == StreamTrimStrategy::MaxLen && metadata->size <= options.max_len) {
    return 0;
  }

  if (options.strategy == StreamTrimStrategy::MinID && metadata->first_entry_id >= options.min_id) {
    return 0;
  }

  uint64_t ret = 0;

  std::string next_version_prefix_key;
  InternalKey(ns_key, "", metadata->version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix_key);
  std::string prefix_key;
  InternalKey(ns_key, "", metadata->version, storage_->IsSlotIdEncoded()).Encode(&prefix_key);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(storage_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix_key);
  read_options.iterate_upper_bound = &upper_bound;
  rocksdb::Slice lower_bound(prefix_key);
  read_options.iterate_lower_bound = &lower_bound;
  storage_->SetReadOptions(read_options);

  auto iter = util::UniqueIterator(storage_, read_options, stream_cf_handle_);
  std::string start_key = internalKeyFromEntryID(ns_key, *metadata, metadata->first_entry_id);
  iter->Seek(start_key);

  std::string last_deleted;
  while (iter->Valid() && metadata->size > 0) {
    if (options.strategy == StreamTrimStrategy::MaxLen && metadata->size <= options.max_len) {
      break;
    }

    if (options.strategy == StreamTrimStrategy::MinID && metadata->first_entry_id >= options.min_id) {
      break;
    }

    batch->Delete(stream_cf_handle_, iter->key());

    ret += 1;
    metadata->size -= 1;
    last_deleted = iter->key().ToString();

    iter->Next();

    if (iter->Valid()) {
      metadata->first_entry_id = entryIDFromInternalKey(iter->key());
      metadata->recorded_first_entry_id = metadata->first_entry_id;
    } else {
      metadata->first_entry_id.Clear();
      metadata->recorded_first_entry_id.Clear();
    }
  }

  if (metadata->size == 0) {
    metadata->first_entry_id.Clear();
    metadata->last_entry_id.Clear();
    metadata->recorded_first_entry_id.Clear();
  }

  if (ret > 0) {
    metadata->max_deleted_entry_id = entryIDFromInternalKey(last_deleted);
  }

  return ret;
}

rocksdb::Status Stream::SetId(const Slice &stream_name, const StreamEntryID &last_generated_id,
                              std::optional<uint64_t> entries_added, std::optional<StreamEntryID> max_deleted_id) {
  if (max_deleted_id && last_generated_id < max_deleted_id) {
    return rocksdb::Status::InvalidArgument(errMaxDeletedIdGreaterThanLastGenerated);
  }

  std::string ns_key;
  AppendNamespacePrefix(stream_name, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);

  StreamMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }

  if (s.IsNotFound()) {
    if (!entries_added || entries_added == 0) {
      return rocksdb::Status::InvalidArgument(errEntriesAddedNotSpecifiedForEmptyStream);
    }

    if (!max_deleted_id || (max_deleted_id->ms == 0 && max_deleted_id->seq == 0)) {
      return rocksdb::Status::InvalidArgument(errMaxDeletedIdNotSpecifiedForEmptyStream);
    }

    // create an empty stream
    metadata = StreamMetadata();
  }

  if (metadata.size > 0 && last_generated_id < metadata.last_generated_id) {
    return rocksdb::Status::InvalidArgument(errSetEntryIdSmallerThanLastGenerated);
  }

  if (metadata.size > 0 && entries_added && entries_added < metadata.size) {
    return rocksdb::Status::InvalidArgument(errEntriesAddedSmallerThanStreamSize);
  }

  metadata.last_generated_id = last_generated_id;
  if (entries_added) {
    metadata.entries_added = *entries_added;
  }
  if (max_deleted_id && (max_deleted_id->ms != 0 || max_deleted_id->seq != 0)) {
    metadata.max_deleted_entry_id = *max_deleted_id;
  }

  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisStream, {"XSETID"});
  batch->PutLogData(log_data.Encode());

  std::string bytes;
  metadata.Encode(&bytes);
  batch->Put(metadata_cf_handle_, ns_key, bytes);

  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

}  // namespace redis
