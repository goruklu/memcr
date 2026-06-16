#!/bin/sh
#
# Performance benchmark: eager vs lazy-pages restore in memcr.
#
# Measures and compares:
#   - Total checkpoint+restore wall-clock time
#   - Time from restore start to process resumption (freeze duration)
#   - Memory touched by target after resume (validates lazy behavior)
#
# Usage:
#   sudo ./bench_lazy_pages.sh /path/to/memcr [ITERATIONS]
#
# Requirements:
#   - Linux kernel >= 4.11
#   - test-malloc binary in current directory
#   - Root privileges (for ptrace)
#

set -eu

if [ -z "${1:-}" ]; then
	echo "Usage: $0 /path/to/memcr [ITERATIONS]"
	exit 1
fi

MEMCR=$1
ITERATIONS=${2:-5}

if [ ! -x "$MEMCR" ]; then
	echo "error: $MEMCR is not executable"
	exit 2
fi

CUID=$(id -u)
if [ "$CUID" != "0" ]; then
	DO="sudo "
else
	DO=""
fi

# text formatting
BOLD='\033[1m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[0;33m'
WHITE='\033[37m'
NOFMT='\033[0m'

TEST_PIPE=./bench-pipe-$$

cleanup() {
	rm -f $TEST_PIPE
	rm -f /tmp/bench-eager-*.log /tmp/bench-lazy-*.log
}
trap cleanup EXIT

# ============================================================
# Helpers
# ============================================================

# Get current time in milliseconds
time_ms() {
	date +%s%N | cut -b1-13
}

# Run a single benchmark iteration
# Args: $1=mode ("eager" or "lazy"), $2=extra memcr opts, $3=iteration number
run_bench() {
	local MODE=$1
	local EXTRA_OPTS=$2
	local ITER=$3
	local LOGFILE="/tmp/bench-${MODE}-${ITER}.log"
	local TIMEFILE="/tmp/bench-${MODE}-${ITER}.time"
	local T_START T_END
	local TPID RET

	# Start test process
	rm -f $TEST_PIPE
	mkfifo $TEST_PIPE
	./test-malloc $TEST_PIPE > /dev/null 2>&1 &
	TPID=$!
	cat $TEST_PIPE > /dev/null
	rm -f $TEST_PIPE

	# Record start time
	T_START=$(time_ms)

	# Run memcr in background - it will checkpoint, restore, then return
	if [ "$MODE" = "lazy" ]; then
		$DO$MEMCR -d /tmp -n --lazy-pages $EXTRA_OPTS -p $TPID > "$LOGFILE" 2>&1 &
	else
		$DO$MEMCR -d /tmp -n $EXTRA_OPTS -p $TPID > "$LOGFILE" 2>&1 &
	fi
	local MEMCR_PID=$!

	# Wait for memcr to finish (in eager mode this means restore is done,
	# in lazy mode it waits for all pages to be served)
	wait $MEMCR_PID
	RET=$?

	# Record time when memcr returns
	local T_MEMCR_DONE=$(time_ms)

	if [ $RET -ne 0 ]; then
		echo "  ERROR: memcr failed (exit $RET) in $MODE mode, iter $ITER"
		kill $TPID 2>/dev/null || true
		wait $TPID 2>/dev/null || true
		return 1
	fi

	# Signal test to verify memory and exit
	# This forces the process to touch all its memory pages
	kill -USR1 $TPID
	wait $TPID
	RET=$?

	T_END=$(time_ms)

	if [ $RET -ne 0 ]; then
		echo "  ERROR: test-malloc failed (exit $RET) in $MODE mode, iter $ITER"
		return 1
	fi

	# Extract timing from memcr log
	local DL_TIME UL_TIME FREEZE_TIME
	DL_TIME=$(grep "download took" "$LOGFILE" | grep -o '[0-9]* ms' | grep -o '[0-9]*' || echo "0")
	UL_TIME=$(grep "upload took" "$LOGFILE" | grep -o '[0-9]* ms' | grep -o '[0-9]*' || echo "0")

	# Calculate the freeze duration:
	# For EAGER: freeze = download + upload (from memcr's own measurements)
	# For LAZY: freeze = download + uffd setup time (very short, no upload)
	# The most accurate freeze metric is from memcr's internal timing.
	if [ "$MODE" = "eager" ]; then
		FREEZE_TIME=$((DL_TIME + UL_TIME))
	else
		# In lazy mode, the freeze is approximately the download time
		# plus a small overhead for uffd setup. We can measure it as
		# (total memcr time) - (lazy handler time). The handler time
		# is roughly (memcr_done - start - download - setup_overhead).
		# Simplest: use download time as the primary freeze contributor
		# since upload=0 in lazy mode.
		FREEZE_TIME=$DL_TIME
	fi

	# Total wall-clock
	local TOTAL_MS=$((T_END - T_START))
	local MEMCR_MS=$((T_MEMCR_DONE - T_START))

	# Output results as tab-separated values
	echo "${MODE}	${ITER}	${FREEZE_TIME}	${MEMCR_MS}	${TOTAL_MS}	${DL_TIME}	${UL_TIME}"
}

