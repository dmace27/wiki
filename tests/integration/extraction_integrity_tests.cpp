#include "kc/compiler/compiler.hpp"
#include "kc/domain/path.hpp"
#include "kc/domain/text.hpp"
#include "kc/extraction/extractor.hpp"
#include "kc/import/sha256.hpp"
#include "kc/import/source_importer.hpp"
#include "kc/models/language_model.hpp"
#include "kc/review/review_service.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <functional>
#include <memory>

#if !defined(_WIN32)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
void write_bytes(const std::filesystem::path& path, const std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("fixture write failed");
}

kc::storage::InitOptions options(const std::filesystem::path& root) {
  return {.project_root = root, .vault_path = "vault",
          .migration_directory = KC_TEST_MIGRATIONS_DIR};
}

/// A single-page renderer with a controllable boundary after staging images.
class Pdf final : public kc::extraction::PdfAdapter {
 public:
  std::function<void()> after_render;
  std::string native_text{"A Markov chain has states."};
  std::string image_bytes{"original image"};
  std::string_view name() const noexcept override { return "integrity-fixture"; }
  std::vector<kc::extraction::RenderedPage> render_pages(
      const std::filesystem::path&, const std::filesystem::path& output) override {
    write_bytes(output / "one.png", image_bytes);
    if (after_render) after_render();
    return {{1, output / "one.png"}};
  }
  std::optional<std::string> extract_native_text(
      const std::filesystem::path&, std::uint32_t,
      const std::filesystem::path&) override { return native_text; }
};
class Ocr final : public kc::extraction::OcrProvider {
 public:
  std::string text{"A Markov chain has states."};
  std::string_view name() const noexcept override { return "integrity-ocr"; }
  kc::extraction::OcrResult recognize(const std::filesystem::path&) override {
    return {.succeeded = true, .text = text};
  }
};

/// A real imported source and extracted page, with genuine recorded digests.
struct Fixture {
  kc::test::TemporaryDirectory temporary;
  std::filesystem::path root{temporary.path() / "project"};
  kc::storage::InitResult initialized{kc::storage::initialize_project(options(root))};
  kc::source_import::ImportResult imported;
  kc::domain::ExtractedPage page;

  explicit Fixture(const bool pdf = true) {
    const auto input = temporary.path() / (pdf ? "Notes.pdf" : "Notes.txt");
    write_bytes(input, pdf ? "%PDF integrity fixture" : "A Markov chain has states.");
    imported = kc::source_import::SourceImporter(root).import_file(input);
    kc::extraction::Extractor extractor(root, std::make_unique<Pdf>(), std::make_unique<Ocr>());
    page = extractor.extract(imported.source.source_id).pages.at(0);
  }
};

class Model final : public kc::models::LanguageModel {
 public:
  std::string page_id;
  std::function<void()> during_generation;
  explicit Model(std::string id) : page_id(std::move(id)) {}
  std::string_view provider_name() const noexcept override { return "integrity-model"; }
  std::string_view model_name() const noexcept override { return "fixture"; }
  kc::models::LanguageModelResponse generate(
      const kc::models::LanguageModelRequest&) override {
    if (during_generation) during_generation();
    const nlohmann::json citation{{"page_id", page_id}, {"start_char", 2},
        {"end_char", 14}, {"quote", "Markov chain"}};
    const nlohmann::json proposal{
        {"schema_version", 1}, {"operation", "create_article"},
        {"article", {{"title", "Markov Chains"}, {"slug", "markov-chains"},
                     {"aliases", nlohmann::json::array()}}},
        {"sections", nlohmann::json::array({{
            {"key", "working_explanation"}, {"heading", "Explanation"},
            {"blocks", nlohmann::json::array({{{"kind", "paragraph"},
                {"text", "A Markov chain has states."},
                {"citations", nlohmann::json::array({citation})}}})}}})},
        {"related_concepts", nlohmann::json::array()}};
    return {.content = proposal.dump()};
  }
};

kc::compiler::CompileResult compile(Fixture& fixture,
                                    std::function<void()> during = {}) {
  auto model = std::make_unique<Model>(fixture.page.page_id.value);
  model->during_generation = std::move(during);
  return kc::compiler::Compiler(fixture.root, std::move(model)).compile(
      {.concept_title = "Markov Chains"});
}

