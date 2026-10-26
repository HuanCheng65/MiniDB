// minidb_shell: interactive command line and script runner.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "minidb/minidb.h"

namespace {

struct Options {
  std::string db_path = "minidb_db";
  std::string file;
  std::string log_level = "warn";
  int buffer_frames = 0;
  bool help = false;
};

void PrintUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n"
            << "  --db <path>          database folder (default: minidb_db)\n"
            << "  --file <path>        execute a SQL script and exit\n"
            << "  --buffer <frames>    buffer pool frames (default: 1024)\n"
            << "  --log-level <level>  debug|info|warn|error|off\n"
            << "  --help\n";
}

bool ParseOptions(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](std::string* out) {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (arg == "--help" || arg == "-h") {
      options->help = true;
    } else if (arg == "--db") {
      if (!value(&options->db_path)) return false;
    } else if (arg == "--file") {
      if (!value(&options->file)) return false;
    } else if (arg == "--log-level") {
      if (!value(&options->log_level)) return false;
    } else if (arg == "--buffer") {
      std::string text;
      if (!value(&text)) return false;
      options->buffer_frames = std::stoi(text);
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      return false;
    }
  }
  return true;
}

void PrintTable(const std::vector<std::string>& headers,
                const std::vector<std::vector<std::string>>& rows) {
  if (headers.empty()) return;
  std::vector<size_t> widths;
  widths.reserve(headers.size());
  for (const std::string& header : headers) widths.push_back(header.size());
  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }
  auto print_row = [&](const std::vector<std::string>& row) {
    for (size_t i = 0; i < headers.size(); ++i) {
      const std::string cell = i < row.size() ? row[i] : "";
      std::cout << (i == 0 ? "" : " | ");
      std::cout << cell;
      if (i + 1 < headers.size()) {
        std::cout << std::string(widths[i] - cell.size(), ' ');
      }
    }
    std::cout << "\n";
  };
  print_row(headers);
  for (size_t i = 0; i < headers.size(); ++i) {
    std::cout << (i == 0 ? "" : "-+-")
              << std::string(widths[i], '-');
  }
  std::cout << "\n";
  for (const auto& row : rows) print_row(row);
  std::cout << "(" << rows.size() << " row" << (rows.size() == 1 ? "" : "s")
            << ")\n";
}

bool ExecuteAndPrint(mydb_db* db, const std::string& sql) {
  mydb_stmt* stmt = nullptr;
  if (mydb_prepare(db, sql.c_str(), &stmt) != MYDB_OK) {
    std::cerr << "error: " << (mydb_errmsg(db) ? mydb_errmsg(db) : "unknown")
              << "\n";
    return false;
  }

  std::vector<std::string> headers;
  const int column_count = mydb_column_count(stmt);
  for (int i = 0; i < column_count; ++i) {
    headers.push_back(mydb_column_name(stmt, i) ? mydb_column_name(stmt, i) : "");
  }
  std::vector<std::vector<std::string>> rows;
  int step = 0;
  while ((step = mydb_step(stmt)) == MYDB_ROW) {
    std::vector<std::string> row;
    for (int i = 0; i < column_count; ++i) {
      const char* text = mydb_column_text(stmt, i);
      row.push_back(text == nullptr ? "" : text);
    }
    rows.push_back(std::move(row));
  }
  if (step == MYDB_ERROR) {
    std::cerr << "error while stepping statement\n";
    mydb_finalize(stmt);
    return false;
  }
  if (!headers.empty()) PrintTable(headers, rows);
  const char* message = mydb_message(db);
  if (message != nullptr && headers.empty()) std::cout << message << "\n";
  if (mydb_changes(db) > 0) {
    std::cout << "changes: " << mydb_changes(db) << "\n";
  }
  mydb_finalize(stmt);
  return true;
}

bool ExecuteFile(mydb_db* db, const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "cannot read script " << path << "\n";
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  char* error = nullptr;
  if (mydb_exec(db, buffer.str().c_str(), &error) != MYDB_OK) {
    std::cerr << "error: " << (error ? error : "unknown") << "\n";
    mydb_free(error);
    return false;
  }
  return true;
}

bool HandleMeta(mydb_db* db, const std::string& line) {
  std::istringstream input(line);
  std::string command;
  input >> command;
  std::string argument;
  input >> argument;
  if (command == ".quit" || command == ".exit") return false;
  if (command == ".help") {
    std::cout << "Meta commands: .tables, .schema <table>, .read <file>, "
                 ".quit\n"
              << "SQL: CREATE TABLE, DROP TABLE, INSERT, SELECT, SHOW TABLES, "
                 "DESCRIBE\n";
    return true;
  }
  if (command == ".tables") {
    ExecuteAndPrint(db, "SHOW TABLES;");
    return true;
  }
  if (command == ".schema") {
    if (argument.empty()) {
      std::cout << "usage: .schema <table>\n";
    } else {
      ExecuteAndPrint(db, "DESCRIBE " + argument + ";");
    }
    return true;
  }
  if (command == ".read") {
    if (argument.empty()) {
      std::cout << "usage: .read <file>\n";
    } else {
      ExecuteFile(db, argument);
    }
    return true;
  }
  std::cout << "unknown meta command: " << command << "\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    PrintUsage(argv[0]);
    return 1;
  }
  if (options.help) {
    PrintUsage(argv[0]);
    return 0;
  }

  mydb_config config;
  config.buffer_pool_frames = options.buffer_frames;
  config.replacer = "clock";
  config.log_level = options.log_level.c_str();
  config.log_file = nullptr;

  mydb_db* db = nullptr;
  if (mydb_open(options.db_path.c_str(), &config, &db) != MYDB_OK) {
    std::cerr << "cannot open " << options.db_path << ": "
              << (mydb_last_error() ? mydb_last_error() : "unknown") << "\n";
    return 1;
  }
  std::cout << mydb_version() << " database: " << options.db_path << "\n";

  int exit_code = 0;
  if (!options.file.empty()) {
    exit_code = ExecuteFile(db, options.file) ? 0 : 1;
  } else {
    std::string buffer;
    std::string line;
    bool interactive = isatty(0) != 0;
    if (interactive) std::cout << "minidb> " << std::flush;
    while (std::getline(std::cin, line)) {
      if (buffer.empty() && !line.empty() && line[0] == '.') {
        if (!HandleMeta(db, line)) break;
        if (interactive) std::cout << "minidb> " << std::flush;
        continue;
      }
      if (!buffer.empty()) buffer.push_back('\n');
      buffer += line;
      if (buffer.find(';') == std::string::npos) {
        if (interactive) std::cout << "   ...> " << std::flush;
        continue;
      }
      if (!ExecuteAndPrint(db, buffer)) exit_code = 1;
      buffer.clear();
      if (interactive) std::cout << "minidb> " << std::flush;
    }
    if (!buffer.empty()) {
      if (!ExecuteAndPrint(db, buffer)) exit_code = 1;
    }
  }

  if (mydb_close(db) != MYDB_OK) {
    std::cerr << "error while closing database\n";
    exit_code = 1;
  }
  return exit_code;
}