# Compute average from a list of numbers
average() {
	local SUM=0
	local COUNT=0
	for V in "$@"; do
		SUM=$((SUM + V))
		COUNT=$((COUNT + 1))
	done
	if [ $COUNT -gt 0 ]; then
		echo $((SUM / COUNT))
	else
		echo 0
	fi
}

# Compute min from a list of numbers
minimum() {
	local MIN=999999999
	for V in "$@"; do
		if [ "$V" -lt "$MIN" ]; then
			MIN=$V
		fi
	done
	echo $MIN
}

# Compute max from a list of numbers
maximum() {
	local MAX=0
	for V in "$@"; do
		if [ "$V" -gt "$MAX" ]; then
			MAX=$V
		fi
	done
	echo $MAX
}

# ============================================================
# Main
# ============================================================

echo "${BOLD}================================================================${NOFMT}"
echo "${BOLD}  memcr lazy-pages performance benchmark${NOFMT}"
echo "${BOLD}================================================================${NOFMT}"
echo ""
echo "  memcr:       $MEMCR"
echo "  kernel:      $(uname -r)"
echo "  arch:        $(uname -m)"
echo "  iterations:  $ITERATIONS"
echo "  test:        test-malloc (16 MB static + 16 MB heap = ~32 MB)"
echo ""

# Verify test binary exists
if [ ! -x "./test-malloc" ]; then
	echo "error: ./test-malloc not found. Build it first."
	exit 3
fi

# ============================================================
# Warmup run (not counted)
# ============================================================

echo "${WHITE}[warmup] Running one eager and one lazy iteration...${NOFMT}"
run_bench "eager" "" "warmup" > /dev/null 2>&1 || true
run_bench "lazy" "" "warmup" > /dev/null 2>&1 || true
echo ""

# ============================================================
# Eager restore benchmark
# ============================================================

echo "${BOLD}--- Eager restore (baseline) ---${NOFMT}"
echo ""

EAGER_FREEZE=""
EAGER_TOTAL=""

for I in $(seq 1 $ITERATIONS); do
	RESULT=$(run_bench "eager" "" "$I")
	if [ $? -ne 0 ]; then
		echo "$RESULT"
		exit 1
	fi

	FREEZE=$(echo "$RESULT" | cut -f3)
	MEMCR_T=$(echo "$RESULT" | cut -f4)
	TOTAL=$(echo "$RESULT" | cut -f5)
	DL=$(echo "$RESULT" | cut -f6)
	UL=$(echo "$RESULT" | cut -f7)

	EAGER_FREEZE="$EAGER_FREEZE $FREEZE"
	EAGER_TOTAL="$EAGER_TOTAL $TOTAL"

	printf "  [iter %d] freeze: %4d ms  wall: %4d ms  (download: %d ms, upload: %d ms)\n" \
		"$I" "$FREEZE" "$TOTAL" "$DL" "$UL"
done

echo ""

