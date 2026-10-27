// Small hand-written SQL front end (design doc 14.1). Supports the DDL
// needed by the TPC-H schema plus simple INSERT/SELECT/UPDATE/DELETE/SHOW/
// DESCRIBE, bulk loading (COPY) and engine introspection (PRAGMA).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "record/schema.h"

namespace minidb {

struct Literal {
  enum class Kind { kNull, kInteger, kFloat, kString };

  Kind kind = Kind::kNull;
  int64_t integer = 0;
  double floating = 0.0;
  std::string text;  // original spelling (decimals/dates) or string value
};

struct ColumnSpec {
  std::string name;
  Column column;
  bool primary_key = false;
};

struct InsertSpec {
  std::vector<std::string> columns;
  std::vector<std::vector<Literal>> rows;
};

struct SelectSpec {
  std::vector<std::string> columns;  // may contain "*" next to column names
  bool star = false;
  std::string where_column;
  std::string where_op;
  Literal where_value;
  bool has_where = false;
  int64_t limit = -1;
};

struct DeleteSpec {
  std::string where_column;
  std::string where_op;
  Literal where_value;
  bool has_where = false;
};

struct Assignment {
  std::string column;
  Literal value;
};

struct UpdateSpec {
  std::vector<Assignment> assignments;
  std::string where_column;
  std::string where_op;
  Literal where_value;
  bool has_where = false;
};

// COPY name FROM 'path' [DELIMITER 'c']
struct CopySpec {
  std::string path;
  char delimiter = '|';
};

// PRAGMA name | PRAGMA name(arg, ...) | PRAGMA name = value
// Bare identifiers in the argument list are taken as strings, so both
// PRAGMA extents(supplier) and PRAGMA extents('supplier') work.
struct PragmaSpec {
  std::string name;
  std::vector<Literal> args;
  bool has_value = false;
  Literal value;
};

enum class StatementType {
  kCreateTable,
  kDropTable,
  kInsert,
  kSelect,
  kDelete,
  kUpdate,
  kCopy,
  kPragma,
  kShowTables,
  kDescribe,
  kEmpty,
};

struct Statement {
  StatementType type = StatementType::kEmpty;
  std::string table_name;
  bool if_not_exists = false;
  bool if_exists = false;
  std::vector<ColumnSpec> columns;
  InsertSpec insert;
  SelectSpec select;
  DeleteSpec delete_spec;
  UpdateSpec update;
  CopySpec copy;
  PragmaSpec pragma;
};

Result<std::vector<Statement>> ParseScript(std::string_view sql);

}  // namespace minidb
