#include "kc/storage/file_operation.hpp"
#include "kc/storage/database.hpp"
#include "kc/domain/id.hpp"
#include "kc/domain/json.hpp"
#include "kc/domain/path.hpp"
#include "kc/domain/sha256.hpp"
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cerrno>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#endif

namespace kc::storage {
namespace {
using nlohmann::json;

[[noreturn]] void io_error(const std::string& action) {
#if defined(_WIN32)
  throw StorageError(action + ": " + std::system_category().message(static_cast<int>(GetLastError())));
#else
  throw StorageError(action + ": " + std::generic_category().message(errno));
#endif
}

/// Leases protect live extraction attempts while another process opens state.
class Lease {
 public:
  explicit Lease(const std::filesystem::path& path, bool wait) {
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) io_error("open extraction lease");
    OVERLAPPED position{};
    acquired_ = LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0U : LOCKFILE_FAIL_IMMEDIATELY),
                           0, 1, 0, &position) != 0;
#else
    handle_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (handle_ < 0) io_error("open extraction lease");
    acquired_ = flock(handle_, LOCK_EX | (wait ? 0 : LOCK_NB)) == 0;
#endif
  }
  ~Lease() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (handle_ >= 0) ::close(handle_);
#endif
  }
  bool acquired() const { return acquired_; }
 private:
#if defined(_WIN32)
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int handle_{-1};
#endif
  bool acquired_{false};
};

void sync_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  // File renames below use WRITE_THROUGH. Windows has no supported equivalent
  // to POSIX directory fsync; only macOS arm64 is currently advertised/tested.
  static_cast<void>(path);
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) io_error("open parent directory for sync");
  const auto result = fsync(fd);
  const auto saved = errno;
  ::close(fd);
  errno = saved;
  if (result != 0) io_error("sync parent directory");
#endif
}

/// Fail closed on symlinked ancestors, including in-root aliases: journals own
/// exact paths, and must not delete files through an alias during recovery.
void safe_path(const std::filesystem::path& owner, const std::filesystem::path& path) {
  if (!domain::is_within(owner, path)) throw StorageError("recovery path escapes its owner");
  auto base = owner.lexically_normal();
  while (base.filename().empty() && base.parent_path()!=base) base=base.parent_path();
  for (auto part = path.lexically_normal(); part != base && !part.empty();) {
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(part))) {
      throw StorageError("recovery path contains a symlink");
    }
    const auto parent = part.parent_path();
    if (parent == part) break;
    part = parent;
  }
}

std::string read_bytes(const std::filesystem::path& path) {
  if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path)) {
    throw StorageError("recovery expected a regular file: " + domain::path_to_utf8(path));
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) throw StorageError("cannot read recovery file");
  std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) throw StorageError("incomplete recovery file read");
  return bytes;
}

std::optional<std::string> current_bytes(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return std::nullopt;
  return read_bytes(path);
}

/// Native atomic exchange retains the exact displaced bytes. Unlike a
/// read-then-rename check, it detects an editor's atomic save in the final gap.
void exchange_files(const std::filesystem::path& staged, const std::filesystem::path& target) {
#if defined(__APPLE__)
  if (renameatx_np(AT_FDCWD, staged.c_str(), AT_FDCWD, target.c_str(), RENAME_SWAP) != 0) {
    io_error("exchange article with staged file");
  }
#elif defined(__linux__)
  if (syscall(SYS_renameat2, AT_FDCWD, staged.c_str(), AT_FDCWD, target.c_str(), RENAME_EXCHANGE) != 0) {
    io_error("exchange article with staged file");
  }
#else
  // A Windows replacement keeps the displaced file under a separate name.
  // This implementation is deliberately unavailable until a native recovery
  // test matrix can establish equivalent exchange semantics.
  static_cast<void>(staged); static_cast<void>(target);
  throw StorageError("durable replacement requires a tested atomic-exchange platform");
#endif
  sync_directory(target.parent_path());
}