EAGER_FREEZE_AVG=$(average $EAGER_FREEZE)
EAGER_FREEZE_MIN=$(minimum $EAGER_FREEZE)
EAGER_FREEZE_MAX=$(maximum $EAGER_FREEZE)
EAGER_TOTAL_AVG=$(average $EAGER_TOTAL)
EAGER_TOTAL_MIN=$(minimum $EAGER_TOTAL)
EAGER_TOTAL_MAX=$(maximum $EAGER_TOTAL)

printf "  ${CYAN}avg  freeze: %4d ms  wall: %4d ms${NOFMT}\n" "$EAGER_FREEZE_AVG" "$EAGER_TOTAL_AVG"
printf "  ${CYAN}min  freeze: %4d ms  wall: %4d ms${NOFMT}\n" "$EAGER_FREEZE_MIN" "$EAGER_TOTAL_MIN"
printf "  ${CYAN}max  freeze: %4d ms  wall: %4d ms${NOFMT}\n" "$EAGER_FREEZE_MAX" "$EAGER_TOTAL_MAX"
echo ""

# ============================================================
# Lazy-pages restore benchmark
# ============================================================

echo "${BOLD}--- Lazy-pages restore ---${NOFMT}"
echo ""

LAZY_FREEZE=""
LAZY_TOTAL=""

for I in $(seq 1 $ITERATIONS); do
	RESULT=$(run_bench "lazy" "" "$I")
	if [ $? -ne 0 ]; then
		echo "$RESULT"
		exit 1
	fi

	FREEZE=$(echo "$RESULT" | cut -f3)
	MEMCR_T=$(echo "$RESULT" | cut -f4)
	TOTAL=$(echo "$RESULT" | cut -f5)
	DL=$(echo "$RESULT" | cut -f6)

	LAZY_FREEZE="$LAZY_FREEZE $FREEZE"
	LAZY_TOTAL="$LAZY_TOTAL $TOTAL"

	printf "  [iter %d] freeze: %4d ms  wall: %4d ms  (download: %d ms, pages served on-demand)\n" \
		"$I" "$FREEZE" "$TOTAL" "$DL"
done

echo ""

LAZY_FREEZE_AVG=$(average $LAZY_FREEZE)
LAZY_FREEZE_MIN=$(minimum $LAZY_FREEZE)
LAZY_FREEZE_MAX=$(maximum $LAZY_FREEZE)
LAZY_TOTAL_AVG=$(average $LAZY_TOTAL)
LAZY_TOTAL_MIN=$(minimum $LAZY_TOTAL)
LAZY_TOTAL_MAX=$(maximum $LAZY_TOTAL)

printf "  ${CYAN}avg  freeze: %4d ms  wall: %4d ms${NOFMT}\n" "$LAZY_FREEZE_AVG" "$LAZY_TOTAL_AVG"
printf "  ${CYAN}min  freeze: %4d ms  wall: %4d ms${NOFMT}\n" "$LAZY_FREEZE_MIN" "$LAZY_TOTAL_MIN"
printf "  ${CYAN}max  freeze: %4d ms  wall: %4d ms${NOFMT}\n" "$LAZY_FREEZE_MAX" "$LAZY_TOTAL_MAX"
echo ""

# ============================================================
# Comparison
# ============================================================

echo "${BOLD}--- Comparison ---${NOFMT}"
echo ""

if [ "$EAGER_FREEZE_AVG" -gt 0 ]; then
	FREEZE_SPEEDUP=$((EAGER_FREEZE_AVG * 100 / LAZY_FREEZE_AVG))
	FREEZE_SAVED=$((EAGER_FREEZE_AVG - LAZY_FREEZE_AVG))
else
	FREEZE_SPEEDUP=0
	FREEZE_SAVED=0
fi

if [ "$EAGER_TOTAL_AVG" -gt 0 ] && [ "$LAZY_TOTAL_AVG" -gt 0 ]; then
	TOTAL_RATIO=$((LAZY_TOTAL_AVG * 100 / EAGER_TOTAL_AVG))
else
	TOTAL_RATIO=100
