#!/bin/bash

DATA_DIR="results"
OPCOUNT=400000000
READCOUNT=20000000
DATA_SIZE=180
BLOOM_BITS=16
ENGINE="wt"
MSTAT=~/INSTALL/bin/mstat.py
MSTAT_ARGS="--interval=10 --loops=1000000"
KILL_CMD="kill -term"
if [ "x$ENGINE" == "xwt" ]; then
	BENCHMARK=./runners/wrun_
	OUTDIR_PREFIX=wt
elif [ "x$ENGINE" == "xrocks" ]; then
	BENCHMARK=./runners/rrun_
	OUTDIR_PREFIX=rocks
else
	echo "Error unsupported engine $ENGINE"
	exit 1
fi

run_fillrand=1
run_fillseq=1
run_overwrite=1
run_readrand=1
run_readrand2=1

OUTDIR_BASE=bench

TS=`date +%s`
OP_BRIEF=`echo $OPCOUNT / 1000000 | bc`
OUTDIR_UNIQUE=${OUTDIR_PREFIX}_${TS}_${OP_BRIEF}
OUTDIR=$OUTDIR_BASE/$OUTDIR_UNIQUE

mkdir -p $OUTDIR
# make it easy to find the most recent results.
rm -f $OUTDIR_BASE/latest
ln -s $OUTDIR_UNIQUE $OUTDIR_BASE/latest

echo "Clearing out data directory: $DATA_DIR"
mkdir -p $DATA_DIR
rm -rf $DATA_DIR/*

echo "Running LevelDB benchmark with:" >> $OUTDIR/info
echo "Date:		`date`" >> $OUTDIR/info
echo "ENGINE:		$ENGINE" >> $OUTDIR/info
echo "DATA DIR:		$DATA_DIR" >> $OUTDIR/info
echo "RESULT DIR:	$OUTDIR" >> $OUTDIR/info
echo "OP COUNT:		$OPCOUNT" >> $OUTDIR/info
echo "READ COUNT:	$READCOUNT" >> $OUTDIR/info
echo "DATA SIZE:	$DATA_SIZE" >> $OUTDIR/info
echo "" >> $OUTDIR/info

echo "`df`" >> $OUTDIR/info

if [ $run_fillrand != 0 ] ; then
	echo "Run fill random"
	if [ -e $MSTAT ]; then
		python ~/INSTALL/bin/mstat.py > $OUTDIR/fillrand.stat 2>&1 &
		mstat_pid=$!
	fi
	${BENCHMARK}fillrand.sh						\
		-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
		-b $BLOOM_BITS						\
		 > $OUTDIR/fillrand.out 2>&1 || exit 1
	if [ "x$ENGINE" == "xwt" ]; then
		CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
		echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/fillrand.out
	fi
	if [ -e $MSTAT ]; then
		$KILL_CMD $mstat_pid
	fi

	rm -rf $DATA_DIR/*
fi

if [ $run_fillseq != 0 -o $run_overwrite != 0 ] ; then
	echo "Run fill sequential"
	if [ -e $MSTAT ]; then
		python $MSTAT $MSTAT_ARGS > $OUTDIR/fillseq.stat 2>&1 &
		mstat_pid=$!
	fi
	${BENCHMARK}fillseq.sh						\
		-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
		-b $BLOOM_BITS						\
		> $OUTDIR/fillseq.out 2>&1 || exit 1
	if [ "x$ENGINE" == "xwt" ]; then
		CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
		echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/fillseq.out

		# WiredTiger deserves a compact here - RocksDB artifically loads
		# data into a single level when doing fillseq.
		echo "Run compact in preparation for overwrite phase"
		${BENCHMARK}compact.sh					\
			-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE\
			> /dev/null 2>&1 || exit 1
	fi
	if [ -e $MSTAT ]; then
		$KILL_CMD $mstat_pid
	fi
fi

if [ $run_overwrite != 0 ] ; then
echo "Run overwrite"
	if [ -e $MSTAT ]; then
		python $MSTAT $MSTAT_ARGS > $OUTDIR/overwrite.stat 2>&1 &
		mstat_pid=$!
	fi
	if [ "x$ENGINE" == "xwt" ]; then
		CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
		echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/overwrite.out
	fi
	${BENCHMARK}overwrite.sh					\
		-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
		-b $BLOOM_BITS						\
		>> $OUTDIR/overwrite.out 2>&1 || exit 1
	if [ "x$ENGINE" == "xwt" ]; then
		CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
		echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/overwrite.out
	fi
	if [ -e $MSTAT ]; then
		$KILL_CMD $mstat_pid
	fi
	rm -rf $DATA_DIR/*
fi

if [ $run_readrand != 0 ] ; then
	echo "Run read random"
	if [ -e $MSTAT ]; then
		python $MSTAT $MSTAT_ARGS > $OUTDIR/readrand.stat 2>&1 &
		mstat_pid=$!
	fi
	${BENCHMARK}readrand.sh						\
		-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
		-b $BLOOM_BITS						\
		>> $OUTDIR/readrand.out 2>&1 || exit 1
	if [ "x$ENGINE" == "xwt" ]; then
		CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
		echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/readrand.out
	fi
	if [ -e $MSTAT ]; then
		$KILL_CMD $mstat_pid
	fi

	rm -rf $DATA_DIR/*
fi

if [ $run_readrand2 != 0 ] ; then
	echo "Run complex read random"
	if [ -e $MSTAT ]; then
		python $MSTAT $MSTAT_ARGS > $OUTDIR/readrand2.stat 2>&1 &
		mstat_pid=$!
	fi
	${BENCHMARK}readrand.sh						\
		-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
		-b $BLOOM_BITS -m -o 50000000 				\
		>> $OUTDIR/readrand2.out 2>&1 || exit 1
	if [ "x$ENGINE" == "xwt" ]; then
		CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
		echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/readrand2.out
	fi
	if [ -e $MSTAT ]; then
		$KILL_CMD $mstat_pid
	fi

	rm -rf $DATA_DIR/*
fi

