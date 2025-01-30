#!/bin/bash

# Navigate to the directory containing this script to ensure paths are relative
cd "$(dirname "$0")"

# Set the absolute or relative paths to the tester and Unix socket
TESTER_PATH="../client"
SOCKET_PATH="@/tmp/supdemserv.sock"

# Ensure logs are saved in this directory
rm -f client1.log client2.log client3.log client4.log

# Run each tester with its own test file and redirect output
$TESTER_PATH -s testcase1.txt $SOCKET_PATH > client1.log 2>&1 &
pid1=$!

$TESTER_PATH -s testcase2.txt $SOCKET_PATH > client2.log 2>&1 &
pid2=$!

$TESTER_PATH -s testcase3.txt $SOCKET_PATH > client3.log 2>&1 &
pid3=$!

$TESTER_PATH -s testcase4.txt $SOCKET_PATH > client4.log 2>&1 &
pid4=$!

# Wait for all background processes to finish
wait $pid1 $pid2 $pid3 $pid4
echo "All testcases finished. Check client1.log, client2.log, client3.log, and client4.log for details."

$TESTER_PATH -s testcase0.txt $SOCKET_PATH > client0.log 2>&1 &
pid0=$!

wait $pid0
echo "All testcases finished. Check client0.log for details."
