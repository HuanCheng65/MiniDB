# MiniDB

MiniDB is a self-contained embedded database library implemented in C++20 from
the design in [`doc/minidb-design.md`](doc/minidb-design.md). A database is a
folder in which every relation (including the system catalog relations) owns a
`<name>.dat` data file and a `<name>.fsm` free-space-map file. It exposes a
small SQLite-like C API. The current implementation covers the storage engine
and the DDL/DML subset needed to create and populate the TPC-H schema in
[`test/tpc-h/schema.sql`](test/tpc-h/schema.sql), and it ships with an
interactive shell, an embedded demo and integration tests.

No third-party libraries are required.

## What is implemented

| Layer | Design doc | Implementation |
| --- | --- | --- |
| Page / file format, CRC32C | ch. 4 | 8 KB pages, 32-byte header, 64-page (512 KB) extents, page-1 extent bitmap; CRC32C uses the SSE4.2 instruction when the CPU has it (runtime check, table fallback) |
| Disk manager | ch. 4 | read/write/fsync, page checksums, file header at page 0, files grown with `fallocate` (falls back to `ftruncate`) |
| Buffer pool | ch. 8 | configurable frames, CLOCK replacer, pin counts, page latches (`std::shared_mutex`), RAII read/write guards; one pool spans every data/FSM file, keyed by `(file_id, page_id)` |
| Segments / extents | ch. 5, 7 | segment header, inline extent list, overflow extent-list pages, per-extent page bitmaps; one external `<name>.fsm` file per relation |
| Heap storage | ch. 6 | slotted pages, variable-length records, in-page growth with compaction, one-hop forwarding pointers (`FORWARD`/`MOVED_IN`), page compaction |
| Catalog | ch. 7 | bootstrapped `sys_tables` / `sys_columns` heap relations, visible as read-only system tables |
| Transactions | ch. 9 (exp1 scope) | transaction IDs, auto-commit per statement; no WAL or recovery yet |
| SQL | ch. 10, 14 | `CREATE TABLE`, `DROP TABLE`, `INSERT`, `UPDATE`, `DELETE`, `SELECT` (with a `rid` pseudo-column), `COPY ... FROM` bulk load, `SHOW TABLES`, `DESCRIBE` |
| Introspection | ch. 13 | `PRAGMA` result sets for the buffer pool, extent bitmap, extent lists, FSM, page headers and slot directories; debug logs for misses, evictions, allocation and FSM changes |
| C API | ch. 2.3 | `mydb_open` / `mydb_exec` / `mydb_prepare` / `mydb_step` / `mydb_close` |

The parser understands the real TPC-H DDL, including `CHAR(n)`, `VARCHAR(n)`,
`DECIMAL(p,s)`, `DATE`, `NOT NULL`, inline and table-level `PRIMARY KEY`, and
ignores `FOREIGN KEY` constraints. Comments (`--` and `/* ... */`) are
supported.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Or use the presets:

```sh
cmake --preset dev
cmake --build --preset dev
```

Build products:

* `build/libminidb.so` and `build/libminidb_core.a` — the library
* `build/minidb_shell` — interactive shell / script runner
* `build/minidb_demo` — embedded API demo
* `build/minidb_tests` — integration tests (C API)
* `build/minidb_storage_tests` — storage-layer tests (internal C++ API)
* `build/minidb_verify_tpch` — checks a loaded TPC-H database row by row
  against the `.tbl` files

## Database folder layout

Passing a path to `mydb_open` names a **folder**. If the folder does not exist
(or is empty) MiniDB bootstraps a new database; otherwise it opens the existing
catalog. One table and one FSM file are created per relation:

```
tpch_db/
  sys_tables.dat    sys_tables.fsm     # catalog: relation metadata
  sys_columns.dat   sys_columns.fsm    # catalog: column metadata
  nation.dat        nation.fsm
  region.dat        region.fsm
  supplier.dat      supplier.fsm
  ...
```

