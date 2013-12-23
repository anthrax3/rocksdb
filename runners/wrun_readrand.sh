#!/bin/bash

# RocksDB uses block_size 65536, WiredTiger 4096
# RocksDB cache size 10485760, WiredTiger 1GB (RocksDB has other caches)

mb=67108864	# chunk_size
dds=0		# Disable data sync
sync=0		# Sync writes
opcount=50000000
readcount=500000
threadcount=20
vs=800		# Value size
bloom_bits=10   # Bloom bit count
bs=4096		# Block allocation size
cs=3000000000	# Cache size
si=1000000	# Stats interval
disable_wal=1	# Write ahead logging
data_dir=results
max_compact_wait=6000
extra_opts=--wiredtiger_table_config=lsm=(bloom_oldest=true,merge_threads=2),leaf_page_max=16k,leaf_item_max=2k
extra_opts2=--wiredtiger_open_config=verbose=[lsm]
add_overwrite=0
histogram=1
statistics=1

benchmarks=readrandom
# Not currently used by WiredTiger, but possibly interesting
#del=300000000
#delay=8

# Parse command line options.
while getopts "h?b:d:i:mo:r:s:" opt; do
    case "$opt" in
    h|\?)
        echo "Usage: $0 -i opcount -d <data dir> -r <readcount>"
        exit 0
        ;;
    b)  bloom_bits=$OPTARG
        ;;
    d)  data_dir=$OPTARG
        ;;
    i)  opcount=$OPTARG
        ;;
    m)  benchmarks=overwrite,readrandom
        ;;
    o)  overwrite_count=$OPTARG
        add_overwrite=1
        ;;
    r)  readcount=$OPTARG
        ;;
    s)  vs=$OPTARG
        ;;
    esac
done

echo "Load keys sequentially single threaded"
( set -x ; ./db_bench_wiredtiger --benchmarks=fillseq --mmap_read=1 --statistics=$statistics --histogram=$histogram --num=$opcount --threads=1 --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=$bloom_bits --verify_checksum=1 --db=$data_dir --sync=$sync --disable_wal=$disable_wal --compression_type=none --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=0 $extra_opts )

CHUNK_COUNT=`ls -l $data_dir/*lsm | wc -l`
DATA_SZ=`du -sk $data_dir`
echo "After sequential load. LSM chunks: $CHUNK_COUNT data size: $DATA_SZ"

echo "Allowing populated database to settle."
( set -x ; ./db_bench_wiredtiger --benchmarks=compact --mmap_read=1 --statistics=$statistics --histogram=$histogram --num=$opcount --threads=1 --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=$bloom_bits --db=$data_dir --sync=$sync --disable_wal=$disable_wal --compression_type=none --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=1 --max_compact_wait=$max_compact_wait $extra_opts $extra_opts2 )

CHUNK_COUNT=`ls -l $data_dir/*lsm | wc -l`
DATA_SZ=`du -sk $data_dir`
echo "After compact. LSM chunks: $CHUNK_COUNT data size: $DATA_SZ"

if [ $add_overwrite = 1 ]; then
    echo "Overwrite some items"
    ( set -x ; ./db_bench_wiredtiger --benchmarks=overwrite --mmap_read=1 --statistics=$statistics --histogram=$histogram --num=$overwrite_count --threads=1 --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=$bloom_bits --db=$data_dir --sync=$sync --disable_wal=$disable_wal --compression_type=none --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=1 $extra_opts )

	CHUNK_COUNT=`ls -l $data_dir/*lsm | wc -l`
	DATA_SZ=`du -sk $data_dir`
	echo "After overwrite LSM chunks: $CHUNK_COUNT data size: $DATA_SZ"
fi

NW=$(( 8192 / $threadcount )) # Number of writes to populate memtable
echo "Reading keys in database in random order."
( set -x ; ./db_bench_wiredtiger --benchmarks=$benchmarks --mmap_read=1 --statistics=$statistics --histogram=$histogram --num=$opcount --reads=$readcount --writes=$NW --threads=$threadcount --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=$bloom_bits --db=$data_dir --sync=$sync --disable_wal=$disable_wal --compression_type=none --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=1 $extra_opts )

CHUNK_COUNT=`ls -l $data_dir/*lsm | wc -l`
DATA_SZ=`du -sk $data_dir`
echo "After read. LSM chunks: $CHUNK_COUNT data size: $DATA_SZ"
du -s -k $data_dir
