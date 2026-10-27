#include "sql/parser.h"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <utility>

#include "common/util.h"

namespace minidb {
namespace {

enum class TokenType { kEnd, kIdentifier, kNumber, kString, kSymbol };

struct Token {
  TokenType type = TokenType::kEnd;
  std::string text;
  size_t position = 0;
};

bool IsIdentifierStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

Result<std::vector<Token>> Tokenize(std::string_view sql) {
  std::vector<Token> tokens;
  size_t i = 0;
  while (i < sql.size()) {
    const char c = sql[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      while (i < sql.size() && sql[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      i += 2;
      bool closed = false;
      while (i + 1 < sql.size()) {
        if (sql[i] == '*' && sql[i + 1] == '/') {
          i += 2;
          closed = true;
          break;
        }
        ++i;
      }
      if (!closed) {
        return Status(StatusCode::kInvalidArgument, "unterminated comment");
      }
      continue;
    }
    if (IsIdentifierStart(c)) {
      const size_t start = i;
      while (i < sql.size() && IsIdentifierChar(sql[i])) ++i;
      Token token;
      token.type = TokenType::kIdentifier;
      token.text = ToLower(std::string(sql.substr(start, i - start)));
      token.position = start;
      tokens.push_back(std::move(token));
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      const size_t start = i;
      while (i < sql.size() &&
             std::isdigit(static_cast<unsigned char>(sql[i]))) {
        ++i;
      }
      if (i < sql.size() && sql[i] == '.') {
        ++i;
        while (i < sql.size() &&
               std::isdigit(static_cast<unsigned char>(sql[i]))) {
          ++i;
        }
      }
      if (i < sql.size() && (sql[i] == 'e' || sql[i] == 'E')) {
        ++i;
        if (i < sql.size() && (sql[i] == '+' || sql[i] == '-')) ++i;
        while (i < sql.size() &&
               std::isdigit(static_cast<unsigned char>(sql[i]))) {
          ++i;
        }
      }
      Token token;
      token.type = TokenType::kNumber;
      token.text = std::string(sql.substr(start, i - start));
      token.position = start;
      tokens.push_back(std::move(token));
      continue;
    }
    if (c == '\'') {
      ++i;
      std::string value;
      bool closed = false;
      while (i < sql.size()) {
        if (sql[i] == '\'') {
          if (i + 1 < sql.size() && sql[i + 1] == '\'') {
            value.push_back('\'');
            i += 2;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        value.push_back(sql[i++]);
      }
      if (!closed) {
        return Status(StatusCode::kInvalidArgument,
                      "unterminated string literal");
      }
      Token token;
      token.type = TokenType::kString;
      token.text = std::move(value);
      token.position = i;
      tokens.push_back(std::move(token));
      continue;
    }
    if (c == '"') {
      ++i;
      std::string value;
      bool closed = false;
      while (i < sql.size()) {
        if (sql[i] == '"') {
          ++i;
          closed = true;
          break;
        }
        value.push_back(sql[i++]);
      }
      if (!closed) {
        return Status(StatusCode::kInvalidArgument,
                      "unterminated quoted identifier");
      }
      Token token;
      token.type = TokenType::kIdentifier;
      token.text = ToLower(value);
      token.position = i;
      tokens.push_back(std::move(token));
      continue;
    }

    Token token;
    token.type = TokenType::kSymbol;
    token.position = i;
    if (i + 1 < sql.size()) {
      const std::string two = std::string(sql.substr(i, 2));
      if (two == "<=" || two == ">=" || two == "<>" || two == "!=") {
        token.text = two;
        i += 2;
        tokens.push_back(std::move(token));
        continue;
      }
    }
    if (std::string("(),;*=<>+-./").find(c) == std::string::npos) {
      return Status(StatusCode::kInvalidArgument,
                    "unexpected character '" + std::string(1, c) + "'");
    }
    token.text = std::string(1, c);
    ++i;
    tokens.push_back(std::move(token));
  }
  Token end;
  end.type = TokenType::kEnd;
  tokens.push_back(end);
  return tokens;
}

std::string Upper(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return text;
}

}  // namespace

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  Result<std::vector<Statement>> Parse();

 private:
  const Token& Peek(size_t ahead = 0) const {
    const size_t index = pos_ + ahead;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
  }
  bool AtEnd() const { return Peek().type == TokenType::kEnd; }
  bool IsKeyword(const Token& token, std::string_view keyword) const {
    return token.type == TokenType::kIdentifier &&
           Upper(token.text) == keyword;
  }
  bool MatchKeyword(std::string_view keyword) {
    if (IsKeyword(Peek(), keyword)) {
      ++pos_;
      return true;
    }
    return false;
  }
  bool MatchSymbol(std::string_view symbol) {
    if (Peek().type == TokenType::kSymbol && Peek().text == symbol) {
      ++pos_;
      return true;
    }
    return false;
  }
  bool ExpectKeyword(std::string_view keyword) {
    if (MatchKeyword(keyword)) return true;
    return Fail("expected keyword " + std::string(keyword));
  }
  bool ExpectSymbol(std::string_view symbol) {
    if (MatchSymbol(symbol)) return true;
    return Fail("expected '" + std::string(symbol) + "'");
  }
  bool ExpectIdentifier(std::string* out) {
    if (Peek().type != TokenType::kIdentifier) {
      return Fail("expected identifier");
    }
    *out = Peek().text;
    ++pos_;
    return true;
  }
  bool ExpectInteger(int32_t* out) {
    if (Peek().type != TokenType::kNumber ||
        Peek().text.find('.') != std::string::npos ||
        Peek().text.find('e') != std::string::npos) {
      return Fail("expected integer");
    }
    const long long value = std::strtoll(Peek().text.c_str(), nullptr, 10);
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
      return Fail("integer out of range");
    }
    *out = static_cast<int32_t>(value);
    ++pos_;
    return true;
  }
  bool Fail(std::string message) {
    if (error_.ok()) {
      error_ = Status(StatusCode::kInvalidArgument,
                      std::move(message) + " near position " +
                          std::to_string(Peek().position));
    }
    return false;
  }

  bool ParseStatement(Statement* statement);
  bool ParseCreateTable(Statement* statement);
  bool ParseDropTable(Statement* statement);
  bool ParseInsert(Statement* statement);
  bool ParseSelect(Statement* statement);
  bool ParseDelete(Statement* statement);
  bool ParseUpdate(Statement* statement);
  bool ParseCopy(Statement* statement);
  bool ParsePragma(Statement* statement);
  bool ParsePragmaArgument(Literal* literal);
  bool ParseWhereClause(std::string* column, std::string* op, Literal* value,
                        bool* has_where);
  bool ParseShow(Statement* statement);
  bool ParseDescribe(Statement* statement);
  bool ParseColumnDef(ColumnSpec* spec);
  bool ParseType(Column* column);
  bool ParseColumnConstraint(ColumnSpec* spec);
  bool ParseTableConstraint(std::vector<ColumnSpec>* columns);
  bool ParseIdentifierList(std::vector<std::string>* names);
  bool ParseLiteral(Literal* literal);
  bool SkipBalancedParentheses();

  std::vector<Token> tokens_;
  size_t pos_ = 0;
  Status error_;
};

Result<std::vector<Statement>> ParseScript(std::string_view sql) {
  auto tokens = Tokenize(sql);
  if (!tokens.ok()) return tokens.status();
  Parser parser(tokens.TakeValue());
  return parser.Parse();
}

Result<std::vector<Statement>> Parser::Parse() {
  std::vector<Statement> statements;
  while (!AtEnd()) {
    if (MatchSymbol(";")) continue;
    Statement statement;
    if (!ParseStatement(&statement)) return error_;
    if (statement.type != StatementType::kEmpty) {
      statements.push_back(std::move(statement));
    }
    if (AtEnd()) break;
    if (!ExpectSymbol(";")) return error_;
  }
  if (!error_.ok()) return error_;
  return statements;
}

bool Parser::ParseStatement(Statement* statement) {
  if (IsKeyword(Peek(), "CREATE")) return ParseCreateTable(statement);
  if (IsKeyword(Peek(), "DROP")) return ParseDropTable(statement);
  if (IsKeyword(Peek(), "INSERT")) return ParseInsert(statement);
  if (IsKeyword(Peek(), "SELECT")) return ParseSelect(statement);
  if (IsKeyword(Peek(), "DELETE")) return ParseDelete(statement);
  if (IsKeyword(Peek(), "UPDATE")) return ParseUpdate(statement);
  if (IsKeyword(Peek(), "COPY")) return ParseCopy(statement);
  if (IsKeyword(Peek(), "PRAGMA")) return ParsePragma(statement);
  if (IsKeyword(Peek(), "SHOW")) return ParseShow(statement);
  if (IsKeyword(Peek(), "DESCRIBE") || IsKeyword(Peek(), "DESC")) {
    return ParseDescribe(statement);
  }
  return Fail("unsupported statement");
}

bool Parser::ParseCreateTable(Statement* statement) {
  if (!ExpectKeyword("CREATE")) return false;
  if (!ExpectKeyword("TABLE")) {
    return Fail("only CREATE TABLE is supported");
  }
  statement->type = StatementType::kCreateTable;
  if (MatchKeyword("IF")) {
    if (!ExpectKeyword("NOT")) return false;
    if (!ExpectKeyword("EXISTS")) return false;
    statement->if_not_exists = true;
  }
  if (!ExpectIdentifier(&statement->table_name)) return false;
  if (!ExpectSymbol("(")) return false;

  for (;;) {
    if (Peek().type == TokenType::kSymbol && Peek().text == ")") break;
    if (IsKeyword(Peek(), "PRIMARY") || IsKeyword(Peek(), "FOREIGN") ||
        IsKeyword(Peek(), "UNIQUE") || IsKeyword(Peek(), "CONSTRAINT") ||
        IsKeyword(Peek(), "CHECK")) {
      if (!ParseTableConstraint(&statement->columns)) return false;
    } else {
      ColumnSpec spec;
      if (!ParseColumnDef(&spec)) return false;
      statement->columns.push_back(std::move(spec));
    }
    if (MatchSymbol(",")) continue;
    break;
  }
  if (!ExpectSymbol(")")) return false;
  if (statement->columns.empty()) {
    return Fail("CREATE TABLE has no columns");
  }
  return true;
}

bool Parser::ParseColumnDef(ColumnSpec* spec) {
  if (!ExpectIdentifier(&spec->name)) return false;
  spec->column.name = spec->name;
  if (!ParseType(&spec->column)) return false;
  for (;;) {
    if (Peek().type != TokenType::kIdentifier) break;
    if (!ParseColumnConstraint(spec)) return false;
  }
  return true;
}

bool Parser::ParseType(Column* column) {
  std::string type_name;
  if (!ExpectIdentifier(&type_name)) return false;

  if (type_name == "integer" || type_name == "int" || type_name == "smallint") {
    column->type = TypeId::kInt32;
    column->length = 0;
    return true;
  }
  if (type_name == "bigint" || type_name == "long") {
    column->type = TypeId::kInt64;
    column->length = 0;
    return true;
  }
  if (type_name == "float" || type_name == "double" || type_name == "real") {
    if (type_name == "double") MatchKeyword("PRECISION");
    column->type = TypeId::kDouble;
    column->length = 0;
    return true;
  }
  if (type_name == "date") {
    column->type = TypeId::kDate;
    column->length = 0;
    return true;
  }
  if (type_name == "decimal" || type_name == "numeric") {
    column->type = TypeId::kDecimal;
    column->length = 18;
    column->scale = 0;
    if (MatchSymbol("(")) {
      int32_t precision = 0;
      if (!ExpectInteger(&precision)) return false;
      column->length = static_cast<uint16_t>(precision);
      if (MatchSymbol(",")) {
        int32_t scale = 0;
        if (!ExpectInteger(&scale)) return false;
        column->scale = static_cast<uint8_t>(scale);
      }
      if (!ExpectSymbol(")")) return false;
    }
    return true;
  }
  if (type_name == "char" || type_name == "character") {
    column->type = TypeId::kChar;
    column->length = 1;
  } else if (type_name == "varchar" || type_name == "character varying" ||
             type_name == "text") {
    column->type = TypeId::kVarchar;
    column->length = 255;
    if (type_name == "text") column->length = 8000;
  } else {
    return Fail("unsupported type " + type_name);
  }
  if (MatchSymbol("(")) {
    int32_t length = 0;
    if (!ExpectInteger(&length)) return false;
    if (length < 1 || length > 8000) return Fail("invalid type length");
    column->length = static_cast<uint16_t>(length);
    if (!ExpectSymbol(")")) return false;
  }
  return true;
}

bool Parser::ParseColumnConstraint(ColumnSpec* spec) {
  if (MatchKeyword("NOT")) {
    if (!ExpectKeyword("NULL")) return false;
    spec->column.nullable = false;
    return true;
  }
  if (MatchKeyword("NULL")) {
    spec->column.nullable = true;
    return true;
  }
  if (MatchKeyword("PRIMARY")) {
    if (!ExpectKeyword("KEY")) return false;
    spec->primary_key = true;
    spec->column.nullable = false;
    return true;
  }
  if (MatchKeyword("UNIQUE")) return true;
  if (MatchKeyword("DEFAULT")) {
    Literal literal;
    return ParseLiteral(&literal);
  }
  if (MatchKeyword("REFERENCES")) {
    std::string table;
    if (!ExpectIdentifier(&table)) return false;
    if (MatchSymbol("(")) {
      std::vector<std::string> columns;
      if (!ParseIdentifierList(&columns)) return false;
      if (!ExpectSymbol(")")) return false;
    }
    return true;
  }
  if (MatchKeyword("CHECK")) {
    if (!ExpectSymbol("(")) return false;
    --pos_;
    return SkipBalancedParentheses();
  }
  return Fail("unsupported column constraint");
}

bool Parser::ParseTableConstraint(std::vector<ColumnSpec>* columns) {
  if (MatchKeyword("CONSTRAINT")) {
    std::string name;
    if (!ExpectIdentifier(&name)) return false;
  }
  if (MatchKeyword("PRIMARY")) {
    if (!ExpectKeyword("KEY")) return false;
    if (!ExpectSymbol("(")) return false;
    std::vector<std::string> names;
    if (!ParseIdentifierList(&names)) return false;
    if (!ExpectSymbol(")")) return false;
    for (const std::string& name : names) {
      for (ColumnSpec& column : *columns) {
        if (column.name == name) column.column.nullable = false;
      }
    }
    return true;
  }
  if (MatchKeyword("FOREIGN")) {
    if (!ExpectKeyword("KEY")) return false;
    if (!ExpectSymbol("(")) return false;
    std::vector<std::string> names;
    if (!ParseIdentifierList(&names)) return false;
    if (!ExpectSymbol(")")) return false;
    if (!ExpectKeyword("REFERENCES")) return false;
    std::string table;
    if (!ExpectIdentifier(&table)) return false;
    if (MatchSymbol("(")) {
      std::vector<std::string> referenced;
      if (!ParseIdentifierList(&referenced)) return false;
      if (!ExpectSymbol(")")) return false;
    }
    return true;
  }
  if (MatchKeyword("UNIQUE")) {
    if (!ExpectSymbol("(")) return false;
    std::vector<std::string> names;
    if (!ParseIdentifierList(&names)) return false;
    return ExpectSymbol(")");
  }
  if (MatchKeyword("CHECK")) {
    if (!ExpectSymbol("(")) return false;
    --pos_;
    return SkipBalancedParentheses();
  }
  return Fail("unsupported table constraint");
}

bool Parser::ParseIdentifierList(std::vector<std::string>* names) {
  for (;;) {
    std::string name;
    if (!ExpectIdentifier(&name)) return false;
    names->push_back(name);
    if (MatchSymbol(",")) continue;
    break;
  }
  return true;
}

bool Parser::SkipBalancedParentheses() {
  if (!ExpectSymbol("(")) return false;
  int depth = 1;
  while (depth > 0) {
    if (AtEnd()) return Fail("unbalanced parentheses");
    if (Peek().type == TokenType::kSymbol && Peek().text == "(") ++depth;
    if (Peek().type == TokenType::kSymbol && Peek().text == ")") --depth;
    ++pos_;
  }
  return true;
}

bool Parser::ParseDropTable(Statement* statement) {
  if (!ExpectKeyword("DROP")) return false;
  if (!ExpectKeyword("TABLE")) return Fail("only DROP TABLE is supported");
  statement->type = StatementType::kDropTable;
  if (MatchKeyword("IF")) {
    if (!ExpectKeyword("EXISTS")) return false;
    statement->if_exists = true;
  }
  return ExpectIdentifier(&statement->table_name);
}

bool Parser::ParseInsert(Statement* statement) {
  if (!ExpectKeyword("INSERT")) return false;
  if (!ExpectKeyword("INTO")) return false;
  statement->type = StatementType::kInsert;
  if (!ExpectIdentifier(&statement->table_name)) return false;
  if (MatchSymbol("(")) {
    if (!ParseIdentifierList(&statement->insert.columns)) return false;
    if (!ExpectSymbol(")")) return false;
  }
  if (!ExpectKeyword("VALUES")) return false;
  for (;;) {
    if (!ExpectSymbol("(")) return false;
    std::vector<Literal> row;
    if (!MatchSymbol(")")) {
      for (;;) {
        Literal literal;
        if (!ParseLiteral(&literal)) return false;
        row.push_back(std::move(literal));
        if (MatchSymbol(",")) continue;
        if (!ExpectSymbol(")")) return false;
        break;
      }
    }
    statement->insert.rows.push_back(std::move(row));
    if (!MatchSymbol(",")) break;
  }
  return true;
}

bool Parser::ParseLiteral(Literal* literal) {
  bool negative = false;
  bool signed_number = false;
  if (MatchSymbol("-")) {
    negative = true;
    signed_number = true;
  } else if (MatchSymbol("+")) {
    signed_number = true;
  }

  if (Peek().type == TokenType::kNumber) {
    const std::string text = Peek().text;
    ++pos_;
    literal->text = (negative ? "-" : "") + text;
    if (text.find('.') != std::string::npos ||
        text.find('e') != std::string::npos ||
        text.find('E') != std::string::npos) {
      literal->kind = Literal::Kind::kFloat;
      literal->floating = std::strtod(literal->text.c_str(), nullptr);
    } else {
      literal->kind = Literal::Kind::kInteger;
      literal->integer = std::strtoll(literal->text.c_str(), nullptr, 10);
    }
    return true;
  }
  if (signed_number) return Fail("expected number");
  if (Peek().type == TokenType::kString) {
    literal->kind = Literal::Kind::kString;
    literal->text = Peek().text;
    ++pos_;
    return true;
  }
  if (MatchKeyword("NULL")) {
    literal->kind = Literal::Kind::kNull;
    return true;
  }
  return Fail("expected literal");
}

bool Parser::ParseSelect(Statement* statement) {
  if (!ExpectKeyword("SELECT")) return false;
  statement->type = StatementType::kSelect;
  // Items are column names or '*'; "SELECT rid, *" mixes both.
  for (;;) {
    std::string item;
    if (MatchSymbol("*")) {
      item = "*";
    } else if (!ExpectIdentifier(&item)) {
      return false;
    }
    statement->select.columns.push_back(std::move(item));
    if (!MatchSymbol(",")) break;
  }
  if (statement->select.columns.size() == 1 &&
      statement->select.columns[0] == "*") {
    statement->select.star = true;
    statement->select.columns.clear();
  }
  if (!ExpectKeyword("FROM")) return false;
  if (!ExpectIdentifier(&statement->table_name)) return false;
  if (!ParseWhereClause(&statement->select.where_column,
                        &statement->select.where_op,
                        &statement->select.where_value,
                        &statement->select.has_where)) {
    return false;
  }
  if (MatchKeyword("LIMIT")) {
    int32_t limit = 0;
    if (!ExpectInteger(&limit)) return false;
    if (limit < 0) return Fail("negative LIMIT");
    statement->select.limit = limit;
  }
  return true;
}

bool Parser::ParseDelete(Statement* statement) {
  if (!ExpectKeyword("DELETE")) return false;
  if (!ExpectKeyword("FROM")) return false;
  statement->type = StatementType::kDelete;
  if (!ExpectIdentifier(&statement->table_name)) return false;
  return ParseWhereClause(&statement->delete_spec.where_column,
                          &statement->delete_spec.where_op,
                          &statement->delete_spec.where_value,
                          &statement->delete_spec.has_where);
}

bool Parser::ParseUpdate(Statement* statement) {
  if (!ExpectKeyword("UPDATE")) return false;
  statement->type = StatementType::kUpdate;
  if (!ExpectIdentifier(&statement->table_name)) return false;
  if (!ExpectKeyword("SET")) return false;
  for (;;) {
    Assignment assignment;
    if (!ExpectIdentifier(&assignment.column)) return false;
    if (!ExpectSymbol("=")) return false;
    if (!ParseLiteral(&assignment.value)) return false;
    statement->update.assignments.push_back(std::move(assignment));
    if (!MatchSymbol(",")) break;
  }
  return ParseWhereClause(&statement->update.where_column,
                          &statement->update.where_op,
                          &statement->update.where_value,
                          &statement->update.has_where);
}

bool Parser::ParseCopy(Statement* statement) {
  if (!ExpectKeyword("COPY")) return false;
  statement->type = StatementType::kCopy;
  if (!ExpectIdentifier(&statement->table_name)) return false;
  if (!ExpectKeyword("FROM")) return false;
  if (Peek().type != TokenType::kString) return Fail("expected file path string");
  statement->copy.path = Peek().text;
  ++pos_;
  if (MatchKeyword("DELIMITER")) {
    if (Peek().type != TokenType::kString || Peek().text.size() != 1) {
      return Fail("DELIMITER must be a one-character string");
    }
    statement->copy.delimiter = Peek().text[0];
    ++pos_;
  }
  return true;
}

bool Parser::ParsePragma(Statement* statement) {
  if (!ExpectKeyword("PRAGMA")) return false;
  statement->type = StatementType::kPragma;
  if (!ExpectIdentifier(&statement->pragma.name)) return false;
  if (MatchSymbol("=")) {
    statement->pragma.has_value = true;
    return ParsePragmaArgument(&statement->pragma.value);
  }
  if (MatchSymbol("(")) {
    if (MatchSymbol(")")) return true;
    for (;;) {
      Literal argument;
      if (!ParsePragmaArgument(&argument)) return false;
      statement->pragma.args.push_back(std::move(argument));
      if (MatchSymbol(",")) continue;
      return ExpectSymbol(")");
    }
  }
  return true;
}

bool Parser::ParsePragmaArgument(Literal* literal) {
  if (Peek().type == TokenType::kIdentifier) {
    literal->kind = Literal::Kind::kString;
    literal->text = Peek().text;
    ++pos_;
    return true;
  }
  return ParseLiteral(literal);
}

bool Parser::ParseWhereClause(std::string* column, std::string* op,
                              Literal* value, bool* has_where) {
  *has_where = false;
  if (!MatchKeyword("WHERE")) return true;
  *has_where = true;
  if (!ExpectIdentifier(column)) return false;
  if (Peek().type != TokenType::kSymbol) return Fail("expected comparison");
  *op = Peek().text;
  if (*op != "=" && *op != "<>" && *op != "!=" && *op != "<" && *op != "<=" &&
      *op != ">" && *op != ">=") {
    return Fail("unsupported comparison operator");
  }
  ++pos_;
  return ParseLiteral(value);
}

bool Parser::ParseShow(Statement* statement) {
  if (!ExpectKeyword("SHOW")) return false;
  if (!ExpectKeyword("TABLES")) return Fail("only SHOW TABLES is supported");
  statement->type = StatementType::kShowTables;
  return true;
}

bool Parser::ParseDescribe(Statement* statement) {
  if (!(MatchKeyword("DESCRIBE") || MatchKeyword("DESC"))) return false;
  statement->type = StatementType::kDescribe;
  return ExpectIdentifier(&statement->table_name);
}

}  // namespace minidb
