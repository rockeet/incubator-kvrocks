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

#include "redis_set.h"

#include <map>
#include <memory>

#include "db_util.h"

namespace redis {

rocksdb::Status Set::GetMetadata(const Slice &ns_key, SetMetadata *metadata) {
  return Database::GetMetadata(kRedisSet, ns_key, metadata);
}

// Make sure members are uniq before use Overwrite
rocksdb::Status Set::Overwrite(Slice user_key, const std::vector<std::string> &members) {
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  SetMetadata metadata;
  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisSet);
  batch->PutLogData(log_data.Encode());
  std::string sub_key;
  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    batch->Put(sub_key, Slice());
  }
  metadata.size = static_cast<uint32_t>(members.size());
  std::string bytes;
  metadata.Encode(&bytes);
  batch->Put(metadata_cf_handle_, ns_key, bytes);
  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

rocksdb::Status Set::Add(const Slice &user_key, const std::vector<Slice> &members, int *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  SetMetadata metadata;
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;

  std::string value;
  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisSet);
  batch->PutLogData(log_data.Encode());
  std::string sub_key;
  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    s = storage_->Get(rocksdb::ReadOptions(), sub_key, &value);
    if (s.ok()) continue;
    batch->Put(sub_key, Slice());
    *ret += 1;
  }
  if (*ret > 0) {
    metadata.size += *ret;
    std::string bytes;
    metadata.Encode(&bytes);
    batch->Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

rocksdb::Status Set::Remove(const Slice &user_key, const std::vector<Slice> &members, int *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  std::string value, sub_key;
  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisSet);
  batch->PutLogData(log_data.Encode());
  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    s = storage_->Get(rocksdb::ReadOptions(), sub_key, &value);
    if (!s.ok()) continue;
    batch->Delete(sub_key);
    *ret += 1;
  }
  if (*ret > 0) {
    if (static_cast<int>(metadata.size) != *ret) {
      metadata.size -= *ret;
      std::string bytes;
      metadata.Encode(&bytes);
      batch->Put(metadata_cf_handle_, ns_key, bytes);
    } else {
      batch->Delete(metadata_cf_handle_, ns_key);
    }
  }
  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

rocksdb::Status Set::Card(const Slice &user_key, int *ret) {
  *ret = 0;
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;
  *ret = static_cast<int>(metadata.size);
  return rocksdb::Status::OK();
}

rocksdb::Status Set::Members(const Slice &user_key, std::vector<std::string> *members) {
  members->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  std::string prefix, next_version_prefix;
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(storage_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix);
  read_options.iterate_upper_bound = &upper_bound;
  storage_->SetReadOptions(read_options);

  auto iter = util::UniqueIterator(storage_, read_options);
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix); iter->Next()) {
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    members->emplace_back(ikey.GetSubKey().ToString());
  }
  return rocksdb::Status::OK();
}

rocksdb::Status Set::IsMember(const Slice &user_key, const Slice &member, int *ret) {
  std::vector<int> exists;
  rocksdb::Status s = MIsMember(user_key, {member}, &exists);
  if (!s.ok()) return s;
  *ret = exists[0];
  return s;
}

rocksdb::Status Set::MIsMember(const Slice &user_key, const std::vector<Slice> &members, std::vector<int> *exists) {
  exists->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(storage_);
  read_options.snapshot = ss.GetSnapShot();
  std::string sub_key, value;
  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version, storage_->IsSlotIdEncoded()).Encode(&sub_key);
    s = storage_->Get(read_options, sub_key, &value);
    if (!s.ok() && !s.IsNotFound()) return s;
    if (s.IsNotFound()) {
      exists->emplace_back(0);
    } else {
      exists->emplace_back(1);
    }
  }
  return rocksdb::Status::OK();
}

