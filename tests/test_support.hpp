#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace kc::test {

inline nlohmann::json read_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("could not open fixture: " + path.string());
  }
  return nlohmann::json::parse(input);
}

class TemporaryDirectory {
 public:
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

  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace kc::test
