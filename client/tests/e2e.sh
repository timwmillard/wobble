#!/bin/sh
# End-to-end test: starts the Phoenix server with short timers on its own
# port, has wobble_bot play a full game against it, then stops the server.
#
#   tests/e2e.sh [bot binary]     (from client/; `make e2e` runs it)
#
# E2E_PORT (default 4100), BOTS (default 8) and E2E_TIMEOUT (seconds, default
# 300) override the defaults. Exits with the bot's status: 0 if the game
# finished.
set -eu

BOT=${1:-build/wobble_bot}
PORT=${E2E_PORT:-4100}
BOTS=${BOTS:-8}
TIMEOUT=${E2E_TIMEOUT:-300}
BACKEND=$(cd "$(dirname "$0")/../../backend" && pwd)
LOG=$(pwd)/build/e2e-server.log

if [ ! -x "$BOT" ]; then
   echo "e2e: $BOT not found; build it first" >&2
   exit 1
fi
if curl -s -o /dev/null "http://localhost:$PORT/"; then
   echo "e2e: something is already listening on port $PORT (set E2E_PORT)" >&2
   exit 1
fi

echo "e2e: starting server on port $PORT (log: $LOG)"
(cd "$BACKEND" && PORT=$PORT WOBBLE_FAST=1 exec mix phx.server) >"$LOG" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; wait $SERVER 2>/dev/null || true' EXIT INT TERM

# The first run compiles the backend, so give it a while.
i=0
until curl -s -o /dev/null "http://localhost:$PORT/"; do
   if ! kill -0 $SERVER 2>/dev/null; then
      echo "e2e: server exited; last lines of the log:" >&2
      tail -n 30 "$LOG" >&2
      exit 1
   fi
   i=$((i + 1))
   if [ $i -ge 240 ]; then
      echo "e2e: server not up after 120 s" >&2
      exit 1
   fi
   sleep 0.5
done

echo "e2e: $BOTS bots playing a game"
status=0
"$BOT" -s "ws://localhost:$PORT" -r e2e -n "$BOTS" -t "$TIMEOUT" || status=$?
if [ $status -eq 0 ]; then
   echo "e2e: passed"
else
   echo "e2e: FAILED (wobble_bot exit $status); server log: $LOG" >&2
fi
exit $status
