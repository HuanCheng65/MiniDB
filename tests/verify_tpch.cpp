// Round-trip check for a loaded TPC-H database (design doc 15.2): every row a
// table scan returns, formatted per column type, must match one line of the
// source .tbl file. Rows are compared as a multiset of hashes because scan
// order follows page placement, not file order.
//
//   ./build/minidb_verify_tpch <database folder> <directory with *.tbl>
//
// Exit status 0 when every table matches.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "record/tuple.h"
#include "record/value.h"

namespace minidb {
namespace {

// FLOAT text is compared by value ("4192.40" in the file, 4192.4 stored);
// CHAR(n) drops its padding; everything else must match exactly.
std::string Normalize(const Column& column, std::string field) {
  if (column.type == TypeId::kDouble) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g",
                  std::strtod(field.c_str(), nullptr));
    return buffer;
  }
  if (column.type == TypeId::kChar) {
    while (!field.empty() && field.back() == ' ') field.pop_back();
  }
  return field;
}

uint64_t Fnv1a(const std::string& text) {
  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::vector<uint64_t> HashTable(TableInfo* table, Status* status) {
  std::vector<uint64_t> hashes;
  const Schema& schema = table->schema;
  auto iterator = table->heap->Scan(nullptr);
  RID rid;
  Tuple tuple;
  while (iterator.Next(&rid, &tuple)) {
    auto values = Tuple::Decode(schema, tuple.data().data(), tuple.size());
    if (!values.ok()) {
      *status = values.status();
      return hashes;
    }
    std::string row;
    for (size_t i = 0; i < values.value().size(); ++i) {
      if (i != 0) row.push_back('|');
      row += Normalize(schema.GetColumn(i), ValueToString(values.value()[i]));
    }
    hashes.push_back(Fnv1a(row));
  }
  *status = iterator.status();
  return hashes;
}

std::vector<uint64_t> HashFile(const Schema& schema, const std::string& path) {
  std::vector<uint64_t> hashes;
  std::ifstream input(path);
  std::string line;
  std::vector<std::string> fields;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    fields.clear();
    size_t begin = 0;
    for (;;) {
      const size_t end = line.find('|', begin);
      if (end == std::string::npos) {
        fields.push_back(line.substr(begin));
        break;
      }
      fields.push_back(line.substr(begin, end - begin));
      begin = end + 1;
    }
    if (fields.size() == schema.num_columns() + 1 && fields.back().empty()) {
      fields.pop_back();
    }
    std::string row;
    for (size_t i = 0; i < fields.size() && i < schema.num_columns(); ++i) {
      if (i != 0) row.push_back('|');
      row += Normalize(schema.GetColumn(i), fields[i]);
    }
    hashes.push_back(Fnv1a(row));
  }
  return hashes;
}

}  // namespace
}  // namespace minidb

int main(int argc, char** argv) {
  using namespace minidb;
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <database folder> <tbl directory>\n",
                 argv[0]);
    return 2;
  }
  LogManager log;
  BufferConfig config;
  BufferPool pool(log, config);
  auto catalog = Catalog::Open(argv[1], pool, log);
  if (!catalog.ok()) {
    std::fprintf(stderr, "cannot open %s: %s\n", argv[1],
                 catalog.status().message().c_str());
    return 2;
  }
  int failures = 0;
  for (const char* name : {"region", "nation", "supplier", "customer", "part",
                           "partsupp", "orders", "lineitem"}) {
    auto table = catalog.value()->GetTable(name);
    if (!table.ok()) {
      std::printf("%-9s missing\n", name);
      ++failures;
      continue;
    }
    Status status;
    std::vector<uint64_t> stored = HashTable(table.value(), &status);
    std::vector<uint64_t> source = HashFile(
        table.value()->schema, std::string(argv[2]) + "/" + name + ".tbl");
    std::sort(stored.begin(), stored.end());
    std::sort(source.begin(), source.end());
    const bool match = status.ok() && stored == source;
    std::printf("%-9s rows=%zu file=%zu %s%s\n", name, stored.size(),
                source.size(), match ? "MATCH" : "MISMATCH",
                status.ok() ? "" : (" (" + status.message() + ")").c_str());
    if (!match) ++failures;
  }
  return failures == 0 ? 0 : 1;
}