void force_extract(Fixture& fixture, std::function<void()> during = {},
                   const std::string& image = "replacement image") {
  auto pdf = std::make_unique<Pdf>();
  pdf->after_render = std::move(during);
  pdf->image_bytes = image;
  kc::extraction::Extractor extractor(fixture.root, std::move(pdf), std::make_unique<Ocr>());
  static_cast<void>(extractor.extract(fixture.imported.source.source_id, true));
}

#if !defined(_WIN32)
/// Pipes establish exact process ordering without timing-dependent sleeps.
/// The child pauses inside generation/rendering while the parent commits a
/// competing operation. A timeout kills/reaps a stuck child even on failure.
class ProcessRace {
 public:
  explicit ProcessRace(const std::function<bool(std::function<void()>)>& child_work) {
    if (pipe(ready_) != 0 || pipe(resume_) != 0) throw std::runtime_error("pipe failed");
    child_ = fork();
    if (child_ < 0) throw std::runtime_error("fork failed");
    if (child_ == 0) {
      close(ready_[0]);
      close(resume_[1]);
      bool result = false;
      try {
        result = child_work([&] {
          const char signal = 'r';
          if (write(ready_[1], &signal, 1) != 1) throw std::runtime_error("ready failed");
          char reply = 0;
          if (read(resume_[0], &reply, 1) != 1) throw std::runtime_error("resume failed");
        });
      } catch (...) { result = false; }
      _exit(result ? 0 : 1);
    }
    close(ready_[1]);
    close(resume_[0]);
    pollfd ready{ready_[0], POLLIN, 0};
    if (poll(&ready, 1, 10000) != 1 || !(ready.revents & POLLIN)) {
      kill(child_, SIGKILL);
      waitpid(child_, nullptr, 0);
      child_ = -1;
      close(ready_[0]);
      close(resume_[1]);
      throw std::runtime_error("child never reached race boundary");
    }
    char signal = 0;
    if (read(ready_[0], &signal, 1) != 1) throw std::runtime_error("ready read failed");
  }
  ~ProcessRace() {
    if (child_ > 0) {
      kill(child_, SIGKILL);
      waitpid(child_, nullptr, 0);
    }
    close(ready_[0]);
    close(resume_[1]);
  }
  bool finish() {
    const char signal = 'g';
    if (write(resume_[1], &signal, 1) != 1) return false;
    // The ready pipe closes at child exit, giving a bounded completion wait.
    pollfd completion{ready_[0], POLLHUP, 0};
    if (poll(&completion, 1, 10000) != 1) return false;
    int status = 0;
    if (waitpid(child_, &status, 0) != child_) return false;
    child_ = -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
  ProcessRace(const ProcessRace&) = delete;
  ProcessRace& operator=(const ProcessRace&) = delete;
 private:
  int ready_[2]{};
  int resume_[2]{};
  pid_t child_{-1};
};
#endif
}  // namespace

TEST_CASE("initialization rejects canonical escapes before writing outside the project", "[integrity]") {
  const auto path = GENERATE("sources", "vault", ".knowledge-compiler", "kc.json",
      ".knowledge-compiler/pages", ".knowledge-compiler/backups", ".knowledge-compiler/logs",
      ".knowledge-compiler/state.sqlite");
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto outside = temporary.path() / "outside";
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(outside);
  std::filesystem::create_directories((root / path).parent_path());
  if (std::string_view(path).ends_with("state.sqlite")) {
    // A missing leaf still lets SQLite follow a dangling symlink and create
    // its target. Initialization must reject it before opening the database.
    std::filesystem::create_symlink(outside / "state.sqlite", root / path);
  } else if (std::string_view(path) == "kc.json") {
    write_bytes(outside / "config", "{}");
    std::filesystem::create_symlink(outside / "config", root / path);
  } else std::filesystem::create_directory_symlink(outside, root / path);
  CHECK_THROWS_AS(kc::storage::initialize_project(options(root)), kc::storage::StorageError);
  CHECK_FALSE(std::filesystem::exists(outside / "state.sqlite"));
  CHECK_FALSE(std::filesystem::exists(outside / "pages"));
}

