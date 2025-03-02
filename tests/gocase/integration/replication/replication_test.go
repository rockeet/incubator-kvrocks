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
 */

package replication

import (
	"context"
	"fmt"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/apache/incubator-kvrocks/tests/gocase/util"
	"github.com/go-redis/redis/v9"
	"github.com/stretchr/testify/require"
)

func TestReplicationWithHostname(t *testing.T) {
	srvA := util.StartServer(t, map[string]string{})
	defer srvA.Close()
	rdbA := srvA.NewClient()
	defer func() { require.NoError(t, rdbA.Close()) }()
	util.Populate(t, rdbA, "", 100, 10)

	srvB := util.StartServer(t, map[string]string{})
	defer srvB.Close()
	rdbB := srvB.NewClient()
	defer func() { require.NoError(t, rdbB.Close()) }()

	t.Run("Set instance A as slave of B with localhost, for issue #1182", func(t *testing.T) {
		require.NoError(t, rdbA.SlaveOf(context.Background(), "localhost", fmt.Sprintf("%d", srvB.Port())).Err())
		util.SlaveOf(t, rdbA, srvB)
		util.WaitForSync(t, rdbA)
		ctx := context.Background()

		require.NoError(t, rdbB.Set(ctx, "mykey", "foo", 0).Err())
		require.Eventually(t, func() bool {
			return rdbA.Get(ctx, "mykey").Val() == "foo"
		}, 50*time.Second, 100*time.Millisecond)
	})
}

func TestReplicationLoading(t *testing.T) {
	srvA := util.StartServer(t, map[string]string{})
	defer srvA.Close()
	rdbA := srvA.NewClient()
	defer func() { require.NoError(t, rdbA.Close()) }()
	util.Populate(t, rdbA, "", 100, 10)

	srvB := util.StartServer(t, map[string]string{})
	defer srvB.Close()
	rdbB := srvB.NewClient()
	defer func() { require.NoError(t, rdbB.Close()) }()

	t.Run("Set instance A as slave of B", func(t *testing.T) {
		ctx := context.Background()
		require.NoError(t, rdbA.ConfigSet(ctx, "slave-empty-db-before-fullsync", "yes").Err())
		require.NoError(t, rdbA.ConfigSet(ctx, "fullsync-recv-file-delay", "2").Err())
		util.SlaveOf(t, rdbA, srvB)
		// Become loading state in 5 second
		require.Eventually(t, func() bool {
			return util.FindInfoEntry(rdbA, "loading") == "1"
		}, 5*time.Second, 50*time.Millisecond)

		require.Eventually(t, func() bool {
			return util.FindInfoEntry(rdbA, "loading") == "0"
		}, 50*time.Second, 100*time.Millisecond)

		// Reset config
		time.Sleep(time.Second)
		require.NoError(t, rdbA.ConfigSet(ctx, "slave-empty-db-before-fullsync", "no").Err())
		require.NoError(t, rdbA.ConfigSet(ctx, "fullsync-recv-file-delay", "0").Err())
		util.WaitForSync(t, rdbA)
	})
}

