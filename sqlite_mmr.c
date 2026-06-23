/*
** sqlite_mmr.c - Jaccard MMR virtual table for diversity reranking.
**
** Provides Jaccard-similarity-based Maximal Marginal Relevance (MMR)
** reranking.  Wraps any MATCH-capable table (FTS5, etc.) and reranks
** results by balancing relevance against textual diversity.
**
** CREATE VIRTUAL TABLE t_mmr USING mmr(
**     source_table,
**     text_expr,             -- SQL expression for text to tokenize
**     rank_expr              -- SQL expression for relevance scoring
** );
**
** SELECT rowid, rank, text FROM t_mmr
**   WHERE text MATCH :q AND k = :k AND mmr_lambda = :lambda;
**
** BSD 3-Clause License. See LICENSE for details.
*/

#include "sqlite_mmr.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#else
#include "sqlite3.h"
#endif

/* ---- Error helper ---------------------------------------------------- */

static void vtab_set_error(sqlite3_vtab *pVTab, const char *zFormat, ...) {
  va_list args;
  sqlite3_free(pVTab->zErrMsg);
  va_start(args, zFormat);
  pVTab->zErrMsg = sqlite3_vmprintf(zFormat, args);
  va_end(args);
}

/* ---- Token set -------------------------------------------------------- */

typedef struct mmr_tokenset {
  char **tokens;
  int n;
  int cap;
} mmr_tokenset;

static void mmr_tokenset_init(mmr_tokenset *ts) {
  ts->tokens = NULL;
  ts->n = 0;
  ts->cap = 0;
}

static void mmr_tokenset_free(mmr_tokenset *ts) {
  for (int i = 0; i < ts->n; i++)
    sqlite3_free(ts->tokens[i]);
  sqlite3_free(ts->tokens);
  ts->tokens = NULL;
  ts->n = ts->cap = 0;
}

static int mmr_tokenset_push(mmr_tokenset *ts, const char *tok, int len) {
  if (ts->n >= ts->cap) {
    int newcap = ts->cap ? ts->cap * 2 : 16;
    char **p = sqlite3_realloc(ts->tokens, newcap * sizeof(char *));
    if (!p)
      return SQLITE_NOMEM;
    ts->tokens = p;
    ts->cap = newcap;
  }
  char *s = sqlite3_malloc(len + 1);
  if (!s)
    return SQLITE_NOMEM;
  memcpy(s, tok, len);
  s[len] = '\0';
  ts->tokens[ts->n++] = s;
  return SQLITE_OK;
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(const char **)a, *(const char **)b);
}

/*
** Split pre-tokenized text on whitespace into a token set.
** Input is expected to be space-separated lowercase tokens
** (e.g. from match_tokens()).
*/
static int mmr_tokenset_split(mmr_tokenset *ts, const char *text) {
  if (!text)
    return SQLITE_OK;
  const char *p = text;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char *start = p;
    while (*p && *p != ' ') p++;
    int len = (int)(p - start);
    if (len == 0) continue;
    int rc = mmr_tokenset_push(ts, start, len);
    if (rc != SQLITE_OK)
      return rc;
  }
  return SQLITE_OK;
}

/*
** Sort and deduplicate a tokenset in place.
*/
static void mmr_tokenset_sort_dedup(mmr_tokenset *ts) {
  if (ts->n > 1)
    qsort(ts->tokens, ts->n, sizeof(char *), cmp_str);
  if (ts->n > 0) {
    int w = 1;
    for (int r = 1; r < ts->n; r++) {
      if (strcmp(ts->tokens[r], ts->tokens[w - 1]) != 0) {
        if (w != r)
          ts->tokens[w] = ts->tokens[r];
        w++;
      } else {
        sqlite3_free(ts->tokens[r]);
      }
    }
    ts->n = w;
  }
}

/*
** Jaccard similarity between two token sets.
** Sorts and deduplicates both inputs, then computes via sorted merge.
*/
static double mmr_jaccard(mmr_tokenset *a, mmr_tokenset *b) {
  mmr_tokenset_sort_dedup(a);
  mmr_tokenset_sort_dedup(b);
  if (a->n == 0 && b->n == 0)
    return 0.0;
  int i = 0, j = 0, inter = 0;
  while (i < a->n && j < b->n) {
    int c = strcmp(a->tokens[i], b->tokens[j]);
    if (c == 0) {
      inter++;
      i++;
      j++;
    } else if (c < 0) {
      i++;
    } else {
      j++;
    }
  }
  int uni = a->n + b->n - inter;
  return uni > 0 ? (double)inter / (double)uni : 0.0;
}

