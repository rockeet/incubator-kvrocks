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

#include <gtest/gtest.h>

#include <memory>

#include "test_base.h"
#include "types/redis_zset.h"

class RedisZSetTest : public TestBase {
 protected:
  RedisZSetTest() { zset_ = std::make_unique<redis::ZSet>(storage_, "zset_ns"); }
  ~RedisZSetTest() override = default;

  void SetUp() override {
    key_ = "test_zset_key";
    fields_ = {"zset_test_key-1", "zset_test_key-2", "zset_test_key-3", "zset_test_key-4",
               "zset_test_key-5", "zset_test_key-6", "zset_test_key-7"};
    scores_ = {-100.1, -100.1, -1.234, 0, 1.234, 1.234, 100.1};
  }

  std::vector<double> scores_;
  std::unique_ptr<redis::ZSet> zset_;
};

TEST_F(RedisZSetTest, Add) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(static_cast<int>(fields_.size()), ret);
  for (size_t i = 0; i < fields_.size(); i++) {
    double got = 0.0;
    rocksdb::Status s = zset_->Score(key_, fields_[i], &got);
    EXPECT_EQ(scores_[i], got);
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(ret, 0);
  zset_->Del(key_);
}

TEST_F(RedisZSetTest, IncrBy) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(fields_.size(), ret);
  for (size_t i = 0; i < fields_.size(); i++) {
    double increment = 12.3;
    double score = 0.0;
    zset_->IncrBy(key_, fields_[i], increment, &score);
    EXPECT_EQ(scores_[i] + increment, score);
  }
  zset_->Del(key_);
}

TEST_F(RedisZSetTest, Remove) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(fields_.size(), ret);
  zset_->Remove(key_, fields_, &ret);
  EXPECT_EQ(fields_.size(), ret);
  for (auto &field : fields_) {
    double score = 0.0;
    rocksdb::Status s = zset_->Score(key_, field, &score);
    EXPECT_TRUE(s.IsNotFound());
  }
  zset_->Del(key_);
}

TEST_F(RedisZSetTest, Range) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  int count = static_cast<int>(mscores.size() - 1);
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(fields_.size(), ret);
  RangeRankSpec rank_spec;
  rank_spec.start = 0;
  rank_spec.stop = -2;
  zset_->RangeByRank(key_, rank_spec, &mscores, nullptr);
  EXPECT_EQ(mscores.size(), count);
  for (size_t i = 0; i < mscores.size(); i++) {
    EXPECT_EQ(mscores[i].member, fields_[i].ToString());
    EXPECT_EQ(mscores[i].score, scores_[i]);
  }
  zset_->Del(key_);
}

TEST_F(RedisZSetTest, RevRange) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  int count = static_cast<int>(mscores.size() - 1);
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(static_cast<int>(fields_.size()), ret);
  RangeRankSpec rank_spec;
  rank_spec.start = 0;
  rank_spec.stop = -2;
  rank_spec.reversed = true;
  zset_->RangeByRank(key_, rank_spec, &mscores, nullptr);
  EXPECT_EQ(mscores.size(), count);
  for (size_t i = 0; i < mscores.size(); i++) {
    EXPECT_EQ(mscores[i].member, fields_[count - i].ToString());
    EXPECT_EQ(mscores[i].score, scores_[count - i]);
  }
  zset_->Del(key_);
}

TEST_F(RedisZSetTest, PopMin) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(static_cast<int>(fields_.size()), ret);
  zset_->Pop(key_, static_cast<int>(mscores.size() - 1), true, &mscores);
  for (size_t i = 0; i < mscores.size(); i++) {
    EXPECT_EQ(mscores[i].member, fields_[i].ToString());
    EXPECT_EQ(mscores[i].score, scores_[i]);
  }
  zset_->Pop(key_, 1, true, &mscores);
  EXPECT_EQ(mscores[0].member, fields_[fields_.size() - 1].ToString());
  EXPECT_EQ(mscores[0].score, scores_[fields_.size() - 1]);
}

