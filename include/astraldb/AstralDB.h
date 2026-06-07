#ifndef ASTRALDB_ASTRALDB_H
#define ASTRALDB_ASTRALDB_H

#ifdef __cplusplus
extern "C" {
#endif

/** Product release version string (matches CMake project VERSION). */
#define ASTRALDB_VERSION "2.0"

typedef struct AstralDb AstralDb;
typedef struct AstralDbStmt AstralDbStmt;

/** Return 0 to continue row iteration, non-zero to stop. */
typedef int (*AstralDbRowCallback)(void *Ctx, int Row, int Col, const char *ColName, const char *Value);

const char *AstralDbVersion(void);

/** Open or create a database at \a Path. Returns 0 on success. */
int AstralDbOpen(const char *Path, AstralDb **OutDb);

void AstralDbClose(AstralDb *Db);

/** Execute DDL/DML/procedure-definition SQL. Returns 0 on success. */
int AstralDbExec(AstralDb *Db, const char *Sql);

/** \c CALL name; shorthand. Returns 0 on success. */
int AstralDbCall(AstralDb *Db, const char *ProcedureName);

const char *AstralDbLastError(const AstralDb *Db);

/**
 * Run a \c SELECT (or other row-producing statement) and invoke \a Callback per cell.
 * When \a Callback is null, behaves like AstralDbExec.
 */
int AstralDbExecQuery(AstralDb *Db, const char *Sql, AstralDbRowCallback Callback, void *Ctx);

/** Prepare a \c SELECT with \c ? placeholders (1-based bind indices). */
int AstralDbPrepare(AstralDb *Db, const char *Sql, AstralDbStmt **OutStmt);

int AstralDbBindInt64(AstralDbStmt *Stmt, int Index, long long Value);
int AstralDbBindText(AstralDbStmt *Stmt, int Index, const char *Value);

/** Returns 1 if a row is available, 0 at end, -1 on error. First step runs the query. */
int AstralDbStmtStep(AstralDbStmt *Stmt);

void AstralDbStmtReset(AstralDbStmt *Stmt);

int AstralDbStmtColumnCount(const AstralDbStmt *Stmt);
const char *AstralDbStmtColumnName(const AstralDbStmt *Stmt, int Col);
const char *AstralDbStmtColumnText(const AstralDbStmt *Stmt, int Col);

void AstralDbStmtFinalize(AstralDbStmt *Stmt);

#ifdef __cplusplus
}
#endif

#endif
