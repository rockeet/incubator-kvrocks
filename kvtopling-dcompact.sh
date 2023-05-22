#!/bin/bash

if [ -z "$type" ]; then
  type=rls
fi
libdirs=(
  /node-shared/lib
)
libdirs=("${libdirs[@]}") # join array to one string
export LD_LIBRARY_PATH=${libdirs// /:}:${LD_LIBRARY_PATH}

export ROCKSDB_KICK_OUT_OPTIONS_FILE=1
export MULTI_PROCESS=0

#export LOG_LEVEL=3

# MyTopling 企业版(包含 topling-rocks 模块) 必须配置该变量
export ZIP_SERVER_OPTIONS="listening_ports=8090:num_threads=32"
export ZipServer_nltBuildThreads=11
rm -f /tmp/Topling-* # 清理上次运行结束时的遗留垃圾文件

#export ToplingZipTable_debugLevel=2

#export LOG_LEVEL=3
#export SidePluginRepo_DebugLevel=3
export CPU_CORE_COUNT=`nproc`
export MAX_PARALLEL_COMPACTIONS=$[$CPU_CORE_COUNT * 1]
export MAX_WAITING_COMPACTIONS=$[$CPU_CORE_COUNT * 2]
export DEL_WORKER_TEMP_DB=0
export WORKER_DB_ROOT=/nvme-shared/infolog/kvrocks-data
export NFS_MOUNT_ROOT=/nvme-shared/kvrocks
export DictZipBlobStore_zipThreads=32
export ToplingZipTable_localTempDir=/tmp

mkdir -p $WORKER_DB_ROOT

ulimit -n 100000
#sudo sysctl -w vm.max_map_count=1048576

if [ $type = dbg ]; then
  dbg="gdb --args"
fi
#dbg="ldd"
#dbg="gdb --args"
env LD_PRELOAD=libkvrocks_dc.so $dbg /node-shared/bin/dcompact_worker.exe \
    -D listening_ports=8080 -D num_threads=50 \
    -D document_root=$WORKER_DB_ROOT #>> $WORKER_DB_ROOT/stdout 2>> $WORKER_DB_ROOT/stderr
