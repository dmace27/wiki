#include "kc/storage/database.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <utility>

namespace kc::storage {
namespace {

std::string error_message(sqlite3* connection, const std::string_view action) {
  return std::string(action) + ": " + (connection == nullptr ? "unknown SQLite error" : sqlite3_errmsg(connection));
}

class Statement {
 public:
  Statement(sqlite3* connection, const std::string_view sql) : connection_(connection) {
    const auto query = std::string(sql);
    if (sqlite3_prepare_v2(connection_, query.c_str(), -1, &statement_, nullptr) != SQLITE_OK) {
      throw StorageError(error_message(connection_, "failed to prepare SQLite statement"));
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

 private:
  sqlite3* connection_;
  sqlite3_stmt* statement_{nullptr};
};

}  // namespace

Database::Database(const std::filesystem::path& path) {
  const auto filename = path.string();
  const auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(filename.c_str(), &connection_, flags, nullptr) != SQLITE_OK) {
    const auto message = error_message(connection_, "failed to open state database");
    sqlite3_close(connection_);
    connection_ = nullptr;
    throw StorageError(message);
  }
  sqlite3_busy_timeout(connection_, 5'000);
  execute("PRAGMA foreign_keys = ON;");
}

Database::~Database() {
  if (connection_ != nullptr) {
    sqlite3_close(connection_);
  }
}

Database::Database(Database&& other) noexcept : connection_(std::exchange(other.connection_, nullptr)) {}

Database& Database::operator=(Database&& other) noexcept {
  if (this != &other) {
    if (connection_ != nullptr) {
      sqlite3_close(connection_);
    }
    connection_ = std::exchange(other.connection_, nullptr);
  }
  return *this;
}

void Database::execute(const std::string_view sql) {
  const auto query = std::string(sql);
  char* raw_error = nullptr;
  const auto result = sqlite3_exec(connection_, query.c_str(), nullptr, nullptr, &raw_error);
  if (result != SQLITE_OK) {
    const std::string message = raw_error == nullptr ? error_message(connection_, "SQLite execution failed")
                                                     : std::string(raw_error);
    sqlite3_free(raw_error);
    throw StorageError(message);
  }
}

std::int64_t Database::scalar_integer(const std::string_view sql) const {
  Statement statement(connection_, sql);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw StorageError(error_message(connection_, "SQLite query returned no row"));
  }
  return sqlite3_column_int64(statement.get(), 0);
}

std::string Database::scalar_text(const std::string_view sql) const {
  Statement statement(connection_, sql);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw StorageError(error_message(connection_, "SQLite query returned no row"));
  }
  const auto* value = sqlite3_column_text(statement.get(), 0);
  return value == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(value));
}

}  // namespace kc::storage