rocksdb::Status Set::Take(const Slice &user_key, std::vector<std::string> *members, int count, bool pop) {
  int n = 0;
  members->clear();
  if (count <= 0) return rocksdb::Status::OK();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  std::unique_ptr<LockGuard> lock_guard;
  if (pop) lock_guard = std::make_unique<LockGuard>(storage_->GetLockManager(), ns_key);

  SetMetadata metadata(false);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisSet);
  batch->PutLogData(log_data.Encode());

  std::string prefix, next_version_prefix;
  InternalKey(ns_key, "", metadata.version, storage_->IsSlotIdEncoded()).Encode(&prefix);
  InternalKey(ns_key, "", metadata.version + 1, storage_->IsSlotIdEncoded()).Encode(&next_version_prefix);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(storage_);
  read_options.snapshot = ss.GetSnapShot();
  rocksdb::Slice upper_bound(next_version_prefix);
  read_options.iterate_upper_bound = &upper_bound;
  storage_->SetReadOptions(read_options);

  auto iter = util::UniqueIterator(storage_, read_options);
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix); iter->Next()) {
    InternalKey ikey(iter->key(), storage_->IsSlotIdEncoded());
    members->emplace_back(ikey.GetSubKey().ToString());
    if (pop) batch->Delete(iter->key());
    if (++n >= count) break;
  }
  if (pop && n > 0) {
    metadata.size -= n;
    std::string bytes;
    metadata.Encode(&bytes);
    batch->Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

rocksdb::Status Set::Move(const Slice &src, const Slice &dst, const Slice &member, int *ret) {
  RedisType type = kRedisNone;
  rocksdb::Status s = Type(dst, &type);
  if (!s.ok()) return s;
  if (type != kRedisNone && type != kRedisSet) {
    return rocksdb::Status::InvalidArgument(kErrMsgWrongType);
  }

  std::vector<Slice> members{member};
  s = Remove(src, members, ret);
  if (!s.ok() || *ret == 0) {
    return s;
  }
  return Add(dst, members, ret);
}

rocksdb::Status Set::Scan(const Slice &user_key, const std::string &cursor, uint64_t limit,
                          const std::string &member_prefix, std::vector<std::string> *members) {
  return SubKeyScanner::Scan(kRedisSet, user_key, cursor, limit, member_prefix, members);
}

/*
 * Returns the members of the set resulting from the difference between
 * the first set and all the successive sets. For example:
 * key1 = {a,b,c,d}
 * key2 = {c}
 * key3 = {a,c,e}
 * DIFF key1 key2 key3 = {b,d}
 */
rocksdb::Status Set::Diff(const std::vector<Slice> &keys, std::vector<std::string> *members) {
  members->clear();
  std::vector<std::string> source_members;
  auto s = Members(keys[0], &source_members);
  if (!s.ok()) return s;

  std::map<std::string, bool> exclude_members;
  std::vector<std::string> target_members;
  for (size_t i = 1; i < keys.size(); i++) {
    s = Members(keys[i], &target_members);
    if (!s.ok()) return s;
    for (const auto &member : target_members) {
      exclude_members[member] = true;
    }
  }
  for (const auto &member : source_members) {
    if (exclude_members.find(member) == exclude_members.end()) {
      members->push_back(member);
    }
  }
  return rocksdb::Status::OK();
}

/*
 * Returns the members of the set resulting from the union of all the given sets.
 * For example:
 * key1 = {a,b,c,d}
 * key2 = {c}
 * key3 = {a,c,e}
 * UNION key1 key2 key3 = {a,b,c,d,e}
 */
rocksdb::Status Set::Union(const std::vector<Slice> &keys, std::vector<std::string> *members) {
  members->clear();

  std::map<std::string, bool> union_members;
  std::vector<std::string> target_members;
  for (const auto &key : keys) {
    auto s = Members(key, &target_members);
    if (!s.ok()) return s;
    for (const auto &member : target_members) {
      union_members[member] = true;
    }
  }
  for (const auto &iter : union_members) {
    members->emplace_back(iter.first);
  }
  return rocksdb::Status::OK();
}

/*
 * Returns the members of the set resulting from the intersection of all the given sets.
 * For example:
 * key1 = {a,b,c,d}
 * key2 = {c}
 * key3 = {a,c,e}
 * INTER key1 key2 key3 = {c}
 */
rocksdb::Status Set::Inter(const std::vector<Slice> &keys, std::vector<std::string> *members) {
  members->clear();

  std::map<std::string, size_t> member_counters;
  std::vector<std::string> target_members;
  auto s = Members(keys[0], &target_members);
  if (!s.ok() || target_members.empty()) return s;
  for (const auto &member : target_members) {
    member_counters[member] = 1;
  }
  for (size_t i = 1; i < keys.size(); i++) {
    s = Members(keys[i], &target_members);
    if (!s.ok() || target_members.empty()) return s;
    for (const auto &member : target_members) {
      if (member_counters.find(member) == member_counters.end()) continue;
      member_counters[member]++;
    }
  }
  for (const auto &iter : member_counters) {
    if (iter.second == keys.size()) {  // all the sets contain this member
      members->emplace_back(iter.first);
    }
  }
  return rocksdb::Status::OK();
}

rocksdb::Status Set::DiffStore(const Slice &dst, const std::vector<Slice> &keys, int *ret) {
  *ret = 0;
  std::vector<std::string> members;
  auto s = Diff(keys, &members);
  if (!s.ok()) return s;
  *ret = static_cast<int>(members.size());
  return Overwrite(dst, members);
}

rocksdb::Status Set::UnionStore(const Slice &dst, const std::vector<Slice> &keys, int *ret) {
  *ret = 0;
  std::vector<std::string> members;
  auto s = Union(keys, &members);
  if (!s.ok()) return s;
  *ret = static_cast<int>(members.size());
  return Overwrite(dst, members);
}

rocksdb::Status Set::InterStore(const Slice &dst, const std::vector<Slice> &keys, int *ret) {
  *ret = 0;
  std::vector<std::string> members;
  auto s = Inter(keys, &members);
  if (!s.ok()) return s;
  *ret = static_cast<int>(members.size());
  return Overwrite(dst, members);
}
}  // namespace redis
