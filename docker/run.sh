#!/usr/bin/env bash

DIR=$(dirname $(realpath "$0"))
cd $DIR

set -e

CONF=/var/lib/kvrocks/kvrocks.conf

conf() {
  sed -i -e "/^$1 /c$2" $CONF
}

conf dir "dir ${kvrocks_dir-/var/lib/kvrocks}"

[ -n "$kvrocks_port" ] && conf port "port $kvrocks_port"
[ -n "$kvrocks_log_dir" ] && conf "log-dir" "log-dir $kvrocks_log_dir"
[ -n "$kvrocks_timeout" ] && conf timeout "timeout $kvrocks_timeout"
[ -n "$kvrocks_workers" ] && conf workers "workers $kvrocks_workers"
[ -n "$kvrocks_cluster_enabled" ] && conf "cluster-enabled" "cluster-enabled $kvrocks_cluster_enabled"

[ -n "$kvrocks_rocksdb_enable_blob_files" ] && conf "rocksdb.enable_blob_files" "rocksdb.enable_blob_files $kvrocks_rocksdb_enable_blob_files"
[ -n "$kvrocks_rocksdb_compression" ] && conf "rocksdb.compression" "rocksdb.compression $kvrocks_rocksdb_compression"

[ -n "$kvrocks_requirepass" ] && conf "# requirepass" "requirepass $kvrocks_requirepass"
[ -n "$kvrocks_masterauth" ] && conf "# masterauth" "masterauth $kvrocks_masterauth"
[ -n "$kvrocks_slaveof" ] && conf "# slaveof 127.0.0.1" "slaveof $kvrocks_slaveof"
[ -n "$kvrocks_dir" ] && conf "dir " "dir $kvrocks_dir"

conf "rocksdb.read_options.async_io " "rocksdb.read_options.async_io yes"
conf "db-name " "db-name db"

run=$(cat _run.sh)
mv _run.sh run.sh
eval $"$run"
