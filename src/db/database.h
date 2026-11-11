// Database facade: assembles components and executes statements
// (design doc 3.4, 18).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "common/status.h"
#include "index/bplus_tree.h"
#include "log/log_manager.h"
#include "record/value.h"
#include "sql/parser.h"
#include "txn/txn_manager.h"

namespace minidb {

struct Config {
  // A database is a folder; path is the folder itself.
  std::string path;
  uint32_t buffer_pool_frames = 1024;
  std::string replacer = "clock";
  std::string log_level = "info";
  std::string log_file;
  // Memory CREATE INDEX may sort in before spilling sorted runs to disk.
  size_t index_sort_memory = BPlusTree::kDefaultSortMemory;
};

struct QueryColumn {
  std::string name;
  TypeId type = TypeId::kInt32;
};

struct QueryResult {
  std::vector<QueryColumn> columns;
  std::vector<std::vector<Value>> rows;
  int changes = 0;
  std::string message;
};

// The single `column op value` condition a statement's WHERE clause allows.
struct RowFilter {
  int column = -1;  // schema ordinal; -1 when there is no WHERE
  CompareOp op = CompareOp::kEq;
  Value value;
};

class Database {
 public:
  static Result<std::unique_ptr<Database>> Open(const Config& config);
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  Status Close();
  // Executes one or more ';'-separated statements. DDL/DML messages and the
  // last SELECT result are returned in *result (which may be nullptr).
  Status Execute(const std::string& sql, QueryResult* result);

  Catalog& catalog() { return *catalog_; }
  TxnManager& txns() { return *txns_; }
  BufferPool& buffer_pool() { return *pool_; }

 private:
  explicit Database(const Config& config) : config_(config) {}

  Status ExecuteStatement(Statement* statement, Transaction* txn,
                          QueryResult* result);
  Status ExecuteCreateTable(Statement* statement, Transaction* txn,
                            QueryResult* result);
  Status ExecuteDropTable(Statement* statement, Transaction* txn,
                          QueryResult* result);
  Status ExecuteInsert(Statement* statement, Transaction* txn,
                       QueryResult* result);
  Status ExecuteDelete(Statement* statement, Transaction* txn,
                       QueryResult* result);
  Status ExecuteUpdate(Statement* statement, Transaction* txn,
                       QueryResult* result);
  Status ExecuteSelect(Statement* statement, Transaction* txn,
                       QueryResult* result);
  Status ExecuteCopy(Statement* statement, Transaction* txn,
                     QueryResult* result);
  // Engine introspection and settings (pragma.cpp, design doc 13.2).
  Status ExecutePragma(Statement* statement, Transaction* txn,
                       QueryResult* result);
  Status ExecuteShowTables(QueryResult* result);
  Status ExecuteDescribe(Statement* statement, QueryResult* result);

  Status LoadIndexes();
  Status ExecuteCreateIndex(Statement*, Transaction*, QueryResult*);
  Status ExecuteDropIndex(Statement*, QueryResult*);
  Status DropIndex(const std::string& name);
  Status DropTableIndexes(const TableInfo& table);
  BPlusTree* FindIndex(const TableInfo& table, size_t column);
  bool HasIndexes(const TableInfo& table) const;
  // Visits the rows of `table` that satisfy `filter` until the visitor
  // returns false: through an index on the filtered column when one helps,
  // otherwise by scanning the heap. Returns the index used, or null. The
  // visitor must not modify the table.
  using RowVisitor =
      std::function<bool(RID, const Tuple&, const std::vector<Value>&)>;
  Result<BPlusTree*> FindRows(const TableInfo& table, Transaction* txn,
                              const RowFilter& filter, const RowVisitor& visit);
  Status MaintainIndexes(const TableInfo& table, RID rid, const Tuple* before,
                         const Tuple* after);

  Config config_;
  // Declaration order is construction order; destruction runs in reverse, so
  // the catalog (and its per-relation segments/heaps) dies before the buffer
  // pool and logger it references.
  std::unique_ptr<LogManager> log_;
  std::unique_ptr<BufferPool> pool_;
  std::unique_ptr<TxnManager> txns_;
  std::unique_ptr<Catalog> catalog_;
  std::map<std::string, std::unique_ptr<BPlusTree>> indexes_;
  bool closed_ = false;
};

}  // namespace minidb