void rename_exclusive(const std::filesystem::path& source, const std::filesystem::path& target) {
#if defined(__APPLE__)
  if (renameatx_np(AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_EXCL) != 0) io_error("publish new file exclusively");
#elif defined(__linux__)
  if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) != 0) io_error("publish new file exclusively");
#else
  if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) io_error("publish new file exclusively");
#endif
  sync_directory(target.parent_path());
}

void atomic_json(const std::filesystem::path& path, const json& value) {
  auto pending = path; pending += ".pending";
  durable_remove(pending);
  durable_write(pending, value.dump());
#if defined(_WIN32)
  if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) io_error("publish journal");
#else
  std::filesystem::rename(pending, path);
  sync_directory(path.parent_path());
#endif
}

/// Queries take identifiers as bound data, including journal IDs loaded from
/// disk. The connection already belongs to the caller's recovery transaction.
bool operation_committed(Database& db, const std::string& id) {
  sqlite3_stmt* row = nullptr;
  if (sqlite3_prepare_v2(db.native_handle(), "SELECT 1 FROM file_operations WHERE operation_id=?", -1, &row, nullptr) != SQLITE_OK) {
    throw StorageError("durable journal requires migration 007; run kc init");
  }
  sqlite3_bind_text(row, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  const int result = sqlite3_step(row);
  sqlite3_finalize(row);
  if (result != SQLITE_ROW && result != SQLITE_DONE) throw StorageError("cannot read operation commit marker");
  return result == SQLITE_ROW;
}

std::filesystem::path resolve(const std::filesystem::path& root, const std::filesystem::path& owner,
                              const json& value) {
  const auto relative = domain::path_from_utf8(value.get<std::string>());
  if (relative.is_absolute() || relative.empty()) throw StorageError("invalid recovery relative path");
  const auto path = root / relative;
  safe_path(owner, path);
  return path;
}

/// A conflict never clobbers an external save. Keep the journal, staged bytes,
/// and prior backup available until an explicit keep-user-file recovery.
[[noreturn]] void conflict(const std::filesystem::path& journal) {
  throw StorageError("recovery found newer user bytes; no file was overwritten. Review " +
      domain::path_to_utf8(journal) + " then run kc recover --keep-user-file, or restore the recorded bytes and retry");
}

/// An editor holding an old inode can keep writing the displaced file after
/// exchange. Keep any unexpected displaced bytes even after SQLite committed;
/// recovery must not silently discard that independent user save.
void preserve_displaced(const std::filesystem::path& root, const domain::ProjectConfig& config,
    const std::string& id, const std::filesystem::path& temporary,
    const std::optional<std::string>& before, const std::string& after) {
  const auto bytes = current_bytes(temporary);
  if (!bytes || bytes == before || *bytes == after) return;
  const auto archive = root / config.paths.cache / "recovery-conflicts";
  safe_path(root / config.paths.cache, archive);
  durable_directories(archive);
  const auto path = archive / (id + "-displaced-" + domain::sha256_text(*bytes) + ".md");
  if (!std::filesystem::exists(path)) durable_write(path, *bytes);
  else if (read_bytes(path) != *bytes) throw StorageError("displaced recovery archive has changed");
}

void recover_journal(Database& db, const std::filesystem::path& root,
                     const domain::ProjectConfig& config, const std::filesystem::path& journal,
                     const DurableObserver& observer = {}) {
  safe_path(root / config.paths.cache / "transactions", journal);
  const auto record = json::parse(read_bytes(journal));
  const auto id = record.at("id").get<std::string>();
  if (!domain::is_valid_id(id, "run_") || journal.filename() != id + ".json") {
    throw StorageError("invalid journal identity");
  }
  const auto target = resolve(root, root / config.paths.vault, record.at("destination"));
  const auto temp = resolve(root, root / config.paths.vault, record.at("temporary"));
  auto expected_temp = target; expected_temp += ".kc-" + id + ".tmp";
  if (temp != expected_temp) throw StorageError("journal temporary path is not owned by this operation");
  const auto before = record.at("before").is_null() ? std::optional<std::string>{}
                                                    : std::optional<std::string>{record.at("before").get<std::string>()};
  const auto after = record.at("after").get<std::string>();
  if (domain::sha256_text(after) != record.at("after_sha256").get<std::string>() ||
      (before && domain::sha256_text(*before) != record.at("before_sha256").get<std::string>())) {
    throw StorageError("journal content digest mismatch; preserve journal for manual recovery");
  }
  const auto point = [&](std::string_view name) { if (observer) observer(name); };
  const bool committed = operation_committed(db, id);
  if (!committed && record.at("phase") != "prepared") {
    const auto actual = current_bytes(target);
    // If exchange raced an editor, the displaced file is the authoritative
    // pre-replacement content, not the earlier preview. Preserve that save.
    auto restore = before;
    const auto displaced = current_bytes(temp);
    if (displaced && *displaced != after) restore = displaced;
    auto recovery_path = temp; recovery_path += ".recovery";
    const auto recovery_bytes = current_bytes(recovery_path);
    if (recovery_bytes && *recovery_bytes != after && recovery_bytes != restore) conflict(journal);
    if (actual == std::optional<std::string>{after}) {
      if (restore) {
        auto recovery = temp; recovery += ".recovery";
        point("before_recovery_stage");
        durable_remove(recovery);
        durable_write(recovery, *restore);
        point("after_recovery_stage");
        point("before_recovery_replace");
        exchange_files(recovery, target);
        point("after_recovery_exchange");
        if (read_bytes(recovery) != after) {
          // A second external save also survives. Exchange back only when our
          // own bytes still occupy the destination; otherwise retain both.
          if (current_bytes(target) == restore) exchange_files(recovery, target);
          conflict(journal);
        }
        point("after_recovery_replace");
        durable_remove(recovery);
      } else {
        // Move aside instead of unlinking by pathname after an unsafe check.
        auto removed = temp; removed += ".recovery";
        rename_exclusive(target, removed);
        if (read_bytes(removed) != after) {
          if (!std::filesystem::exists(target)) rename_exclusive(removed, target);
          conflict(journal);
        }
        durable_remove(removed);
      }
    } else if (actual != restore) {
      conflict(journal);
    }
  }
  if (committed) preserve_displaced(root, config, id, temp, before, after);
  point("before_recovery_cleanup");
  // Cleanup is idempotent. Only this operation's newly created source copies
  // and backup are removed on rollback; previously existing copies are absent
  // from the manifest. Committed copies/backups are retained as history.
  for (const auto& copy : record.at("copies")) {
    const auto destination = resolve(root, root / config.paths.vault / config.vault.source_directory, copy.at("destination"));
    const auto staging = resolve(root, root / config.paths.vault, copy.at("temporary"));
    auto owned = destination; owned += ".kc-" + id + ".tmp";
    if (staging != owned) throw StorageError("unowned source staging path in journal");
    durable_remove(staging);
    if (!committed && std::filesystem::exists(destination)) {
      if (domain::sha256_file(destination) != copy.at("sha256").get<std::string>()) conflict(journal);
      durable_remove(destination);
    }
  }
  if (!committed && !record.at("backup").is_null()) {
    const auto backup = resolve(root, root / config.paths.cache / "backups" / "transactions", record.at("backup"));
    if (backup.filename() != id + ".md") throw StorageError("unowned backup path");
    if (std::filesystem::exists(backup)) {
      const auto bytes = read_bytes(backup);
      // A short backup can result from ENOSPC before article replacement. Its
      // bytes are redundant with the fully synced journal, so safe rollback
      // removes a matching prefix; unrelated changes require explicit review.
      if (!before || !before->starts_with(bytes)) conflict(journal);
    }
    durable_remove(backup);
  }
  durable_remove(temp);
  auto recovery = temp; recovery += ".recovery";
  durable_remove(recovery);
  durable_remove(journal);
  auto pending = journal; pending += ".pending";
  durable_remove(pending);
  point("after_recovery_cleanup");
}

std::optional<std::pair<std::filesystem::path, domain::ProjectConfig>> project_for_state(const std::filesystem::path& state) {
  const auto absolute = std::filesystem::absolute(state).lexically_normal();
  for (auto parent = absolute.parent_path(); !parent.empty();) {
    const auto config_path = parent / "kc.json";
    if (std::filesystem::is_regular_file(config_path)) {
      std::ifstream input(config_path);
      const auto parsed = domain::parse_project_config(json::parse(input));
      if (parsed && std::filesystem::weakly_canonical(parent / parsed.value->paths.state) ==
                    std::filesystem::weakly_canonical(absolute)) {
        domain::validate_project_paths(parent, *parsed.value);
        return std::pair{parent, *parsed.value};
      }
    }
    const auto next = parent.parent_path();
    if (next == parent) break;
    parent = next;
  }
  return std::nullopt;
}

/// Reconcile only lease-free runs. A running adapter may have no SQLite lock,
/// but its OS lease prevents another command from treating it as abandoned.
void reconcile_extractions(Database& db, const std::filesystem::path& root,
                           const domain::ProjectConfig& config) {
  if (db.scalar_integer("SELECT COUNT(*) FROM sqlite_master WHERE name='extraction_runs'") == 0) return;
  sqlite3_stmt* rows = nullptr;
  if (sqlite3_prepare_v2(db.native_handle(), "SELECT run_id FROM extraction_runs WHERE status='running'", -1, &rows, nullptr) != SQLITE_OK) {
    throw StorageError("cannot inspect interrupted extraction runs");
  }
  std::vector<std::string> ids;
  while (sqlite3_step(rows) == SQLITE_ROW) ids.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(rows, 0)));
  sqlite3_finalize(rows);
  for (const auto& id : ids) {
    if (!domain::is_valid_id(id, "run_")) continue; // Never turn arbitrary database data into a deletion path.
    const auto cache = root / config.paths.cache;
    const auto lease_path = cache / "extraction-leases" / (id + ".lock");
    safe_path(cache, lease_path);
    durable_directories(lease_path.parent_path());
    Lease lease(lease_path, false);
    if (!lease.acquired()) continue;
    db.execute("BEGIN IMMEDIATE");
    try {
      // A completed run may have committed after our first read. Preserve all
      // directories with referenced images, regardless of the run's status.
      sqlite3_stmt* update = nullptr;
      sqlite3_prepare_v2(db.native_handle(), "UPDATE extraction_runs SET status='failed', completed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'), error_message='interrupted extraction; retry' WHERE run_id=? AND status='running'", -1, &update, nullptr);
      sqlite3_bind_text(update, 1, id.c_str(), -1, SQLITE_TRANSIENT);
      const auto result = sqlite3_step(update);
      sqlite3_finalize(update);
      if (result != SQLITE_DONE) throw StorageError("cannot mark interrupted extraction");
      // Adapter staging is private. Page images are removed only when no row
      // (including historical citations) references a path in that directory.
      const auto staging = cache / "extraction-staging" / id;
      safe_path(cache, staging);
      std::filesystem::remove_all(staging);
      sync_directory(staging.parent_path());
      const auto pages = cache / "pages";
      if (std::filesystem::is_directory(pages)) {
        for (const auto& version : std::filesystem::directory_iterator(pages)) {
          if (!version.is_directory() || version.is_symlink()) continue;
          const auto output = version.path() / id;
          safe_path(cache, output);
          sqlite3_stmt* referenced = nullptr;
          sqlite3_prepare_v2(db.native_handle(), "SELECT 1 FROM source_pages WHERE substr(image_path,1,?)=? LIMIT 1", -1, &referenced, nullptr);
          const auto prefix = domain::path_to_utf8(output.lexically_relative(root)) + "/";
          sqlite3_bind_int(referenced, 1, static_cast<int>(prefix.size()));
          sqlite3_bind_text(referenced, 2, prefix.c_str(), -1, SQLITE_TRANSIENT);
          const bool used = sqlite3_step(referenced) == SQLITE_ROW;
          sqlite3_finalize(referenced);
          if (!used) { std::filesystem::remove_all(output); sync_directory(version.path()); }
        }
      }
      db.execute("COMMIT");
    } catch (...) { db.execute("ROLLBACK"); throw; }
    // Keep lease files: deleting a locked inode permits a second independent
    // lock at the same path. They are small ownership records, not temp images.
  }
}
} // namespace

