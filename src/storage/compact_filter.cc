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

#include "compact_filter.h"
#include "time_util.h"

#include <glog/logging.h>

#include <string>
#include <utility>

#include "types/redis_bitmap.h"
#ifdef KVROCKS_USE_TOPLINGDB
#include <terark/io/DataIO.hpp>
#include <terark/io/FileStream.hpp>
#include <topling/side_plugin_factory.h>
#endif

namespace Engine {
using rocksdb::Slice;

bool MetadataFilter::Filter(int level, const Slice &key, const Slice &value, std::string *new_value,
                            bool *modified) const {
  std::string ns, user_key, bytes = value.ToString();
  Metadata metadata(kRedisNone, false);
  rocksdb::Status s = metadata.Decode(bytes);
  ExtractNamespaceKey(key, &ns, &user_key, IsSlotIdEncoded_);
  if (!s.ok()) {
    LOG(WARNING) << "[compact_filter/metadata] Failed to decode,"
                 << ", namespace: " << ns << ", key: " << user_key << ", err: " << s.ToString();
    return false;
  }
  DLOG(INFO) << "[compact_filter/metadata] "
             << "namespace: " << ns << ", key: " << user_key
             << ", result: " << (metadata.Expired(now_) ? "deleted" : "reserved");
  return metadata.Expired(now_);
}

std::unique_ptr<rocksdb::CompactionFilter>
MetadataFilterFactory::CreateCompactionFilter(
    const rocksdb::CompactionFilter::Context &context) {
#ifdef KVROCKS_USE_TOPLINGDB
  auto now = rocksdb::IsCompactionWorker() ? now_ : Util::GetTimeStamp();
#else
  auto now = Util::GetTimeStamp();
#endif
  return std::make_unique<MetadataFilter>(now, IsSlotIdEncoded_);
}

Status SubKeyFilter::GetMetadata(const InternalKey &ikey, Metadata *metadata) const {
  std::string metadata_key;

  auto db = stor_->GetDB();
  const auto cf_handles = stor_->GetCFHandles();
  // storage close the would delete the column family handler and DB
  if (!db || cf_handles->size() < 2) return Status(Status::NotOK, "storage is closed");
  ComposeNamespaceKey(ikey.GetNamespace(), ikey.GetKey(), &metadata_key, stor_->IsSlotIdEncoded());

  if (cached_key_.empty() || metadata_key != cached_key_) {
    std::string bytes;
    rocksdb::Status s = db->Get(rocksdb::ReadOptions(), (*cf_handles)[1], metadata_key, &bytes);
    cached_key_ = std::move(metadata_key);
    if (s.ok()) {
      cached_metadata_ = std::move(bytes);
    } else if (s.IsNotFound()) {
      // metadata was deleted(perhaps compaction or manual)
      // clear the metadata
      cached_metadata_.clear();
      return Status(Status::NotFound, "metadata is not found");
    } else {
      cached_key_.clear();
      cached_metadata_.clear();
      return Status(Status::NotOK, "fetch error: " + s.ToString());
    }
  }
  // the metadata was not found
  if (cached_metadata_.empty()) return Status(Status::NotFound, "metadata is not found");
  // the metadata is cached
  rocksdb::Status s = metadata->Decode(cached_metadata_);
  if (!s.ok()) {
    cached_key_.clear();
    return Status(Status::NotOK, "decode error: " + s.ToString());
    ;
  }
  return Status::OK();
}

bool SubKeyFilter::IsMetadataExpired(const InternalKey &ikey, const Metadata &metadata) const {
  if (metadata.Type() == kRedisString  // metadata key was overwrite by set command
      || metadata.Expired() || ikey.GetVersion() != metadata.version) {
    return true;
  }
  return false;
}

rocksdb::CompactionFilter::Decision SubKeyFilter::FilterBlobByKey(int level, const Slice &key, std::string *new_value,
                                                                  std::string *skip_until) const {
  InternalKey ikey(key, stor_->IsSlotIdEncoded());
  Metadata metadata(kRedisNone, false);
  Status s = GetMetadata(ikey, &metadata);
  if (s.Is<Status::NotFound>()) {
    return rocksdb::CompactionFilter::Decision::kRemove;
  }
  if (!s.IsOK()) {
    LOG(ERROR) << "[compact_filter/subkey] Failed to get metadata"
               << ", namespace: " << ikey.GetNamespace().ToString() << ", key: " << ikey.GetKey().ToString()
               << ", err: " << s.Msg();
    return rocksdb::CompactionFilter::Decision::kKeep;
  }
  // bitmap will be checked in Filter
  if (metadata.Type() == kRedisBitmap) {
    return rocksdb::CompactionFilter::Decision::kUndetermined;
  }

  bool result = IsMetadataExpired(ikey, metadata);
  return result ? rocksdb::CompactionFilter::Decision::kRemove : rocksdb::CompactionFilter::Decision::kKeep;
}

bool SubKeyFilter::Filter(int level, const Slice &key, const Slice &value, std::string *new_value,
                          bool *modified) const {
  InternalKey ikey(key, stor_->IsSlotIdEncoded());
  Metadata metadata(kRedisNone, false);
  Status s = GetMetadata(ikey, &metadata);
  if (s.Is<Status::NotFound>()) {
    return true;
  }
  if (!s.IsOK()) {
    LOG(ERROR) << "[compact_filter/subkey] Failed to get metadata"
               << ", namespace: " << ikey.GetNamespace().ToString() << ", key: " << ikey.GetKey().ToString()
               << ", err: " << s.Msg();
    return false;
  }

  return IsMetadataExpired(ikey, metadata) || (metadata.Type() == kRedisBitmap && Redis::Bitmap::IsEmptySegment(value));
}

}  // namespace Engine

#ifdef KVROCKS_USE_TOPLINGDB
namespace rocksdb {
using namespace Engine;
ROCKSDB_REG_Plugin(MetadataFilterFactory , CompactionFilterFactory);
//ROCKSDB_REG_Plugin(SubKeyFilterFactory   , CompactionFilterFactory);
ROCKSDB_REG_Plugin(PropagateFilterFactory, CompactionFilterFactory);
ROCKSDB_REG_Plugin(PubSubFilterFactory   , CompactionFilterFactory);

using namespace terark;
struct MetadataFilterFactory_SerDe : SerDeFunc<CompactionFilterFactory> {
  void Serialize(FILE* output, const CompactionFilterFactory& base)
  const override {
    auto& fac = dynamic_cast<const MetadataFilterFactory&>(base);
    LittleEndianDataOutput<NonOwnerFileStream> dio(output);
    if (IsCompactionWorker()) {
      // do nothing
    } else {
      auto now = Util::GetTimeStamp();
      auto IsSlotIdEncoded = fac.stor_->IsSlotIdEncoded();
      dio << now;
      dio << IsSlotIdEncoded;
    }
  }
  void DeSerialize(FILE* reader, CompactionFilterFactory* base)
  const override {
    auto fac = dynamic_cast<MetadataFilterFactory*>(base);
    LittleEndianDataInput<NonOwnerFileStream> dio(reader);
    if (IsCompactionWorker()) {
      dio >> fac->now_;
      dio >> fac->IsSlotIdEncoded_;
    } else {
      // do nothing
    }
  }
};
ROCKSDB_REG_PluginSerDe(MetadataFilterFactory);

}
#endif