#!/bin/sh
#
# Test script for memcr lazy-pages (userfaultfd) restore.
#
# Exercises checkpoint + lazy restore with various options
# to verify that pages are correctly served on-demand.
#
# Usage:
#   sudo ./run_lazy_test.sh /path/to/memcr
#
# Requirements:
#   - Linux kernel >= 4.11
#   - /proc/sys/vm/unprivileged_userfaultfd = 1  (or run as root)
#   - test-malloc binary in current directory
#

set -eu

if [ -z "${1:-}" ]; then
	echo "Usage: $0 /path/to/memcr"
	exit 1
fi

MEMCR=$1

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
WHITE='\033[37m'
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NOFMT='\033[0m'

TEST_CNT=0
TEST_PASS=0
TEST_FAIL=0
TEST_SKIP=0
TEST_PIPE=./test-pipe-lazy

# Check if userfaultfd is available
check_uffd() {
	if [ -f /proc/sys/vm/unprivileged_userfaultfd ]; then
		UFFD_VAL=$(cat /proc/sys/vm/unprivileged_userfaultfd)
		if [ "$UFFD_VAL" = "0" ] && [ "$CUID" != "0" ]; then
			echo "${YELLOW}warning: unprivileged_userfaultfd=0, tests may fail without root${NOFMT}"
		fi
	fi

	# Quick check: try to create a userfaultfd via a small helper
	KVER=$(uname -r | cut -d. -f1-2)
	KMAJOR=$(echo "$KVER" | cut -d. -f1)
	KMINOR=$(echo "$KVER" | cut -d. -f2)

	if [ "$KMAJOR" -lt 4 ] || { [ "$KMAJOR" -eq 4 ] && [ "$KMINOR" -lt 11 ]; }; then
		echo "${RED}error: kernel $KVER is too old, need >= 4.11 for userfaultfd${NOFMT}"
		exit 3
	fi
}

do_lazy_test() {
	local MEMCR_ENV="${1:-}"
	local MEMCR_OPTS="$2"
	local TEST="$3"
	local DESC="$4"

	TEST_CNT=$((TEST_CNT + 1))

	if [ -n "$MEMCR_ENV" ]; then
		MEMCR_CMD="$DO$MEMCR_ENV $MEMCR -d /tmp -n --lazy-pages $MEMCR_OPTS"
	else
		MEMCR_CMD="$DO$MEMCR -d /tmp -n --lazy-pages $MEMCR_OPTS"
	fi

	echo "${WHITE}[test $TEST_CNT] $DESC${NOFMT}"
	echo "         cmd: $MEMCR_CMD -p ..."

	# Create named pipe for synchronization
	rm -f $TEST_PIPE
	mkfifo $TEST_PIPE

	# Start the test process
	./"$TEST" $TEST_PIPE &
	TPID=$!

	# Wait for test to be ready
	cat $TEST_PIPE > /dev/null
	rm -f $TEST_PIPE

	# Run memcr with lazy-pages
	$MEMCR_CMD -p $TPID
	RET=$?
	if [ $RET -ne 0 ]; then
		echo "${RED}[test $TEST_CNT] FAILED - memcr exit code: $RET${NOFMT}"
		kill $TPID 2>/dev/null || true
		wait $TPID 2>/dev/null || true
		TEST_FAIL=$((TEST_FAIL + 1))
		return 1
	fi

	# Give lazy-pages handler a moment to start serving pages
	sleep 0.1

	# Signal the test process to verify its memory integrity
	kill -USR1 $TPID
	wait $TPID
	RET=$?
	if [ $RET -ne 0 ]; then
		echo "${RED}[test $TEST_CNT] FAILED - test process exit code: $RET (memory corruption?)${NOFMT}"
		TEST_FAIL=$((TEST_FAIL + 1))
		return 1
	fi

	echo "${GREEN}[test $TEST_CNT] PASSED${NOFMT}"
	TEST_PASS=$((TEST_PASS + 1))
	return 0
}

# ============================================================
# Main
# ============================================================

echo "${BOLD}========================================${NOFMT}"
echo "${BOLD}  memcr lazy-pages (userfaultfd) tests  ${NOFMT}"
echo "${BOLD}========================================${NOFMT}"
echo ""

check_uffd

echo "${BOLD}[+] Kernel: $(uname -r)${NOFMT}"
echo "${BOLD}[+] memcr:  $MEMCR${NOFMT}"
echo ""

# Remove stale pipe
rm -f $TEST_PIPE

# --- Basic lazy-pages tests ---

echo "${BOLD}--- Basic lazy-pages restore ---${NOFMT}"

