//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <cstddef>
#include <sys/types.h>
#include <stdio.h>
#include <memory>
#include <sstream>
#include <stdlib.h>
#include <unistd.h> /* For sleep */
#include <gflags/gflags.h>
#include "db/version_set.h"
#include "wiredtiger.h"
#include "port/port.h"
#include "util/bit_set.h"
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"

#define MAX(a, b) ((a) < (b) ? (b) : (a))
// Nicked from RocksDB util_posix.cc.
static std::string TimeToString(uint64_t secondsSince1970) {
  const time_t seconds = (time_t)secondsSince1970;
  struct tm t;
  int maxsize = 64;
  std::string dummy;
  dummy.reserve(maxsize);
  dummy.resize(maxsize);
  char* p = &dummy[0];
  localtime_r(&seconds, &t);
  snprintf(p, maxsize,
           "%04d/%02d/%02d-%02d:%02d:%02d ",
           t.tm_year + 1900,
           t.tm_mon + 1,
           t.tm_mday,
           t.tm_hour,
           t.tm_min,
           t.tm_sec);
  return dummy;
}

DEFINE_string(benchmarks,

              "fillseq,"
              "fillsync,"
              "fillrandom,"
              "overwrite,"
              "readrandom,"
              "readrandom,"
              "readseq,"
              "readreverse,"
              "compact,"
              "readrandom,"
              "readseq,"
              "readreverse,"
              "readwhilewriting,"
              "readrandomwriterandom,"
              "updaterandom,"
              "randomwithverify,"
              "fill100K,"
              "crc32c,"
              "snappycomp,"
              "snappyuncomp,"
              "acquireload,",

              "Comma-separated list of operations to run in the specified order"
              "Actual benchmarks:\n"
              "\tfillseq       -- write N values in sequential key"
              " order in async mode\n"
              "\tfillrandom    -- write N values in random key order in async"
              " mode\n"
              "\toverwrite     -- overwrite N values in random key order in"
              " async mode\n"
              "\tfillsync      -- write N/100 values in random key order in "
              "sync mode\n"
              "\tfill100K      -- write N/1000 100K values in random order in"
              " async mode\n"
              "\tdeleteseq     -- delete N keys in sequential order\n"
              "\tdeleterandom  -- delete N keys in random order\n"
              "\treadseq       -- read N times sequentially\n"
              "\treadreverse   -- read N times in reverse order\n"
              "\treadrandom    -- read N times in random order\n"
              "\treadmissing   -- read N missing keys in random order\n"
              "\treadhot       -- read N times in random order from 1% section "
              "of DB\n"
              "\treadwhilewriting      -- 1 writer, N threads doing random "
              "reads\n"
              "\treadrandomwriterandom -- N threads doing random-read, "
              "random-write\n"
              "\tprefixscanrandom      -- prefix scan N times in random order\n"
              "\tupdaterandom  -- N threads doing read-modify-write for random "
              "keys\n"
              "\tappendrandom  -- N threads doing read-modify-write with "
              "growing values\n"
              "\tmergerandom   -- same as updaterandom/appendrandom using merge"
              " operator. "
              "Must be used with merge_operator\n"
              "\tseekrandom    -- N random seeks\n"
              "\tcrc32c        -- repeated crc32c of 4K of data\n"
              "\tacquireload   -- load N*1000 times\n"
              "Meta operations:\n"
              "\tcompact     -- Compact the entire DB\n"
              "\tstats       -- Print DB stats\n"
              "\tlevelstats  -- Print the number of files and bytes per level\n"
              "\tsstables    -- Print sstable info\n"
              "\theapprofile -- Dump a heap profile (if supported by this"
              " port)\n");

DEFINE_int64(num, 1000000, "Number of key/values to place in database");

DEFINE_int64(numdistinct, 1000,
             "Number of distinct keys to use. Used in RandomWithVerify to "
             "read/write on fewer keys so that gets are more likely to find the"
             " key and puts are more likely to update the same key");

DEFINE_int64(reads, -1, "Number of read operations to do.  "
             "If negative, do FLAGS_num reads.");

DEFINE_int64(read_range, 1, "When ==1 reads use ::Get, when >1 reads use"
             " an iterator");

DEFINE_bool(use_prefix_blooms, false, "Whether to place prefixes in blooms");

DEFINE_bool(use_prefix_api, false, "Whether to set ReadOptions.prefix for"
            " prefixscanrandom. If true, use_prefix_blooms must also be true.");

DEFINE_int64(seed, 0, "Seed base for random number generators. "
             "When 0 it is deterministic.");

DEFINE_int32(threads, 1, "Number of concurrent threads to run.");

DEFINE_int32(duration, 0, "Time in seconds for the random-ops tests to run."
             " When 0 then num & reads determine the test duration");

DEFINE_int32(value_size, 100, "Size of each value");


// the maximum size of key in bytes
static const long long kMaxKeySize = 128;
static bool ValidateKeySize(const char* flagname, int32_t value) {
  if (value > kMaxKeySize) {
    fprintf(stderr, "Invalid value for --%s: %d, must be < %d\n",
            flagname, value, (int)kMaxKeySize);
    return false;
  }
  return true;
}
DEFINE_int32(key_size, 16, "size of each key");

DEFINE_double(compression_ratio, 0.5, "Arrange to generate values that shrink"
              " to this fraction of their original size after compression");

DEFINE_bool(histogram, false, "Print histogram of operation timings");

DEFINE_int32(write_buffer_size, 0,
             "Number of bytes to buffer in memtable before compacting");

DEFINE_int32(max_write_buffer_number, 0,
             "The number of in-memory memtables. Each memtable is of size"
             "write_buffer_size.");

DEFINE_int32(min_write_buffer_number_to_merge, 0,
             "The minimum number of write buffers that will be merged together"
             "before writing to storage. This is cheap because it is an"
             "in-memory merge. If this feature is not enabled, then all these"
             "write buffers are flushed to L0 as separate files and this "
             "increases read amplification because a get request has to check"
             " in all of these files. Also, an in-memory merge may result in"
             " writing less data to storage if there are duplicate records "
             " in each of these individual write buffers.");

DEFINE_int32(max_background_compactions, 0,
             "The maximum number of concurrent background compactions"
             " that can occur in parallel.");

DEFINE_int32(compaction_style, 0,
             "style of compaction: level-based vs universal");

DEFINE_int32(universal_size_ratio, 0,
             "Percentage flexibility while comparing file size"
             " (for universal compaction only).");

DEFINE_int32(universal_min_merge_width, 0, "The minimum number of files in a"
             " single compaction run (for universal compaction only).");

DEFINE_int32(universal_max_merge_width, 0, "The max number of files to compact"
             " in universal style compaction");

DEFINE_int32(universal_max_size_amplification_percent, 0,
             "The max size amplification for universal style compaction");

DEFINE_int64(cache_size, -1, "Number of bytes to use as a cache of uncompressed"
             "data. Negative means use default settings.");

DEFINE_int32(block_size, 0,
             "Number of bytes in a block.");

DEFINE_int64(compressed_cache_size, -1,
             "Number of bytes to use as a cache of compressed data.");

DEFINE_int32(open_files, 0,
             "Maximum number of files to keep open at the same time"
             " (use default if == 0)");

DEFINE_int32(bloom_bits, -1, "Bloom filter bits per key. Negative means"
             " use default settings.");

DEFINE_bool(use_existing_db, false, "If true, do not destroy the existing"
            " database.  If you set this flag and also specify a benchmark that"
            " wants a fresh database, that benchmark will fail.");

DEFINE_string(db, "", "Use the db with the following name.");