void durable_directories(const std::filesystem::path& path) {
  if (path.empty()) throw StorageError("empty durable directory path");
  if (std::filesystem::is_directory(path)) return;
  durable_directories(path.parent_path());
  std::filesystem::create_directory(path);
  sync_directory(path.parent_path());
  sync_directory(path);
}

void durable_write(const std::filesystem::path& path, std::string_view bytes) {
#if defined(_WIN32)
  const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) io_error("create durable file");
  DWORD written = 0;
  const bool success = bytes.size() <= MAXDWORD && WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                       written == bytes.size() && FlushFileBuffers(handle);
  const auto saved = GetLastError(); CloseHandle(handle); SetLastError(saved);
  if (!success) io_error("write/sync durable file");
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) io_error("create durable file");
  std::size_t offset = 0;
  bool success = true;
  while (offset < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { success = false; break; }
    offset += static_cast<std::size_t>(count);
  }
  if (success) success = fsync(fd) == 0;
#if defined(__APPLE__)
  // fsync reaches the OS; F_FULLFSYNC also asks the device to flush its cache.
  if (success) success = fcntl(fd, F_FULLFSYNC) == 0;
#endif
  const auto saved = errno; ::close(fd); errno = saved;
  if (!success) io_error("write/sync durable file");
#endif
  sync_directory(path.parent_path());
}