func TestReplicationBasics(t *testing.T) {
	master := util.StartServer(t, map[string]string{})
	defer master.Close()
	masterClient := master.NewClient()
	defer func() { require.NoError(t, masterClient.Close()) }()

	ctx := context.Background()
	require.NoError(t, masterClient.Set(ctx, "mykey", "foo", 0).Err())
	require.NoError(t, masterClient.Set(ctx, "mystring", "a", 0).Err())
	require.NoError(t, masterClient.LPush(ctx, "mylist", "a", "b", "c").Err())
	require.NoError(t, masterClient.SAdd(ctx, "myset", "a", "b", "c").Err())
	require.NoError(t, masterClient.HMSet(ctx, "myhash", "a", 1, "b", 2, "c", 3).Err())
	require.NoError(t, masterClient.ZAdd(ctx, "myzset",
		redis.Z{Score: 1, Member: "a"},
		redis.Z{Score: 2, Member: "b"},
		redis.Z{Score: 3, Member: "c"},
	).Err())

	slave := util.StartServer(t, map[string]string{})
	defer slave.Close()
	slaveClient := slave.NewClient()
	defer func() { require.NoError(t, slaveClient.Close()) }()

	t.Run("Second server should have role master at first", func(t *testing.T) {
		util.Populate(t, slaveClient, "", 100, 10)
		require.Equal(t, "master", util.FindInfoEntry(slaveClient, "role"))
	})

	t.Run("The role should immediately be changed to 'replica'", func(t *testing.T) {
		util.SlaveOf(t, slaveClient, master)
		require.Equal(t, "slave", util.FindInfoEntry(slaveClient, "role"))
	})

	util.WaitForSync(t, slaveClient)
	t.Run("Sync should have transferred keys from master", func(t *testing.T) {
		require.Equal(t, masterClient.Get(ctx, "mykey"), slaveClient.Get(ctx, "mykey"))
		require.Equal(t, masterClient.Get(ctx, "mystring"), slaveClient.Get(ctx, "mystring"))
		require.Equal(t, masterClient.LRange(ctx, "mylist", 0, -1),
			slaveClient.LRange(ctx, "mylist", 0, -1))
		require.Equal(t, masterClient.HGetAll(ctx, "myhash"), slaveClient.HGetAll(ctx, "myhash"))
		require.Equal(t, masterClient.ZRangeWithScores(ctx, "myzset", 0, -1),
			slaveClient.ZRangeWithScores(ctx, "myzset", 0, -1))
		require.Equal(t, masterClient.SMembers(ctx, "myhash"), slaveClient.SMembers(ctx, "myhash"))
	})

	t.Run("The link status should be up", func(t *testing.T) {
		require.Equal(t, "up", util.FindInfoEntry(slaveClient, "master_link_status"))
	})

	t.Run("SET on the master should immediately propagate", func(t *testing.T) {
		require.NoError(t, masterClient.Set(ctx, "mykey", "bar", 0).Err())
		require.Eventually(t, func() bool {
			return slaveClient.Get(ctx, "mykey").Val() == "bar"
		}, 50*time.Second, 100*time.Millisecond)
	})

	t.Run("FLUSHALL should be replicated", func(t *testing.T) {
		require.NoError(t, masterClient.FlushAll(ctx).Err())
		time.Sleep(100 * time.Millisecond)
		require.NoError(t, masterClient.Do(ctx, "dbsize", "scan").Err())
		require.Eventually(t, func() bool {
			masterDBSize, err := masterClient.Do(ctx, "dbsize").Result()
			require.NoError(t, err)
			slaveDBSize, err := slaveClient.Do(ctx, "dbsize").Result()
			require.NoError(t, err)
			return masterDBSize.(int64) == 0 && slaveDBSize.(int64) == 0
		}, 5*time.Second, 100*time.Millisecond)
	})

	t.Run("ROLE in master reports master with a slave", func(t *testing.T) {
		vals, err := masterClient.Do(ctx, "role").Slice()
		require.NoError(t, err)
		require.EqualValues(t, 3, len(vals))
		// The order of vals in master is: role, offset, slaves
		require.EqualValues(t, "master", vals[0])
		masterOffset, err := strconv.Atoi(vals[1].(string))
		require.NoError(t, err)
		require.Greater(t, masterOffset, 0)
		slaves, ok := vals[2].([]interface{})
		require.True(t, ok)
		require.EqualValues(t, 1, len(slaves))
		slave0, _ := slaves[0].([]interface{})
		require.EqualValues(t, 3, len(slave0))
		slaveOffset, err := strconv.Atoi(slave0[2].(string))
		require.NoError(t, err)
		util.BetweenValues(t, slaveOffset, 1, masterOffset)
	})

	t.Run("ROLE in slave reports slave in connected state", func(t *testing.T) {
		vals, err := slaveClient.Do(ctx, "role").StringSlice()
		require.NoError(t, err)
		require.EqualValues(t, 5, len(vals))
		// The order of vals in slave is: role, master_host, master_port, slave_state, offset
		require.Equal(t, "slave", vals[0])
		require.Equal(t, "connected", vals[3])
	})
}

