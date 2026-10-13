#pragma once

#include "kc/domain/types.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kc::storage {
class Database;

/// An injectable boundary observer for crash/error tests. Production has no
/// environment-triggered failure points; an empty observer has no side effects.
using DurableObserver = std::function<void(std::string_view)>;

/// A new immutable source copy owned by this operation; existing copies are
/// excluded so failed operations never remove somebody else's retained bytes.
struct FileCopy {
  std::filesystem::path retained;
  std::filesystem::path destination;
  std::string sha256;
};

/// Filesystem half of a SQLite/file transaction. Caller holds BEGIN IMMEDIATE
/// throughout prepare/replace/DB publication. The durable journal contains the
/// exact previous/new bytes and every owned artifact before any vault mutation.
class FileOperation {
 public:
  FileOperation(std::filesystem::path root, domain::ProjectConfig config,
                std::string operation_id, std::filesystem::path destination,
                std::optional<std::string> before, std::string after,
                std::vector<FileCopy> copies = {}, DurableObserver observer = {});
  void prepare();
  void replace();
  /// Remove journal/temporary artifacts only AFTER SQLite COMMIT succeeds.
  void finish();
  [[nodiscard]] std::optional<std::filesystem::path> backup_path() const;
 private:
  struct State;
  std::shared_ptr<State> state_;
};

/// Synchronize complete file data and its parent directory. All durability
/// failures propagate; no stream flush is presented as stable-storage sync.
void durable_write(const std::filesystem::path& path, std::string_view bytes);
void durable_directories(const std::filesystem::path& path);
void durable_remove(const std::filesystem::path& path);

/// Called by Database on project open. Acquires the SQLite write lock before
/// recovery, preserves conflicting user bytes, and reconciles abandoned
/// extraction runs using per-run OS leases rather than timing guesses.
void recover_on_open(Database& database, const std::filesystem::path& state_path,
                     bool read_only, DurableObserver observer = {});
/// Explicitly keep user bytes for a conflicted recovery. Active journals and
/// original/new/displaced bytes are archived for review rather than deleted.
void keep_recovery_conflicts(const std::filesystem::path& root);

/// Held for the entire external extraction attempt. OS locks vanish on process
/// death; stale lease files remain identifiable and safe to reconcile.
class ExtractionLease {
 public:
  ExtractionLease(const std::filesystem::path& cache, std::string run_id);
  ~ExtractionLease();
  ExtractionLease(const ExtractionLease&) = delete;
  ExtractionLease& operator=(const ExtractionLease&) = delete;
 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace kc::storage
