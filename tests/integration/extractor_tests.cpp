#include "kc/extraction/extractor.hpp"
#include "kc/domain/id.hpp"
#include "kc/import/source_importer.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

kc::storage::InitOptions init_options(const std::filesystem::path& root) {
  return {
      .project_root = root,
      .vault_path = "vault",
      .migration_directory = KC_TEST_MIGRATIONS_DIR};
}

class FakePdfAdapter final : public kc::extraction::PdfAdapter {
 public:
  explicit FakePdfAdapter(std::vector<std::optional<std::string>> native_text)
      : native_text_(std::move(native_text)) {}

  [[nodiscard]] std::string_view name() const noexcept override {
    return "fake-poppler";
  }

  [[nodiscard]] std::vector<kc::extraction::RenderedPage> render_pages(
      const std::filesystem::path&,
      const std::filesystem::path& output_directory) override {
    ++render_calls;
    std::vector<kc::extraction::RenderedPage> pages;
    for (std::size_t index = 0; index < native_text_.size(); ++index) {
      const auto page_number = static_cast<std::uint32_t>(index + 1U);
      const auto image =
          output_directory / ("rendered-" + std::to_string(page_number) + ".png");
      write_file(image, "fake PNG page " + std::to_string(page_number));
      pages.push_back({page_number, image});
    }
    return pages;
  }

  [[nodiscard]] std::optional<std::string> extract_native_text(
      const std::filesystem::path&, const std::uint32_t page_number,
      const std::filesystem::path&) override {
    ++native_calls;
    return native_text_.at(static_cast<std::size_t>(page_number - 1U));
  }

  std::size_t render_calls{0};
  std::size_t native_calls{0};

 private:
  std::vector<std::optional<std::string>> native_text_;
};

class FakeOcrProvider final : public kc::extraction::OcrProvider {
 public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "fake-ocr";
  }

  [[nodiscard]] kc::extraction::OcrResult recognize(
      const std::filesystem::path& image_path) override {
    ++calls;
    if (image_path.filename() == "rendered-2.png") {
      return {.succeeded = true, .text = "OCR recovered page two"};
    }
    throw kc::extraction::AdapterError("intentional OCR test failure");
  }

  std::size_t calls{0};
};

class FailingPdfAdapter final : public kc::extraction::PdfAdapter {
 public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "failing-renderer";
  }

  [[nodiscard]] std::vector<kc::extraction::RenderedPage> render_pages(
      const std::filesystem::path& pdf_path,
      const std::filesystem::path&) override {
    throw kc::extraction::AdapterError(
        "intentional renderer failure: " + pdf_path.string());
  }

  [[nodiscard]] std::optional<std::string> extract_native_text(
      const std::filesystem::path&, std::uint32_t,
      const std::filesystem::path&) override {
    return std::nullopt;
  }
};

}  // namespace

TEST_CASE("Markdown and text imports each extract to one native logical page") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto markdown = temporary.path() / "probability.md";
  const auto text = temporary.path() / "notes.txt";
  write_file(markdown, "# Markov chains\nNative Markdown text.\n");
  write_file(text, "Native plain text.\n");

  kc::source_import::SourceImporter importer(project_root);
  const auto markdown_source = importer.import_file(markdown);
  const auto text_source = importer.import_file(text);

  auto pdf = std::make_unique<FakePdfAdapter>(
      std::vector<std::optional<std::string>>{});
  auto* pdf_observer = pdf.get();
  auto ocr = std::make_unique<FakeOcrProvider>();
  auto* ocr_observer = ocr.get();
  kc::extraction::Extractor extractor(
      project_root, std::move(pdf), std::move(ocr));

  const auto markdown_result =
      extractor.extract(markdown_source.source.source_id);
  const auto text_result = extractor.extract(text_source.source.source_id);

  REQUIRE(markdown_result.pages.size() == 1);
  CHECK(markdown_result.pages[0].page_number == 1);
  CHECK(markdown_result.pages[0].text ==
        "# Markov chains\nNative Markdown text.\n");
  CHECK(markdown_result.pages[0].text_status ==
        kc::domain::TextStatus::native);
  CHECK_FALSE(markdown_result.pages[0].image_path.has_value());
  REQUIRE(text_result.pages.size() == 1);
  CHECK(text_result.pages[0].text == "Native plain text.\n");
  CHECK(text_result.pages[0].text_status == kc::domain::TextStatus::native);
  CHECK(pdf_observer->render_calls == 0);
  CHECK(ocr_observer->calls == 0);

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_pages") == 2);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM extraction_runs WHERE status = 'completed'") ==
        2);
}

