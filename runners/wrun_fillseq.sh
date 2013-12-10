#!/bin/bash

# RocksDB uses block_size 65536, WiredTiger 4096
# RocksDB cache size 10485760, WiredTiger 1GB (RocksDB has other caches)

mb=67108864	# chunk_size
dds=1		# Disable data sync
sync=0		# Sync writes
opcount=50000000
readcount=1000000
threadcount=1
vs=800		# Value size
bs=4096		# Block allocation size
cs=1000000000	# Cache size
si=1000000	# Stats interval
disable_wal=1	# Write ahead logging
data_dir=results

# Not currently used by WiredTiger, but possibly interesting
#del=300000000
#delay=8

# Parse command line options.
while getopts "h?d:i:r:s:" opt; do
    case "$opt" in
    h|\?)
        echo "Usage: $0 -i opcount -d <data dir> -r <readcount>"
        exit 0
        ;;
    d)  data_dir=$OPTARG
        ;;
    i)  opcount=$OPTARG
        ;;
    r)  readcount=$OPTARG
        ;;
    s)  vs=$OPTARG
        ;;
    esac
done

echo "Load 1B keys sequentially into database....."
( set -x ; ./db_bench_wiredtiger --benchmarks=fillseq --mmap_read=0 --statistics=1 --histogram=1 --num=$opcount --threads=$threadcount --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=10 --verify_checksum=1 --db=$data_dir --sync=$sync --disable_wal=$disable_wal --compression_type=snappy --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=0 )

du -s -k $data_dir
