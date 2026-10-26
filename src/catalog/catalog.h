// Catalog persisted in the sys_tables / sys_columns heap relations.
//
// A database is a folder. Every relation (including the two system relations)
// owns <name>.dat and <name>.fsm files in that folder. sys_tables records the
// file names and the segment header page so the catalog can be reopened.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "buffer/buffer_pool.h"
#include "common/status.h"
#include "log/log_manager.h"
#include "record/schema.h"
#include "space/free_space_map.h"
#include "space/space_manager.h"
#include "storage/disk_manager.h"
#include "table/table_heap.h"
#include "txn/transaction.h"

namespace minidb {

struct TableInfo {
  uint32_t table_id = 0;
  std::string name;
  bool is_system = false;
  std::string data_file;  // file name inside the database folder
  std::string fsm_file;
  Schema schema;

  // Destruction order matters: heap -> segment -> fsm -> space -> disks.
  DiskManager::Ptr data_disk;
  DiskManager::Ptr fsm_disk;
  std::unique_ptr<SpaceManager> space;
  std::shared_ptr<FreeSpaceMap> fsm;
  std::shared_ptr<Segment> segment;
  std::unique_ptr<TableHeap> heap;
};

class Catalog {
 public:
  static Result<std::unique_ptr<Catalog>> Bootstrap(const std::string& directory,
                                                    BufferPool& buffer_pool,
                                                    LogManager& log);
  static Result<std::unique_ptr<Catalog>> Open(const std::string& directory,
                                               BufferPool& buffer_pool,
                                               LogManager& log);

  Result<TableInfo*> CreateTable(Transaction* txn, std::string name,
                                 Schema schema);
  Result<TableInfo*> GetTable(std::string_view name);
  Status DropTable(Transaction* txn, std::string_view name);

  std::vector<TableInfo*> Tables() const;
  Status FlushAllSegments(Transaction* txn);

  // Called when opening an existing database: warns about dirty relations and
  // marks every file as in-use (clean_shutdown = 0). On bootstrap, pass false
  // to suppress warnings for the freshly created files.
  Status BeginSession(bool existing_database);
  // Marks every relation file (data and FSM) cleanly shut down and syncs it.
  Status SetCleanShutdown(bool clean);

  static Schema SysTablesSchema();
  static Schema SysColumnsSchema();

  static constexpr uint32_t kSysTablesId = 1;
  static constexpr uint32_t kSysColumnsId = 2;

 private:
  Catalog(std::string directory, BufferPool& buffer_pool, LogManager& log)
      : directory_(std::move(directory)),
        buffer_pool_(buffer_pool),
        log_(log) {}

  Status LoadSystemCatalog();
  Result<TableInfo*> CreateRelation(Transaction* txn, uint32_t table_id,
                                    std::string name, std::string data_file,
                                    std::string fsm_file, Schema schema,
                                    bool is_system);
  Result<TableInfo*> OpenRelation(Transaction* txn, uint32_t table_id,
                                  std::string name, std::string data_file,
                                  std::string fsm_file, page_id_t segment_page,
                                  Schema schema, bool is_system);
  Status InsertSysTableRow(Transaction* txn, const TableInfo& table,
                           page_id_t segment_page);
  Status InsertSysColumnRows(Transaction* txn, uint32_t table_id,
                             const Schema& schema);
  std::string FilePath(const std::string& file_name) const;
  std::string ChooseFileName(const std::string& table_name,
                             const std::string& suffix,
                             uint32_t table_id) const;

  std::string directory_;
  BufferPool& buffer_pool_;
  LogManager& log_;

  // Owned by the TableInfo entries in tables_.
  TableHeap* sys_tables_heap_ = nullptr;
  TableHeap* sys_columns_heap_ = nullptr;

  std::unordered_map<std::string, std::unique_ptr<TableInfo>> tables_;
  uint32_t next_table_id_ = kSysColumnsId + 1;
};

}  // namespace minidb
