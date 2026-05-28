#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct astraldb_t astraldb_t;
typedef struct astraldb_stmt_t astraldb_stmt_t;

int astraldb_open(const char *path, astraldb_t **out_db);
void astraldb_close(astraldb_t *db);

int astraldb_exec(astraldb_t *db, const char *sql);
const char *astraldb_last_error(const astraldb_t *db);

int astraldb_prepare(astraldb_t *db, const char *sql, astraldb_stmt_t **out_stmt);
int astraldb_stmt_step(astraldb_stmt_t *stmt);
int astraldb_stmt_column_count(const astraldb_stmt_t *stmt);
const char *astraldb_stmt_column_name(const astraldb_stmt_t *stmt, int col);
const char *astraldb_stmt_column_text(const astraldb_stmt_t *stmt, int col);
void astraldb_stmt_finalize(astraldb_stmt_t *stmt);

#ifdef __cplusplus
}
#endif
