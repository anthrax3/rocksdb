#!/bin/bash

DATA_DIR="/ssd1/scratch/wiredtiger/results"
OPCOUNT=100000000
READCOUNT=1000000
DATA_SIZE=200
ENGINE="wt"
if [ "x$ENGINE" == "xwt" ]; then
	BENCHMARK=./runners/wrun_
	OUTDIR_PREFIX=wt
else
	echo "Error only WiredTiger engine supported by script"
	exit 1
	BENCHMARK=./runners/rrun_
	OUTDIR_PREFIX=rocks
fi

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

echo "Run fill random"
${BENCHMARK}fillrand.sh						\
	-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
	 > $OUTDIR/fillrand.out 2>&1 || exit 1
if [ "x$ENGINE" == "xwt" ]; then
	CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
	echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/fillrand.out
fi

rm -rf $DATA_DIR/*

echo "Run fill sequential"
${BENCHMARK}fillseq.sh						\
	-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
	> $OUTDIR/fillseq.out 2>&1 || exit 1
if [ "x$ENGINE" == "xwt" ]; then
	CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
	echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/fillseq.out
fi

echo "Run compact in preparation for overwrite phase"
${BENCHMARK}compact.sh						\
	-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
	> /dev/null 2>&1 || exit 1

echo "Run overwrite"
if [ "x$ENGINE" == "xwt" ]; then
	CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
	echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/overwrite.out
fi
${BENCHMARK}overwrite.sh					\
	-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
	>> $OUTDIR/overwrite.out 2>&1 || exit 1
if [ "x$ENGINE" == "xwt" ]; then
	CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
	echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/overwrite.out
fi

echo "Run read random"
${BENCHMARK}readrand.sh						\
	-i $OPCOUNT -d $DATA_DIR -r $READCOUNT -s $DATA_SIZE	\
	>> $OUTDIR/readrand.out 2>&1 || exit 1
if [ "x$ENGINE" == "xwt" ]; then
	CHUNK_COUNT=`ls -l $DATA_DIR/*lsm | wc -l`
	echo "LSM chunks: $CHUNK_COUNT" >> $OUTDIR/readrand.out
fi

rm -rf $DATA_DIR/*

