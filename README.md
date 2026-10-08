# `sqlite-mmr`

A small SQLite extension for Maximal Marginal Relevance (MMR) reranking and
Jaccard similarity.

- `mmr`: a virtual table that wraps any MATCH-capable source table (FTS5, etc.)
  and reranks its results to balance relevance against textual diversity
- `jaccard(a, b)`: scalar Jaccard similarity over two whitespace-tokenized strings
- Written in pure C, no dependencies beyond SQLite
- Single file (`mmr0.c`), compiles to a small shared library

For an FTS5 source, [`sqlite-fts5x`](https://github.com/MayCXC/sqlite-fts5x)'s `match_tokens`
(and `tokenize`, `snippet_text`) give already-tokenized text that makes a clean text expression;
any MATCH-capable table works too.

## The problem

Search engines rank results by relevance, which tends to cluster topically
similar documents at the top. If your top 10 results for "memory" are all
slight variations of the same page, you are missing useful results further down
the list.

## The solution

`mmr` sits on top of any MATCH-capable source table. It overfetches candidates,
computes pairwise Jaccard similarity between their token sets, and runs a greedy
MMR selection loop to trade relevance off against diversity.

`mmr` evaluates a **text expression** and a **rank expression** against the
source table, then reranks on the text. The text
expression is split on whitespace into a token set, so it should yield
already-tokenized text. For an FTS5 source,
[`sqlite-fts5x`](https://github.com/MayCXC/sqlite-fts5x)'s `match_tokens(fts)`
reads the matched tokens straight from the inverted index (no content
decompression), which makes it the natural text expression; a plain column or
`snippet()` also works, but carries punctuation into the token set.

## Sample usage

```sql
.load ./fts5x    -- match_tokens, from sqlite-fts5x
.load ./mmr0     -- mmr, jaccard

CREATE VIRTUAL TABLE docs USING fts5x(body);

INSERT INTO docs(rowid, body) VALUES
  (1, 'how to take care of your cat and keep them healthy'),
  (2, 'best cat food brands for indoor cats and kittens'),
  (3, 'fun cat toys and games for cats to play with'),
  (4, 'how to take care of your dog and keep them healthy'),
  (5, 'popular cat breeds like siamese persian and maine coon'),
  (6, 'common cat health issues and veterinary care tips'),
  (7, 'grooming tips for long hair cats and short hair cats');

CREATE VIRTUAL TABLE docs_mmr USING mmr(
  docs,                  -- source table (must support MATCH)
  match_tokens(docs),    -- text expression (from sqlite-fts5x)
  rank                   -- relevance expression
);

-- lambda 1.0 = pure relevance
SELECT rowid, text FROM docs_mmr
  WHERE text MATCH 'cat' AND k = 5 AND mmr_lambda = 1.0;

-- lambda 0.5 = relevance balanced against diversity
SELECT rowid, text FROM docs_mmr
  WHERE text MATCH 'cat' AND k = 5 AND mmr_lambda = 0.5;

SELECT jaccard('hello world hello', 'world test');  -- 0.333 (dedupes internally)
```

## API

### Virtual table `mmr`

```sql
CREATE VIRTUAL TABLE <name> USING mmr(
    <source_table>,   -- a table that supports MATCH
    <text_expr>,      -- SQL expression, split on whitespace into the token set
    <rank_expr>       -- SQL expression for relevance (lower = better)
);
```

Each query runs, internally:

```sql
SELECT rowid, <rank_expr>, <text_expr>
  FROM <source_table> WHERE <source_table> MATCH ?1
  ORDER BY <rank_expr> LIMIT <overfetch>;
```

so any function used in `text_expr` or `rank_expr` must be callable on the
source table (e.g. `match_tokens` requires the source to be an `fts5x` table).
An error from this query, such as a MATCH the source rejects, fails the `mmr`
query with the source's message.

A `rowid IN (...)` constraint, over a list or a subquery, restricts the
candidates to those source rowids before the top `k` are taken, so the query
returns the best ranked rows inside the set rather than the members of the
overall top `k`:

```sql
SELECT rowid, text FROM docs_mmr
  WHERE text MATCH 'cat' AND k = 5
    AND rowid IN (SELECT id FROM docs_meta WHERE lang = 'en');
```

Each value compares with a rowid as SQLite compares it, with numeric affinity:
`'5'` and `5.0` match rowid 5, while `NULL`, `'abc'` and `5.5` match none.

The source query then runs without its `LIMIT`, and rows outside the set are
skipped until the overfetch count is reached. One `rowid IN` constraint is
allowed per query; it needs SQLite 3.38 or newer, which added
[`sqlite3_vtab_in()`](https://www.sqlite.org/c3ref/vtab_in.html).

A `rank MATCH` constraint is passed to the source query as its own
(`... MATCH ?1 AND rank MATCH ?2 ...`), so an FTS5 source ranks the candidates
by the function it names for that query, as FTS5 takes
[`rank MATCH`](https://www.sqlite.org/fts5.html#sorting_by_auxiliary_function_results).
It changes the order when `rank_expr` reads the source's `rank`, as the
`docs_mmr` above does:

```sql
SELECT rowid, rank, text FROM docs_mmr
  WHERE text MATCH 'cat' AND k = 5 AND rank MATCH 'bm25(10.0, 1.0)';
```

| Query column | Type | Hidden | Description |
|--------------|------|--------|-------------|
| `rank` | REAL | yes | Relevance score from `rank_expr`; `rank MATCH` sets the source's ranking function |
| `text` | TEXT | no | Result of `text_expr` |
| `k` | INT | yes | Number of results to return (required) |
| `mmr_lambda` | REAL | yes | `1.0` = pure relevance, `0.5` = balanced, `0.0` = pure diversity (default `1.0`) |

### Scalar function

| Function | Description |
|----------|-------------|
| `jaccard(a, b)` | Splits both strings on whitespace, sorts and deduplicates each, returns `\|intersection\| / \|union\|`. Case-sensitive: it expects already-lowercased tokens, as `match_tokens` produces. |

### How MMR works

When `mmr_lambda < 1.0`:

1. Overfetch `k * 5` candidates from the source table (top by `rank_expr`,
   inside the `rowid IN` set when there is one)
2. Split each `text_expr` result on whitespace into a token set
3. Normalize ranks to relevance scores in `[0, 1]` (min-max, best rank maps to `1.0`)
4. Greedy selection loop picks the candidate maximizing

   `score = lambda * relevance - (1 - lambda) * max_jaccard_to_selected`

5. Return the top `k` selected rows

When `mmr_lambda >= 1.0`: no reranking, returns the top `k` by rank directly.

## Building

```sh
make            # builds mmr0.so (or mmr0.dylib on macOS)
make test       # runs the smoke tests and tests/test_filter.sh
make install    # installs to /usr/local/lib and /usr/local/include
```

Requires a C compiler and SQLite development headers (`sqlite3ext.h`).

For static linking:

```sh
make static     # compiles mmr0.o with -DSQLITE_CORE
```

## License

BSD 3-Clause. See [LICENSE](LICENSE).
