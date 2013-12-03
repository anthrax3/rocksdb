#!/bin/bash

# RocksDB uses block_size 65536, WiredTiger 4096
# RocksDB cache size 10485760, WiredTiger 1GB (RocksDB has other caches)

RESULT_DIR=results
#echo "Load 1B keys sequentially into database....."
#bpl=10485760;overlap=10;mcz=2;del=300000000;levels=6;ctrig=4; delay=8; stop=12; wbn=3; mbc=20; mb=67108864;wbs=134217728; dds=1; sync=0; r=200000000; t=1; vs=800; bs=4096; cs=1000000000; of=500000; si=1000000; ./db_bench_wiredtiger --benchmarks=fillseq --mmap_read=0 --statistics=1 --histogram=1 --num=$r --threads=$t --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=10 --verify_checksum=1 --db=$RESULT_DIR --sync=$sync --disable_wal=1 --compression_type=none --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=0
#echo "Allowing populated database to settle."
#bpl=10485760;overlap=10;mcz=2;del=300000000;levels=6;ctrig=4; delay=8; stop=12; wbn=3; mbc=20; mb=67108864;wbs=134217728; dds=0; sync=0; r=1000000; t=1; vs=800; bs=4096; cs=1000000000; of=500000; si=1000000; ./db_bench_wiredtiger --benchmarks=compact --mmap_read=0 --statistics=1 --histogram=1 --num=$r --threads=$t --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=10 --db=$RESULT_DIR --sync=$sync --disable_wal=1 --compression_type=none --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=1 --wiredtiger_table_config="lsm_bloom_oldest=true"
echo "Reading 1B keys in database in random order...."
bpl=10485760;overlap=10;mcz=2;del=300000000;levels=6;ctrig=4; delay=8; stop=12; wbn=3; mbc=20; mb=67108864;wbs=134217728; dds=0; sync=0; r=200000000; reads=500000; t=32; vs=800; bs=4096; cs=1000000000; of=500000; si=100000; ./db_bench_wiredtiger --benchmarks=readrandom --mmap_read=0 --statistics=1 --histogram=1 --num=$r --reads=$reads --threads=$t --value_size=$vs --block_size=$bs --cache_size=$cs --bloom_bits=10 --db=$RESULT_DIR --sync=$sync --disable_wal=1 --compression_type=none --stats_interval=$si --disable_data_sync=$dds --target_file_size_base=$mb --stats_per_interval=1 --use_existing_db=1

du -s -k $RESULT_DIR