/* ---- Row buffer ------------------------------------------------------- */

typedef struct mmr_row {
  sqlite3_int64 rowid;
  double rank_value;
  char *text;
  mmr_tokenset tokens;
  int selected;
} mmr_row;

static void mmr_row_free(mmr_row *r) {
  sqlite3_free(r->text);
  mmr_tokenset_free(&r->tokens);
}

/* ---- Virtual table ---------------------------------------------------- */

typedef struct mmr_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
  char *source_table;
  char *text_expr;
  char *rank_expr;
} mmr_vtab;

typedef struct mmr_cursor {
  sqlite3_vtab_cursor base;
  mmr_row *rows;
  int n_rows;
  int current;
} mmr_cursor;

/* ---- xCreate / xConnect ---------------------------------------------- */

static int mmrInit(sqlite3 *db, void *pAux, int argc,
                    const char *const *argv, sqlite3_vtab **ppVtab,
                    char **pzErr) {
  (void)pAux;
  if (argc < 6) {
    *pzErr = sqlite3_mprintf(
        "mmr: expected (source_table, text_expr, rank_expr)");
    return SQLITE_ERROR;
  }

  const char *source_table = argv[3];
  const char *text_expr = argv[4];
  const char *rank_expr = argv[5];

  /*
  ** Schema:
  **   rank       REAL  HIDDEN  (col 0)  relevance score from source
  **   text       TEXT          (col 1)  text expression result
  **   k          INT   HIDDEN  (col 2)  result count
  **   mmr_lambda REAL  HIDDEN  (col 3)  1.0=relevance, 0.5=balanced
  */
  int rc = sqlite3_declare_vtab(db,
    "CREATE TABLE x("
    "rank REAL HIDDEN, "
    "text TEXT, "
    "k INT HIDDEN, "
    "mmr_lambda REAL HIDDEN)");
  if (rc != SQLITE_OK)
    return rc;

  mmr_vtab *pNew = sqlite3_malloc(sizeof(*pNew));
  if (!pNew)
    return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(*pNew));
  pNew->db = db;
  pNew->source_table = sqlite3_mprintf("%s", source_table);
  pNew->text_expr = sqlite3_mprintf("%s", text_expr);
  pNew->rank_expr = sqlite3_mprintf("%s", rank_expr);

  if (!pNew->source_table || !pNew->text_expr || !pNew->rank_expr) {
    sqlite3_free(pNew->source_table);
    sqlite3_free(pNew->text_expr);
    sqlite3_free(pNew->rank_expr);
    sqlite3_free(pNew);
    return SQLITE_NOMEM;
  }

  *ppVtab = &pNew->base;
  return SQLITE_OK;
}

static int mmrDestroy(sqlite3_vtab *pVtab) {
  mmr_vtab *p = (mmr_vtab *)pVtab;
  sqlite3_free(p->source_table);
  sqlite3_free(p->text_expr);
  sqlite3_free(p->rank_expr);
  sqlite3_free(p);
  return SQLITE_OK;
}

/* ---- xBestIndex ------------------------------------------------------ */

/*
** idxStr encoding: 4 bytes per constraint.
**   byte 0: kind character
**   bytes 1-3: filler ('_')
**
** Kind characters:
**   'M' = MATCH query
**   'K' = k (result count)
**   'L' = mmr_lambda (diversity parameter)
**
** MATCH + k are required.  mmr_lambda is optional (default 1.0).
*/

#define MMR_IDXSTR_KIND_MATCH  'M'
#define MMR_IDXSTR_KIND_K      'K'
#define MMR_IDXSTR_KIND_LAMBDA 'L'

