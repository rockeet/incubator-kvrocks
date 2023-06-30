#!/usr/bin/env bash
set -ex

cd /kvrocks

if [ -n "$1" ]; then
  args=$@
else
  args="-c /var/lib/kvrocks/kvrocks.conf"
fi

exec ./bin/kvrocks $args