static bool ValidateCacheNumshardbits(const char* flagname, int32_t value) {
  if (value >= 20) {
    fprintf(stderr, "Invalid value for --%s: %d, must be < 20\n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_int32(cache_numshardbits, -1, "Number of shards for the block cache"
             " is 2 ** cache_numshardbits. Negative means use default settings."
             " This is applied only if FLAGS_cache_size is non-negative.");

DEFINE_int32(cache_remove_scan_count_limit, 32, "");

DEFINE_bool(verify_checksum, false, "Verify checksum for every block read"
            " from storage");

DEFINE_bool(statistics, false, "Database statistics");

DEFINE_int64(writes, -1, "Number of write operations to do. If negative, do"
             " --num reads.");

DEFINE_int32(writes_per_second, 0, "Per-thread rate limit on writes per second."
             " No limit when <= 0. Only for the readwhilewriting test.");

DEFINE_bool(sync, false, "Sync all writes to disk");

DEFINE_bool(disable_data_sync, false, "If true, do not wait until data is"
            " synced to disk.");

DEFINE_bool(use_fsync, false, "If true, issue fsync instead of fdatasync");

DEFINE_bool(disable_wal, false, "If true, do not write WAL for write.");

DEFINE_bool(use_snapshot, false, "If true, create a snapshot per query when"
            " randomread benchmark is used");

DEFINE_bool(get_approx, false, "If true, call GetApproximateSizes per query"
            " when read_range is > 1 and randomread benchmark is used");

DEFINE_int32(num_levels, 7, "The total number of levels");

DEFINE_int32(target_file_size_base, 2 * 1048576, "Target file size at level-1");

DEFINE_int32(target_file_size_multiplier, 1,
             "A multiplier to compute target level-N file size (N >= 2)");

DEFINE_uint64(max_bytes_for_level_base,  10 * 1048576, "Max bytes for level-1");

DEFINE_int32(max_bytes_for_level_multiplier, 10,
             "A multiplier to compute max bytes for level-N (N >= 2)");

DEFINE_string(max_bytes_for_level_multiplier_additional, "",
              "A vector that specifies additional fanout per level");

DEFINE_int32(level0_stop_writes_trigger, 12, "Number of files in level-0"
             " that will trigger put stop.");

DEFINE_int32(level0_slowdown_writes_trigger, 8, "Number of files in level-0"
             " that will slow down writes.");

DEFINE_int32(level0_file_num_compaction_trigger, 4, "Number of files in level-0"
             " when compactions start");

static bool ValidateInt32Percent(const char* flagname, int32_t value) {
  if (value <= 0 || value>=100) {
    fprintf(stderr, "Invalid value for --%s: %d, 0< pct <100 \n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_int32(readwritepercent, 90, "Ratio of reads to reads/writes (expressed"
             " as percentage) for the ReadRandomWriteRandom workload. The "
             "default value 90 means 90% operations out of all reads and writes"
             " operations are reads. In other words, 9 gets for every 1 put.");

DEFINE_int32(deletepercent, 2, "Percentage of deletes out of reads/writes/"
             "deletes (used in RandomWithVerify only). RandomWithVerify "
             "calculates writepercent as (100 - FLAGS_readwritepercent - "
             "deletepercent), so deletepercent must be smaller than (100 - "
             "FLAGS_readwritepercent)");

DEFINE_int32(disable_seek_compaction, false, "Option to disable compaction"
             " triggered by read.");

DEFINE_uint64(delete_obsolete_files_period_micros, 0, "Option to delete "
              "obsolete files periodically. 0 means that obsolete files are"
              " deleted after every compaction run.");

DEFINE_string(compression_type, "snappy",
              "Algorithm to use to compress the database");

DEFINE_int32(min_level_to_compress, -1, "If non-negative, compression starts"
             " from this level. Levels with number < min_level_to_compress are"
             " not compressed. Otherwise, apply compression_type to "
             "all levels.");

static bool ValidateTableCacheNumshardbits(const char* flagname,
                                           int32_t value) {
  if (0 >= value || value > 20) {
    fprintf(stderr, "Invalid value for --%s: %d, must be  0 < val <= 20\n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_int32(table_cache_numshardbits, 4, "");

DEFINE_int64(stats_interval, 0, "Stats are reported every N operations when "
             "this is greater than zero. When 0 the interval grows over time.");

DEFINE_int32(stats_per_interval, 0, "Reports additional stats per interval when"
             " this is greater than 0.");

static bool ValidateRateLimit(const char* flagname, double value) {
  static constexpr double EPSILON = 1e-10;
  if ( value < -EPSILON ) {
    fprintf(stderr, "Invalid value for --%s: %12.6f, must be >= 0.0\n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_double(soft_rate_limit, 0.0, "");

DEFINE_double(hard_rate_limit, 0.0, "When not equal to 0 this make threads "
              "sleep at each stats reporting interval until the compaction"
              " score for all levels is less than or equal to this value.");

DEFINE_int32(rate_limit_delay_max_milliseconds, 1000,
             "When hard_rate_limit is set then this is the max time a put will"
             " be stalled.");

DEFINE_int32(max_grandparent_overlap_factor, 10, "Control maximum bytes of "
             "overlaps in grandparent (i.e., level+2) before we stop building a"
             " single file in a level->level+1 compaction.");

DEFINE_bool(readonly, false, "Run read only benchmarks.");

DEFINE_bool(disable_auto_compactions, false, "Do not auto trigger compactions");

DEFINE_int32(source_compaction_factor, 1, "Cap the size of data in level-K for"
             " a compaction run that compacts Level-K with Level-(K+1) (for"
             " K >= 1)");

DEFINE_uint64(wal_ttl_seconds, 0, "Set the TTL for the WAL Files in seconds.");
DEFINE_uint64(wal_size_limit_MB, 0, "Set the size limit for the WAL Files"
              " in MB.");

DEFINE_bool(bufferedio, true, "Allow buffered io using OS buffers");

DEFINE_bool(mmap_read, true, "Allow reads to occur via mmap-ing files");

DEFINE_bool(mmap_write, true, "Allow writes to occur via mmap-ing files");

DEFINE_bool(advise_random_on_open, false,
            "Advise random access on table file open");

DEFINE_string(compaction_fadvice, "NORMAL",
              "Access pattern advice when a file is compacted");

DEFINE_bool(use_multiget, false,
            "Use multiget to access a series of keys instead of get");

DEFINE_int64(keys_per_multiget, 90, "If use_multiget is true, determines number"
             " of keys to group per call Arbitrary default is good because it"
             " agrees with readwritepercent");

// TODO: Apply this flag to generic Get calls too. Currently only with Multiget
DEFINE_bool(warn_missing_keys, true, "Print a message to user when a key is"
            " missing in a Get/MultiGet call");

DEFINE_bool(use_adaptive_mutex, false, "Use adaptive mutex");

DEFINE_uint64(bytes_per_sync, 0, 
              "Allows OS to incrementally sync files to disk while they are"
              " being written, in the background. Issue one request for every"
              " bytes_per_sync written. 0 turns it off.");
DEFINE_bool(filter_deletes, false, " On true, deletes use bloom-filter and drop"
            " the delete if key not present");

// WiredTiger specific configuration options.
DEFINE_bool(use_lsm, true, "Create LSM trees, not btrees");

DEFINE_int32(max_compact_wait, 1200,
             "Maximum amount of time to allow LSM trees to settle.");

DEFINE_string(wiredtiger_open_config, "",
              "Additional configuration options for WiredTiger open.");

DEFINE_string(wiredtiger_table_config, "",
              "Additional configuration options for WiredTiger table create.");

static bool ValidatePrefixSize(const char* flagname, int32_t value) {
  if (value < 0 || value>=2000000000) {
    fprintf(stderr, "Invalid value for --%s: %d. 0<= PrefixSize <=2000000000\n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_int32(prefix_size, 0, "Control the prefix size for PrefixHashRep");

enum RepFactory {
  kSkipList,
  kPrefixHash,
  kUnsorted,
  kVectorRep
};
enum RepFactory StringToRepFactory(const char* ctype) {
  assert(ctype);

  if (!strcasecmp(ctype, "skip_list"))
    return kSkipList;
  else if (!strcasecmp(ctype, "prefix_hash"))
    return kPrefixHash;
  else if (!strcasecmp(ctype, "unsorted"))
    return kUnsorted;
  else if (!strcasecmp(ctype, "vector"))
    return kVectorRep;

  fprintf(stdout, "Cannot parse memreptable %s\n", ctype);
  return kSkipList;
}
static enum RepFactory FLAGS_rep_factory;
DEFINE_string(memtablerep, "skip_list", "");

DEFINE_string(merge_operator, "", "The merge operator to use with the database."
              "If a new merge operator is specified, be sure to use fresh"
              " database The possible merge operators are defined in"
              " utilities/merge_operators.h");

static const bool FLAGS_soft_rate_limit_dummy __attribute__((unused)) =
  google::RegisterFlagValidator(&FLAGS_soft_rate_limit,
                                &ValidateRateLimit);

static const bool FLAGS_hard_rate_limit_dummy __attribute__((unused)) =
  google::RegisterFlagValidator(&FLAGS_hard_rate_limit, &ValidateRateLimit);

static const bool FLAGS_prefix_size_dummy __attribute__((unused)) =
  google::RegisterFlagValidator(&FLAGS_prefix_size, &ValidatePrefixSize);

static const bool FLAGS_key_size_dummy __attribute__((unused)) =
  google::RegisterFlagValidator(&FLAGS_key_size, &ValidateKeySize);

static const bool FLAGS_cache_numshardbits_dummy __attribute__((unused)) =
  google::RegisterFlagValidator(&FLAGS_cache_numshardbits,
                                &ValidateCacheNumshardbits);

static const bool FLAGS_readwritepercent_dummy __attribute__((unused)) =
  google::RegisterFlagValidator(&FLAGS_readwritepercent,
                                &ValidateInt32Percent);

static const bool FLAGS_deletepercent_dummy __attribute__((unused)) =
  google::RegisterFlagValidator(&FLAGS_deletepercent,
                                &ValidateInt32Percent);
static const bool
  FLAGS_table_cache_numshardbits_dummy __attribute__((unused)) =
  google::RegisterFlagValidator(&FLAGS_table_cache_numshardbits,
                                &ValidateTableCacheNumshardbits);

namespace rocksdb {

using namespace std;

// Helper for quickly generating random data.
class RandomGenerator {
 private:
  std::string data_;
  unsigned int pos_;

 public:
  RandomGenerator() {
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < (unsigned)std::max(1048576, FLAGS_value_size)) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(unsigned int len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

static void AppendWithSpace(std::string* str, Slice msg) {
  if (msg.empty()) return;
  if (!str->empty()) {
    str->push_back(' ');
  }
  str->append(msg.data(), msg.size());
}

class Stats {
 private:
  int id_;
  double start_;
  double finish_;
  double seconds_;
  long long done_;
  long long last_report_done_;
  long long next_report_;
  int64_t bytes_;
  double last_op_finish_;
  double last_report_finish_;
  HistogramImpl hist_;
  std::string message_;
  bool exclude_from_merge_;

 public:
  Stats() { Start(-1); }

  void Start(int id) {
    id_ = id;
    next_report_ = FLAGS_stats_interval ? FLAGS_stats_interval : 100;
    last_op_finish_ = start_;
    hist_.Clear();
    done_ = 0;
    last_report_done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = Env::Default()->NowMicros();
    finish_ = start_;
    last_report_finish_ = start_;
    message_.clear();
    // When set, stats from this thread won't be merged with others.
    exclude_from_merge_ = false;
  }

  void Merge(const Stats& other) {
    if (other.exclude_from_merge_)
      return;

    hist_.Merge(other.hist_);
    done_ += other.done_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;

    // Just keep the messages from one thread
    if (message_.empty()) message_ = other.message_;
  }

  void Stop() {
    finish_ = Env::Default()->NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void AddMessage(Slice msg) {
    AppendWithSpace(&message_, msg);
  }

  void SetId(int id) { id_ = id; }
  void SetExcludeFromMerge() { exclude_from_merge_ = true; }

  void FinishedSingleOp(DB* db) {
    if (FLAGS_histogram) {
      double now = Env::Default()->NowMicros();
      double micros = now - last_op_finish_;
      hist_.Add(micros);
      if (micros > 20000 && !FLAGS_stats_interval) {
        fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
        fflush(stderr);
      }
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
      if (!FLAGS_stats_interval) {
        if      (next_report_ < 1000)   next_report_ += 100;
        else if (next_report_ < 5000)   next_report_ += 500;
        else if (next_report_ < 10000)  next_report_ += 1000;
        else if (next_report_ < 50000)  next_report_ += 5000;
        else if (next_report_ < 100000) next_report_ += 10000;
        else if (next_report_ < 500000) next_report_ += 50000;
        else                            next_report_ += 100000;
        fprintf(stderr, "... finished %lld ops%30s\r", done_, "");
        fflush(stderr);
      } else {
        double now = Env::Default()->NowMicros();
        fprintf(stderr,
                "%s ... thread %d: (%lld,%lld) ops and "
                "(%.1f,%.1f) ops/second in (%.6f,%.6f) seconds\n",
                TimeToString((uint64_t) now/1000000).c_str(),
                id_,
                done_ - last_report_done_, done_,
                (done_ - last_report_done_) /
                ((now - last_report_finish_) / 1000000.0),
                done_ / ((now - start_) / 1000000.0),
                (now - last_report_finish_) / 1000000.0,
                (now - start_) / 1000000.0);

        if (FLAGS_stats_per_interval) {
          std::string stats;
          if (db && db->GetProperty("rocksdb.stats", &stats))
            fprintf(stderr, "%s\n", stats.c_str());
        }

        fflush(stderr);
        next_report_ += FLAGS_stats_interval;
        last_report_finish_ = now;
        last_report_done_ = done_;
      }
    }
  }

  void AddBytes(int64_t n) {
    bytes_ += n;
  }

  void Report(const Slice& name) {
    // Pretend at least one op was done in case we are running a benchmark
    // that does not call FinishedSingleOp(nullptr).
    if (done_ < 1) done_ = 1;

    std::string extra;
    if (bytes_ > 0) {
      // Rate is computed on actual elapsed time, not the sum of per-thread
      // elapsed times.
      double elapsed = (finish_ - start_) * 1e-6;
      char rate[100];
      snprintf(rate, sizeof(rate), "%6.1f MB/s",
               (bytes_ / 1048576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);
    double elapsed = (finish_ - start_) * 1e-6;
    double throughput = (double)done_/elapsed;

    fprintf(stdout, "%-12s : %11.3f micros/op %ld ops/sec;%s%s\n",
            name.ToString().c_str(),
            elapsed * 1e6 / done_,
            (long)throughput,
            (extra.empty() ? "" : " "),
            extra.c_str());
    if (FLAGS_histogram) {
      fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
    }
    fflush(stdout);
  }
};

// State shared by all concurrent executions of the same benchmark.
struct SharedState {
  port::Mutex mu;
  port::CondVar cv;
  int total;

  // Each thread goes through the following states:
  //    (1) initializing
  //    (2) waiting for others to be initialized
  //    (3) running
  //    (4) done

  long num_initialized;
  long num_done;
  bool start;

  SharedState() : cv(&mu) { }
};

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState {
  int tid;             // 0..n-1 when running in n threads
  Random64 rand;         // Has different seeds for different threads
  Stats stats;
  SharedState* shared;
  WT_SESSION *session;

  /* implicit */ ThreadState(int index, WT_CONNECTION *conn)
      : tid(index),
        rand((FLAGS_seed ? FLAGS_seed : 1000) + index) {
    conn->open_session(conn, NULL, NULL, &session);
    assert(session != NULL);
  }
  ~ThreadState() {
    session->close(session, NULL);
  }
};

class Duration {
 public:
  Duration(int max_seconds, long long max_ops) {
    max_seconds_ = max_seconds;
    max_ops_= max_ops;
    ops_ = 0;
    start_at_ = Env::Default()->NowMicros();
  }

  bool Done(int increment) {
    if (increment <= 0) increment = 1;    // avoid Done(0) and infinite loops
    ops_ += increment;

    if (max_seconds_) {
      // Recheck every appx 1000 ops (exact iff increment is factor of 1000)
      if ((ops_/1000) != ((ops_-increment)/1000)) {
        double now = Env::Default()->NowMicros();
        return ((now - start_at_) / 1000000.0) >= max_seconds_;
      } else {
        return false;
      }
    } else {
      return ops_ > max_ops_;
    }
  }

 private:
  int max_seconds_;
  long long max_ops_;
  long long ops_;
  double start_at_;
};

class Benchmark {
 private:
  WT_CONNECTION *conn_;
  std::string uri_;
  int db_num_;

  bool sync_;
  long long num_;
  int value_size_;
  int key_size_;
  int entries_per_batch_;
  long long reads_;
  long long writes_;
  long long readwrites_;
  int heap_counter_;
  char keyFormat_[100]; // will contain the format of key. e.g "%016lld"
  void PrintHeader() {
    PrintEnvironment();
    fprintf(stdout, "Keys:       %d bytes each\n", FLAGS_key_size);
    fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
            FLAGS_value_size,
            static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    fprintf(stdout, "Entries:    %lld\n", num_);
    fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
            ((static_cast<int64_t>(FLAGS_key_size + FLAGS_value_size) * num_)
             / 1048576.0));
    fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
            (((FLAGS_key_size + FLAGS_value_size * FLAGS_compression_ratio)
              * num_)
             / 1048576.0));
    fprintf(stdout, "Write rate limit: %d\n", FLAGS_writes_per_second);
    fprintf(stdout, "Compression: %s\n", FLAGS_compression_type.c_str());

    PrintWarnings();
    fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    fprintf(stdout,
            "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n"
            );
#endif
#ifndef NDEBUG
    fprintf(stdout,
            "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
  }

// Current the following isn't equivalent to OS_LINUX.
#if defined(__linux)
  static Slice TrimSpace(Slice s) {
    unsigned int start = 0;
    while (start < s.size() && isspace(s[start])) {
      start++;
    }
    unsigned int limit = s.size();
    while (limit > start && isspace(s[limit-1])) {
      limit--;
    }
    return Slice(s.data() + start, limit - start);
  }
#endif

  void PrintEnvironment() {
    fprintf(stderr, "LevelDB:    version %d.%d\n",
            kMajorVersion, kMinorVersion);

#if defined(__linux)
    time_t now = time(nullptr);
    fprintf(stderr, "Date:       %s", ctime(&now));  // ctime() adds newline

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != nullptr) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != nullptr) {
        const char* sep = strchr(line, ':');
        if (sep == nullptr) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      fclose(cpuinfo);
      fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#endif
  }

 public:
  Benchmark()
   :db_num_(0),
    num_(FLAGS_num),
    value_size_(FLAGS_value_size),
    key_size_(FLAGS_key_size),
    entries_per_batch_(1),
    reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
    writes_(FLAGS_writes < 0 ? FLAGS_num : FLAGS_writes),
    readwrites_((FLAGS_writes < 0  && FLAGS_reads < 0)? FLAGS_num :
                ((FLAGS_writes > FLAGS_reads) ? FLAGS_writes : FLAGS_reads)
               ),
    heap_counter_(0) {
    std::vector<std::string> files;
    Env::Default()->GetChildren(FLAGS_db, &files);
    for (unsigned int i = 0; i < files.size(); i++) {
      if (Slice(files[i]).starts_with("heap-")) {
        Env::Default()->DeleteFile(FLAGS_db + "/" + files[i]);
      }
    }
    if (!FLAGS_use_existing_db) {
      DestroyDB(FLAGS_db, Options());
    }
  }

  ~Benchmark() {
  }

  //this function will construct string format for key. e.g "%016lld"
  void ConstructStrFormatForKey(char* str, int keySize) {
    str[0] = '%';
    str[1] = '0';
    sprintf(str+2, "%dlld%s", keySize, "%s");
  }

  unique_ptr<char []> GenerateKeyFromInt(long long v, const char* suffix = "") {
    unique_ptr<char []> keyInStr(new char[kMaxKeySize]);
    snprintf(keyInStr.get(), kMaxKeySize, keyFormat_, v, suffix);
    return keyInStr;
  }

  void Run() {
    PrintHeader();
    Open();
    const char* benchmarks = FLAGS_benchmarks.c_str();
    while (benchmarks != nullptr) {
      const char* sep = strchr(benchmarks, ',');
      Slice name;
      if (sep == nullptr) {
        name = benchmarks;
        benchmarks = nullptr;
      } else {
        name = Slice(benchmarks, sep - benchmarks);
        benchmarks = sep + 1;
      }

      // Sanitize parameters
      num_ = FLAGS_num;
      reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
      writes_ = (FLAGS_writes < 0 ? FLAGS_num : FLAGS_writes);
      value_size_ = FLAGS_value_size;
      key_size_ = FLAGS_key_size;
      ConstructStrFormatForKey(keyFormat_, key_size_);
      entries_per_batch_ = 1;
      if (FLAGS_sync)
        sync_ = true;
      else
        sync_ = false;

      void (Benchmark::*method)(ThreadState*) = nullptr;
      bool fresh_db = false;
      int num_threads = FLAGS_threads;

      if (name == Slice("fillseq")) {
        fresh_db = true;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillbatch")) {
        fresh_db = true;
        entries_per_batch_ = 1000;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillrandom")) {
        fresh_db = true;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("filluniquerandom")) {
        fresh_db = true;
        if (num_threads > 1) {
          fprintf(stderr, "filluniquerandom multithreaded not supported"
                           " set --threads=1");
          exit(1);
        }
        method = &Benchmark::WriteUniqueRandom;
      } else if (name == Slice("overwrite")) {
        fresh_db = false;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fillsync")) {
        fresh_db = true;
        num_ /= 1000;
        sync_ = true;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fill100K")) {
        fresh_db = true;
        num_ /= 1000;
        value_size_ = 100 * 1000;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("readseq")) {
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("readreverse")) {
        method = &Benchmark::ReadReverse;
      } else if (name == Slice("readrandom")) {
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("readmissing")) {
        method = &Benchmark::ReadMissing;
      } else if (name == Slice("seekrandom")) {
        method = &Benchmark::SeekRandom;
      } else if (name == Slice("readhot")) {
        method = &Benchmark::ReadHot;
      } else if (name == Slice("readrandomsmall")) {
        reads_ /= 1000;
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("prefixscanrandom")) {
        method = &Benchmark::PrefixScanRandom;
      } else if (name == Slice("deleteseq")) {
        method = &Benchmark::DeleteSeq;
      } else if (name == Slice("deleterandom")) {
        method = &Benchmark::DeleteRandom;
      } else if (name == Slice("readwhilewriting")) {
        num_threads++;  // Add extra thread for writing
        method = &Benchmark::ReadWhileWriting;
      } else if (name == Slice("readrandomwriterandom")) {
        method = &Benchmark::ReadRandomWriteRandom;
      } else if (name == Slice("updaterandom")) {
        method = &Benchmark::UpdateRandom;
      } else if (name == Slice("appendrandom")) {
        method = &Benchmark::AppendRandom;
      } else if (name == Slice("mergerandom")) {
        if (FLAGS_merge_operator.empty()) {
          fprintf(stdout, "%-12s : skipped (--merge_operator is unknown)\n",
                  name.ToString().c_str());
          method = nullptr;
        } else {
          method = &Benchmark::MergeRandom;
        }
      } else if (name == Slice("randomwithverify")) {
        method = &Benchmark::RandomWithVerify;
      } else if (name == Slice("compact")) {
        method = &Benchmark::Compact;
      } else if (name == Slice("crc32c")) {
        method = &Benchmark::Crc32c;
      } else if (name == Slice("acquireload")) {
        method = &Benchmark::AcquireLoad;
      } else if (name == Slice("snappycomp")) {
        method = &Benchmark::SnappyCompress;
      } else if (name == Slice("snappyuncomp")) {
        method = &Benchmark::SnappyUncompress;
      } else if (name == Slice("heapprofile")) {
        HeapProfile();
      } else if (name == Slice("stats")) {
        PrintStats("rocksdb.stats");
      } else if (name == Slice("levelstats")) {
        PrintStats("rocksdb.levelstats");
      } else if (name == Slice("sstables")) {
        PrintStats("rocksdb.sstables");
      } else {
        if (name != Slice()) {  // No error message for empty name
          fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
        }
      }

      if (fresh_db) {
        if (FLAGS_use_existing_db) {
          fprintf(stdout, "%-12s : skipped (--use_existing_db is true)\n",
                  name.ToString().c_str());
          method = nullptr;
        } else {
          if (conn_ != NULL) {
            conn_->close(conn_, NULL);
            conn_ = NULL;
          }
          std::vector<std::string> files;
          Env::Default()->GetChildren(FLAGS_db, &files);
          if (!FLAGS_use_existing_db) {
            for (unsigned int i = 0; i < files.size(); i++) {
              std::string file_name(FLAGS_db);
              file_name += "/";
              file_name += files[i];
              Env::Default()->DeleteFile(file_name.c_str());
            }
          }
          Open();
        }
      }

      if (method != nullptr) {
        fprintf(stdout, "DB path: [%s]\n", FLAGS_db.c_str());
        RunBenchmark(num_threads, name, method);
      }
    }
    if (conn_ != NULL) {
      conn_->close(conn_, NULL);
      conn_ = NULL;
    }
    if (FLAGS_statistics) {
     fprintf(stdout, "STATISTICS:\n%s\n", "TODO: dump WiredTiger stats");
    }
  }

 private:
  struct ThreadArg {
    Benchmark* bm;
    SharedState* shared;
    ThreadState* thread;
    void (Benchmark::*method)(ThreadState*);
  };

  static void ThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    ThreadState* thread = arg->thread;
    {
      MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    thread->stats.Start(thread->tid);
    (arg->bm->*(arg->method))(thread);
    thread->stats.Stop();

    {
      MutexLock l(&shared->mu);
      shared->num_done++;
      if (shared->num_done >= shared->total) {
        shared->cv.SignalAll();
      }
    }
  }

  void RunBenchmark(int n, Slice name,
                    void (Benchmark::*method)(ThreadState*)) {
    SharedState shared;
    shared.total = n;
    shared.num_initialized = 0;
    shared.num_done = 0;
    shared.start = false;

    ThreadArg* arg = new ThreadArg[n];
    for (int i = 0; i < n; i++) {
      arg[i].bm = this;
      arg[i].method = method;
      arg[i].shared = &shared;
      arg[i].thread = new ThreadState(i, conn_);
      arg[i].thread->shared = &shared;
      Env::Default()->StartThread(ThreadBody, &arg[i]);
    }

    shared.mu.Lock();
    while (shared.num_initialized < n) {
      shared.cv.Wait();
    }

    shared.start = true;
    shared.cv.SignalAll();
    while (shared.num_done < n) {
      shared.cv.Wait();
    }
    shared.mu.Unlock();

    // Stats for some threads can be excluded.
    Stats merge_stats;
    for (int i = 0; i < n; i++) {
      merge_stats.Merge(arg[i].thread->stats);
    }
    merge_stats.Report(name);

    for (int i = 0; i < n; i++) {
      delete arg[i].thread;
    }
    delete[] arg;
  }

  void Crc32c(ThreadState* thread) {
    // Checksum about 500MB of data total
    const int size = 4096;
    const char* label = "(4K per op)";
    std::string data(size, 'x');
    int64_t bytes = 0;
    uint32_t crc = 0;
    while (bytes < 500 * 1048576) {
      crc = crc32c::Value(data.data(), size);
      thread->stats.FinishedSingleOp(nullptr);
      bytes += size;
    }
    // Print so result is not dead
    fprintf(stderr, "... crc=0x%x\r", static_cast<unsigned int>(crc));

    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(label);
  }

  void AcquireLoad(ThreadState* thread) {
    int dummy;
    port::AtomicPointer ap(&dummy);
    int count = 0;
    void *ptr = nullptr;
    thread->stats.AddMessage("(each op is 1000 loads)");
    while (count < 100000) {
      for (int i = 0; i < 1000; i++) {
        ptr = ap.Acquire_Load();
      }
      count++;
      thread->stats.FinishedSingleOp(nullptr);
    }
    if (ptr == nullptr) exit(1); // Disable unused variable warning.
  }

  void SnappyCompress(ThreadState* thread) {
    RandomGenerator gen;
    Slice input = gen.Generate(4096);
    int64_t bytes = 0;
    int64_t produced = 0;
    bool ok = true;
    std::string compressed;
    while (ok && bytes < 1024 * 1048576) {  // Compress 1G
      ok = port::Snappy_Compress(Options().compression_opts, input.data(),
                                 input.size(), &compressed);
      produced += compressed.size();
      thread->stats.FinishedSingleOp(nullptr);
      bytes += input.size();
    }

    if (!ok) {
      thread->stats.AddMessage("(snappy failure)");
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "(output: %.1f%%)",
               (produced * 100.0) / bytes);
      thread->stats.AddMessage(buf);
      thread->stats.AddBytes(bytes);
    }
  }

  void SnappyUncompress(ThreadState* thread) {
    RandomGenerator gen;
    Slice input = gen.Generate(4096);
    std::string compressed;
    bool ok = port::Snappy_Compress(Options().compression_opts, input.data(),
                                    input.size(), &compressed);
    int64_t bytes = 0;
    char* uncompressed = new char[input.size()];
    while (ok && bytes < 1024 * 1048576) {  // Compress 1G
      ok =  port::Snappy_Uncompress(compressed.data(), compressed.size(),
                                    uncompressed);
      thread->stats.FinishedSingleOp(nullptr);
      bytes += input.size();
    }
    delete[] uncompressed;

    if (!ok) {
      thread->stats.AddMessage("(snappy failure)");
    } else {
      thread->stats.AddBytes(bytes);
    }
  }

  int checkConfig() {

    int config_error = 0;

    // Check for options that WiredTiger doesn't support.
    if (FLAGS_compressed_cache_size != -1) {
      fprintf(stderr, "Error: compressed cache not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_max_write_buffer_number) {
      fprintf(stderr, "Error: max_write_buffer_number not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_min_write_buffer_number_to_merge) {
      fprintf(stderr,
        "Error: min_write_buffer_number_to_merge not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_max_background_compactions > 4)
      fprintf(stderr,
        "Warning: Running WiredTiger with more than 4 compaction threads isn't"
        "recommended\n");
    if (FLAGS_compaction_style) {
      fprintf(stderr, "Error: compaction_style not supported by WiredTiger\n");
      config_error = 1;
    if (FLAGS_open_files) {
      fprintf(stderr, "Error: open_files not supported by WiredTiger\n");
      config_error = 1;
    }
    if (Env::Default()) {
      fprintf(stderr, "Error: custom environment not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_num_levels) {
      fprintf(stderr, "Error: num_levels not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_target_file_size_multiplier) {
      fprintf(stderr,
        "Error: target_file_size_multiplier not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_max_bytes_for_level_base ||
      FLAGS_max_bytes_for_level_multiplier ||
      FLAGS_max_bytes_for_level_multiplier_additional != "") {
      fprintf(stderr,
        "Error: max_bytes_for_level not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_filter_deletes)
      fprintf(stderr, "Error: filter_deletes not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_prefix_size || FLAGS_rep_factory) {
      fprintf(stderr, "Error: prefix skipping not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_rep_factory) {
      fprintf(stderr,
        "Error: rep_factory configuration not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_level0_stop_writes_trigger != 12) {
      fprintf(stderr,
        "Error: level0_stop_writes_trigger not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_level0_file_num_compaction_trigger != 4) {
      fprintf(stderr,
        "Error: level0_file_num_compaction_trigger not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_level0_slowdown_writes_trigger != 8) {
      fprintf(stderr,
        "Error: level0_slowdown_writes_trigger not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_wal_ttl_seconds || FLAGS_wal_size_limit_MB) {
      fprintf(stderr,
        "Error: Write ahead log archive not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_min_level_to_compress != -1) {
      fprintf(stderr, "Error: min_level_to_compress not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_disable_seek_compaction) {
      fprintf(stderr,
        "Error: disable_seek_compaction not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_delete_obsolete_files_period_micros) {
      fprintf(stderr,
        "Error: delete_obsolete_files_period_micros not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_soft_rate_limit != 0.0 || FLAGS_hard_rate_limit != 0.0 ||
      FLAGS_rate_limit_delay_max_milliseconds != 1000) {
      fprintf(stderr, "Error: rate limiting not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_table_cache_numshardbits != 4) {
      fprintf(stderr,
        "Error: table_cache_numshardbits not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_max_grandparent_overlap_factor != 10) {
      fprintf(stderr,
        "Error: max_grandparent_overlap_factor not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_source_compaction_factor != 1) {
      fprintf(stderr,
        "Error: source_compaction_factor not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_advise_random_on_open) {
      fprintf(stderr, "Error: advise_random_on_open not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_compaction_fadvice != "NORMAL") {
      fprintf(stderr, "Error: compaction_fadvice not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_use_adaptive_mutex) {
      fprintf(stderr, "Error: use_adaptive_mutex not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_bytes_per_sync) {
      fprintf(stderr, "Error: bytes_per_sync not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_merge_operator != "" || FLAGS_universal_size_ratio ||
      FLAGS_universal_min_merge_width || FLAGS_universal_max_merge_width ||
      FLAGS_universal_max_size_amplification_percent) {
      fprintf(stderr, "Error: merge_operator not supported by WiredTiger\n");
      config_error = 1;
    }
    if (FLAGS_write_buffer_size) {
      fprintf(stderr, "Error: write_buffer_size not supported by WiredTiger\n");
      fprintf(stderr, "\t The configured cache is automatically used as a write"
        " buffer in WiredTiger.\n");
      config_error = 1;
    }
    return (config_error);
  }

  void Open() {

    if (checkConfig() != 0)
      exit (1);

    std::stringstream config;
    std::stringstream table_config;
    config.str("");
    table_config.str("");

    // Setup WiredTiger open options
    if (!FLAGS_use_existing_db)
      config << "create";
    if (FLAGS_cache_size != -1)
      config << ",cache_size=" << FLAGS_cache_size;
    if (FLAGS_statistics)
      config << ",statistics=[fast,clear]";
    if (FLAGS_disable_data_sync)
      config << ",checkpoint_sync=false";
    if (FLAGS_use_fsync)
      config << ",transaction_sync=fsync";
    if (strncmp(FLAGS_compression_type.c_str(),
       "snappy", strlen("snappy")) == 0) {
      config << ",extensions=[libwiredtiger_snappy.so]";
      table_config << ",block_compressor=snappy";
    } else if (strncmp(FLAGS_compression_type.c_str(),
      "bzip2", strlen("bzip2")) == 0) {
      config << ",extensions=[libwiredtiger_bzip2.so]";
      table_config << ",block_compressor=bzip2";
    } else if (strncmp(FLAGS_compression_type.c_str(),
      "none", strlen("none")) != 0) {
      fprintf(stderr,
        "Error: Unsupported compression type: %s\n",
        FLAGS_compression_type.c_str());
      exit(1);
    }
    if (FLAGS_disable_auto_compactions)
      config << ",lsm_merge=false";
    if (!FLAGS_bufferedio)
      config << ",direct_io=[data]";
    if (!FLAGS_mmap_read || !FLAGS_mmap_write)
      config << ",mmap=false";
    if (FLAGS_disable_wal)
      config << ",log=(enabled=false)";
    else
      config << ",log=(enabled=true)";

    // Setup table configuration options
    if (FLAGS_block_size)
      table_config << ",allocation_size=" << FLAGS_block_size;

    if (!FLAGS_verify_checksum)
      table_config << ",checksum=off";

    // WiredTiger specific table configuration options.
    table_config << ",key_format=S,value_format=S";
    if (FLAGS_use_lsm) {
      table_config << ",type=lsm";
      table_config << ",prefix_compression=false";
      table_config << ",leaf_page_max=16kb";
      table_config << ",leaf_item_max=2kb";
      table_config << ",lsm=(";
      table_config << "chunk_size=20MB";
      table_config << ",chunk_max=100GB";
      if (FLAGS_bloom_bits > 0)
        table_config << ",bloom_bit_count=" << FLAGS_bloom_bits;
      if (FLAGS_bloom_bits > 4)
        table_config << ",bloom_hash_count=" << FLAGS_bloom_bits / 2;
      if (FLAGS_target_file_size_base)
        table_config << ",chunk_size=" << FLAGS_target_file_size_base;
      if (FLAGS_max_background_compactions)
        table_config <<
          ",merge_threads=" << FLAGS_max_background_compactions;
      table_config << ")"; // close LSM configuration group.
    }

    // Add user configuration to the end - so it overrides other values.
    config << "," << FLAGS_wiredtiger_open_config;
    table_config << "," << FLAGS_wiredtiger_table_config;

    int ret;
    Env::Default()->CreateDir(FLAGS_db);
    if ((ret = wiredtiger_open(
      FLAGS_db.c_str(), NULL, config.str().c_str(), &conn_)) != 0)  {
      fprintf(stderr, "Error: Failed wiredtiger_open: %s\n",
        wiredtiger_strerror(ret));
      exit(1);
    }
    fprintf(stderr, "wiredtiger_open(%s, %s)\n",
      FLAGS_db.c_str(), config.str().c_str());

    WT_SESSION *session;
    if ((ret = conn_->open_session(conn_, NULL, NULL, &session)) != 0) {
      fprintf(stderr,
        "Error: Failed open_session: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    std::stringstream uri;
    uri.str("");
    uri << "table:dbbench_wt";
    uri_ = uri.str();
    if ((ret = session->create(
      session, uri_.c_str(), table_config.str().c_str())) != 0) {
      fprintf(stderr, "Error opening table. uri: %s, config: %s, error: %s\n",
        uri_.c_str(), table_config.str().c_str(), wiredtiger_strerror(ret));
      exit(1);
    }
    fprintf(stderr, "session->create(%s, %s)\n",
      uri_.c_str(), table_config.str().c_str());
    session->close(session, NULL);
  }

  enum WriteMode {
    RANDOM, SEQUENTIAL, UNIQUE_RANDOM
  };

  void WriteSeq(ThreadState* thread) {
    DoWrite(thread, SEQUENTIAL);
  }

  void WriteRandom(ThreadState* thread) {
    DoWrite(thread, RANDOM);
  }

  void WriteUniqueRandom(ThreadState* thread) {
    DoWrite(thread, UNIQUE_RANDOM);
  }

  void DoWrite(ThreadState* thread, WriteMode write_mode) {
    const int test_duration = write_mode == RANDOM ? FLAGS_duration : 0;
    const int num_ops = writes_ == 0 ? num_ : writes_ ;
    Duration duration(test_duration, num_ops);
    unique_ptr<BitSet> bit_set;

    if (write_mode == UNIQUE_RANDOM) {
      bit_set.reset(new BitSet(num_ops));
    }

    if (num_ != FLAGS_num) {
      char msg[100];
      snprintf(msg, sizeof(msg), "(%lld ops)", num_);
      thread->stats.AddMessage(msg);
    }
    std::stringstream txn_config;
    txn_config.str("");
    txn_config << "isolation=snapshot";
    if (sync_)
        txn_config << ",sync=full";
    else
        txn_config << ",sync=none";

    WT_CURSOR *cursor;
    std::stringstream cur_config;
    cur_config.str("");
    cur_config << "overwrite";
    if (write_mode == SEQUENTIAL && FLAGS_threads == 1)
  cur_config << ",bulk=true";

    int ret = thread->session->open_cursor(
      thread->session, uri_.c_str(), NULL, cur_config.str().c_str(), &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }

    RandomGenerator gen;
    Status s;
    int64_t bytes = 0;
    int i = 0;
    while (!duration.Done(entries_per_batch_)) {
      /* TODO: Should batches be wrapped in a transaction? */
      for (int j = 0; j < entries_per_batch_; j++) {
        long long k = 0;
        switch(write_mode) {
          case SEQUENTIAL:
            k = i +j;
            break;
          case RANDOM:
            k = thread->rand.Next() % FLAGS_num;
            break;
          case UNIQUE_RANDOM:
            {
              const long long t = thread->rand.Next() % FLAGS_num;
              if (!bit_set->test(t)) {
                // best case
                k = t;
              } else {
                bool found = false;
                // look forward
                for (size_t i = t + 1; i < bit_set->size(); ++i) {
                  if (!bit_set->test(i)) {
                    found = true;
                    k = i;
                    break;
                  }
                }
                if (!found) {
                  for (size_t i = t; i-- > 0;) {
                    if (!bit_set->test(i)) {
                      found = true;
                      k = i;
                      break;
                    }
                  }
                }
              }
              bit_set->set(k);
              break;
            }
        };
        if (k == 0)
          continue; /* Wired Tiger does not support 0 keys. */
        char key[100];
        snprintf(key, sizeof(key), "%016lld", k);
        cursor->set_key(cursor, key);
        std::string value = gen.Generate(value_size_).ToString();
        cursor->set_value(cursor, value.c_str());
        int ret = cursor->insert(cursor);
        if (ret != 0) {
          fprintf(stderr, "insert error: %s\n", wiredtiger_strerror(ret));
          exit(1);
        }
        bytes += value_size_ + strlen(key);
        thread->stats.FinishedSingleOp(nullptr);
      }
      i += entries_per_batch_;
    }

    cursor->close(cursor);
    thread->stats.AddBytes(bytes);
  }

  void ReadSequential(ThreadState* thread) {
    const char *ckey, *cvalue;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(
      thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }

    int64_t bytes = 0;
    long long i = 0;
    while ((ret = cursor->next(cursor)) == 0 && i < reads_) {
      cursor->get_key(cursor, &ckey);
      cursor->get_value(cursor, &cvalue);
      thread->stats.FinishedSingleOp(nullptr);
      bytes += strlen(ckey) + strlen(cvalue);
      ++i;
    }
    cursor->close(cursor);
    thread->stats.AddBytes(bytes);
  }

  void ReadReverse(ThreadState* thread) {
    const char *ckey, *cvalue;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(
      thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }

    int64_t bytes = 0;
    long long i = 0;
    while ((ret = cursor->prev(cursor)) == 0 && i < reads_) {
      cursor->get_key(cursor, &ckey);
      cursor->get_value(cursor, &cvalue);
      thread->stats.FinishedSingleOp(nullptr);
      bytes += strlen(ckey) + strlen(cvalue);
      ++i;
    }
    cursor->close(cursor);
    thread->stats.AddBytes(bytes);
  }

  // Calls MultiGet over a list of keys from a random distribution.
  // Returns the total number of keys found.
  long MultiGetRandom(ReadOptions& options, int num_keys,
                     Random64& rand, long long range, const char* suffix) {
#if 1
    fprintf(stderr, "MultiGetRandom not implemented for WiredTiger\n");
    exit(1);
    return (0);
#else
    assert(num_keys > 0);
    std::vector<Slice> keys(num_keys);
    std::vector<std::string> values(num_keys);
    std::vector<unique_ptr<char []> > gen_keys(num_keys);

    int i;
    long long k;

    // Fill the keys vector
    for(i=0; i<num_keys; ++i) {
      k = rand.Next() % range;
      gen_keys[i] = GenerateKeyFromInt(k,suffix);
      keys[i] = gen_keys[i].get();
    }

    if (FLAGS_use_snapshot) {
      options.snapshot = db_->GetSnapshot();
    }

    // Apply the operation
    std::vector<Status> statuses = db_->MultiGet(options, keys, &values);
    assert((long)statuses.size() == num_keys);
    assert((long)keys.size() == num_keys);  // Should always be the case.
    assert((long)values.size() == num_keys);

    if (FLAGS_use_snapshot) {
      db_->ReleaseSnapshot(options.snapshot);
      options.snapshot = nullptr;
    }

    // Count number found
    long found = 0;
    for(i=0; i<num_keys; ++i) {
      if (statuses[i].ok()){
        ++found;
      } else if (FLAGS_warn_missing_keys == true) {
        // Key not found, or error.
        fprintf(stderr, "get error: %s\n", statuses[i].ToString().c_str());
      }
    }
    return found;
#endif
  }

  void ReadRandom(ThreadState* thread) {
    long long found = 0;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(
      thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }

    if (FLAGS_use_multiget) {
      fprintf(stderr, "Error: WiredTiger doesn't implement multi-get\n");
      exit(1);
    } else {
      Duration duration(FLAGS_duration, reads_);
      while (!duration.Done(1)) {
        char key[100];
        const long long k = thread->rand.Next() % FLAGS_num;
        if (k == 0) {
          found++; /* Wired Tiger does not support 0 keys. */
          continue;
        }
        snprintf(key, sizeof(key), "%016lld", k);
        cursor->set_key(cursor, key);
        if (cursor->search(cursor) == 0)
         found++;
        if (found && FLAGS_read_range > 1) {
          if (FLAGS_get_approx)
            fprintf(stderr, "Warning: WiredTiger skipping get_approx\n");

            for (int count = 1; count < FLAGS_read_range; ++count) {
              if (cursor->next(cursor) != 0)
                break;
            }
        }
        thread->stats.FinishedSingleOp(nullptr);
      }
    }
    cursor->close(cursor);
    char msg[100];
    snprintf(msg, sizeof(msg), "(%lld of %lld found)", found, reads_);
    thread->stats.AddMessage(msg);
  }

  void PrefixScanRandom(ThreadState* thread) {
#if 1
    fprintf(stderr,
      "Error: PrefixScanRandom requires RocksDB specific prefix transform"
      " function.\n");
    exit (1);
#else
    Duration duration(FLAGS_duration, reads_);
    const char *ckey;
    long long found = 0;
    char key[100];
    long long k;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(
      thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    while (!duration.Done(1)) {
      k = thread->rand.Next() % FLAGS_num;
      if (k == 0) {
        found++; /* Wired Tiger does not support 0 keys. */
        continue;
      }
      snprintf(key, sizeof(key), "%016lld", k);
      cursor->set_key(cursor, key);
      for (ret = cursor->search(cursor); ret == 0; ret = cursor->next(cursor)) {
        cursor->get_key(cursor, &ckey);
        if (strncmp(ckey, prefix.c_str(), prefix.size()) != 0)
        if (!std::string(ckey).compare(0, prefix.size(), prefix))
          break;
        found++;
      }
      thread->stats.FinishedSingleOp(nullptr);
    }
    cursor->close(cursor);
    char msg[100];
    snprintf(msg, sizeof(msg), "(%lld of %lld found)", found, reads_);
    thread->stats.AddMessage(msg);
#endif
  }

  void ReadMissing(ThreadState* thread) {
    FLAGS_warn_missing_keys = false;    // Never warn about missing keys

    Duration duration(FLAGS_duration, reads_);

    if (FLAGS_use_multiget) {
      fprintf(stderr, "Error: WiredTiger doesn't implement multi-get\n");
      exit(1);
    } else {  // Regular case (not MultiGet)
      WT_CURSOR *cursor;
      int ret = thread->session->open_cursor(
        thread->session, uri_.c_str(), NULL, NULL, &cursor);
      if (ret != 0) {
        fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
        exit(1);
      }
      char key[100];
      long long k;
      while (!duration.Done(1)) {
        k = thread->rand.Next() % FLAGS_num;
        snprintf(key, sizeof(key), "%016lld.", k);
        cursor->set_key(cursor, key);
        cursor->search(cursor);
        thread->stats.FinishedSingleOp(nullptr);
      }
      cursor->close(cursor);
    }
  }

  void ReadHot(ThreadState* thread) {
    Duration duration(FLAGS_duration, reads_);
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(
      thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    cursor->close(cursor);
    long long found = 0;

    if (FLAGS_use_multiget) {
      fprintf(stderr, "Error: WiredTiger doesn't implement multi-get\n");
      exit(1);
    } else {
      const long long range = (FLAGS_num + 99) / 100;
      char key[100];
      while (!duration.Done(1)) {
        const long long k = thread->rand.Next() % range;
        snprintf(key, sizeof(key), "%016lld", k);
        cursor->set_key(cursor, key);
        if (cursor->search(cursor) == 0)
          ++found;
        thread->stats.FinishedSingleOp(nullptr);
      }
    }

    char msg[100];
    snprintf(msg, sizeof(msg), "(%lld of %lld found)", found, reads_);
    thread->stats.AddMessage(msg);
  }

  void SeekRandom(ThreadState* thread) {
    Duration duration(FLAGS_duration, reads_);
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(
      thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    long long found = 0;
    char key[100];
    while (!duration.Done(1)) {
      const long long k = thread->rand.Next() % FLAGS_num;
      snprintf(key, sizeof(key), "%016lld", k);
      cursor->set_key(cursor, key);
      if(cursor->search(cursor) == 0) {
        found++;
      }
      thread->stats.FinishedSingleOp(nullptr);
    }
    cursor->close(cursor);
    char msg[100];
    snprintf(msg, sizeof(msg), "(%lld of %lld found)", found, num_);
    thread->stats.AddMessage(msg);

  }

  void DoDelete(ThreadState* thread, bool seq) {
    int64_t bytes = 0;
    WT_CURSOR *cursor;
    int ret = thread->session->open_cursor(
      thread->session, uri_.c_str(), NULL, NULL, &cursor);
    if (ret != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    std::stringstream txn_config;
    txn_config.str("");
    txn_config << "isolation=snapshot";
    if (sync_)
      txn_config << ",sync=full";
    else
      txn_config << ",sync=none";
    Duration duration(seq ? 0 : FLAGS_duration, num_);
    char key[100];
    long i = 0;
    while (!duration.Done(entries_per_batch_)) {
      // TODO: Wrap this batch in a transaction?
      for (int j = 0; j < entries_per_batch_; j++) {
        long long k = seq ? i+j : (thread->rand.Next() % FLAGS_num);
        snprintf(key, sizeof(key), "%016lld", k);
        if (k == 0)
          continue; /* Wired Tiger does not support 0 keys. */
        cursor->set_key(cursor, key);
        if ((ret = cursor->remove(cursor)) != 0) {
          if (FLAGS_threads == 1 || ret != WT_NOTFOUND) {
            fprintf(stderr,
              "del error: key %s %s\n", key, wiredtiger_strerror(ret));
            exit(1);
          }
        }
        thread->stats.FinishedSingleOp(nullptr);
        bytes += strlen(key);
      }
      ++i;
    }
    cursor->close(cursor);
    thread->stats.AddBytes(bytes);
  }

  void DeleteSeq(ThreadState* thread) {
    DoDelete(thread, true);
  }

  void DeleteRandom(ThreadState* thread) {
    DoDelete(thread, false);
  }

  void ReadWhileWriting(ThreadState* thread) {
    fprintf(stderr, "ReadWhileWriting not implemented.\n");
    exit (1);
#if 0
    if (thread->tid > 0) {
      ReadRandom(thread);
    } else {
      // Special thread that keeps writing until other threads are done.
      RandomGenerator gen;
      double last = Env::Default()->NowMicros();
      int writes_per_second_by_10 = 0;
      int num_writes = 0;

      // --writes_per_second rate limit is enforced per 100 milliseconds
      // intervals to avoid a burst of writes at the start of each second.

      if (FLAGS_writes_per_second > 0)
        writes_per_second_by_10 = FLAGS_writes_per_second / 10;

      // Don't merge stats from this thread with the readers.
      thread->stats.SetExcludeFromMerge();

      while (true) {
        {
          MutexLock l(&thread->shared->mu);
          if (thread->shared->num_done + 1 >= thread->shared->num_initialized) {
            // Other threads have finished
            break;
          }
        }

        const long long k = thread->rand.Next() % FLAGS_num;
        unique_ptr<char []> key = GenerateKeyFromInt(k);
        Status s = db_->Put(write_options_, key.get(),
                            gen.Generate(value_size_));
        if (!s.ok()) {
          fprintf(stderr, "put error: %s\n", s.ToString().c_str());
          exit(1);
        }
        thread->stats.FinishedSingleOp(db_);

        ++num_writes;
        if (writes_per_second_by_10 && num_writes >= writes_per_second_by_10) {
          double now = Env::Default()->NowMicros();
          double usecs_since_last = now - last;

          num_writes = 0;
          last = now;

          if (usecs_since_last < 100000.0) {
            Env::Default()->SleepForMicroseconds(100000.0 - usecs_since_last);
            last = Env::Default()->NowMicros();
          }
        }
      }
    }
#endif
  }

  // Given a key K and value V, this puts (K+"0", V), (K+"1", V), (K+"2", V)
  // in DB atomically i.e in a single batch. Also refer GetMany.
  Status PutMany(const WriteOptions& writeoptions,
                  const Slice& key, const Slice& value) {
    fprintf(stderr, "PutMany not implemented.\n");
    exit (1);
#if 0
    std::string suffixes[3] = {"2", "1", "0"};
    std::string keys[3];

    WriteBatch batch;
    Status s;
    for (int i = 0; i < 3; i++) {
      keys[i] = key.ToString() + suffixes[i];
      batch.Put(keys[i], value);
    }

    s = db_->Write(writeoptions, &batch);
    return s;
  }


  // Given a key K, this deletes (K+"0", V), (K+"1", V), (K+"2", V)
  // in DB atomically i.e in a single batch. Also refer GetMany.
  Status DeleteMany(const WriteOptions& writeoptions,
                  const Slice& key) {
    std::string suffixes[3] = {"1", "2", "0"};
    std::string keys[3];

    WriteBatch batch;
    Status s;
    for (int i = 0; i < 3; i++) {
      keys[i] = key.ToString() + suffixes[i];
      batch.Delete(keys[i]);
    }

    s = db_->Write(writeoptions, &batch);
    return s;
#endif
  }

  // Given a key K and value V, this gets values for K+"0", K+"1" and K+"2"
  // in the same snapshot, and verifies that all the values are identical.
  // ASSUMES that PutMany was used to put (K, V) into the DB.
  Status GetMany(const ReadOptions& readoptions,
                  const Slice& key, std::string* value) {
    fprintf(stderr, "GetMany not implemented.\n");
    exit (1);
#if 0
    std::string suffixes[3] = {"0", "1", "2"};
    std::string keys[3];
    Slice key_slices[3];
    std::string values[3];
    ReadOptions readoptionscopy = readoptions;
    readoptionscopy.snapshot = db_->GetSnapshot();
    Status s;
    for (int i = 0; i < 3; i++) {
      keys[i] = key.ToString() + suffixes[i];
      key_slices[i] = keys[i];
      s = db_->Get(readoptionscopy, key_slices[i], value);
      if (!s.ok() && !s.IsNotFound()) {
        fprintf(stderr, "get error: %s\n", s.ToString().c_str());
        values[i] = "";
        // we continue after error rather than exiting so that we can
        // find more errors if any
      } else if (s.IsNotFound()) {
        values[i] = "";
      } else {
        values[i] = *value;
      }
    }
    db_->ReleaseSnapshot(readoptionscopy.snapshot);

    if ((values[0] != values[1]) || (values[1] != values[2])) {
      fprintf(stderr, "inconsistent values for key %s: %s, %s, %s\n",
              key.ToString().c_str(), values[0].c_str(), values[1].c_str(),
              values[2].c_str());
      // we continue after error rather than exiting so that we can
      // find more errors if any
    }

    return s;
#endif
  }

  // Differs from readrandomwriterandom in the following ways:
  // (a) Uses GetMany/PutMany to read/write key values. Refer to those funcs.
  // (b) Does deletes as well (per FLAGS_deletepercent)
  // (c) In order to achieve high % of 'found' during lookups, and to do
  //     multiple writes (including puts and deletes) it uses upto
  //     FLAGS_numdistinct distinct keys instead of FLAGS_num distinct keys.
  // (d) Does not have a MultiGet option.
  void RandomWithVerify(ThreadState* thread) {
    fprintf(stderr, "RandomWithVerify not implemented.\n");
    exit (1);
#if 0
    ReadOptions options(FLAGS_verify_checksum, true);
    RandomGenerator gen;
    std::string value;
    long long found = 0;
    int get_weight = 0;
    int put_weight = 0;
    int delete_weight = 0;
    long long gets_done = 0;
    long long puts_done = 0;
    long long deletes_done = 0;

    // the number of iterations is the larger of read_ or write_
    for (long long i = 0; i < readwrites_; i++) {
      const long long k = thread->rand.Next() % (FLAGS_numdistinct);
      unique_ptr<char []> key = GenerateKeyFromInt(k);
      if (get_weight == 0 && put_weight == 0 && delete_weight == 0) {
        // one batch completed, reinitialize for next batch
        get_weight = FLAGS_readwritepercent;
        delete_weight = FLAGS_deletepercent;
        put_weight = 100 - get_weight - delete_weight;
      }
      if (get_weight > 0) {
        // do all the gets first
        Status s = GetMany(options, key.get(), &value);
        if (!s.ok() && !s.IsNotFound()) {
          fprintf(stderr, "getmany error: %s\n", s.ToString().c_str());
          // we continue after error rather than exiting so that we can
          // find more errors if any
        } else if (!s.IsNotFound()) {
          found++;
        }
        get_weight--;
        gets_done++;
      } else if (put_weight > 0) {
        // then do all the corresponding number of puts
        // for all the gets we have done earlier
        Status s = PutMany(write_options_, key.get(),
                           gen.Generate(value_size_));
        if (!s.ok()) {
          fprintf(stderr, "putmany error: %s\n", s.ToString().c_str());
          exit(1);
        }
        put_weight--;
        puts_done++;
      } else if (delete_weight > 0) {
        Status s = DeleteMany(write_options_, key.get());
        if (!s.ok()) {
          fprintf(stderr, "deletemany error: %s\n", s.ToString().c_str());
          exit(1);
        }
        delete_weight--;
        deletes_done++;
      }

      thread->stats.FinishedSingleOp(db_);
    }
    char msg[100];
    snprintf(msg, sizeof(msg),
             "( get:%lld put:%lld del:%lld total:%lld found:%lld)",
             gets_done, puts_done, deletes_done, readwrites_, found);
    thread->stats.AddMessage(msg);
#endif
  }

  // This is different from ReadWhileWriting because it does not use
  // an extra thread.
  void ReadRandomWriteRandom(ThreadState* thread) {
    fprintf(stderr, "ReadRandomWriteRandom not implemented.\n");
    exit (1);
#if 0
    if (FLAGS_use_multiget){
      // Separate function for multiget (for ease of reading)
      ReadRandomWriteRandomMultiGet(thread);
      return;
    }

    ReadOptions options(FLAGS_verify_checksum, true);
    RandomGenerator gen;
    std::string value;
    long long found = 0;
    int get_weight = 0;
    int put_weight = 0;
    long long reads_done = 0;
    long long writes_done = 0;
    Duration duration(FLAGS_duration, readwrites_);

    // the number of iterations is the larger of read_ or write_
    while (!duration.Done(1)) {
      const long long k = thread->rand.Next() % FLAGS_num;
      unique_ptr<char []> key = GenerateKeyFromInt(k);
      if (get_weight == 0 && put_weight == 0) {
        // one batch completed, reinitialize for next batch
        get_weight = FLAGS_readwritepercent;
        put_weight = 100 - get_weight;
      }
      if (get_weight > 0) {

        if (FLAGS_use_snapshot) {
          options.snapshot = db_->GetSnapshot();
        }

        if (FLAGS_get_approx) {
          char key2[100];
          snprintf(key2, sizeof(key2), "%016lld", k + 1);
          Slice skey2(key2);
          Slice skey(key2);
          Range range(skey, skey2);
          uint64_t sizes;
          db_->GetApproximateSizes(&range, 1, &sizes);
        }

        // do all the gets first
        Status s = db_->Get(options, key.get(), &value);
        if (!s.ok() && !s.IsNotFound()) {
          fprintf(stderr, "get error: %s\n", s.ToString().c_str());
          // we continue after error rather than exiting so that we can
          // find more errors if any
        } else if (!s.IsNotFound()) {
          found++;
        }

        get_weight--;
        reads_done++;

        if (FLAGS_use_snapshot) {
          db_->ReleaseSnapshot(options.snapshot);
        }

      } else  if (put_weight > 0) {
        // then do all the corresponding number of puts
        // for all the gets we have done earlier
        Status s = db_->Put(write_options_, key.get(),
                            gen.Generate(value_size_));
        if (!s.ok()) {
          fprintf(stderr, "put error: %s\n", s.ToString().c_str());
          exit(1);
        }
        put_weight--;
        writes_done++;
      }
      thread->stats.FinishedSingleOp(db_);
    }
    char msg[100];
    snprintf(msg, sizeof(msg),
             "( reads:%lld writes:%lld total:%lld found:%lld)",
             reads_done, writes_done, readwrites_, found);
    thread->stats.AddMessage(msg);
#endif
  }

  // ReadRandomWriteRandom (with multiget)
  // Does FLAGS_keys_per_multiget reads (per multiget), followed by some puts.
  // FLAGS_readwritepercent will specify the ratio of gets to puts.
  // e.g.: If FLAGS_keys_per_multiget == 100 and FLAGS_readwritepercent == 75
  // Then each block will do 100 multigets and 33 puts
  // So there are 133 operations in-total: 100 of them (75%) are gets, and 33
  // of them (25%) are puts.
  void ReadRandomWriteRandomMultiGet(ThreadState* thread) {
    fprintf(stderr, "ReadRandomWriteRandomMultiGet not implemented.\n");
    exit (1);
#if 0
    ReadOptions options(FLAGS_verify_checksum, true);
    RandomGenerator gen;

    // For multiget
    const long& kpg = FLAGS_keys_per_multiget;  // keys per multiget group

    long keys_left = readwrites_;  // number of keys still left to read
    long num_keys;                  // number of keys to read in current group
    long num_put_keys;              // number of keys to put in current group

    long found = 0;
    long reads_done = 0;
    long writes_done = 0;
    long multigets_done = 0;

    // the number of iterations is the larger of read_ or write_
    Duration duration(FLAGS_duration, readwrites_);
    while(true) {
      // Read num_keys keys, then write num_put_keys keys.
      // The ratio of num_keys to num_put_keys is always FLAGS_readwritepercent
      // And num_keys is set to be FLAGS_keys_per_multiget (kpg)
      // num_put_keys is calculated accordingly (to maintain the ratio)
      // Note: On the final iteration, num_keys and num_put_keys will be smaller
      num_keys = std::min(keys_left*(FLAGS_readwritepercent + 99)/100, kpg);
      num_put_keys = num_keys * (100-FLAGS_readwritepercent)
                     / FLAGS_readwritepercent;

      // This will break the loop when duration is complete
      if (duration.Done(num_keys + num_put_keys)) {
        break;
      }

      // A quick check to make sure our formula doesn't break on edge cases
      assert(num_keys >= 1);
      assert(num_keys + num_put_keys <= keys_left);

      // Apply the MultiGet operations
      found += MultiGetRandom(options, num_keys, thread->rand, FLAGS_num, "");
      ++multigets_done;
      reads_done+=num_keys;
      thread->stats.FinishedSingleOp(db_);

      // Now do the puts
      int i;
      long long k;
      for(i=0; i<num_put_keys; ++i) {
        k = thread->rand.Next() % FLAGS_num;
        unique_ptr<char []> key = GenerateKeyFromInt(k);
        Status s = db_->Put(write_options_, key.get(),
                            gen.Generate(value_size_));
        if (!s.ok()) {
          fprintf(stderr, "put error: %s\n", s.ToString().c_str());
          exit(1);
        }
        writes_done++;
        thread->stats.FinishedSingleOp(db_);
      }

      keys_left -= (num_keys + num_put_keys);
    }
    char msg[100];
    snprintf(msg, sizeof(msg),
             "( reads:%ld writes:%ld total:%lld multiget_ops:%ld found:%ld)",
             reads_done, writes_done, readwrites_, multigets_done, found);
    thread->stats.AddMessage(msg);
#endif
  }

  //
  // Read-modify-write for random keys
  void UpdateRandom(ThreadState* thread) {
    fprintf(stderr, "UpdateRandom not implemented.\n");
    exit (1);
#if 0
    ReadOptions options(FLAGS_verify_checksum, true);
    RandomGenerator gen;
    std::string value;
    long long found = 0;
    Duration duration(FLAGS_duration, readwrites_);

    // the number of iterations is the larger of read_ or write_
    while (!duration.Done(1)) {
      const long long k = thread->rand.Next() % FLAGS_num;
      unique_ptr<char []> key = GenerateKeyFromInt(k);

      if (FLAGS_use_snapshot) {
        options.snapshot = db_->GetSnapshot();
      }

      if (FLAGS_get_approx) {
        char key2[100];
        snprintf(key2, sizeof(key2), "%016lld", k + 1);
        Slice skey2(key2);
        Slice skey(key2);
        Range range(skey, skey2);
        uint64_t sizes;
        db_->GetApproximateSizes(&range, 1, &sizes);
      }

      if (db_->Get(options, key.get(), &value).ok()) {
        found++;
      }

      if (FLAGS_use_snapshot) {
        db_->ReleaseSnapshot(options.snapshot);
      }

      Status s = db_->Put(write_options_, key.get(), gen.Generate(value_size_));
      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
      thread->stats.FinishedSingleOp(db_);
    }
    char msg[100];
    snprintf(msg, sizeof(msg),
             "( updates:%lld found:%lld)", readwrites_, found);
    thread->stats.AddMessage(msg);
#endif
  }

  // Read-modify-write for random keys.
  // Each operation causes the key grow by value_size (simulating an append).
  // Generally used for benchmarking against merges of similar type
  void AppendRandom(ThreadState* thread) {
    fprintf(stderr, "AppendRandom not implemented.\n");
    exit (1);
#if 0
    ReadOptions options(FLAGS_verify_checksum, true);
    RandomGenerator gen;
    std::string value;
    long found = 0;

    // The number of iterations is the larger of read_ or write_
    Duration duration(FLAGS_duration, readwrites_);
    while (!duration.Done(1)) {
      const long long k = thread->rand.Next() % FLAGS_num;
      unique_ptr<char []> key = GenerateKeyFromInt(k);

      if (FLAGS_use_snapshot) {
        options.snapshot = db_->GetSnapshot();
      }

      if (FLAGS_get_approx) {
        char key2[100];
        snprintf(key2, sizeof(key2), "%016lld", k + 1);
        Slice skey2(key2);
        Slice skey(key2);
        Range range(skey, skey2);
        uint64_t sizes;
        db_->GetApproximateSizes(&range, 1, &sizes);
      }

      // Get the existing value
      if (db_->Get(options, key.get(), &value).ok()) {
        found++;
      } else {
        // If not existing, then just assume an empty string of data
        value.clear();
      }

      if (FLAGS_use_snapshot) {
        db_->ReleaseSnapshot(options.snapshot);
      }

      // Update the value (by appending data)
      Slice operand = gen.Generate(value_size_);
      if (value.size() > 0) {
        // Use a delimeter to match the semantics for StringAppendOperator
        value.append(1,',');
      }
      value.append(operand.data(), operand.size());

      // Write back to the database
      Status s = db_->Put(write_options_, key.get(), value);
      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
      thread->stats.FinishedSingleOp(db_);
    }
    char msg[100];
    snprintf(msg, sizeof(msg), "( updates:%lld found:%ld)", readwrites_, found);
    thread->stats.AddMessage(msg);
#endif
  }

  // Read-modify-write for random keys (using MergeOperator)
  // The merge operator to use should be defined by FLAGS_merge_operator
  // Adjust FLAGS_value_size so that the keys are reasonable for this operator
  // Assumes that the merge operator is non-null (i.e.: is well-defined)
  //
  // For example, use FLAGS_merge_operator="uint64add" and FLAGS_value_size=8
  // to simulate random additions over 64-bit integers using merge.
  void MergeRandom(ThreadState* thread) {
    fprintf(stderr, "MergeRandom not implemented.\n");
    exit (1);
#if 0
    RandomGenerator gen;

    // The number of iterations is the larger of read_ or write_
    Duration duration(FLAGS_duration, readwrites_);
    while (!duration.Done(1)) {
      const long long k = thread->rand.Next() % FLAGS_num;
      unique_ptr<char []> key = GenerateKeyFromInt(k);

      Status s = db_->Merge(write_options_, key.get(),
                            gen.Generate(value_size_));

      if (!s.ok()) {
        fprintf(stderr, "merge error: %s\n", s.ToString().c_str());
        exit(1);
      }
      thread->stats.FinishedSingleOp(db_);
    }

    // Print some statistics
    char msg[100];
    snprintf(msg, sizeof(msg), "( updates:%lld)", readwrites_);
    thread->stats.AddMessage(msg);
#endif
  }

  void Compact(ThreadState* thread) {
#if 1
    /*
     * TODO: It probably makes sense to run multiple compacts in parallel
     * if there are multiple LSM trees. The code to do that involves
     * opening multiple session handles (and possibly multiple threads), so
     * don't do that for now.
     */
    WT_CURSOR *md_cursor;
    int exact, ret;
    const char *key;
    char buf[100]; // For the timeout.

    if (!FLAGS_use_lsm)
      return ;

    if ((ret = thread->session->open_cursor(
      thread->session, "metadata:", NULL, NULL, &md_cursor)) != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    md_cursor->set_key(md_cursor, "lsm:");
    for (ret = md_cursor->search_near(md_cursor, &exact);
      ret == 0; ret = md_cursor->next(md_cursor)) {
      /* If the initial search result is behind, move our cursor forwards */
      if (exact == -1) {
        ret = md_cursor->next(md_cursor);
        exact = 1;
      }
      ret = md_cursor->get_key(md_cursor, &key);
      if (ret != 0 || strncmp(key, "lsm:", 4) != 0)
        break;

      // TODO: track how long we've been waiting if there are multiple
      // LSM trees to compact.
      snprintf(buf, 100, "timeout=%d", FLAGS_max_compact_wait);
      // Run a compact on this LSM.
      if ((ret = thread->session->compact(thread->session, key, buf)) != 0 &&
        ret != ETIMEDOUT) {
        fprintf(stderr, "compact error: %s\n", wiredtiger_strerror(ret));
        exit(1);
      }
    }
    md_cursor->close(md_cursor);

#else
    WT_CURSOR **lsm_cursors, *md_cursor, *stat_cursor;
    int allocated_cursors, exact, i, lsm_cursor_count, ret;
    const char *key;
    if (!FLAGS_use_lsm)
      return ;

    allocated_cursors = lsm_cursor_count = 0;
    lsm_cursors = NULL;
    if ((ret = thread->session->open_cursor(
      thread->session, "metadata:", NULL, NULL, &md_cursor)) != 0) {
      fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
      exit(1);
    }
    md_cursor->set_key(md_cursor, "lsm:");
    for (ret = md_cursor->search_near(md_cursor, &exact);
      ret == 0; ret = md_cursor->next(md_cursor)) {
      /* If the initial search result is behind, move our cursor forwards */
      if (exact == -1) {
        ret = md_cursor->next(md_cursor);
        exact = 1;
      }
      ret = md_cursor->get_key(md_cursor, &key);
      if (ret != 0 || strncmp(key, "lsm:", 4) != 0)
        break;
      // Grow our cursor array if required.
      if (lsm_cursor_count >= allocated_cursors) {
        lsm_cursors = (WT_CURSOR **)realloc(lsm_cursors,
          (allocated_cursors + 10) * sizeof(WT_CURSOR *));
        if (lsm_cursors == NULL) {
          fprintf(stderr, "Malloc failure.\n");
          exit (1);
        }
        allocated_cursors += 10;
      }

      // Open a cursor on this LSM, so that merges will happen.
      ret = thread->session->open_cursor(thread->session,
        key, NULL, NULL, &lsm_cursors[lsm_cursor_count]);
      if (ret != 0) {
        fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
        exit(1);
      }
      ++lsm_cursor_count;

    }
    md_cursor->close(md_cursor);
    /* Don't bother waiting if there were no LSM trees. */
    if (lsm_cursor_count != 0) {
      fprintf(stderr,
        "Allowing LSM tree to be merged. Can take up to %d seconds.\n",
        FLAGS_max_compact_wait);
#define COMPACT_STAT_WAIT_TIME  15
      int intervals_left = FLAGS_max_compact_wait / COMPACT_STAT_WAIT_TIME;
      const char *desc, *pvalue;
      uint64_t value;
      // Track statistics, to so we can tell that merges are happening.
      while (--intervals_left > 0) {
        sleep(COMPACT_STAT_WAIT_TIME);
        if ((ret = thread->session->open_cursor(
          thread->session, "statistics:", NULL, NULL, &stat_cursor)) != 0) {
          fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
          break;
        }
        stat_cursor->set_key(stat_cursor, WT_STAT_CONN_LSM_ROWS_MERGED);			
        if ((ret = stat_cursor->search(stat_cursor)) != 0) {
          fprintf(stderr, "stat_cursor->search error: %s\n",
            wiredtiger_strerror(ret));
          break;
        }
        if ((ret = stat_cursor->get_value(
          stat_cursor, &desc, &pvalue, &value)) != 0) {
          fprintf(stderr, "stat_cursor->search error: %s\n",
            wiredtiger_strerror(ret));
          break;
        }
        if (value == 0) {
          fprintf(stderr, "Compact: No merge activity in %d seconds, done.\n",
            COMPACT_STAT_WAIT_TIME);
          break;
        }
        stat_cursor->close(stat_cursor);
        stat_cursor = NULL;
      }
    }

    // Close our cursors.
    if (stat_cursor != NULL)
      stat_cursor->close(stat_cursor);
    for (i = 0; i < lsm_cursor_count; i++)
      lsm_cursors[i]->close(lsm_cursors[i]);
    free(lsm_cursors);
#endif
  }

  void PrintStats(const char* key) {
    fprintf(stderr, "Implement WiredTiger stat dump.\n");
  }

  static void WriteToFile(void* arg, const char* buf, int n) {
    reinterpret_cast<WritableFile*>(arg)->Append(Slice(buf, n));
  }

  void HeapProfile() {
    char fname[100];
    snprintf(fname, sizeof(fname), "%s/heap-%04d", FLAGS_db.c_str(),
             ++heap_counter_);
    EnvOptions soptions;
    unique_ptr<WritableFile> file;
    Status s =
      Env::Default()->NewWritableFile(std::string(fname), &file, soptions);
    if (!s.ok()) {
      fprintf(stderr, "%s\n", s.ToString().c_str());
      return;
    }
    bool ok = port::GetHeapProfile(WriteToFile, file.get());
    if (!ok) {
      fprintf(stderr, "heap profiling not supported\n");
      Env::Default()->DeleteFile(fname);
    }
  }
};

}  // namespace wiredtiger

int main(int argc, char** argv) {
  google::SetUsageMessage(std::string("\nUSAGE:\n") + std::string(argv[0]) +
                          " [OPTIONS]...");
  google::ParseCommandLineFlags(&argc, &argv, true);

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db.empty()) {
    std::string default_db_path;
    rocksdb::Env::Default()->GetTestDirectory(&default_db_path);
    default_db_path += "/dbbench";
    FLAGS_db = default_db_path;
  }

  rocksdb::Benchmark benchmark;
  benchmark.Run();
  return 0;
}