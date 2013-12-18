#!/bin/bash

# NOTES: RocksDB disables compactions during fillrandom.
# RocksDB uses target_file_size_base 1073741824, WiredTiger 67108864
# RocksDB uses block_size 65536, WiredTiger 4096
# RocksDB cache size 10485760, WiredTiger 1GB (RocksDB has other caches)

mb=67108864	# chunk_size
dds=1		# Disable data sync
sync=0		# Sync writes
opcount=50000000
readcount=1000000
threadcount=1
vs=800		# Value size
bloom_bits=10   # Bloom bit count
bs=4096		# Block allocation size
cs=2000000000	# Cache size
si=1000000	# Stats interval
disable_wal=1	# Write ahead logging
extra_opts=--wiredtiger_table_config=lsm=(bloom_oldest=true,merge_threads=2)
data_dir=results

# Not currently used by WiredTiger, but possibly interesting
#del=300000000
#delay=10000000;

# Parse command line options.
while getopts "h?b:d:i:r:s:" opt; do
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
    r)  readcount=$OPTARG
        ;;
    s)  vs=$OPTARG
        ;;
    esac
done

echo "Bulk load database ...."
( set -x ; ./db_bench_wiredtiger --benchmarks=fillrandom --mmap_read=0 --statistics=1 --histogram=1 --num=$opcount --threads=$threadcount --value_size=$vs --cache_size=$cs --bloom_bits=$bloom_bits --verify_checksum=1 --db=$data_dir --sync=$sync --disable_wal=$disable_wal --compression_type=snappy --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=0 $extra_opts )

echo "Running manual compaction...."
( set -x ; ./db_bench_wiredtiger --benchmarks=compact --mmap_read=0 --statistics=1 --histogram=1 --num=$opcount --threads=$threadcount --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=$bloom_bits --verify_checksum=1 --db=$data_dir --sync=$sync --disable_wal=$disable_wal --compression_type=snappy --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=1 $extra_opts )
du -s -k $data_dir
