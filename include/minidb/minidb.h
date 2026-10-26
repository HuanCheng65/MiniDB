/*
 * MiniDB embedded C API (design doc 2.3).
 *
 * Typical use:
 *
 *   mydb_db* db;
 *   if (mydb_open("data/tpch_db", NULL, &db) != MYDB_OK) { ... }
 *   if (mydb_exec(db, "CREATE TABLE t (a INTEGER NOT NULL);", NULL) != MYDB_OK) { ... }
 *   mydb_stmt* stmt;
 *   mydb_prepare(db, "SELECT * FROM t;", &stmt);
 *   while (mydb_step(stmt) == MYDB_ROW) { ... mydb_column_text(stmt, 0) ... }
 *   mydb_finalize(stmt);
 *   mydb_close(db);
 */
#ifndef MINIDB_MINIDB_H
#define MINIDB_MINIDB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mydb_db mydb_db;
typedef struct mydb_stmt mydb_stmt;

typedef struct mydb_config {
  /* Number of 8 KB buffer frames; 0 means the default (1024). */
  int buffer_pool_frames;
  /* "clock" (default) or NULL. */
  const char* replacer;
  /* "debug", "info", "warn", "error" or "off"; NULL means "info". */
  const char* log_level;
  /* Optional log file; NULL means stderr only. */
  const char* log_file;
} mydb_config;

enum mydb_rc {
  MYDB_OK = 0,
  MYDB_ERROR = 1,
  MYDB_ROW = 100,
  MYDB_DONE = 101
};

enum mydb_type {
  MYDB_TYPE_NULL = 0,
  MYDB_TYPE_INT = 1,
  MYDB_TYPE_DOUBLE = 2,
  MYDB_TYPE_TEXT = 3,
  MYDB_TYPE_DATE = 4,
  MYDB_TYPE_DECIMAL = 5
};

/* Opens (creating when necessary) the database folder. Every relation is
 * stored as <name>.dat plus <name>.fsm inside that folder. Returns MYDB_OK or
 * MYDB_ERROR; on failure *out_db is NULL and mydb_last_error() has details. */
int mydb_open(const char* path, const mydb_config* config, mydb_db** out_db);

/* Flushes all dirty pages and marks the database cleanly shut down. */
int mydb_close(mydb_db* db);

/* Executes one or more ';'-separated statements. On failure returns
 * MYDB_ERROR and, when errmsg is non-NULL, stores a malloc'ed message that
 * the caller frees with mydb_free(). */
int mydb_exec(mydb_db* db, const char* sql, char** errmsg);

/* Parses and executes the statement(s), keeping the rows of the last result
 * set. The statement is executed immediately, so DDL takes effect here. */
int mydb_prepare(mydb_db* db, const char* sql, mydb_stmt** out_stmt);

/* Returns MYDB_ROW for each result row, then MYDB_DONE. Statements without a
 * result set return MYDB_DONE on the first call. */
int mydb_step(mydb_stmt* stmt);

int mydb_column_count(mydb_stmt* stmt);
const char* mydb_column_name(mydb_stmt* stmt, int column);
int mydb_column_type(mydb_stmt* stmt, int column);
long long mydb_column_int64(mydb_stmt* stmt, int column);
double mydb_column_double(mydb_stmt* stmt, int column);
/* Formatted text of the current row's column (dates and DECIMALs included).
 * The pointer is owned by the statement and valid until the next step. */
const char* mydb_column_text(mydb_stmt* stmt, int column);

void mydb_finalize(mydb_stmt* stmt);

/* Rows inserted/updated by the most recent mydb_exec / mydb_prepare. */
int mydb_changes(mydb_db* db);

/* Human-readable message of the last executed statement (may be NULL). */
const char* mydb_message(mydb_db* db);

/* Error text of the last failing operation on this connection. */
const char* mydb_errmsg(mydb_db* db);

/* Error text of the last failing mydb_open (process/thread-local). */
const char* mydb_last_error(void);

void mydb_free(void* pointer);
const char* mydb_version(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MINIDB_MINIDB_H */
