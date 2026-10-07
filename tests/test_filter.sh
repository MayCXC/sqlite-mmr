#!/bin/sh
#
# xFilter's contract, run against the built extension: an error from the source
# query fails the mmr query instead of ending its rows, and a rowid IN (...)
# constraint restricts the candidates before the top k are taken.
#
# Usage: sh tests/test_filter.sh

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/mmr0"
test -f "$EXT.so" || test -f "$EXT.dylib" || {
	echo "$EXT extension not built"; exit 1; }
SQLITE3="${SQLITE3:-sqlite3}"

PASS=0
FAIL=0
eq() {
	if [ "$2" = "$3" ]; then
		PASS=$((PASS + 1)); echo "ok - $1"
	else
		FAIL=$((FAIL + 1)); echo "NOT ok - $1 (got '$2', want '$3')"
	fi
}

# Six documents of six tokens, rowid n holding 7 - n copies of "cat", so bm25
# ranks them 1 through 6; rowid 7 has no "cat".
FIXTURE="CREATE VIRTUAL TABLE docs USING fts5(body);
INSERT INTO docs(rowid, body) VALUES
  (1, 'cat cat cat cat cat cat'), (2, 'cat cat cat cat cat dog'),
  (3, 'cat cat cat cat dog dog'), (4, 'cat cat cat dog dog dog'),
  (5, 'cat cat dog dog dog dog'), (6, 'cat dog dog dog dog dog'),
  (7, 'dog dog dog dog dog dog');
CREATE VIRTUAL TABLE docs_mmr USING mmr(docs, body, rank);"

# One in-memory database per assertion. The asserted value is the last line of
# output: the final statement's result, or the error the CLI prints for it.
session() { $SQLITE3 :memory: -cmd ".load $EXT" "$FIXTURE $1" 2>&1 | tail -1; }
ids() { session "SELECT ifnull(group_concat(rowid), '') FROM ($1);"; }

eq "fixture: FTS5 ranks the cat documents 1 through 6" \
	"$(ids "SELECT rowid FROM docs WHERE docs MATCH 'cat' ORDER BY rank")" "1,2,3,4,5,6"
eq "without a rowid set, the rows are the top k by rank" \
	"$(ids "SELECT rowid FROM docs_mmr WHERE text MATCH 'cat' AND k = 3")" "1,2,3"

# The error is compared with the one the CLI prints for the same failure outside
# mmr, so the assertion holds whatever the CLI's error format.
eq "a MATCH the source rejects fails the query with the source's error" \
	"$(session "SELECT count(*) FROM docs_mmr WHERE text MATCH 'cat-dog' AND k = 2;")" \
	"$(session "SELECT count(*) FROM docs WHERE docs MATCH 'cat-dog';")"
eq "an error partway through the source's rows fails the query" \
	"$(session "CREATE VIRTUAL TABLE bad_mmr USING mmr(docs, CASE WHEN rowid = 3 THEN abs(-9223372036854775808) ELSE body END, rank);
	SELECT count(*) FROM bad_mmr WHERE text MATCH 'cat' AND k = 6;")" \
	"$(session "SELECT abs(-9223372036854775808);")"

# The top k are taken inside the set, not filtered out of the top k overall.
eq "rowid IN (list) keeps the best ranked rows inside the set" \
	"$(ids "SELECT rowid FROM docs_mmr WHERE text MATCH 'cat' AND k = 2 AND rowid IN (3, 4, 5, 6)")" "3,4"
eq "rowid IN (subquery) keeps the best ranked rows inside the set" \
	"$(ids "SELECT rowid FROM docs_mmr WHERE text MATCH 'cat' AND k = 2 AND rowid IN (SELECT rowid FROM docs WHERE rowid > 2)")" "3,4"
eq "rowid IN under MMR returns k rows, all inside the set" \
	"$(session "SELECT count(*) || '|' || min(rowid) FROM (SELECT rowid FROM docs_mmr WHERE text MATCH 'cat' AND k = 2 AND mmr_lambda = 0.5 AND rowid IN (3, 4, 5, 6));")" \
	"2|3"
eq "a NULL in rowid IN (...) matches no row, not rowid 0" \
	"$(session "INSERT INTO docs(rowid, body) VALUES (0, 'cat cat cat cat cat cat');
	SELECT ifnull(group_concat(rowid), '') FROM (SELECT rowid FROM docs_mmr WHERE text MATCH 'cat' AND k = 2 AND rowid IN (SELECT NULL UNION ALL SELECT 5));")" \
	"5"
eq "rowid IN (...) compares as SQLite compares with an integer primary key" \
	"$(session "INSERT INTO docs(rowid, body) VALUES (0, 'cat cat cat cat cat cat');
	SELECT ifnull(group_concat(rowid), '') FROM (SELECT rowid FROM docs_mmr WHERE text MATCH 'cat' AND k = 7 AND rowid IN ('2', 3.0, 'abc', 4.5, x'05', 1e19) ORDER BY rowid);")" \
	"$(session "CREATE TABLE p(id INTEGER PRIMARY KEY); INSERT INTO p VALUES (0), (1), (2), (3), (4), (5), (6);
	SELECT ifnull(group_concat(id), '') FROM (SELECT id FROM p WHERE id IN ('2', 3.0, 'abc', 4.5, x'05', 1e19) ORDER BY id);")"
eq "an empty rowid IN (...) returns no rows" \
	"$(session "SELECT count(*) FROM docs_mmr WHERE text MATCH 'cat' AND k = 2 AND rowid IN (SELECT rowid FROM docs WHERE 0);")" \
	"0"
eq "a second rowid IN (...) is refused" \
	"$(session "SELECT count(*) FROM docs_mmr WHERE text MATCH 'cat' AND k = 2 AND rowid IN (1, 2, 3) AND rowid IN (2, 3, 4);" | grep -c 'only one rowid IN')" \
	"1"

echo "# filter: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