void durable_remove(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path) && !std::filesystem::is_symlink(path)) return;
  std::filesystem::remove(path);
  sync_directory(path.parent_path());
}

struct FileOperation::State {
  std::filesystem::path root;
  domain::ProjectConfig config;
  std::string id;
  json record;
  std::filesystem::path journal;
  DurableObserver observer;
  void point(std::string_view name) { if (observer) observer(name); }
};

FileOperation::FileOperation(std::filesystem::path root, domain::ProjectConfig config,
    std::string id, std::filesystem::path destination, std::optional<std::string> before,
    std::string after, std::vector<FileCopy> copies, DurableObserver observer)
    : state_(std::make_shared<State>()) {
  auto& s = *state_;
  s.root = std::move(root); s.config = std::move(config); s.id = std::move(id); s.observer = std::move(observer);
  if (!domain::is_valid_id(s.id, "run_")) throw StorageError("invalid file operation ID");
  safe_path(s.root / s.config.paths.vault, s.root / destination);
  auto temporary = destination; temporary += ".kc-" + s.id + ".tmp";
  const auto backup = s.config.paths.cache / "backups" / "transactions" / (s.id + ".md");
  s.journal = s.root / s.config.paths.cache / "transactions" / (s.id + ".json");
  safe_path(s.root / s.config.paths.cache, s.journal);
  s.record = {{"id",s.id},{"phase","prepared"},{"destination",domain::path_to_utf8(destination)},
      {"temporary",domain::path_to_utf8(temporary)}, {"before",before ? json(*before) : json(nullptr)},
      {"before_sha256",before ? json(domain::sha256_text(*before)) : json(nullptr)},
      {"after",after},{"after_sha256",domain::sha256_text(after)},
      {"backup",before ? json(domain::path_to_utf8(backup)) : json(nullptr)}, {"copies",json::array()}};
  std::set<std::filesystem::path> destinations;
  for (const auto& copy : copies) {
    if (!destinations.insert(copy.destination).second) continue;
    safe_path(s.root / s.config.paths.sources, s.root / copy.retained);
    safe_path(s.root / s.config.paths.vault / s.config.vault.source_directory, s.root / copy.destination);
    auto temp = copy.destination; temp += ".kc-" + s.id + ".tmp";
    s.record["copies"].push_back({{"retained",domain::path_to_utf8(copy.retained)},
        {"destination",domain::path_to_utf8(copy.destination)}, {"temporary",domain::path_to_utf8(temp)}, {"sha256",copy.sha256}});
  }
}