TEST_F(RedisZSetTest, PopMax) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  int count = static_cast<int>(fields_.size());
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(static_cast<int>(fields_.size()), ret);
  zset_->Pop(key_, static_cast<int>(mscores.size() - 1), false, &mscores);
  for (size_t i = 0; i < mscores.size(); i++) {
    EXPECT_EQ(mscores[i].member, fields_[count - i - 1].ToString());
    EXPECT_EQ(mscores[i].score, scores_[count - i - 1]);
  }
  zset_->Pop(key_, 1, true, &mscores);
  EXPECT_EQ(mscores[0].member, fields_[0].ToString());
}

TEST_F(RedisZSetTest, RangeByLex) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(fields_.size(), ret);

  RangeLexSpec spec;
  spec.min = fields_[0].ToString();
  spec.max = fields_[fields_.size() - 1].ToString();
  std::vector<std::string> members;
  zset_->RangeByLex(key_, spec, &members, nullptr);
  EXPECT_EQ(members.size(), fields_.size());
  for (size_t i = 0; i < members.size(); i++) {
    EXPECT_EQ(members[i], fields_[i].ToString());
  }

  spec.minex = true;
  zset_->RangeByLex(key_, spec, &members, nullptr);
  EXPECT_EQ(members.size(), fields_.size() - 1);
  for (size_t i = 0; i < members.size(); i++) {
    EXPECT_EQ(members[i], fields_[i + 1].ToString());
  }

  spec.minex = false;
  spec.maxex = true;
  zset_->RangeByLex(key_, spec, &members, nullptr);
  EXPECT_EQ(members.size(), fields_.size() - 1);
  for (size_t i = 0; i < members.size(); i++) {
    EXPECT_EQ(members[i], fields_[i].ToString());
  }

  spec.minex = true;
  spec.maxex = true;
  zset_->RangeByLex(key_, spec, &members, nullptr);
  EXPECT_EQ(members.size(), fields_.size() - 2);
  for (size_t i = 0; i < members.size(); i++) {
    EXPECT_EQ(members[i], fields_[i + 1].ToString());
  }
  spec.minex = false;
  spec.maxex = false;
  spec.min = "-";
  spec.max = "+";
  spec.max_infinite = true;
  spec.reversed = true;
  zset_->RangeByLex(key_, spec, &members, nullptr);
  EXPECT_EQ(members.size(), fields_.size());
  for (size_t i = 0; i < members.size(); i++) {
    EXPECT_EQ(members[i], fields_[6 - i].ToString());
  }

  zset_->Del(key_);
}

TEST_F(RedisZSetTest, RangeByScore) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(fields_.size(), ret);

  // test case: inclusive the min and max score
  RangeScoreSpec spec;
  spec.min = scores_[0];
  spec.max = scores_[scores_.size() - 2];
  zset_->RangeByScore(key_, spec, &mscores, nullptr);
  EXPECT_EQ(mscores.size(), scores_.size() - 1);
  for (size_t i = 0; i < mscores.size(); i++) {
    EXPECT_EQ(mscores[i].member, fields_[i].ToString());
    EXPECT_EQ(mscores[i].score, scores_[i]);
  }
  // test case: exclusive the min score
  spec.minex = true;
  zset_->RangeByScore(key_, spec, &mscores, nullptr);
  EXPECT_EQ(mscores.size(), scores_.size() - 3);
  for (size_t i = 0; i < mscores.size(); i++) {
    EXPECT_EQ(mscores[i].member, fields_[i + 2].ToString());
    EXPECT_EQ(mscores[i].score, scores_[i + 2]);
  }
  // test case: exclusive the max score
  spec.minex = false;
  spec.maxex = true;
  zset_->RangeByScore(key_, spec, &mscores, nullptr);
  EXPECT_EQ(mscores.size(), scores_.size() - 3);
  for (size_t i = 0; i < mscores.size(); i++) {
    EXPECT_EQ(mscores[i].member, fields_[i].ToString());
    EXPECT_EQ(mscores[i].score, scores_[i]);
  }
  // test case: exclusive the min and max score
  spec.minex = true;
  spec.maxex = true;
  zset_->RangeByScore(key_, spec, &mscores, nullptr);
  EXPECT_EQ(mscores.size(), scores_.size() - 5);
  for (size_t i = 0; i < mscores.size(); i++) {
    EXPECT_EQ(mscores[i].member, fields_[i + 2].ToString());
    EXPECT_EQ(mscores[i].score, scores_[i + 2]);
  }
  zset_->Del(key_);
}

