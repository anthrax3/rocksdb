#!/bin/bash

# NOTES: RocksDB disables compactions during fillrandom.
# RocksDB uses target_file_size_base 1073741824, WiredTiger 67108864
# RocksDB uses block_size 65536, WiredTiger 4096
# RocksDB cache size 10485760, WiredTiger 1GB (RocksDB has other caches)

RESULT_DIR=results
echo "Bulk load database ...."
bpl=10485760;overlap=10;mcz=2;del=300000000;levels=2;ctrig=10000000; delay=10000000; stop=10000000; wbn=30; mbc=20; mb=67108864;wbs=268435456; dds=1; sync=0; r=200000000; t=1; vs=800; bs=4096; cs=1000000000; of=500000; si=1000000; ./db_bench_wiredtiger --benchmarks=fillrandom --mmap_read=0 --statistics=1 --histogram=1 --num=$r --threads=$t --value_size=$vs --cache_size=$cs --bloom_bits=10 --verify_checksum=1 --db=$RESULT_DIR --sync=$sync --disable_wal=1 --compression_type=snappy --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=0
echo "Running manual compaction...."
bpl=10485760;overlap=10;mcz=2;del=300000000;levels=2;ctrig=10000000; delay=10000000; stop=10000000; wbn=30; mbc=20; mb=67108864;wbs=268435456; dds=1; sync=0; r=200000000; t=1; vs=800; bs=4096; cs=1000000000; of=500000; si=1000000; ./db_bench_wiredtiger --benchmarks=compact --mmap_read=0 --statistics=1 --histogram=1 --num=$r --threads=$t --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=10 --verify_checksum=1 --db=$RESULT_DIR --sync=$sync --disable_wal=1 --compression_type=snappy --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=1
du -s -k $RESULT_DIR
