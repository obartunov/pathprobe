#!/bin/sh
# Run every example against the server selected by the usual PG* variables.
# Each script fails on the first structural check that does not hold.
set -e
cd "$(dirname "$0")"
for f in 0*.sql; do
	printf '%s ... ' "$f"
	psql -X -q -v ON_ERROR_STOP=1 -f "$f" > /dev/null
	echo ok
done
