
#export LD_LIBRARY_PATH=/path/to/lib/dir:$LD_LIBRARY_PATH
rm -rf /nvme-shared/infolog/kvrocks-data/job-*
env TOPLING_CONF=`pwd`/kvtopling.json build/kvrocks -c kvrocks.conf
