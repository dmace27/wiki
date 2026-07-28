#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kc::extraction {

/// Failure reported by a concrete PDF or OCR integration.
class AdapterError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// One page image produced by a PDF renderer.
struct RenderedPage {
  std::uint32_t page_number{1};
  std::filesystem::path image_path;
};

/// Result from a local OCR provider.
struct OcrResult {
  bool succeeded{false};
  std::string text;
  /// Safe diagnostic text suitable for the local extraction run record.
  std::string error_message;
};

/// Executes one local program without passing arguments through a shell.
///
/// Keeping process execution injectable lets adapter behavior be tested
/// without requiring Poppler or Tesseract in the unit-test environment.
class CommandRunner {
 public:
  virtual ~CommandRunner() = default;
  [[nodiscard]] virtual int run(
      std::string_view executable,
      const std::vector<std::string>& arguments) = 0;
};

/// Production command runner backed by direct OS process creation.
class LocalCommandRunner final : public CommandRunner {
 public:
  [[nodiscard]] int run(
      std::string_view executable,
      const std::vector<std::string>& arguments) override;
};

/// Pluggable PDF text and rendering boundary.
class PdfAdapter {
 public:
  virtual ~PdfAdapter() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Render every PDF page into `output_directory`.
  [[nodiscard]] virtual std::vector<RenderedPage> render_pages(
      const std::filesystem::path& pdf_path,
      const std::filesystem::path& output_directory) = 0;

  /// Return page-local native text. `nullopt` means native extraction failed;
  /// an empty string means extraction ran but found no text.
  [[nodiscard]] virtual std::optional<std::string> extract_native_text(
      const std::filesystem::path& pdf_path,
      std::uint32_t page_number,
      const std::filesystem::path& working_directory) = 0;
};

/// Pluggable local OCR boundary.
class OcrProvider {
 public:
  virtual ~OcrProvider() = default;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual OcrResult recognize(
      const std::filesystem::path& image_path) = 0;
};

/// Poppler command-line adapter using `pdftoppm` and `pdftotext`.
class PopplerPdfAdapter final : public PdfAdapter {
 public:
  explicit PopplerPdfAdapter(
      std::shared_ptr<CommandRunner> command_runner =
          std::make_shared<LocalCommandRunner>(),
      std::string renderer_executable = "pdftoppm",
      std::string text_executable = "pdftotext");

  [[nodiscard]] std::string_view name() const noexcept override;
  [[nodiscard]] std::vector<RenderedPage> render_pages(
      const std::filesystem::path& pdf_path,
      const std::filesystem::path& output_directory) override;
  [[nodiscard]] std::optional<std::string> extract_native_text(
      const std::filesystem::path& pdf_path,
      std::uint32_t page_number,
      const std::filesystem::path& working_directory) override;

 private:
  std::shared_ptr<CommandRunner> command_runner_;
  std::string renderer_executable_;
  std::string text_executable_;
};

/// Local typed-document OCR adapter using the Tesseract command-line program.
class TesseractOcrProvider final : public OcrProvider {
 public:
  explicit TesseractOcrProvider(
      std::string language,
      std::shared_ptr<CommandRunner> command_runner =
          std::make_shared<LocalCommandRunner>(),
      std::string executable = "tesseract");

  [[nodiscard]] std::string_view name() const noexcept override;
  [[nodiscard]] OcrResult recognize(
      const std::filesystem::path& image_path) override;

 private:
  std::string language_;
  std::shared_ptr<CommandRunner> command_runner_;
  std::string executable_;
};

}  // namespace kc::extraction
