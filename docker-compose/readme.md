# kvrocks + rocksdb / topling 社区版 性能测试对比

## topling

[topling](https://topling.cn)是国产魔改版的 rocksdb 。

但是能开源构建只有社区版，所以评测的是社区版。

[topling 企业版性能要更好](https://github.com/apache/incubator-kvrocks/pull/1206#issuecomment-1549490143), 官方评测数据随机读是 topling 社区版的 4 倍。

## 测试环境

测试服务器是 [contabo VPS](https://contabo.com/en/vps/vps-m-ssd) , CPU 是 AMD EPYC 7282 16-Core Processor , 6 核 ，SSD。

DOCKER 容器内存都限制为 1GiB。

### kvrocks 版本

kvrocks 代码版本是[2023-05-17](https://github.com/apache/incubator-kvrocks/commit/27e843c5ee4e01bb0f4d083b362f22b61c285365)

我修改 github action 构建了 2 个镜像，基础镜像是 ubuntu:22.10，镜像地址如下：

* https://hub.docker.com/r/wactax/kvrocks-topling
* https://hub.docker.com/r/wactax/kvrocks

### 数据库配置参数

```
kvrocks_workers: 6
kvrocks_rocksdb_compression: zstd
kvrocks_timeout: 360
kvrocks_rocksdb_enable_blob_files: "yes"
kvrocks_read_options_async_io: "yes"
```

## 测试代码

每个操作测试的时长是 10 分钟，客户端连接数 50，线程数 20，get/set 测试的读写比例是 100:1 ，数据大小是 4-4096。

测试代码 [./benchmark.sh](https://github.com/wacfork/incubator-kvrocks/blob/unstable/docker-compose/benchmark.sh) 如下，可克隆仓库自己运行：

```bash
#!/usr/bin/env bash

DIR=$(dirname $(realpath "$0"))
cd $DIR
set -ex

PORT=51002
TESTTIME=600

mb="memtier_benchmark -t 20 -c 50 -s 127.0.0.1 -p $PORT --distinct-client-seed \
    --key-minimum=2 --key-maximum=100 \
    --random-data --data-size-range=4-4096 --test-time=$TESTTIME"

mbc() {
  args="$@"
  $mb --key-prefix=$1 --command="$args"
}

_bench() {
  cd $DIR/$1
  docker-compose pull
  docker-compose down
  rm -rf /tmp/benchmark
  docker-compose up -d
  sleep 2
  until nc -z localhost $PORT; do
    echo "wait PORT $PORT" >&2
    sleep 2
  done
  sleep 2

  echo "benchmark $1"

  $mb --ratio=1:100 --key-prefix=":" --key-pattern=G:G --key-stddev=10 --key-median=20
  $mb --command='zadd __key__ __key__ __data__' --key-prefix=''
  mbc sadd __key__ __data__
  mbc hset __key__ __data__ __data__

  docker stats --no-stream $(docker ps --format={{.Names}})
  docker-compose down
  trap "cd $DIR/$1 && docker-compose down" INT
}

bench() {
  cd $DIR
  rm -rf $1.log
  _bench $1 | tee $DIR/$1.txt
}

bench kvrocks
bench kvrocks-topling
rm -rf /tmp/benchmark
```

## 测试结果

测试结果两者差距不大，可能是因为开源的社区版 topling 与 rocksdb 的改动并不大。

另外可能是 memtier_benchmark 生成随机数不利于压缩，如果用真实世界的文本也许 topling 会有优势。

但是，真实世界的文本，如果在客户端接入类似我以前写的 [rmw-utf8](https://github.com/rmw-link/rmw-utf8#readme) 对文本提前压缩，是不是可以缩小 rocksdb 和 topling 的差距？

因为时间原因，先不测试了。以后有空再玩。

### [kvrocks 测试结果](https://github.com/wacfork/incubator-kvrocks/blob/unstable/docker-compose/kvrocks.txt) 

```
❯ cat kvrocks.txt|rg Ops -A5
Type         Ops/sec     Hits/sec   Misses/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec
----------------------------------------------------------------------------------------------------------------------------
Sets          518.79          ---          ---        19.82097        17.40700        60.92700       134.14300      1057.88
Gets        51797.27     51794.89         2.38        19.09143        17.02300        56.57500        82.43100    105356.62
Waits           0.00          ---          ---             ---             ---             ---             ---          ---
Totals      52316.06     51794.89         2.38        19.09866        17.02300        56.57500        82.94300    106414.50
--
Type         Ops/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec
--------------------------------------------------------------------------------------------------
Zadds        5922.19       156.32444       144.38300       358.39900       483.32700     12096.48
Totals       5922.19       156.32444       144.38300       358.39900       483.32700     12096.48


--
Type         Ops/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec
--------------------------------------------------------------------------------------------------
Sadds        6856.88       145.86921       139.26300       307.19900       395.26300     13984.92
Totals       6856.88       145.86921       139.26300       307.19900       395.26300     13984.92


--
Type         Ops/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec
--------------------------------------------------------------------------------------------------
Hsets        4482.00       210.46223       194.55900       522.23900       794.62300     18151.69
Totals       4482.00       210.46223       194.55900       522.23900       794.62300     18151.69

```

### [kvrocks-topling 测试结果](https://github.com/wacfork/incubator-kvrocks/blob/unstable/docker-compose/kvrocks-topling.txt) 

```
❯ cat kvrocks-topling.txt|rg Ops -A5
Type         Ops/sec     Hits/sec   Misses/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec
----------------------------------------------------------------------------------------------------------------------------
Sets          527.17          ---          ---        19.46656        17.15100        59.90300       132.09500      1075.03
Gets        52634.09     52631.70         2.39        18.78902        16.76700        55.55100        81.40700    107173.83
Waits           0.00          ---          ---             ---             ---             ---             ---          ---
Totals      53161.26     52631.70         2.39        18.79574        16.76700        55.55100        81.91900    108248.87
--
Type         Ops/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec
--------------------------------------------------------------------------------------------------
Zadds        5483.42       168.62517       151.55100       419.83900       581.63100     11199.59
Totals       5483.42       168.62517       151.55100       419.83900       581.63100     11199.59


--
Type         Ops/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec
--------------------------------------------------------------------------------------------------
Sadds        6690.33       149.41016       143.35900       305.15100       401.40700     13645.16
Totals       6690.33       149.41016       143.35900       305.15100       401.40700     13645.16


--
Type         Ops/sec    Avg. Latency     p50 Latency     p99 Latency   p99.9 Latency       KB/sec
--------------------------------------------------------------------------------------------------
Hsets        4207.68       224.91175       205.82300       548.86300       782.33500     17039.71
Totals       4207.68       224.91175       205.82300       548.86300       782.33500     17039.71
```

## 参考文章

* [Tends 存储版的测试方案](http://tendis.cn/#/Tendisplus/%E6%95%B4%E4%BD%93%E4%BB%8B%E7%BB%8D/%E6%80%A7%E8%83%BD%E6%8A%A5%E5%91%8A)
