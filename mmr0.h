/*
** mmr0.h — MMR diversity reranking, Jaccard similarity, FTS5 token extraction.
**
** BSD 3-Clause License. See LICENSE for details.
*/
#ifndef SQLITE_MMR_H
#define SQLITE_MMR_H

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#ifdef SQLITE_MMR_STATIC
  #define SQLITE_MMR_API
#else
  #ifdef _WIN32
    #define SQLITE_MMR_API __declspec(dllexport)
  #else
    #define SQLITE_MMR_API
  #endif
#endif

#define SQLITE_MMR_VERSION "v2.0.0"

#ifdef __cplusplus
extern "C" {
#endif

SQLITE_MMR_API int sqlite3_mmr_init(sqlite3 *db, char **pzErrMsg,
                                     const sqlite3_api_routines *pApi);

#ifdef __cplusplus
}
#endif

#endif /* ifndef SQLITE_MMR_H */