func TestReplicationWithMultiSlaves(t *testing.T) {
	srvA := util.StartServer(t, map[string]string{})
	defer srvA.Close()
	rdbA := srvA.NewClient()
	defer func() { require.NoError(t, rdbA.Close()) }()
	util.Populate(t, rdbA, "", 100, 10)

	srvB := util.StartServer(t, map[string]string{})
	defer srvB.Close()
	rdbB := srvB.NewClient()
	defer func() { require.NoError(t, rdbB.Close()) }()
	util.Populate(t, rdbB, "", 100, 10)

	srvC := util.StartServer(t, map[string]string{})
	defer srvC.Close()
	rdbC := srvC.NewClient()
	defer func() { require.NoError(t, rdbC.Close()) }()
	util.Populate(t, rdbC, "", 50, 10)

	t.Run("Multi slaves full sync with master at the same time", func(t *testing.T) {
		util.SlaveOf(t, rdbA, srvC)
		util.SlaveOf(t, rdbB, srvC)
		util.WaitForSync(t, rdbA)
		util.WaitForSync(t, rdbB)
		require.Eventually(t, func() bool {
			roleA := rdbA.Do(context.Background(), "role").String()
			roleB := rdbB.Do(context.Background(), "role").String()
			return strings.Contains(roleA, "connected") && strings.Contains(roleB, "connected")
		}, 50*time.Second, 100*time.Millisecond)
		require.Equal(t, "2", util.FindInfoEntry(rdbC, "sync_full"))
	})
}

func TestReplicationWithLimitSpeed(t *testing.T) {
	master := util.StartServer(t, map[string]string{
		"max-replication-mb":            "1",
		"rocksdb.compression":           "no",
		"rocksdb.write_buffer_size":     "1",
		"rocksdb.target_file_size_base": "1",
	})
	defer master.Close()
	masterClient := master.NewClientWithOption(&redis.Options{
		ReadTimeout: 10 * time.Second,
	})
	defer func() { require.NoError(t, masterClient.Close()) }()
	util.Populate(t, masterClient, "", 1024, 10240)

	ctx := context.Background()
	require.NoError(t, masterClient.Set(ctx, "a", "b", 0).Err())
	require.NoError(t, masterClient.Do(ctx, "compact").Err())

	require.Eventually(t, func() bool {
		return util.FindInfoEntry(masterClient, "is_compacting") == "no"
	}, 10*time.Second, 100*time.Millisecond)

	slave := util.StartServer(t, map[string]string{})
	defer slave.Close()
	slaveClient := slave.NewClient()
	defer func() { require.NoError(t, slaveClient.Close()) }()
	util.Populate(t, slaveClient, "", 1026, 1)

	t.Run("resume broken transfer based files", func(t *testing.T) {
		// Try to transfer some files, because max-replication-mb 1,
		// so maybe more than 5 files are transferred for sleep 5s.
		util.SlaveOf(t, slaveClient, master)
		time.Sleep(5 * time.Second)

		// Restart master server, let the slave try to full sync with master again,
		// because slave already received some SST files, so we will skip them.
		master.Restart()
		masterClient.Close()
		masterClient = master.NewClient()

		require.NoError(t, masterClient.ConfigSet(ctx, "max-replication-mb", "0").Err())
		require.Eventually(t, func() bool {
			return slave.LogFileMatches(t, ".*skip count: 1.*")
		}, 50*time.Second, 1000*time.Millisecond)
		util.WaitForSync(t, slaveClient)
		require.Equal(t, "b", slaveClient.Get(ctx, "a").Val())
	})
}

func TestReplicationShareCheckpoint(t *testing.T) {
	master := util.StartServer(t, map[string]string{})
	defer master.Close()
	masterClient := master.NewClient()
	defer func() { require.NoError(t, masterClient.Close()) }()
	util.Populate(t, masterClient, "", 1024, 1)
	ctx := context.Background()
	require.NoError(t, masterClient.Set(ctx, "a", "b", 0).Err())
	require.NoError(t, masterClient.Do(ctx, "compact").Err())
	time.Sleep(time.Second)

	slave1 := util.StartServer(t, map[string]string{})
	defer slave1.Close()
	slave1Client := slave1.NewClient()
	defer func() { require.NoError(t, slave1Client.Close()) }()
	util.Populate(t, slave1Client, "", 1026, 1)

	slave2 := util.StartServer(t, map[string]string{})
	defer slave2.Close()
	slave2Client := slave2.NewClient()
	defer func() { require.NoError(t, slave2Client.Close()) }()
	util.Populate(t, slave2Client, "", 1026, 1)

	t.Run("two slaves share one checkpoint for full replication", func(t *testing.T) {
		util.SlaveOf(t, slave1Client, master)
		util.SlaveOf(t, slave2Client, master)

		require.Eventually(t, func() bool {
			return master.LogFileMatches(t, ".*Use current existing checkpoint.*")
		}, 50*time.Second, 100*time.Millisecond)
		util.WaitForSync(t, slave1Client)
		util.WaitForSync(t, slave2Client)
		require.Equal(t, "b", slave1Client.Get(ctx, "a").Val())
		require.Equal(t, "b", slave2Client.Get(ctx, "a").Val())
	})
}