void FileOperation::prepare() {
  auto& s = *state_;
  s.point("before_journal");
  durable_directories(s.journal.parent_path());
  atomic_json(s.journal, s.record);
  s.point("after_journal");
  s.point("before_backup");
  if (!s.record["backup"].is_null()) {
    const auto backup = s.root / domain::path_from_utf8(s.record["backup"].get<std::string>());
    safe_path(s.root / s.config.paths.cache, backup);
    durable_directories(backup.parent_path());
    durable_write(backup, s.record["before"].get<std::string>());
  }
  s.point("after_backup");
  s.point("before_source_copies");
  for (const auto& copy : s.record["copies"]) {
    const auto temp = s.root / domain::path_from_utf8(copy["temporary"].get<std::string>());
    const auto dest = s.root / domain::path_from_utf8(copy["destination"].get<std::string>());
    durable_directories(dest.parent_path());
    // Streaming sources avoid loading long PDFs into the journal. Complete
    // staging bytes and their digest are verified before exclusive publication.
    std::filesystem::copy_file(s.root / domain::path_from_utf8(copy["retained"].get<std::string>()), temp);
    // Synchronize the copied file without reopening it for truncation.
#if defined(_WIN32)
    const auto handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE || !FlushFileBuffers(handle)) io_error("sync source copy");
    CloseHandle(handle);
#else
    const int fd = ::open(temp.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) io_error("open source copy for sync");
    bool success = fsync(fd) == 0;
#if defined(__APPLE__)
    if (success) success = fcntl(fd, F_FULLFSYNC) == 0;
#endif
    const auto saved = errno; ::close(fd); errno = saved;
    if (!success) io_error("sync source copy");
#endif
    if (domain::sha256_file(temp) != copy["sha256"].get<std::string>()) throw StorageError("source changed while copying");
    s.point("before_source_publish");
    rename_exclusive(temp, dest);
    s.point("after_source_publish");
  }
  s.point("after_source_copies");
  s.point("before_article_stage");
  durable_write(s.root / domain::path_from_utf8(s.record["temporary"].get<std::string>()), s.record["after"].get<std::string>());
  s.point("after_article_stage");
}