`sys_tables(table_id, name, data_file, fsm_file, segment_header_page,
column_count, is_system)` and `sys_columns(table_id, ordinal, name, type,
length, scale, nullable)` are ordinary heap relations stored in their own
files. They appear in `SHOW TABLES` and can be queried/`DESCRIBE`d like user
tables, but `INSERT` and `DROP` are rejected and the `sys_` name prefix is
reserved. System relation IDs are 1 and 2; user relation IDs start at 3 and are
derived from the catalog when reopening.

Every data file has a 512 KB initial extent; page 0 is the file header, page 1
is the extent bitmap and page 1 of the FSM file onward holds one free-space
category byte per data page. Deleting a table removes both its `.dat` and
`.fsm` files. Databases created by the older single-file MiniDB format cannot
be opened by this version (file header magic/version is `MINIDBF2`).

## Quick start

### Embedded C API

```c
#include <stdio.h>
#include "minidb/minidb.h"

int main(void) {
    mydb_db* db;
    if (mydb_open("data/tpch_db", NULL, &db) != MYDB_OK) {
        fprintf(stderr, "%s\n", mydb_last_error());
        return 1;
    }

    char* errmsg = NULL;
    if (mydb_exec(db, "CREATE TABLE t (a INTEGER NOT NULL, b VARCHAR(32));",
                  &errmsg) != MYDB_OK) {
        fprintf(stderr, "%s\n", errmsg);
        mydb_free(errmsg);
        mydb_close(db);
        return 1;
    }

    mydb_prepare(db, "INSERT INTO t VALUES (1, 'hello');", NULL);
    mydb_stmt* stmt;
    mydb_prepare(db, "SELECT a, b FROM t;", &stmt);
    while (mydb_step(stmt) == MYDB_ROW) {
        printf("%lld %s\n", mydb_column_int64(stmt, 0),
               mydb_column_text(stmt, 1));
    }
    mydb_finalize(stmt);
    mydb_close(db);
    return 0;
}
```

Compile against the static library:

```sh
cc app.c -Iinclude -Lbuild -lminidb_core -lstdc++ -lpthread -o app
```

Link `-lminidb` instead to use the shared library.

### Demo

`examples/demo.cpp` executes the complete TPC-H schema through the C API,
inspects `sys_tables` to show the generated `.dat`/`.fsm` file names, inserts
sample `nation`/`region`/`supplier`/`orders` rows, runs prepared queries
(`SELECT`, `SHOW TABLES`, `DESCRIBE`), deletes rows with and without `WHERE`,
then closes and reopens the database to show persistence and prints the size of
the database folder:

```sh
./build/minidb_demo
# or choose the database and schema explicitly
./build/minidb_demo /tmp/tpch_db test/tpc-h/schema.sql
```

### Shell

```sh
# execute a script
./build/minidb_shell --db /tmp/tpch_db --file test/tpc-h/schema.sql

# interactive (or pipe SQL on stdin)
./build/minidb_shell --db /tmp/tpch_db
```

Options: `--db <path>`, `--file <path>`, `--buffer <frames>`,
`--log-level debug|info|warn|error|off`, `--log-file <path>` (log to a file as
well as stderr). Scripts (`--file`, `.read`) may mix
SQL and meta commands. Meta commands are shorthand for SQL, as in sqlite3:

| Command | Runs |
| --- | --- |
| `.tables`, `.schema t` | `SHOW TABLES`, `DESCRIBE t` |
| `.load t file.tbl` | `COPY t FROM 'file.tbl' DELIMITER '\|'` |
| `.get t page:slot` | `SELECT rid, * FROM t WHERE rid = 'page:slot'` |
| `.stats`, `.frames` | `PRAGMA buffer_stats`, `PRAGMA buffer_frames` |
| `.info t`, `.extents t`, `.bitmap t` | `PRAGMA table_info / extents / extent_bitmap` |
| `.fsm t`, `.free t` | `PRAGMA free_space_summary / free_space` |
| `.page t p` | `PRAGMA page_header` and `PRAGMA page_slots` |
| `.flush`, `.log [level]` | `PRAGMA flush`, `PRAGMA log_level [= level]` |
| `.read f`, `.echo on\|off`, `.timer on\|off`, `.help`, `.quit` | client-side |