TEST_CASE("all services reject escaped state and overlapping configured ownership", "[integrity]") {
  const auto target = GENERATE("state", "cache", "vault");
  Fixture fixture(false);
  auto document = kc::test::read_json(fixture.root / "kc.json");
  document["paths"][target] = std::string("sources/") + target;
  write_bytes(fixture.root / "kc.json", document.dump());
  CHECK_THROWS_AS(kc::storage::initialize_project(options(fixture.root)), kc::storage::StorageError);
  CHECK_THROWS_AS(kc::source_import::SourceImporter(fixture.root), kc::source_import::ImportError);
  CHECK_THROWS_AS(kc::extraction::Extractor(fixture.root), kc::extraction::ExtractionError);
  CHECK_THROWS_AS(kc::review::ReviewService(fixture.root), kc::review::ReviewError);
  CHECK_THROWS_AS(compile(fixture), kc::compiler::CompilerError);
}

TEST_CASE("cache staging and database symlinks cannot escape after initialization", "[integrity]") {
  const auto relative = GENERATE(".knowledge-compiler/import-staging",
      ".knowledge-compiler/extraction-staging", ".knowledge-compiler/pages",
      ".knowledge-compiler/state.sqlite");
  Fixture fixture;
  const auto outside = fixture.temporary.path() / "outside";
  std::filesystem::create_directory(outside);
  const auto path = fixture.root / relative;
  if (std::string_view(relative).ends_with("state.sqlite")) {
    std::filesystem::copy_file(path, outside / "state.sqlite");
    std::filesystem::remove(path);
    std::filesystem::create_symlink(outside / "state.sqlite", path);
    CHECK_THROWS_AS(kc::source_import::SourceImporter(fixture.root), kc::source_import::ImportError);
    CHECK_THROWS_AS(kc::extraction::Extractor(fixture.root), kc::extraction::ExtractionError);
    CHECK_THROWS_AS(kc::review::ReviewService(fixture.root), kc::review::ReviewError);
    CHECK_THROWS_AS(compile(fixture), kc::compiler::CompilerError);
  } else {
    std::filesystem::remove_all(path);
    std::filesystem::create_directory_symlink(outside, path);
    if (std::string_view(relative).ends_with("import-staging")) {
      const auto input = fixture.temporary.path() / "Another.txt";
      write_bytes(input, "another input");
      CHECK_THROWS_AS(kc::source_import::SourceImporter(fixture.root).import_file(input),
                      kc::source_import::ImportError);
    } else CHECK_THROWS_AS(force_extract(fixture), kc::extraction::ExtractionError);
    CHECK(std::filesystem::is_empty(outside));
  }
}

TEST_CASE("retained source tampering and missing files fail before cache reuse or model use", "[integrity]") {
  const auto missing = GENERATE(false, true);
  Fixture fixture;
  const auto retained = fixture.root / fixture.imported.version.stored_path;
  std::filesystem::permissions(retained, std::filesystem::perms::owner_write,
                              std::filesystem::perm_options::add);
  if (missing) std::filesystem::remove(retained);
  else write_bytes(retained, "tampered bytes");
  CHECK_THROWS_AS(kc::extraction::Extractor(fixture.root).extract(fixture.imported.source.source_id),
                  kc::extraction::ExtractionError);
  CHECK_THROWS_AS(force_extract(fixture), kc::extraction::ExtractionError);
  CHECK_THROWS_AS(kc::review::ReviewService(fixture.root).review_extraction(fixture.imported.source.source_id),
                  kc::review::ReviewError);
  CHECK_THROWS_AS(compile(fixture), kc::compiler::CompilerError);
  kc::storage::Database database(fixture.initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM model_runs") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM extraction_runs") == 1);
}

TEST_CASE("retained source child paths reject escaping symlinks and embedded NULs", "[integrity]") {
  const auto nul_path = GENERATE(false, true);
  Fixture fixture;
  const auto retained = fixture.root / fixture.imported.version.stored_path;
  const auto outside = fixture.temporary.path() / "outside.pdf";
  std::filesystem::copy_file(retained, outside);
  if (nul_path) {
    kc::storage::Database database(fixture.initialized.state_path);
    database.execute("UPDATE source_versions SET stored_path = stored_path || char(0) || 'ignored'");
  } else {
    std::filesystem::remove(retained);
    std::filesystem::create_symlink(outside, retained);
  }
  CHECK_THROWS_AS(kc::extraction::Extractor(fixture.root).extract(fixture.imported.source.source_id),
                  kc::extraction::ExtractionError);
  CHECK_THROWS_AS(compile(fixture), kc::compiler::CompilerError);
  CHECK_THROWS_AS(kc::source_import::SourceImporter(fixture.root).import_file(outside),
                  kc::source_import::ImportError);
}

