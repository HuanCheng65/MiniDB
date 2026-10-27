// minidb_shell: interactive command line and script runner.
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
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
  std::string log_file;
  int buffer_frames = 0;
  bool help = false;
};

void PrintUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n"
            << "  --db <path>          database folder (default: minidb_db)\n"
            << "  --file <path>        run a script (SQL and meta commands) and exit\n"
            << "  --buffer <frames>    buffer pool frames (default: 1024)\n"
            << "  --log-level <level>  debug|info|warn|error|off\n"
            << "  --log-file <path>    also append the log to a file\n"
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
    } else if (arg == "--log-file") {
      if (!value(&options->log_file)) return false;
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

// Quotes text as a SQL string literal.
std::string Quote(const std::string& text) {
  std::string quoted = "'";
  for (char c : text) {
    if (c == '\'') quoted.push_back('\'');
    quoted.push_back(c);
  }
  return quoted + "'";
}

// True when sql ends with a ';' that is outside string literals and comments,
// so a ';' inside 'text' or -- a comment does not end the statement early.
bool IsCompleteStatement(const std::string& sql) {
  bool in_string = false;
  bool complete = false;
  for (size_t i = 0; i < sql.size(); ++i) {
    const char c = sql[i];
    if (in_string) {
      if (c == '\'') {
        if (i + 1 < sql.size() && sql[i + 1] == '\'') {
          ++i;
        } else {
          in_string = false;
        }
      }
      continue;
    }
    if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      while (i < sql.size() && sql[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      const size_t end = sql.find("*/", i + 2);
      if (end == std::string::npos) return false;
      i = end + 1;
      continue;
    }
    if (c == '\'') {
      in_string = true;
      complete = false;
    } else if (c == ';') {
      complete = true;
    } else if (!std::isspace(static_cast<unsigned char>(c))) {
      complete = false;
    }
  }
  return complete && !in_string;
}

void PrintHelp() {
  std::cout
      << "SQL (end with ';'):\n"
         "  CREATE TABLE / DROP TABLE / INSERT / UPDATE / DELETE / SELECT\n"
         "  SHOW TABLES; DESCRIBE <table>;\n"
         "  COPY <table> FROM '<file>' [DELIMITER '|'];   bulk load (.tbl)\n"
         "  SELECT rid, ... / WHERE rid = 'page:slot'    record ids\n"
         "  PRAGMA <name>[(args)] [= value];             see PRAGMA pragma_list;\n"
         "Meta commands (no ';'; each is shorthand for the SQL shown):\n"
         "  .tables                 SHOW TABLES\n"
         "  .schema <table>         DESCRIBE <table>\n"
         "  .load <table> <file>    COPY <table> FROM '<file>' DELIMITER '|'\n"
         "  .get <table> <p:s>      SELECT rid, * ... WHERE rid = 'p:s'\n"
         "  .stats                  PRAGMA buffer_stats\n"
         "  .frames                 PRAGMA buffer_frames\n"
         "  .info <table>           PRAGMA table_info(<table>)\n"
         "  .extents <table>        PRAGMA extents(<table>)\n"
         "  .bitmap <table>         PRAGMA extent_bitmap(<table>)\n"
         "  .fsm <table>            PRAGMA free_space_summary(<table>)\n"
         "  .free <table>           PRAGMA free_space(<table>)\n"
         "  .page <table> <page>    PRAGMA page_header + page_slots\n"
         "  .flush                  PRAGMA flush\n"
         "  .log [level]            PRAGMA log_level [= level]\n"
         "Client-only:\n"
         "  .read <file>            run a script (SQL and meta commands)\n"
         "  .echo on|off            print each command before running it\n"
         "  .timer on|off           print the run time of every statement\n"
         "  .help                   this text\n"
         "  .quit / .exit\n";
}

class Shell {
 public:
  explicit Shell(mydb_db* db) : db_(db) {}

  // Runs SQL and meta commands from a stream (stdin, --file or .read).
  // Returns false when .quit was requested.
  bool Run(std::istream& input, bool interactive) {
    std::string buffer;
    std::string line;
    const auto prompt = [&](const char* text) {
      if (interactive) std::cout << text << std::flush;
    };
    prompt("minidb> ");
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (buffer.empty()) {
        const size_t first = line.find_first_not_of(" \t");
        // Blank lines and whole-line comments between statements: echoed, so
        // a script can narrate itself, but never glued to the next command.
        if (first == std::string::npos || line.compare(first, 2, "--") == 0) {
          if (echo_) std::cout << line << "\n";
          prompt("minidb> ");
          continue;
        }
        if (line[first] == '.') {
          if (echo_) std::cout << line << std::endl;
          if (!HandleMeta(line.substr(first))) return false;
          prompt("minidb> ");
          continue;
        }
      }
      if (!buffer.empty()) buffer.push_back('\n');
      buffer += line;
      if (!IsCompleteStatement(buffer)) {
        prompt("   ...> ");
        continue;
      }
      RunSql(buffer);
      buffer.clear();
      prompt("minidb> ");
    }
    if (buffer.find_first_not_of(" \t\n") != std::string::npos) RunSql(buffer);
    return true;
  }

  bool failed() const { return failed_; }

 private:
  void RunSql(const std::string& sql) {
    if (echo_) std::cout << sql << "\n";
    // Flush around each statement so engine log lines (stderr) stay in order
    // with the results even when output is redirected to a file.
    std::cout << std::flush;
    const auto start = std::chrono::steady_clock::now();
    if (!ExecuteAndPrint(db_, sql)) failed_ = true;
    if (timer_) {
      const double seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
      std::cout << "Run Time: " << std::fixed << std::setprecision(3) << seconds
                << " s\n";
      std::cout.unsetf(std::ios::floatfield);
    }
    std::cout << std::flush;
  }

  bool RunFile(const std::string& path) {
    if (depth_ >= 16) {
      std::cerr << "error: .read nested too deeply\n";
      failed_ = true;
      return true;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      std::cerr << "cannot read script " << path << "\n";
      failed_ = true;
      return true;
    }
    ++depth_;
    const bool keep_going = Run(file, /*interactive=*/false);
    --depth_;
    return keep_going;
  }

  bool HandleMeta(const std::string& line) {
    std::istringstream input(line);
    std::string command;
    input >> command;
    std::vector<std::string> args;
    for (std::string arg; input >> arg;) args.push_back(arg);
    const auto need = [&](size_t count, const char* usage) {
      if (args.size() == count) return true;
      std::cout << "usage: " << usage << "\n";
      failed_ = true;
      return false;
    };
    const auto run = [&](const std::string& sql) {
      const bool echo = echo_;
      echo_ = false;  // the meta command itself was already echoed
      RunSql(sql);
      echo_ = echo;
    };

    if (command == ".quit" || command == ".exit") return false;
    if (command == ".help") {
      PrintHelp();
    } else if (command == ".tables") {
      run("SHOW TABLES;");
    } else if (command == ".schema") {
      if (need(1, ".schema <table>")) run("DESCRIBE " + args[0] + ";");
    } else if (command == ".read") {
      if (need(1, ".read <file>")) return RunFile(args[0]);
    } else if (command == ".load") {
      if (need(2, ".load <table> <file.tbl>")) {
        run("COPY " + args[0] + " FROM " + Quote(args[1]) + " DELIMITER '|';");
      }
    } else if (command == ".get") {
      if (need(2, ".get <table> <page:slot>")) {
        run("SELECT rid, * FROM " + args[0] + " WHERE rid = " + Quote(args[1]) +
            ";");
      }
    } else if (command == ".stats") {
      run("PRAGMA buffer_stats;");
    } else if (command == ".frames") {
      run("PRAGMA buffer_frames;");
    } else if (command == ".info") {
      if (need(1, ".info <table>")) run("PRAGMA table_info(" + args[0] + ");");
    } else if (command == ".extents") {
      if (need(1, ".extents <table>")) run("PRAGMA extents(" + args[0] + ");");
    } else if (command == ".bitmap") {
      if (need(1, ".bitmap <table>")) {
        run("PRAGMA extent_bitmap(" + args[0] + ");");
      }
    } else if (command == ".fsm") {
      if (need(1, ".fsm <table>")) {
        run("PRAGMA free_space_summary(" + args[0] + ");");
      }
    } else if (command == ".free") {
      if (need(1, ".free <table>")) run("PRAGMA free_space(" + args[0] + ");");
    } else if (command == ".page") {
      if (need(2, ".page <table> <page_id>")) {
        run("PRAGMA page_header(" + args[0] + ", " + args[1] + ");");
        run("PRAGMA page_slots(" + args[0] + ", " + args[1] + ");");
      }
    } else if (command == ".flush") {
      run("PRAGMA flush;");
    } else if (command == ".log") {
      if (args.empty()) {
        run("PRAGMA log_level;");
      } else if (need(1, ".log [debug|info|warn|error|off]")) {
        run("PRAGMA log_level = " + args[0] + ";");
      }
    } else if (command == ".timer") {
      if (need(1, ".timer on|off")) timer_ = args[0] == "on";
    } else if (command == ".echo") {
      if (need(1, ".echo on|off")) echo_ = args[0] == "on";
    } else {
      std::cout << "unknown meta command: " << command << " (try .help)\n";
      failed_ = true;
    }
    return true;
  }

  mydb_db* db_;
  bool timer_ = false;
  bool echo_ = false;
  bool failed_ = false;
  int depth_ = 0;
};

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
  config.log_file =
      options.log_file.empty() ? nullptr : options.log_file.c_str();

  mydb_db* db = nullptr;
  if (mydb_open(options.db_path.c_str(), &config, &db) != MYDB_OK) {
    std::cerr << "cannot open " << options.db_path << ": "
              << (mydb_last_error() ? mydb_last_error() : "unknown") << "\n";
    return 1;
  }
  std::cout << mydb_version() << " database: " << options.db_path << "\n";

  Shell shell(db);
  if (!options.file.empty()) {
    std::ifstream file(options.file, std::ios::binary);
    if (!file) {
      std::cerr << "cannot read script " << options.file << "\n";
      mydb_close(db);
      return 1;
    }
    shell.Run(file, /*interactive=*/false);
  } else {
    shell.Run(std::cin, isatty(0) != 0);
  }
  int exit_code = shell.failed() ? 1 : 0;

  if (mydb_close(db) != MYDB_OK) {
    std::cerr << "error while closing database\n";
    exit_code = 1;
  }
  return exit_code;
}
