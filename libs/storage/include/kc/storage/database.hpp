#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;

namespace kc::storage {

/// Exception thrown when opening or using the project SQLite database fails.
class StorageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Owns one SQLite connection using RAII.
///
/// Constructing opens the database and enables foreign-key enforcement;
/// destruction closes it. Copying is forbidden because two owners must not
/// close the same connection, while moving safely transfers ownership.
class Database {
 public:
  /// Open `path`, creating the database file if it does not exist.
  explicit Database(const std::filesystem::path& path);
  /// Close the owned SQLite connection.
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  /// Transfer an open connection from another Database object.
  Database(Database&& other) noexcept;
  Database& operator=(Database&& other) noexcept;

  /// Execute one or more SQL statements that do not need returned rows.
  void execute(std::string_view sql);
  /// Execute a query and return column 0 of its first row as an integer.
  [[nodiscard]] std::int64_t scalar_integer(std::string_view sql) const;
  /// Execute a query and return column 0 of its first row as text.
  [[nodiscard]] std::string scalar_text(std::string_view sql) const;
  /// Borrow the raw handle for SQLite operations not wrapped by this class.
  /// The caller must not close it and must not keep it after this object dies.
  [[nodiscard]] sqlite3* native_handle() noexcept { return connection_; }

 private:
  sqlite3* connection_{nullptr};
};

}  // namespace kc::storage