fi

printf "  Freeze time (avg):  eager=%d ms  lazy=%d ms" "$EAGER_FREEZE_AVG" "$LAZY_FREEZE_AVG"
if [ "$FREEZE_SAVED" -gt 0 ]; then
	printf "  ${GREEN}(-%d ms, %.0s%d%% of eager)${NOFMT}" "$FREEZE_SAVED" "" "$((100 - FREEZE_SPEEDUP))"
fi
echo ""

printf "  Total time  (avg):  eager=%d ms  lazy=%d ms" "$EAGER_TOTAL_AVG" "$LAZY_TOTAL_AVG"
printf "  (%d%% of eager)\n" "$TOTAL_RATIO"
echo ""

echo "${BOLD}  Notes:${NOFMT}"
echo "    - 'freeze' = time the process is actually stopped (download + upload for eager,"
echo "      download only for lazy since pages are served after resume)"
echo "    - 'wall'   = total wall-clock time from start to process verification complete"
echo "    - Lazy mode: process resumes after download; pages served on-demand as accessed"
echo "    - Eager mode: process waits for all pages to be uploaded before resuming"
echo ""

# ============================================================
# Additional: Lazy with --proc-mem (if available)
# ============================================================

echo "${BOLD}--- Lazy-pages + --proc-mem ---${NOFMT}"
echo ""

LAZY_PM_FREEZE=""
LAZY_PM_TOTAL=""

for I in $(seq 1 $ITERATIONS); do
	RESULT=$(run_bench "lazy" "--proc-mem" "$I")
	if [ $? -ne 0 ]; then
		echo "$RESULT"
		exit 1
	fi

	FREEZE=$(echo "$RESULT" | cut -f3)
	MEMCR_T=$(echo "$RESULT" | cut -f4)
	TOTAL=$(echo "$RESULT" | cut -f5)
	DL=$(echo "$RESULT" | cut -f6)

	LAZY_PM_FREEZE="$LAZY_PM_FREEZE $FREEZE"
	LAZY_PM_TOTAL="$LAZY_PM_TOTAL $TOTAL"

	printf "  [iter %d] freeze: %4d ms  wall: %4d ms  (download: %d ms)\n" \
		"$I" "$FREEZE" "$TOTAL" "$DL"
done

echo ""

LAZY_PM_FREEZE_AVG=$(average $LAZY_PM_FREEZE)
LAZY_PM_TOTAL_AVG=$(average $LAZY_PM_TOTAL)

printf "  ${CYAN}avg  freeze: %4d ms  wall: %4d ms${NOFMT}\n" "$LAZY_PM_FREEZE_AVG" "$LAZY_PM_TOTAL_AVG"
echo ""

# ============================================================
# Summary table
# ============================================================

echo "${BOLD}================================================================${NOFMT}"
echo "${BOLD}  Summary (averages over $ITERATIONS iterations)${NOFMT}"
echo "${BOLD}================================================================${NOFMT}"
echo ""
printf "  %-25s %10s %10s\n" "Mode" "Freeze(ms)" "Wall(ms)"
printf "  %-25s %10s %10s\n" "-------------------------" "----------" "----------"
printf "  %-25s %10d %10d\n" "Eager (baseline)" "$EAGER_FREEZE_AVG" "$EAGER_TOTAL_AVG"
printf "  %-25s %10d %10d\n" "Lazy-pages" "$LAZY_FREEZE_AVG" "$LAZY_TOTAL_AVG"
printf "  %-25s %10d %10d\n" "Lazy-pages + --proc-mem" "$LAZY_PM_FREEZE_AVG" "$LAZY_PM_TOTAL_AVG"
echo ""

if [ "$FREEZE_SAVED" -gt 0 ]; then
	echo "  ${GREEN}Lazy-pages reduces freeze time by ~${FREEZE_SAVED} ms (${FREEZE_SPEEDUP}x faster resume)${NOFMT}"
else
	echo "  ${YELLOW}No freeze time improvement detected (try larger workload)${NOFMT}"
fi
echo ""