void FileOperation::replace() {
  auto& s = *state_;
  const auto target = s.root / domain::path_from_utf8(s.record["destination"].get<std::string>());
  const auto temp = s.root / domain::path_from_utf8(s.record["temporary"].get<std::string>());
  safe_path(s.root / s.config.paths.vault, target);
  safe_path(s.root / s.config.paths.vault, temp);
  const auto before = s.record["before"].is_null() ? std::optional<std::string>{} : std::optional<std::string>{s.record["before"].get<std::string>()};
  if (current_bytes(target) != before) throw StorageError("article changed after preview/read; inspect and approve again");
  s.record["phase"] = "replacing";
  atomic_json(s.journal, s.record);
  s.point("before_article_replace");
  if (before) exchange_files(temp, target);
  else rename_exclusive(temp, target);
  s.point("after_article_exchange");
  if (before && read_bytes(temp) != *before) throw StorageError("external editor changed the displaced article; recover before retrying");
  if (current_bytes(target) != std::optional<std::string>{s.record["after"].get<std::string>()}) {
    conflict(s.journal);
  }
  s.record["phase"] = "replaced";
  atomic_json(s.journal, s.record);
  s.point("after_article_replace");
}

void FileOperation::finish() {
  auto& s = *state_;
  s.point("before_cleanup");
  const auto temporary = s.root / domain::path_from_utf8(s.record["temporary"].get<std::string>());
  const auto before = s.record["before"].is_null() ? std::optional<std::string>{} : std::optional<std::string>{s.record["before"].get<std::string>()};
  preserve_displaced(s.root,s.config,s.id,temporary,before,s.record["after"].get<std::string>());
  durable_remove(temporary);
  durable_remove(s.journal);
  s.point("after_cleanup");
}