TEST_CASE("missing tampered and legacy unhashed images cannot be silently reused", "[integrity]") {
  const auto damage = GENERATE("missing", "tampered", "unhashed", "escaped");
  Fixture fixture;
  const auto image = fixture.root / *fixture.page.image_path;
  if (std::string_view(damage) == "unhashed") {
    kc::storage::Database database(fixture.initialized.state_path);
    database.execute("UPDATE source_pages SET image_sha256 = NULL");
  } else if (std::string_view(damage) == "tampered") write_bytes(image, "tampered");
  else {
    std::filesystem::remove(image);
    if (std::string_view(damage) == "escaped") {
      const auto outside = fixture.temporary.path() / "outside.png";
      write_bytes(outside, "original image");
      std::filesystem::create_symlink(outside, image);
    }
  }
  CHECK_THROWS_AS(kc::extraction::Extractor(fixture.root).extract(fixture.imported.source.source_id),
                  kc::extraction::ExtractionError);
  CHECK_THROWS_AS(kc::review::ReviewService(fixture.root).review_extraction(fixture.imported.source.source_id),
                  kc::review::ReviewError);
  CHECK_THROWS_AS(compile(fixture), kc::compiler::CompilerError);
  // An explicit uncited re-extraction repairs images without trusting them.
  CHECK_NOTHROW(force_extract(fixture));
  CHECK_NOTHROW(compile(fixture));
}

TEST_CASE("supported text boundaries reject malformed UTF-8 and embedded NULs", "[integrity]") {
  const auto text = GENERATE(std::string("A\0B", 3), std::string("\xC0\xAF", 2),
      std::string("\xED\xA0\x80", 3), std::string("\xF4\x90\x80\x80", 4),
      std::string("\xE2\x82", 2));
  Fixture fixture(false);
  const auto input = fixture.temporary.path() / "Bad.txt";
  write_bytes(input, text);
  CHECK_THROWS_AS(kc::source_import::SourceImporter(fixture.root).import_file(input),
                  kc::source_import::ImportError);
  CHECK_THROWS_AS(kc::review::ReviewService(fixture.root).correct_page_text(
      fixture.imported.source.source_id, 1, text), kc::review::ReviewError);
  kc::storage::Database database(fixture.initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 1);
  CHECK(database.scalar_integer("SELECT evidence_revision FROM source_pages") == 0);
  CHECK(kc::domain::is_valid_source_text("日本語 café\n\t"));
}

TEST_CASE("legacy unextracted text with valid file hashes still requires valid encoding", "[integrity]") {
  Fixture fixture(false);
  const auto retained = fixture.root / fixture.imported.version.stored_path;
  const std::string bytes("A\0B", 3);
  std::filesystem::permissions(retained, std::filesystem::perms::owner_write,
                              std::filesystem::perm_options::add);
  write_bytes(retained, bytes);
  kc::storage::Database database(fixture.initialized.state_path);
  database.execute("DELETE FROM source_pages");
  database.execute("UPDATE source_versions SET sha256 = '" +
                   kc::source_import::sha256_text(bytes) + "'");
  CHECK_THROWS_AS(kc::extraction::Extractor(fixture.root).extract(fixture.imported.source.source_id),
                  kc::extraction::ExtractionError);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_pages") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM extraction_runs WHERE status = 'failed'") == 1);
}

TEST_CASE("native and OCR adapter text must use valid UTF-8 without NULs", "[integrity]") {
  const auto ocr_boundary = GENERATE(false, true);
  Fixture fixture;
  auto pdf = std::make_unique<Pdf>();
  auto ocr = std::make_unique<Ocr>();
  if (ocr_boundary) {
    pdf->native_text = "";
    ocr->text = std::string("A\0B", 3);
  } else pdf->native_text = std::string("A\xFF", 2);
  kc::extraction::Extractor extractor(fixture.root, std::move(pdf), std::move(ocr));
  CHECK_THROWS_AS(extractor.extract(fixture.imported.source.source_id, true),
                  kc::extraction::ExtractionError);
  CHECK_NOTHROW(kc::review::ReviewService(fixture.root).review_extraction(fixture.imported.source.source_id));
}

