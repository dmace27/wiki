#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace kc::test {

/// Load a JSON fixture from disk, throwing a useful error if it cannot be read.
inline nlohmann::json read_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("could not open fixture: " + path.string());
  }
  return nlohmann::json::parse(input);
}

/// Owns a unique scratch directory for the lifetime of one test scope.
///
/// This is RAII: the constructor creates the directory and the destructor
/// removes it, even when a test assertion throws.
class TemporaryDirectory {
 public:
  /// Try several unique names until an unused temporary directory is created.
  TemporaryDirectory() {
    const auto base = std::filesystem::temp_directory_path();
    for (unsigned attempt = 0; attempt < 100U; ++attempt) {
      path_ = base / ("kc-test-" + std::to_string(
                                  std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(path_)) {
        return;
      }
    }
    throw std::runtime_error("could not create test directory");
  }

  /// Recursively clean up files created by the test.
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  /// Return the directory tests may safely create files inside.
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace kc::test