### Acceptance demo

[`scripts/demo_supplier.txt`](scripts/demo_supplier.txt) walks through loading
TPC-H `supplier`, the extent bitmap, extent lists, FSM, slotted pages,
forwarding on update, page and extent reuse after deletes, and the buffer
pool. Generate `supplier.tbl` with TPC-H dbgen into `test/tpc-h/` first
(files there other than `schema.sql` are git-ignored), then:

```sh
rm -rf /tmp/demo_db
./build/minidb_shell --db /tmp/demo_db --buffer 64 --file scripts/demo_supplier.txt
```

[`scripts/load_tpch.txt`](scripts/load_tpch.txt) loads all eight tables (SF1,
about 8.66 million rows) and prints per-table storage statistics. With the
course data this takes about 17 s in a Release build (lineitem: 6,001,215 rows
in 12.6 s) and produces a 1.3 GB database; `minidb_verify_tpch` then confirms
every row:

```sh
rm -rf /tmp/tpch_db
./build-release/minidb_shell --db /tmp/tpch_db --log-level info --file scripts/load_tpch.txt
./build-release/minidb_verify_tpch /tmp/tpch_db test/tpc-h
```

### Tests

```sh
ctest --test-dir build --output-on-failure
```

The integration test uses a tiny 8-frame buffer pool to force page eviction,
runs the actual TPC-H schema, checks the one-`.dat`-plus-one-`.fsm`-per-relation
folder layout, queries `sys_tables`/`sys_columns`, verifies that system tables
are read-only, round-trips `CHAR`, `VARCHAR`, `FLOAT`, `DECIMAL`, `DATE` and `INTEGER`
values, exercises `UPDATE` (in-page growth, multi-column, NOT NULL and
system-table errors) and `DELETE` with and without `WHERE`, bulk-loads 20,000
rows spanning several extents, grows 500 of them so they move behind forwarding
pointers, and checks that schema and data survive a close/reopen cycle and that
`DROP TABLE` deletes both files.

The storage tests cover CRC32C (hardware vs. portable), forwarding (no
duplicate rows in scans, re-forwarding, collect-then-delete), in-page growth,
FSM search wrap-around, extent-hole reuse, tolerance of an unwritten FSM page
after reopen, buffer-pool frame recycling and schema size limits.

## SQL subset

```sql
CREATE TABLE [IF NOT EXISTS] name (
    col INTEGER | BIGINT | DOUBLE | REAL | FLOAT
      | CHAR(n) | VARCHAR(n) | DECIMAL(p,s) | DATE
      [NOT NULL] [PRIMARY KEY] [UNIQUE] [CHECK (...)]
    [, ...]
    [, PRIMARY KEY (col, ...)]
    [, FOREIGN KEY (...) REFERENCES other (...)]
);

DROP TABLE [IF EXISTS] name;

INSERT INTO name [(col, ...)] VALUES (expr, ...) [, (...)];

UPDATE name SET col = literal [, col = literal ...] [WHERE col op literal];

COPY name FROM 'file.tbl' [DELIMITER '|'];    -- dbgen's trailing '|' is ignored

DELETE FROM name [WHERE col op literal];

SELECT [* | col | rid, ...] FROM name
  [WHERE col op literal | WHERE rid = 'page:slot'] [LIMIT n];

PRAGMA name [(arg, ...)] [= value];             -- PRAGMA pragma_list; lists them

SHOW TABLES;
DESCRIBE name;
```

