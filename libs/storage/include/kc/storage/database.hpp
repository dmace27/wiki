#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;

namespace kc::storage {

class StorageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Database {
 public:
  explicit Database(const std::filesystem::path& path);
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&& other) noexcept;
  Database& operator=(Database&& other) noexcept;

  void execute(std::string_view sql);
  [[nodiscard]] std::int64_t scalar_integer(std::string_view sql) const;
  [[nodiscard]] std::string scalar_text(std::string_view sql) const;
  [[nodiscard]] sqlite3* native_handle() noexcept { return connection_; }

 private:
  sqlite3* connection_{nullptr};
};

}  // namespace kc::storage

