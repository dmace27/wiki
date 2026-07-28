#include "kc/extraction/adapters.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kc::extraction {
namespace {

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw AdapterError("adapter did not produce expected text output");
  }
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string padded_page_number(const std::uint32_t page_number) {
  std::ostringstream output;
  output << std::setw(4) << std::setfill('0') << page_number;
  return output.str();
}

}  // namespace

int LocalCommandRunner::run(
    const std::string_view executable,
    const std::vector<std::string>& arguments) {
  auto executable_string = std::string(executable);
  std::vector<char*> argument_vector;
  argument_vector.reserve(arguments.size() + 2U);
  argument_vector.push_back(executable_string.data());
  for (const auto& argument : arguments) {
    argument_vector.push_back(const_cast<char*>(argument.c_str()));
  }
  argument_vector.push_back(nullptr);

#if defined(_WIN32)
  const auto status =
      _spawnvp(_P_WAIT, executable_string.c_str(), argument_vector.data());
  return status < 0 ? 127 : static_cast<int>(status);
#else
  const auto process = fork();
  if (process < 0) {
    return 126;
  }
  if (process == 0) {
    // Adapter programs write their useful results to explicit files. Redirect
    // stdout so `kc --json` remains exactly one JSON document.
    const auto null_output = open("/dev/null", O_WRONLY);
    if (null_output >= 0) {
      static_cast<void>(dup2(null_output, STDOUT_FILENO));
      close(null_output);
    }
    execvp(executable_string.c_str(), argument_vector.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  int status = 0;
  while (waitpid(process, &status, 0) < 0) {
    if (errno != EINTR) {
      return 126;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 126;
#endif
}

PopplerPdfAdapter::PopplerPdfAdapter(
    std::shared_ptr<CommandRunner> command_runner,
    std::string renderer_executable,
    std::string text_executable)
    : command_runner_(std::move(command_runner)),
      renderer_executable_(std::move(renderer_executable)),
      text_executable_(std::move(text_executable)) {
  if (!command_runner_) {
    throw AdapterError("Poppler adapter requires a command runner");
  }
}

std::string_view PopplerPdfAdapter::name() const noexcept { return "poppler"; }

std::vector<RenderedPage> PopplerPdfAdapter::render_pages(
    const std::filesystem::path& pdf_path,
    const std::filesystem::path& output_directory) {
  std::filesystem::create_directories(output_directory);
  const auto output_prefix = output_directory / "page";
  const std::vector<std::string> arguments{
      "-png", "-r", "150", pdf_path.string(), output_prefix.string()};
  const auto status = command_runner_->run(renderer_executable_, arguments);
  if (status != 0) {
    throw AdapterError(
        "Poppler page rendering failed with exit code " +
        std::to_string(status));
  }

  const std::regex page_pattern(R"(^page-([0-9]+)\.png$)");
  std::vector<RenderedPage> pages;
  for (const auto& entry :
       std::filesystem::directory_iterator(output_directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::smatch match;
    const auto filename = entry.path().filename().string();
    if (!std::regex_match(filename, match, page_pattern)) {
      continue;
    }
    try {
      const auto parsed = std::stoul(match[1].str());
      if (parsed == 0U ||
          parsed > static_cast<unsigned long>(
                       std::numeric_limits<std::uint32_t>::max())) {
        throw AdapterError("Poppler produced an invalid PDF page number");
      }
      pages.push_back(
          {static_cast<std::uint32_t>(parsed), entry.path()});
    } catch (const std::invalid_argument&) {
      throw AdapterError("Poppler produced an invalid PDF page filename");
    } catch (const std::out_of_range&) {
      throw AdapterError("Poppler produced an out-of-range PDF page number");
    }
  }
  std::ranges::sort(pages, {}, &RenderedPage::page_number);
  if (pages.empty()) {
    throw AdapterError("Poppler rendered no PDF pages");
  }
  for (std::size_t index = 0; index < pages.size(); ++index) {
    const auto expected = static_cast<std::uint32_t>(index + 1U);
    if (pages[index].page_number != expected) {
      throw AdapterError("Poppler output has a missing PDF page");
    }
  }
  return pages;
}

std::optional<std::string> PopplerPdfAdapter::extract_native_text(
    const std::filesystem::path& pdf_path,
    const std::uint32_t page_number,
    const std::filesystem::path& working_directory) {
  const auto output_path =
      working_directory /
      ("native-page-" + padded_page_number(page_number) + ".txt");
  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
  const auto page = std::to_string(page_number);
  const std::vector<std::string> arguments{
      "-f", page, "-l", page, "-layout", "-enc", "UTF-8",
      pdf_path.string(), output_path.string()};
  if (command_runner_->run(text_executable_, arguments) != 0 ||
      !std::filesystem::is_regular_file(output_path)) {
    return std::nullopt;
  }
  return read_text_file(output_path);
}

TesseractOcrProvider::TesseractOcrProvider(
    std::string language,
    std::shared_ptr<CommandRunner> command_runner,
    std::string executable)
    : language_(std::move(language)),
      command_runner_(std::move(command_runner)),
      executable_(std::move(executable)) {
  if (language_.empty()) {
    throw AdapterError("Tesseract language must not be empty");
  }
  if (!command_runner_) {
    throw AdapterError("Tesseract adapter requires a command runner");
  }
}

std::string_view TesseractOcrProvider::name() const noexcept {
  return "tesseract";
}

OcrResult TesseractOcrProvider::recognize(
    const std::filesystem::path& image_path) {
  const auto output_base =
      image_path.parent_path() / (image_path.stem().string() + "-ocr");
  auto output_path = output_base;
  output_path += ".txt";
  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
  const std::vector<std::string> arguments{
      image_path.string(), output_base.string(), "-l", language_, "--psm", "6"};
  const auto status = command_runner_->run(executable_, arguments);
  if (status != 0) {
    return {
        .succeeded = false,
        .error_message =
            "Tesseract OCR failed with exit code " + std::to_string(status)};
  }
  if (!std::filesystem::is_regular_file(output_path)) {
    return {
        .succeeded = false,
        .error_message = "Tesseract OCR produced no text output"};
  }
  return {.succeeded = true, .text = read_text_file(output_path)};
}

}  // namespace kc::extraction