std::optional<std::filesystem::path> FileOperation::backup_path() const {
  if (state_->record["backup"].is_null()) return std::nullopt;
  return domain::path_from_utf8(state_->record["backup"].get<std::string>());
}

void recover_on_open(Database& database, const std::filesystem::path& state_path, bool read_only, DurableObserver observer) {
  const auto project = project_for_state(state_path);
  if (!project) return;
  const auto& [root, config] = *project;
  const auto directory = root / config.paths.cache / "transactions";
  std::vector<std::filesystem::path> journals;
  bool pending_journal = false;
  if (std::filesystem::is_directory(directory)) {
    safe_path(root / config.paths.cache, directory);
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().extension() == ".json") journals.push_back(entry.path());
      if (entry.path().filename().string().ends_with(".json.pending")) pending_journal = true;
    }
  }
  if (read_only) {
    // The read-only inspection connection itself stays read-only. Recovery is
    // a separate project-open prerequisite, only when interrupted work exists.
    const bool has_extractions = database.scalar_integer("SELECT COUNT(*) FROM sqlite_master WHERE name='extraction_runs'") != 0;
    const bool running = has_extractions && database.scalar_integer("SELECT COUNT(*) FROM extraction_runs WHERE status='running'") != 0;
    if (!journals.empty() || pending_journal || running) {
      Database recovery(state_path, OpenMode::read_write, false);
      recover_on_open(recovery, state_path, false);
    }
    return;
  }
  if (!journals.empty()) {
    database.execute("BEGIN IMMEDIATE");
    try {
      // Re-read under the lock: another opener may have recovered this journal.
      for (const auto& journal : journals) if (std::filesystem::exists(journal)) recover_journal(database, root, config, journal, observer);
      database.execute("COMMIT");
    } catch (...) { database.execute("ROLLBACK"); throw; }
  }
  // A .pending journal can only precede any owned vault/backup mutation.
  // Delete these under the write lock so a live writer cannot lose its journal.
  if (pending_journal) {
    database.execute("BEGIN IMMEDIATE");
    try {
      for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.ends_with(".json.pending") && domain::is_valid_id(name.substr(0, name.size()-13), "run_")) {
          safe_path(directory, entry.path()); durable_remove(entry.path());
        }
      }
      database.execute("COMMIT");
    } catch (...) { database.execute("ROLLBACK"); throw; }
  }
  reconcile_extractions(database, root, config);
}