func TestReplicationContinueRunning(t *testing.T) {
	master := util.StartServer(t, map[string]string{})
	defer master.Close()
	masterClient := master.NewClient()
	defer func() { require.NoError(t, masterClient.Close()) }()

	slave := util.StartServer(t, map[string]string{})
	defer slave.Close()
	slaveClient := slave.NewClient()
	defer func() { require.NoError(t, slaveClient.Close()) }()

	util.SlaveOf(t, slaveClient, master)
	util.WaitForSync(t, slaveClient)

	t.Run("Master doesn't pause replicating with replicas, #346", func(t *testing.T) {
		ctx := context.Background()
		// In #346, we find a bug, if one command contains more than special
		// number updates, master won't send replication stream to replicas.
		masterClient.HSet(ctx, "myhash", map[string]interface{}{
			"0": 0, "1": 1, "2": 2, "3": 3, "4": 4, "5": 5, "6": 6, "7": 7, "8": 8, "9": 9,
			"a": "a", "b": "b", "c": "c", "d": "d", "e": "e", "f": "f", "g": "g", "h": "h", "i": "i", "j": "j", "k": "k"})
		require.EqualValues(t, 21, masterClient.HLen(ctx, "myhash").Val())
		util.WaitForOffsetSync(t, masterClient, slaveClient)
		require.Equal(t, "1", slaveClient.HGet(ctx, "myhash", "1").Val())
		require.Equal(t, "a", slaveClient.HGet(ctx, "myhash", "a").Val())
	})
}

func TestReplicationChangePassword(t *testing.T) {
	master := util.StartServer(t, map[string]string{})
	defer master.Close()
	masterClient := master.NewClient()
	defer func() { require.NoError(t, masterClient.Close()) }()

	slave := util.StartServer(t, map[string]string{})
	defer slave.Close()
	slaveClient := slave.NewClient()
	defer func() { require.NoError(t, slaveClient.Close()) }()

	util.SlaveOf(t, slaveClient, master)
	util.WaitForSync(t, slaveClient)

	t.Run("Slave can re-sync with master after password change", func(t *testing.T) {
		ctx := context.Background()
		require.Contains(t, slaveClient.Info(ctx, "replication").String(), "role:slave")
		masterReplicationInfo := masterClient.Info(ctx, "replication").String()
		require.Contains(t, masterReplicationInfo, "role:master")
		require.Contains(t, masterReplicationInfo, slave.Host())
		require.Contains(t, masterReplicationInfo, strconv.Itoa(int(slave.Port())))

		// Change password and break repl connection
		require.NoError(t, masterClient.ConfigSet(ctx, "requirepass", "pass").Err())
		require.NoError(t, slaveClient.ConfigSet(ctx, "requirepass", "pass").Err())
		require.NoError(t, slaveClient.ConfigSet(ctx, "masterauth", "pass").Err())

		killedSlaveCount, err := masterClient.ClientKillByFilter(ctx, "type", "slave").Result()
		require.NoError(t, err)
		require.Greater(t, killedSlaveCount, int64(0))

		// Sleep to wait for the killed connection state to prevent `WaitForSync` running
		// before the slave finds the connection is down.
		time.Sleep(time.Second)
		util.WaitForSync(t, slaveClient)
		masterReplicationInfo = masterClient.Info(ctx, "replication").String()
		require.Contains(t, masterReplicationInfo, "role:master")
		require.Contains(t, masterReplicationInfo, slave.Host())
		require.Contains(t, masterReplicationInfo, strconv.Itoa(int(slave.Port())))
	})
}