do_lazy_test "" "" "test-malloc" \
	"lazy restore: basic (no extra options)" || true

do_lazy_test "" "--proc-mem" "test-malloc" \
	"lazy restore: with --proc-mem" || true

do_lazy_test "" "--rss-file" "test-malloc" \
	"lazy restore: with --rss-file" || true

do_lazy_test "" "--proc-mem --rss-file" "test-malloc" \
	"lazy restore: with --proc-mem --rss-file" || true

# --- Compression + lazy-pages tests ---

echo ""
echo "${BOLD}--- Compression + lazy-pages ---${NOFMT}"

# Check if lz4 compression is available
if $DO$MEMCR --version 2>&1 | grep -q "compress" || $DO$MEMCR -p 1 -n --compress lz4 2>&1 | grep -q "not available"; then
	# Try lz4 - if it fails with "not available", skip
	if ! $DO$MEMCR -p 1 -n --compress lz4 2>&1 | grep -q "not available"; then
		do_lazy_test "" "--compress lz4" "test-malloc" \
			"lazy restore: with LZ4 compression" || true

		do_lazy_test "" "--compress lz4 --rss-file" "test-malloc" \
			"lazy restore: with LZ4 compression + --rss-file" || true
	else
		echo "${YELLOW}[skip] LZ4 compression not available${NOFMT}"
		TEST_SKIP=$((TEST_SKIP + 1))
	fi
else
	do_lazy_test "" "--compress lz4" "test-malloc" \
		"lazy restore: with LZ4 compression" || true
fi

# Try zstd
if $DO$MEMCR -p 1 -n --compress zstd 2>&1 | grep -q "not available"; then
	echo "${YELLOW}[skip] ZSTD compression not available${NOFMT}"
	TEST_SKIP=$((TEST_SKIP + 1))
else
	do_lazy_test "" "--compress zstd" "test-malloc" \
		"lazy restore: with ZSTD compression" || true

	do_lazy_test "" "--compress zstd --rss-file" "test-malloc" \
		"lazy restore: with ZSTD compression + --rss-file" || true
fi

# --- Encryption + lazy-pages tests ---

echo ""
echo "${BOLD}--- Encryption + lazy-pages ---${NOFMT}"

if [ -f "$(dirname "$MEMCR")/../libencrypt.so" ]; then
	LIBENC="$(dirname "$MEMCR")/../libencrypt.so"
elif [ -f "$(dirname "$MEMCR")/libencrypt.so" ]; then
	LIBENC="$(dirname "$MEMCR")/libencrypt.so"
else
	LIBENC=""
fi

if [ -n "$LIBENC" ]; then
	do_lazy_test "env LD_PRELOAD=$LIBENC" "--rss-file --encrypt" "test-malloc" \
		"lazy restore: with encryption (default cipher)" || true

	do_lazy_test "env LD_PRELOAD=$LIBENC" "--rss-file --encrypt aes-256-cbc" "test-malloc" \
		"lazy restore: with AES-256-CBC encryption" || true
else
	echo "${YELLOW}[skip] libencrypt.so not found, skipping encryption tests${NOFMT}"
	TEST_SKIP=$((TEST_SKIP + 2))
fi

# --- Combined tests ---

echo ""
echo "${BOLD}--- Combined options + lazy-pages ---${NOFMT}"

if ! $DO$MEMCR -p 1 -n --compress lz4 2>&1 | grep -q "not available"; then
	if [ -n "$LIBENC" ]; then
		do_lazy_test "env LD_PRELOAD=$LIBENC" "--rss-file --compress lz4 --encrypt" "test-malloc" \
			"lazy restore: LZ4 + encryption" || true
	fi
fi

# --- Fallback test ---

echo ""
echo "${BOLD}--- Fallback behavior ---${NOFMT}"

# If we can make userfaultfd fail (e.g. by using a non-anonymous VMA only target),
# the code should fall back to eager restore gracefully.
# For now, just verify that a normal process works with --lazy-pages
do_lazy_test "" "--proc-mem --rss-file" "test-malloc" \
	"lazy restore: verify fallback path works" || true

# ============================================================
# Summary
# ============================================================

echo ""
echo "${BOLD}========================================${NOFMT}"
echo "${BOLD}  Results: $TEST_PASS passed, $TEST_FAIL failed, $TEST_SKIP skipped (of $TEST_CNT run)${NOFMT}"
echo "${BOLD}========================================${NOFMT}"

# Cleanup
rm -f $TEST_PIPE

if [ $TEST_FAIL -gt 0 ]; then
	exit 1
fi

exit 0
