#!/usr/bin/env bash

DIR=$(dirname $(realpath "$0"))
cd $DIR
set -ex

docker build -t kvrocks-topling:latest -t kvrocks-topling:$(date "+%Y%m%d") -f topling.dockerfile .
