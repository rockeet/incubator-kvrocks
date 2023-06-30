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

#include <string>
#include <vector>

#include "lua.hpp"
#include "server/redis_connection.h"
#include "status.h"

#define REDIS_LUA_FUNC_SHA_PREFIX "f_"  // NOLINT

namespace lua {

lua_State *CreateState(bool read_only = false);
void DestroyState(lua_State *lua);

void LoadFuncs(lua_State *lua, bool read_only = false);
void LoadLibraries(lua_State *lua);
void RemoveUnsupportedFunctions(lua_State *lua);
void EnableGlobalsProtection(lua_State *lua);

int RedisCallCommand(lua_State *lua);
int RedisPCallCommand(lua_State *lua);
int RedisGenericCommand(lua_State *lua, int raise_error);
int RedisSha1hexCommand(lua_State *lua);
int RedisStatusReplyCommand(lua_State *lua);
int RedisErrorReplyCommand(lua_State *lua);
int RedisLogCommand(lua_State *lua);

Status CreateFunction(Server *srv, const std::string &body, std::string *sha, lua_State *lua, bool need_to_store);

Status EvalGenericCommand(redis::Connection *conn, const std::string &body_or_sha, const std::vector<std::string> &keys,
                          const std::vector<std::string> &argv, bool evalsha, std::string *output,
                          bool read_only = false);

const char *RedisProtocolToLuaType(lua_State *lua, const char *reply);
const char *RedisProtocolToLuaTypeInt(lua_State *lua, const char *reply);
const char *RedisProtocolToLuaTypeBulk(lua_State *lua, const char *reply);
const char *RedisProtocolToLuaTypeStatus(lua_State *lua, const char *reply);
const char *RedisProtocolToLuaTypeError(lua_State *lua, const char *reply);
const char *RedisProtocolToLuaTypeAggregate(lua_State *lua, const char *reply, int atype);
const char *RedisProtocolToLuaTypeNull(lua_State *lua, const char *reply);
const char *RedisProtocolToLuaTypeBool(lua_State *lua, const char *reply, int tf);
const char *RedisProtocolToLuaTypeDouble(lua_State *lua, const char *reply);

std::string ReplyToRedisReply(lua_State *lua);

void PushError(lua_State *lua, const char *err);
[[noreturn]] int RaiseError(lua_State *lua);

void SortArray(lua_State *lua);
void SetGlobalArray(lua_State *lua, const std::string &var, const std::vector<std::string> &elems);

void SHA1Hex(char *digest, const char *script, size_t len);

int RedisMathRandom(lua_State *l);
int RedisMathRandomSeed(lua_State *l);

}  // namespace lua
