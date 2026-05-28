#pragma once

#include <stddef.h>

#define ASTRALDB_VERSION "2.0"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstralDb AstralDb;
typedef struct AstralDbStmt AstralDbStmt;

/** Per-cell callback for \c AstralDbExecQuery (\c Row is 0-based). Return non-zero to stop. */
typedef int (*AstralDbRowCallback)(void *Ctx, int Row, int Col, const char *ColName, const char *ColText);

const char *AstralDbVersion(void);

int AstralDbOpen(const char *Path, AstralDb **OutDb);
void AstralDbClose(AstralDb *Db);

int AstralDbExec(AstralDb *Db, const char *Sql);
const char *AstralDbLastError(const AstralDb *Db);

/** Execute a registered procedure by name (equivalent to \c CALL Name;). */
int AstralDbCall(AstralDb *Db, const char *ProcedureName);

/**
 * Run a \c SELECT and invoke \c Callback once per column per row.
 * \c ColText may be null when the cell is SQL NULL.
 */
int AstralDbExecQuery(AstralDb *Db, const char *Sql, AstralDbRowCallback Callback, void *Ctx);

int AstralDbPrepare(AstralDb *Db, const char *Sql, AstralDbStmt **OutStmt);
/** Bind a \c ? placeholder (1-based index) before \c AstralDbPrepare or after \c AstralDbStmtReset. */
int AstralDbBindInt64(AstralDbStmt *Stmt, int Index, long long Value);
int AstralDbBindText(AstralDbStmt *Stmt, int Index, const char *Value);
int AstralDbStmtStep(AstralDbStmt *Stmt);
void AstralDbStmtReset(AstralDbStmt *Stmt);
int AstralDbStmtColumnCount(const AstralDbStmt *Stmt);
const char *AstralDbStmtColumnName(const AstralDbStmt *Stmt, int Col);
const char *AstralDbStmtColumnText(const AstralDbStmt *Stmt, int Col);
void AstralDbStmtFinalize(AstralDbStmt *Stmt);

#ifdef __cplusplus
}
#endif