static int mmrBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo) {
  (void)pVtab;
  int iMatch = -1, iK = -1, iLambda = -1;

  for (int i = 0; i < pInfo->nConstraint; i++) {
    if (!pInfo->aConstraint[i].usable)
      continue;
    int col = pInfo->aConstraint[i].iColumn;
    int op = pInfo->aConstraint[i].op;

    if (op == SQLITE_INDEX_CONSTRAINT_MATCH && (col == 1 || col == -1)) {
      iMatch = i;
    } else if (col == 2 && op == SQLITE_INDEX_CONSTRAINT_EQ) {
      iK = i;
    } else if (col == 3 && op == SQLITE_INDEX_CONSTRAINT_EQ) {
      iLambda = i;
    }
  }

  if (iMatch < 0 || iK < 0) {
    pInfo->estimatedCost = 1e12;
    return SQLITE_OK;
  }

  /* Build idxStr: 4 bytes per constraint, '_' padding */
  sqlite3_str *idxStr = sqlite3_str_new(NULL);
  int argvIndex = 1;

  sqlite3_str_appendchar(idxStr, 1, MMR_IDXSTR_KIND_MATCH);
  sqlite3_str_appendchar(idxStr, 3, '_');
  pInfo->aConstraintUsage[iMatch].argvIndex = argvIndex++;
  pInfo->aConstraintUsage[iMatch].omit = 1;

  sqlite3_str_appendchar(idxStr, 1, MMR_IDXSTR_KIND_K);
  sqlite3_str_appendchar(idxStr, 3, '_');
  pInfo->aConstraintUsage[iK].argvIndex = argvIndex++;
  pInfo->aConstraintUsage[iK].omit = 1;

  if (iLambda >= 0) {
    sqlite3_str_appendchar(idxStr, 1, MMR_IDXSTR_KIND_LAMBDA);
    sqlite3_str_appendchar(idxStr, 3, '_');
    pInfo->aConstraintUsage[iLambda].argvIndex = argvIndex++;
    pInfo->aConstraintUsage[iLambda].omit = 1;
  }

  pInfo->idxStr = sqlite3_str_finish(idxStr);
  pInfo->needToFreeIdxStr = 1;
  pInfo->estimatedCost = 100.0;
  pInfo->estimatedRows = 10;
  return SQLITE_OK;
}

/* ---- xOpen / xClose -------------------------------------------------- */

static int mmrOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCur) {
  (void)pVtab;
  mmr_cursor *pCur = sqlite3_malloc(sizeof(*pCur));
  if (!pCur)
    return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCur = &pCur->base;
  return SQLITE_OK;
}

static int mmrClose(sqlite3_vtab_cursor *pCur) {
  mmr_cursor *p = (mmr_cursor *)pCur;
  for (int i = 0; i < p->n_rows; i++)
    mmr_row_free(&p->rows[i]);
  sqlite3_free(p->rows);
  sqlite3_free(p);
  return SQLITE_OK;
}

/* ---- xFilter (main query logic) -------------------------------------- */

