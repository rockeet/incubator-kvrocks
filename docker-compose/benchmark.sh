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