`op` is `=`, `<>`/`!=`, `<`, `<=`, `>`, `>=`. `DELETE` without `WHERE` removes
all rows; empty heap pages (except the segment's first heap page) are
returned to the segment, and an extent whose pages are all free goes back to
the file's extent bitmap. `UPDATE` keeps every row's RID: a longer row grows in
its page when the page has room (after compaction), otherwise it moves and the
original slot keeps a one-hop forwarding pointer. Only single-table scans and
one comparison per `WHERE` are supported; there are no joins, aggregates or
indexes yet. `PRIMARY KEY` and `FOREIGN KEY`
clauses are accepted for compatibility and primary-key columns become
`NOT NULL`, but the constraints are not enforced without an index layer.

`rid` is a pseudo-column (`"page:slot"`, like PostgreSQL's `ctid`): `SELECT rid,
* FROM t LIMIT 5` shows record ids and `WHERE rid = '65:0'` looks one up,
following a forwarding pointer if the row has moved.

`COPY` loads a delimited file inside the engine (about 500k rows/s for
lineitem-shaped rows in a Release build), reports the file and line of a bad
record, and returns a summary with buffer-pool counters for the load.

| PRAGMA | Shows |
| --- | --- |
| `buffer_stats`, `buffer_frames` | pool counters and hit rate; every occupied frame |
| `table_info(t)` | files, extents, pages, rows, forwarded rows, fill factor |
| `extents(t)` | extent list with a per-page allocation mask, including holes |
| `extent_bitmap(t)` | the data file's extent bitmap |
| `free_space(t)`, `free_space_summary(t)` | FSM per page (vs. actual free bytes), histogram |
| `page_header(t, p)`, `page_slots(t, p)` | page header fields and slot directory |
| `flush`, `log_level [= level]` | write back dirty pages; show/set logging |

`SHOW TABLES`/`DESCRIBE`/`SELECT` also work on `sys_tables` and `sys_columns`;
these two system relations reject `INSERT`, `UPDATE`, `DELETE`, `DROP TABLE`
and the reserved
`sys_` name prefix.

## Storage mapping notes

* `INTEGER` → 32-bit, `BIGINT`/`LONG` → 64-bit, `FLOAT`/`DOUBLE`/`REAL` →
  IEEE-754 double, `DATE` → days since the Unix epoch.
* `CHAR(n)` is stored fixed-width and space-padded; comparisons and output trim
  trailing spaces.
* `DECIMAL(p,s)` is stored as a scaled 64-bit integer (`value * 10^s`), which
  supports precision up to 18. This is the one small format extension beyond
  the design document: `sys_columns` carries an extra `scale` field so the
  catalog can reconstruct `DECIMAL(p,s)`.
* Records have an 18-byte header, a null bitmap, a fixed-length area, a
  `uint16` end-offset table for `VARCHAR` values and then the variable data.
* Each relation data/FSM file records a `clean_shutdown` flag in its page-0
  header. It is cleared when the database opens and set after a clean close; a
  warning is logged when an existing relation was left dirty. No redo/undo
  recovery is implemented.
* Every statement runs in its own auto-commit transaction. The WAL and crash
  recovery described in later chapters are stubbed out; a crash may leave the
  database inconsistent, so treat this implementation as educational rather
  than durable.

## Repository layout

```
include/minidb/minidb.h   public C API
src/common/               status, types, CRC32C, logging, date/decimal utils
src/storage/              page and file formats, disk manager
src/buffer/               buffer pool, CLOCK replacer, page guards
src/space/                extents, segments, extent lists, FSM
src/record/               values, schemas, tuple encoding
src/table/                slotted heap pages, table heap, iterators
src/catalog/              system catalog bootstrap and DDL metadata
src/sql/                  lexer + recursive-descent parser
src/db/                   statement execution, COPY, PRAGMA
src/capi/                 C API wrapper
tools/shell/              minidb_shell
examples/demo.cpp         embedded demo
tests/test_minidb.cpp     integration tests
tests/test_storage.cpp    storage-layer tests
scripts/demo_supplier.txt acceptance demo script
scripts/load_tpch.txt     loads all eight TPC-H tables
doc/minidb-design.md      original design document
test/tpc-h/schema.sql     TPC-H schema from the experiment handout (FLOAT
                          prices) used by the demo and
                          tests; other files under test/ (e.g. *.tbl) are
                          git-ignored
```