TEST_CASE("every PDF page is rendered and failed OCR remains explicitly failed") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto pdf_path = temporary.path() / "scanned-notes.pdf";
  write_file(pdf_path, "%PDF fake immutable fixture");

  kc::source_import::SourceImporter importer(project_root);
  const auto imported = importer.import_file(pdf_path);
  auto pdf = std::make_unique<FakePdfAdapter>(
      std::vector<std::optional<std::string>>{
          "Native Markov chain text", "", std::nullopt});
  auto* pdf_observer = pdf.get();
  auto ocr = std::make_unique<FakeOcrProvider>();
  auto* ocr_observer = ocr.get();
  kc::extraction::Extractor extractor(
      project_root, std::move(pdf), std::move(ocr));

  const auto result = extractor.extract(imported.source.source_id);

  REQUIRE(result.pages.size() == 3);
  CHECK(result.failed_pages == 1);
  CHECK(result.pages[0].page_number == 1);
  CHECK(result.pages[0].text_status == kc::domain::TextStatus::native);
  CHECK(result.pages[1].page_number == 2);
  CHECK(result.pages[1].text_status ==
        kc::domain::TextStatus::ocr_unreviewed);
  CHECK(result.pages[1].text == "OCR recovered page two");
  CHECK(result.pages[2].page_number == 3);
  CHECK(result.pages[2].text_status == kc::domain::TextStatus::failed);
  CHECK(result.pages[2].text.empty());
  CHECK(pdf_observer->render_calls == 1);
  CHECK(pdf_observer->native_calls == 3);
  CHECK(ocr_observer->calls == 2);

  for (const auto& page : result.pages) {
    REQUIRE(page.image_path.has_value());
    CHECK_FALSE(page.image_path->is_absolute());
    CHECK(std::filesystem::is_regular_file(project_root / *page.image_path));
  }

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_pages") == 3);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM source_pages WHERE page_number BETWEEN 1 AND 3") ==
        3);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM source_pages WHERE text_status = 'failed'") ==
        1);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM source_pages "
            "WHERE image_path IS NULL OR substr(image_path, 1, 1) = '/'") ==
        0);
  CHECK(database.scalar_text(
            "SELECT status FROM extraction_runs ORDER BY rowid DESC LIMIT 1") ==
        "failed");
  CHECK(database.scalar_text(
            "SELECT error_message FROM extraction_runs "
            "ORDER BY rowid DESC LIMIT 1")
            .find(temporary.path().string()) == std::string::npos);
}

TEST_CASE("extraction is idempotent and force preserves stable page IDs") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto pdf_path = temporary.path() / "native.pdf";
  write_file(pdf_path, "%PDF fake immutable fixture");

  kc::source_import::SourceImporter importer(project_root);
  const auto imported = importer.import_file(pdf_path);
  auto pdf = std::make_unique<FakePdfAdapter>(
      std::vector<std::optional<std::string>>{"Usable native page text"});
  auto* pdf_observer = pdf.get();
  auto ocr = std::make_unique<FakeOcrProvider>();
  kc::extraction::Extractor extractor(
      project_root, std::move(pdf), std::move(ocr));

  const auto first = extractor.extract(imported.source.source_id);
  const auto repeated = extractor.extract(imported.source.source_id);
  const auto forced = extractor.extract(imported.source.source_id, true);

  REQUIRE(first.pages.size() == 1);
  REQUIRE(repeated.pages.size() == 1);
  REQUIRE(forced.pages.size() == 1);
  CHECK_FALSE(first.reused);
  CHECK(repeated.reused);
  CHECK_FALSE(forced.reused);
  CHECK(first.pages[0].page_id == repeated.pages[0].page_id);
  CHECK(first.pages[0].page_id == forced.pages[0].page_id);
  CHECK(pdf_observer->render_calls == 2);

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_pages") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM extraction_runs") == 2);
}

TEST_CASE("forced extraction cannot replace pages cited by a proposal") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto pdf_path = temporary.path() / "cited-native.pdf";
  write_file(pdf_path, "%PDF fake immutable fixture");

  kc::source_import::SourceImporter importer(project_root);
  const auto imported = importer.import_file(pdf_path);
  auto pdf = std::make_unique<FakePdfAdapter>(
      std::vector<std::optional<std::string>>{"Usable native page text"});
  auto* pdf_observer = pdf.get();
  kc::extraction::Extractor extractor(
      project_root, std::move(pdf), std::make_unique<FakeOcrProvider>());
  const auto first = extractor.extract(imported.source.source_id);
  REQUIRE(first.pages.size() == 1U);

  kc::storage::Database database(initialized.state_path);
  const auto proposal_id = kc::domain::generate_proposal_id();
  database.execute(
      "INSERT INTO proposals(proposal_id, operation, payload_json, status, "
      "created_at) VALUES ('" +
      proposal_id.value +
      "', 'create_article', '{}', 'pending', '2026-08-11T12:00:00Z')");
  database.execute(
      "INSERT INTO proposal_citations(proposal_id, section_key, block_index, "
      "page_id, start_char, end_char, quote) VALUES ('" +
      proposal_id.value + "', 'working_explanation', 0, '" +
      first.pages.front().page_id.value + "', 0, 6, 'Usable')");

  try {
    static_cast<void>(extractor.extract(imported.source.source_id, true));
    FAIL("expected cited-page immutability failure");
  } catch (const kc::extraction::ExtractionError& error) {
    CHECK(error.kind() == kc::extraction::ExtractionErrorKind::invalid_state);
  }

  CHECK(pdf_observer->render_calls == 1U);
  CHECK(database.scalar_text("SELECT text FROM source_pages") ==
        "Usable native page text");
  CHECK(database.scalar_text("SELECT text_status FROM source_pages") ==
        "native");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM extraction_runs") == 1);
}

TEST_CASE("fatal PDF rendering failures are recorded without fake page rows") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto pdf_path = temporary.path() / "broken.pdf";
  write_file(pdf_path, "%PDF invalid fixture");

  kc::source_import::SourceImporter importer(project_root);
  const auto imported = importer.import_file(pdf_path);
  kc::extraction::Extractor extractor(
      project_root, std::make_unique<FailingPdfAdapter>(),
      std::make_unique<FakeOcrProvider>());

  try {
    static_cast<void>(extractor.extract(imported.source.source_id));
    FAIL("expected renderer failure");
  } catch (const kc::extraction::ExtractionError& error) {
    CHECK(error.kind() == kc::extraction::ExtractionErrorKind::adapter_error);
  }

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_pages") == 0);
  CHECK(database.scalar_text(
            "SELECT status FROM extraction_runs ORDER BY rowid DESC LIMIT 1") ==
        "failed");
  CHECK(database.scalar_text(
            "SELECT error_message FROM extraction_runs "
            "ORDER BY rowid DESC LIMIT 1")
            .find(temporary.path().string()) == std::string::npos);
}