TEST_CASE("SQLite text readers preserve legacy embedded NULs and reject corrupt evidence", "[integrity]") {
  Fixture fixture(false);
  kc::storage::Database database(fixture.initialized.state_path);
  database.execute("UPDATE source_pages SET text = CAST(X'410042' AS TEXT)");
  CHECK(database.scalar_text("SELECT text FROM source_pages").size() == 3);
  CHECK_THROWS_AS(kc::extraction::Extractor(fixture.root).extract(fixture.imported.source.source_id),
                  kc::extraction::ExtractionError);
  CHECK_THROWS_AS(kc::review::ReviewService(fixture.root).review_extraction(fixture.imported.source.source_id),
                  kc::review::ReviewError);
  CHECK_THROWS_AS(compile(fixture), kc::compiler::CompilerError);
}

TEST_CASE("published generation freezes every selected page including uncited context", "[integrity]") {
  Fixture fixture(false);
  const auto extra = fixture.temporary.path() / "Context.txt";
  write_bytes(extra, "Markov Chains additional context.");
  const auto imported = kc::source_import::SourceImporter(fixture.root).import_file(extra);
  static_cast<void>(kc::extraction::Extractor(fixture.root).extract(imported.source.source_id));
  static_cast<void>(compile(fixture));
  kc::storage::Database database(fixture.initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposal_evidence") == 2);
  CHECK(database.scalar_integer("SELECT COUNT(DISTINCT page_id) FROM proposal_citations") == 1);
  const auto historic = database.scalar_text("SELECT page_id FROM proposal_evidence WHERE page_id NOT IN (SELECT page_id FROM proposal_citations)");
  const auto correction = kc::review::ReviewService(fixture.root).correct_page_text(
      imported.source.source_id, 1, "changed uncited context");
  CHECK(correction.page_id.value != historic);
  CHECK(database.scalar_text("SELECT text FROM source_pages WHERE page_id = '" + historic + "'") == "Markov Chains additional context.");
  CHECK_NOTHROW(kc::extraction::Extractor(fixture.root).extract(imported.source.source_id, true));
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposal_evidence") == 2);
}

#if !defined(_WIN32)
TEST_CASE("two-process compile rejects corrected re-extracted revised and tampered evidence", "[integrity][process-race]") {
  const auto change = GENERATE("correction", "same-text-extraction", "revision", "source-tamper", "image-tamper");
  Fixture fixture;
  ProcessRace race([&](auto pause) {
    try { static_cast<void>(compile(fixture, pause)); }
    catch (const kc::compiler::CompilerError& error) {
      return error.kind() == kc::compiler::CompilerErrorKind::validation_failed;
    }
    return false;
  });
  if (std::string_view(change) == "correction") {
    // Text A -> B -> A still increments revision twice.
    kc::review::ReviewService reviewer(fixture.root);
    static_cast<void>(reviewer.correct_page_text(fixture.imported.source.source_id, 1, "B"));
    static_cast<void>(reviewer.correct_page_text(fixture.imported.source.source_id, 1, fixture.page.text));
  } else if (std::string_view(change) == "same-text-extraction") force_extract(fixture);
  else if (std::string_view(change) == "revision") {
    const auto input = fixture.temporary.path() / "Revision.pdf";
    write_bytes(input, "%PDF new revision");
    static_cast<void>(kc::source_import::SourceImporter(fixture.root).import_file(input,
        {.source_id = fixture.imported.source.source_id}));
  } else if (std::string_view(change) == "image-tamper") {
    write_bytes(fixture.root / *fixture.page.image_path, "changed image");
  } else {
    const auto retained = fixture.root / fixture.imported.version.stored_path;
    std::filesystem::permissions(retained, std::filesystem::perms::owner_write,
                                std::filesystem::perm_options::add);
    write_bytes(retained, "changed source");
  }
  REQUIRE(race.finish());
  kc::storage::Database database(fixture.initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposal_citations") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposal_evidence") == 0);
}

