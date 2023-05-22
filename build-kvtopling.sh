
set -x

if [ -z "$TOPLINGDB_HOME" ]; then
  export TOPLINGDB_HOME=../rocksdb
fi

#export TOPLINGDB_LIBDIR=/node-shared/lib
if [ -z "$TOPLINGDB_LIBDIR" ]; then
  echo env TOPLINGDB_LIBDIR must be defined 1>&2
  exit 1
fi
env ./x.py build -j`nproc` -DCMAKE_BUILD_TYPE=Release
cp -a build/_deps/libevent-build/lib/* $TOPLINGDB_LIBDIR