void keep_recovery_conflicts(const std::filesystem::path& root) {
  std::ifstream input(root / "kc.json");
  const auto parsed = domain::parse_project_config(json::parse(input));
  if (!parsed) throw StorageError("invalid project configuration");
  domain::validate_project_paths(root, *parsed.value);
  const auto& config = *parsed.value;
  Database db(root / config.paths.state, OpenMode::read_write, false);
  db.execute("BEGIN IMMEDIATE");
  try {
    const auto active = root / config.paths.cache / "transactions";
    const auto archive = root / config.paths.cache / "recovery-conflicts";
    durable_directories(archive);
    if (std::filesystem::is_directory(active)) {
      for (const auto& entry : std::filesystem::directory_iterator(active)) {
        if (entry.path().extension() != ".json") continue;
        safe_path(active, entry.path());
        const auto record = json::parse(read_bytes(entry.path()));
        const auto id = record.at("id").get<std::string>();
        if (!domain::is_valid_id(id,"run_") || entry.path().filename()!=id+".json") throw StorageError("invalid journal identity");
        if (operation_committed(db,id)) { recover_journal(db,root,config,entry.path()); continue; }
        const auto target = resolve(root, root / config.paths.vault, record.at("destination"));
        const auto temp = resolve(root, root / config.paths.vault, record.at("temporary"));
        auto expected_temp = target;
        expected_temp += ".kc-" + id + ".tmp";
        if (temp != expected_temp) throw StorageError("unowned article temporary in recovery journal");
        auto archived = record;
        // A conflicting editor may have saved invalid UTF-8. Preserve its
        // exact binary bytes separately instead of forcing them into JSON.
        const auto archive_bytes = [&](const std::filesystem::path& path, const std::string& label) {
          const auto bytes = current_bytes(path);
          if (!bytes) return json(nullptr);
          const auto digest = domain::sha256_text(*bytes);
          const auto saved = archive / (id + "-" + label + "-" + digest + ".md");
          if (!std::filesystem::exists(saved)) durable_write(saved,*bytes);
          else if (read_bytes(saved)!=*bytes) throw StorageError("existing conflict archive changed");
          return json{{"path",domain::path_to_utf8(saved.lexically_relative(root))},{"sha256",digest}};
        };
        archived["kept_user_file"] = archive_bytes(target,"kept");
        archived["displaced_file"] = archive_bytes(temp,"displaced");
        // Archive before cleanup. Sources/backups are deliberately retained
        // when an external article may refer to them; they are recoverable
        // artifacts explicitly recorded in this archive, never silent trash.
        atomic_json(archive / (id + ".json"), archived);
        durable_remove(temp);
        std::size_t source_index = 0;
        for (const auto& copy : record.at("copies")) {
          const auto staged_source = resolve(root, root / config.paths.vault, copy.at("temporary"));
          const auto destination = resolve(root, root / config.paths.vault / config.vault.source_directory, copy.at("destination"));
          auto owned = destination; owned += ".kc-" + id + ".tmp";
          if (staged_source != owned) throw StorageError("unowned source temporary in recovery journal");
          if (std::filesystem::exists(staged_source)) {
            const auto retained = archive / (id + "-source-" + std::to_string(source_index));
            // Rename keeps arbitrary PDF/image bytes without JSON conversion.
            rename_exclusive(staged_source,retained);
            sync_directory(staged_source.parent_path());
          }
          ++source_index;
        }
        auto recovery = temp; recovery += ".recovery";
        if (std::filesystem::exists(recovery)) rename_exclusive(recovery, archive / (id + ".recovery"));
        durable_remove(entry.path());
      }
    }
    db.execute("COMMIT");
  } catch (...) { db.execute("ROLLBACK"); throw; }
}

struct ExtractionLease::State {
  explicit State(const std::filesystem::path& path) : lease(path, true) {}
  Lease lease;
};
ExtractionLease::ExtractionLease(const std::filesystem::path& cache, std::string id) {
  if (!domain::is_valid_id(id,"run_")) throw StorageError("invalid extraction run identity");
  const auto directory = cache / "extraction-leases";
  safe_path(cache, directory);
  durable_directories(directory);
  state_ = std::make_unique<State>(directory / (id + ".lock"));
  if (!state_->lease.acquired()) throw StorageError("cannot acquire extraction run lease");
}
ExtractionLease::~ExtractionLease() = default;
} // namespace kc::storage