TEST_CASE("two-process first extraction publishes exactly one coherent page set", "[integrity][process-race]") {
  Fixture fixture;
  {
    kc::storage::Database database(fixture.initialized.state_path);
    database.execute("DELETE FROM source_pages");
  }
  ProcessRace race([&](auto pause) {
    try { force_extract(fixture, pause, "losing initial image"); }
    catch (const kc::extraction::ExtractionError& error) {
      return error.kind() == kc::extraction::ExtractionErrorKind::invalid_state;
    }
    return false;
  });
  force_extract(fixture, {}, "winning initial image");
  REQUIRE(race.finish());
  kc::storage::Database database(fixture.initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_pages") == 1);
  CHECK(database.scalar_text("SELECT image_sha256 FROM source_pages") ==
        kc::source_import::sha256_text("winning initial image"));
  CHECK_NOTHROW(kc::review::ReviewService(fixture.root).review_extraction(fixture.imported.source.source_id));
}

TEST_CASE("two-process extraction cannot overwrite concurrently compiled corrected or extracted images", "[integrity][process-race]") {
  const auto competitor = GENERATE("compile", "correction", "extraction");
  Fixture fixture;
  const auto original_hash = kc::source_import::sha256_file(fixture.root / *fixture.page.image_path);
  ProcessRace race([&](auto pause) {
    try { force_extract(fixture, pause, "losing attempt image"); }
    catch (const kc::extraction::ExtractionError& error) {
      return error.kind() == kc::extraction::ExtractionErrorKind::invalid_state;
    }
    return false;
  });
  if (std::string_view(competitor) == "compile") static_cast<void>(compile(fixture));
  else if (std::string_view(competitor) == "correction") {
    static_cast<void>(kc::review::ReviewService(fixture.root).correct_page_text(
        fixture.imported.source.source_id, 1, "Reviewed corrected text."));
  } else force_extract(fixture, {}, "winning attempt image");
  REQUIRE(race.finish());
  CHECK(kc::source_import::sha256_file(fixture.root / *fixture.page.image_path) == original_hash);
  const auto reviewed = kc::review::ReviewService(fixture.root).review_extraction(fixture.imported.source.source_id);
  REQUIRE(reviewed.pages.size() == 1);
  const auto active_image = fixture.root / *reviewed.pages[0].image_path;
  const auto active_hash = kc::source_import::sha256_file(active_image);
  CHECK(active_hash == kc::source_import::sha256_text(
      std::string_view(competitor) == "extraction" ? "winning attempt image" : "original image"));
  kc::storage::Database database(fixture.initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM extraction_runs WHERE status = 'failed'") == 1);
  CHECK(database.scalar_text("SELECT image_sha256 FROM source_pages") == active_hash);
  CHECK(std::filesystem::is_empty(fixture.root / ".knowledge-compiler/extraction-staging"));
}
#endif

TEST_CASE("proposal preview validates frozen uncited model context before approval", "[review-preview]") {
  Fixture fixture(false);
  const auto extra = fixture.temporary.path() / "Context.txt";
  write_bytes(extra,"Markov Chains additional context.");
  const auto imported = kc::source_import::SourceImporter(fixture.root).import_file(extra);
  static_cast<void>(kc::extraction::Extractor(fixture.root).extract(imported.source.source_id));
  const auto proposal = compile(fixture);
  kc::vault::VaultWriter writer(fixture.root);
  const auto before = writer.preview(proposal.proposal_id);
  REQUIRE(before.valid);
  kc::storage::Database database(fixture.initialized.state_path);
  database.execute("UPDATE source_pages SET evidence_revision=evidence_revision+1 WHERE source_version_id='" + imported.version.source_version_id.value + "'");
  const auto invalid = writer.preview(proposal.proposal_id);
  CHECK_FALSE(invalid.valid);
  REQUIRE_FALSE(invalid.issues.empty());
  CHECK(invalid.issues.front().message.find("captured proposal evidence changed") != std::string::npos);
  CHECK_THROWS_AS(writer.approve(proposal.proposal_id,before.token),kc::vault::VaultError);
  CHECK(database.scalar_text("SELECT status FROM proposals") == "pending");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
}