TEST_F(RedisZSetTest, RangeByScoreWithLimit) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(fields_.size(), ret);

  RangeScoreSpec spec;
  spec.offset = 1;
  spec.count = 2;
  zset_->RangeByScore(key_, spec, &mscores, nullptr);
  EXPECT_EQ(mscores.size(), 2);
  for (size_t i = 0; i < mscores.size(); i++) {
    EXPECT_EQ(mscores[i].member, fields_[i + 1].ToString());
    EXPECT_EQ(mscores[i].score, scores_[i + 1]);
  }
  zset_->Del(key_);
}

TEST_F(RedisZSetTest, RemRangeByScore) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(fields_.size(), ret);
  RangeScoreSpec spec;
  spec.with_deletion = true;

  spec.min = scores_[0];
  spec.max = scores_[scores_.size() - 2];
  zset_->RangeByScore(key_, spec, nullptr, &ret);
  EXPECT_EQ(scores_.size() - 1, ret);

  spec.min = scores_[scores_.size() - 1];
  spec.max = spec.min;
  zset_->RangeByScore(key_, spec, nullptr, &ret);
  EXPECT_EQ(1, ret);
}

TEST_F(RedisZSetTest, RemoveRangeByRank) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(fields_.size(), ret);

  RangeRankSpec spec;
  spec.with_deletion = true;

  spec.start = 0;
  spec.stop = static_cast<int>(fields_.size() - 2);
  zset_->RangeByRank(key_, spec, nullptr, &ret);
  EXPECT_EQ(fields_.size() - 1, ret);

  spec.start = 0;
  spec.stop = 2;
  zset_->RangeByRank(key_, spec, nullptr, &ret);
  EXPECT_EQ(1, ret);
}

TEST_F(RedisZSetTest, RemoveRevRangeByRank) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(fields_.size(), ret);

  RangeRankSpec spec;
  spec.with_deletion = true;

  spec.start = 0;
  spec.stop = static_cast<int>(fields_.size() - 2);
  zset_->RangeByRank(key_, spec, nullptr, &ret);
  EXPECT_EQ(fields_.size() - 1, ret);

  spec.start = 0;
  spec.stop = 2;
  zset_->RangeByRank(key_, spec, nullptr, &ret);
  EXPECT_EQ(1, ret);
}

TEST_F(RedisZSetTest, Rank) {
  int ret = 0;
  std::vector<MemberScore> mscores;
  for (size_t i = 0; i < fields_.size(); i++) {
    mscores.emplace_back(MemberScore{fields_[i].ToString(), scores_[i]});
  }
  zset_->Add(key_, ZAddFlags::Default(), &mscores, &ret);
  EXPECT_EQ(static_cast<int>(fields_.size()), ret);

  for (size_t i = 0; i < fields_.size(); i++) {
    int rank = 0;
    zset_->Rank(key_, fields_[i], false, &rank);
    EXPECT_EQ(i, rank);
  }
  for (size_t i = 0; i < fields_.size(); i++) {
    int rank = 0;
    zset_->Rank(key_, fields_[i], true, &rank);
    EXPECT_EQ(i, static_cast<int>(fields_.size() - rank - 1));
  }
  std::vector<std::string> no_exist_members = {"a", "b"};
  for (const auto &member : no_exist_members) {
    int rank = 0;
    zset_->Rank(key_, member, true, &rank);
    EXPECT_EQ(-1, rank);
  }
  zset_->Del(key_);
}