static int mmrFilter(sqlite3_vtab_cursor *pCur, int idxNum,
                      const char *idxStr, int argc, sqlite3_value **argv) {
  (void)idxNum;
  mmr_cursor *cur = (mmr_cursor *)pCur;
  mmr_vtab *vtab = (mmr_vtab *)pCur->pVtab;

  /* Free any previous results */
  for (int i = 0; i < cur->n_rows; i++)
    mmr_row_free(&cur->rows[i]);
  sqlite3_free(cur->rows);
  cur->rows = NULL;
  cur->n_rows = 0;
  cur->current = 0;

  /* Parse constraints from idxStr + argv (4 bytes per constraint) */
  const char *match_text = NULL;
  int k = 10;
  double mmr_lambda = 1.0;

  for (int i = 0; i < argc; i++) {
    char kind = idxStr[i * 4];
    switch (kind) {
    case MMR_IDXSTR_KIND_MATCH:
      match_text = (const char *)sqlite3_value_text(argv[i]);
      break;
    case MMR_IDXSTR_KIND_K:
      k = sqlite3_value_int(argv[i]);
      if (k < 1)
        k = 1;
      break;
    case MMR_IDXSTR_KIND_LAMBDA:
      mmr_lambda = sqlite3_value_double(argv[i]);
      break;
    }
  }

  if (!match_text || !match_text[0])
    return SQLITE_OK;

  /* Overfetch candidates for MMR reranking */
#define MMR_OVERFETCH_FACTOR 5
  int fetch_limit = (mmr_lambda < 1.0) ? k * MMR_OVERFETCH_FACTOR : k;
  if (fetch_limit < k)
    fetch_limit = k;

  /* Prepare internal query against source table */
  char *sql = sqlite3_mprintf(
      "SELECT rowid, %s, %s "
      "FROM \"%w\" WHERE \"%w\" MATCH ?1 ORDER BY %s LIMIT %d",
      vtab->rank_expr, vtab->text_expr,
      vtab->source_table, vtab->source_table,
      vtab->rank_expr, fetch_limit);
  if (!sql)
    return SQLITE_NOMEM;

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(vtab->db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    vtab_set_error(&vtab->base, "%s", sqlite3_errmsg(vtab->db));
    return rc;
  }

  sqlite3_bind_text(stmt, 1, match_text, -1, SQLITE_TRANSIENT);

  /* Fetch all candidates */
  int cap = fetch_limit > 0 ? fetch_limit : 64;
  mmr_row *rows = sqlite3_malloc(cap * sizeof(mmr_row));
  if (!rows) {
    sqlite3_finalize(stmt);
    return SQLITE_NOMEM;
  }
  int n = 0;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (n >= cap) {
      cap *= 2;
      mmr_row *p = sqlite3_realloc(rows, cap * sizeof(mmr_row));
      if (!p) {
        for (int i = 0; i < n; i++)
          mmr_row_free(&rows[i]);
        sqlite3_free(rows);
        sqlite3_finalize(stmt);
        return SQLITE_NOMEM;
      }
      rows = p;
    }
    rows[n].rowid = sqlite3_column_int64(stmt, 0);
    rows[n].rank_value = sqlite3_column_double(stmt, 1);
    const char *txt = (const char *)sqlite3_column_text(stmt, 2);
    rows[n].text = sqlite3_mprintf("%s", txt ? txt : "");
    mmr_tokenset_init(&rows[n].tokens);
    rows[n].selected = 0;
    if (!rows[n].text) {
      for (int i = 0; i < n; i++)
        mmr_row_free(&rows[i]);
      sqlite3_free(rows);
      sqlite3_finalize(stmt);
      return SQLITE_NOMEM;
    }
    n++;
  }
  sqlite3_finalize(stmt);

  if (n == 0) {
    sqlite3_free(rows);
    return SQLITE_OK;
  }

  /* ---- MMR reranking --------------------------------------------------- */
  if (mmr_lambda < 1.0 && n > 1) {
    /* Tokenize all text */
    for (int i = 0; i < n; i++) {
      rc = mmr_tokenset_split(&rows[i].tokens, rows[i].text);
      if (rc != SQLITE_OK) {
        for (int j = 0; j < n; j++)
          mmr_row_free(&rows[j]);
        sqlite3_free(rows);
        return rc;
      }
    }

    /*
    ** Normalize ranks to relevance [0, 1].
    ** Assumes lower rank = better (ORDER BY rank ASC).
    ** Uses min-max normalization: best rank maps to 1.0, worst to 0.0.
    */
    double min_rank = rows[0].rank_value;
    double max_rank = rows[0].rank_value;
    for (int i = 1; i < n; i++) {
      if (rows[i].rank_value < min_rank)
        min_rank = rows[i].rank_value;
      if (rows[i].rank_value > max_rank)
        max_rank = rows[i].rank_value;
    }
    double range = max_rank - min_rank;

    double *relevance = sqlite3_malloc(n * sizeof(double));
    if (!relevance) {
      for (int i = 0; i < n; i++)
        mmr_row_free(&rows[i]);
      sqlite3_free(rows);
      return SQLITE_NOMEM;
    }
    for (int i = 0; i < n; i++) {
      relevance[i] =
          (range > 0.0) ? (max_rank - rows[i].rank_value) / range : 1.0;
    }

    /* Greedy MMR selection */
    int actual_k = k < n ? k : n;
    int *order = sqlite3_malloc(actual_k * sizeof(int));
    if (!order) {
      sqlite3_free(relevance);
      for (int i = 0; i < n; i++)
        mmr_row_free(&rows[i]);
      sqlite3_free(rows);
      return SQLITE_NOMEM;
    }

    int selected_count = 0;
    for (int step = 0; step < actual_k; step++) {
      int best_idx = -1;
      double best_score = -1e18;

      for (int i = 0; i < n; i++) {
        if (rows[i].selected)
          continue;

        /* max Jaccard similarity to any already-selected row */
        double max_sim = 0.0;
        for (int s = 0; s < selected_count; s++) {
          double sim =
              mmr_jaccard(&rows[i].tokens, &rows[order[s]].tokens);
          if (sim > max_sim)
            max_sim = sim;
        }

        double mmr_score =
            mmr_lambda * relevance[i] - (1.0 - mmr_lambda) * max_sim;

        if (mmr_score > best_score) {
          best_score = mmr_score;
          best_idx = i;
        }
      }

      if (best_idx < 0)
        break;
      rows[best_idx].selected = 1;
      order[selected_count++] = best_idx;
    }

    /* Compact: reorder rows[] to selected order */
    mmr_row *reordered = sqlite3_malloc(selected_count * sizeof(mmr_row));
    if (!reordered) {
      sqlite3_free(order);
      sqlite3_free(relevance);
      for (int i = 0; i < n; i++)
        mmr_row_free(&rows[i]);
      sqlite3_free(rows);
      return SQLITE_NOMEM;
    }
    for (int i = 0; i < selected_count; i++)
      reordered[i] = rows[order[i]];

    /* Free unselected rows */
    for (int i = 0; i < n; i++) {
      if (!rows[i].selected)
        mmr_row_free(&rows[i]);
    }
    sqlite3_free(rows);
    sqlite3_free(order);
    sqlite3_free(relevance);
    rows = reordered;
    n = selected_count;
  } else {
    /* No MMR: just take top k by rank (already sorted by source) */
    if (n > k) {
      for (int i = k; i < n; i++)
        mmr_row_free(&rows[i]);
      n = k;
    }
  }

  cur->rows = rows;
  cur->n_rows = n;
  cur->current = 0;
  return SQLITE_OK;
}

