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
eq "a rowid IN (...) reading another table keeps, for each of its rows, the best ranked rows inside the set" \
	"$(session "CREATE TABLE sel(lo INTEGER); INSERT INTO sel VALUES (2), (4);
	SELECT ifnull(group_concat(x), '') FROM (SELECT s.lo || '>' || m.rowid AS x FROM docs_mmr m, sel s WHERE m.text MATCH 'cat' AND m.k = 2 AND m.rowid IN (SELECT rowid FROM docs WHERE rowid > s.lo) ORDER BY s.lo, m.rowid);")" \
	"2>3,2>4,4>5,4>6"
eq "a second rowid IN (...) is refused" \
	"$(session "SELECT count(*) FROM docs_mmr WHERE text MATCH 'cat' AND k = 2 AND rowid IN (1, 2, 3) AND rowid IN (2, 3, 4);" | grep -c 'only one rowid IN')" \
	"1"

# MATCH and k are required arguments (https://www.sqlite.org/vtab.html,
# "Enforcing Required Parameters On Table-Valued Functions"). The first
# assertion's statistics make reading docs_mmr before q the cheaper order on
# paper, an order in which neither argument has its value yet.
eq "a MATCH taken from another table is supplied where the planner prefers reading docs_mmr first" \
	"$(session "CREATE TABLE q(term TEXT); INSERT INTO q VALUES ('cat');
	ANALYZE; UPDATE sqlite_stat1 SET stat = '100000000000' WHERE tbl = 'q'; ANALYZE sqlite_schema;
	SELECT count(*) FROM docs_mmr m, q WHERE m.text MATCH q.term AND m.k = 2;")" \
	"2"
eq "a join that reads the MATCH's table after docs_mmr fails instead of returning no rows" \
	"$(session "CREATE TABLE q(term TEXT); INSERT INTO q VALUES ('cat');
	SELECT count(*) FROM docs_mmr m CROSS JOIN q WHERE m.text MATCH q.term AND m.k = 2;" | grep -c 'MATCH and k are required')" \
	"1"
eq "a join that reads k's table after docs_mmr fails instead of returning no rows" \
	"$(session "CREATE TABLE q(n INTEGER); INSERT INTO q VALUES (2);
	SELECT count(*) FROM docs_mmr m CROSS JOIN q WHERE m.text MATCH 'cat' AND m.k = q.n;" | grep -c 'MATCH and k are required')" \
	"1"
eq "a query without MATCH fails" \
	"$(session "SELECT count(*) FROM docs_mmr WHERE k = 2;" | grep -c 'MATCH and k are required')" \
	"1"
eq "a query without k fails" \
	"$(session "SELECT count(*) FROM docs_mmr WHERE text MATCH 'cat';" | grep -c 'MATCH and k are required')" \
	"1"

# Two columns, so bm25's column weights change the order: rowid 1 has "cat" once
# in its title, rowid 2 three times in its body, every row four tokens long, and
# the "bird" rows keep "cat"'s IDF positive. Unweighted, rowid 2 ranks first;
# with the title weighted 10, rowid 1 does.
PETS="CREATE VIRTUAL TABLE pets USING fts5(title, body);
INSERT INTO pets(rowid, title, body) VALUES
  (1, 'cat', 'dog dog dog'), (2, 'dog', 'cat cat cat'),
  (3, 'bird', 'dog dog dog'), (4, 'bird', 'dog dog dog'), (5, 'bird', 'dog dog dog');
CREATE VIRTUAL TABLE pets_mmr USING mmr(pets, body, rank);"
pets() { session "$PETS $1"; }
pet_ids() { pets "SELECT ifnull(group_concat(rowid), '') FROM ($1);"; }

eq "fixture: unweighted, FTS5 ranks rowid 2 before rowid 1" \
	"$(pet_ids "SELECT rowid FROM pets WHERE pets MATCH 'cat' ORDER BY rank")" "2,1"
eq "rank MATCH reaches the source and reorders the candidates" \
	"$(pet_ids "SELECT rowid FROM pets_mmr WHERE text MATCH 'cat' AND k = 2 AND rank MATCH 'bm25(10.0, 1.0)'")" \
	"$(pet_ids "SELECT rowid FROM pets WHERE pets MATCH 'cat' AND rank MATCH 'bm25(10.0, 1.0)' ORDER BY rank")"
eq "rank MATCH gives the order the weights call for" \
	"$(pet_ids "SELECT rowid FROM pets_mmr WHERE text MATCH 'cat' AND k = 2 AND rank MATCH 'bm25(10.0, 1.0)'")" "1,2"
eq "the rank column under rank MATCH is the source's rank under it" \
	"$(pets "SELECT group_concat(rowid || ':' || printf('%.12f', rank)) FROM (SELECT rowid, rank FROM pets_mmr WHERE text MATCH 'cat' AND k = 2 AND rank MATCH 'bm25(10.0, 1.0)');")" \
	"$(pets "SELECT group_concat(rowid || ':' || printf('%.12f', rank)) FROM (SELECT rowid, rank FROM pets WHERE pets MATCH 'cat' AND rank MATCH 'bm25(10.0, 1.0)' ORDER BY rank);")"
eq "rank MATCH and rowid IN (...) together" \
	"$(pet_ids "SELECT rowid FROM pets_mmr WHERE text MATCH 'cat' AND k = 2 AND rank MATCH 'bm25(10.0, 1.0)' AND rowid IN (2, 1)")" "1,2"
# FTS5 looks the rank function up only when the rank is used, so the reference
# orders by it, as mmr's source query does.
eq "a rank function the source rejects fails the query with the source's error" \
	"$(pets "SELECT count(*) FROM pets_mmr WHERE text MATCH 'cat' AND k = 2 AND rank MATCH 'nosuchfn()';")" \
	"$(pets "SELECT count(*) FROM (SELECT rowid FROM pets WHERE pets MATCH 'cat' AND rank MATCH 'nosuchfn()' ORDER BY rank);")"

echo "# filter: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