/* ---- Cursor navigation ----------------------------------------------- */

static int mmrEof(sqlite3_vtab_cursor *pCur) {
  return ((mmr_cursor *)pCur)->current >= ((mmr_cursor *)pCur)->n_rows;
}

static int mmrNext(sqlite3_vtab_cursor *pCur) {
  ((mmr_cursor *)pCur)->current++;
  return SQLITE_OK;
}

static int mmrRowid(sqlite3_vtab_cursor *pCur, sqlite3_int64 *pRowid) {
  *pRowid = ((mmr_cursor *)pCur)->rows[((mmr_cursor *)pCur)->current].rowid;
  return SQLITE_OK;
}

static int mmrColumn(sqlite3_vtab_cursor *pCur, sqlite3_context *ctx,
                      int col) {
  mmr_row *row =
      &((mmr_cursor *)pCur)->rows[((mmr_cursor *)pCur)->current];
  switch (col) {
  case 0: /* rank */
    sqlite3_result_double(ctx, row->rank_value);
    break;
  case 1: /* text */
    sqlite3_result_text(ctx, row->text, -1, SQLITE_TRANSIENT);
    break;
  case 2: /* k (input-only) */
  case 3: /* mmr_lambda (input-only) */
    sqlite3_result_null(ctx);
    break;
  }
  return SQLITE_OK;
}

/* ---- Module definition ----------------------------------------------- */

static sqlite3_module mmrModule = {
    /* iVersion    */ 0,
    /* xCreate     */ mmrInit,
    /* xConnect    */ mmrInit,
    /* xBestIndex  */ mmrBestIndex,
    /* xDisconnect */ mmrDestroy,
    /* xDestroy    */ mmrDestroy,
    /* xOpen       */ mmrOpen,
    /* xClose      */ mmrClose,
    /* xFilter     */ mmrFilter,
    /* xNext       */ mmrNext,
    /* xEof        */ mmrEof,
    /* xColumn     */ mmrColumn,
    /* xRowid      */ mmrRowid,
    /* xUpdate     */ 0,
    /* xBegin      */ 0,
    /* xSync       */ 0,
    /* xCommit     */ 0,
    /* xRollback   */ 0,
    /* xFindMethod */ 0,
    /* xRename     */ 0,
    /* xSavepoint  */ 0,
    /* xRelease    */ 0,
    /* xRollbackTo */ 0,
    /* xShadowName */ 0,
#if SQLITE_VERSION_NUMBER >= 3044000
    /* xIntegrity  */ 0,
#endif
};

/* ---- jaccard() scalar function --------------------------------------- */
/*
** jaccard(a, b) — Jaccard similarity on two text strings.
** Tokenizes both inputs (lowercase, split), sorts and deduplicates
** internally, then computes |intersection| / |union|.
*/
static void sql_jaccard(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  const char *a = (const char *)sqlite3_value_text(argv[0]);
  const char *b = (const char *)sqlite3_value_text(argv[1]);
  mmr_tokenset ta, tb;
  mmr_tokenset_init(&ta);
  mmr_tokenset_init(&tb);
  mmr_tokenset_split(&ta, a);
  mmr_tokenset_split(&tb, b);
  sqlite3_result_double(ctx, mmr_jaccard(&ta, &tb));
  mmr_tokenset_free(&ta);
  mmr_tokenset_free(&tb);
}

/* ---- Entry point ----------------------------------------------------- */

SQLITE_MMR_API int sqlite3_mmr_init(sqlite3 *db, char **pzErrMsg,
                                       const sqlite3_api_routines *pApi) {
  (void)pzErrMsg;
#ifndef SQLITE_CORE
  SQLITE_EXTENSION_INIT2(pApi);
#endif

  int rc;

  /* Virtual table module */
  rc = sqlite3_create_module_v2(db, "mmr", &mmrModule, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  /* Scalar functions */
  rc = sqlite3_create_function(db, "jaccard", 2, SQLITE_UTF8, 0,
                               sql_jaccard, 0, 0);

  return SQLITE_OK;
}
