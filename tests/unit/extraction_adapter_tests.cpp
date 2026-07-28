#include "kc/extraction/adapters.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

class FixtureCommandRunner final : public kc::extraction::CommandRunner {
 public:
  [[nodiscard]] int run(
      const std::string_view executable,
      const std::vector<std::string>& arguments) override {
    commands.emplace_back(executable);
    if (executable == "fixture-renderer") {
      const auto prefix = std::filesystem::path(arguments.back());
      write_file(prefix.string() + "-1.png", "page one image");
      write_file(prefix.string() + "-2.png", "page two image");
      return 0;
    }
    if (executable == "fixture-text") {
      write_file(arguments.back(), "Native page text");
      return 0;
    }
    if (executable == "fixture-ocr") {
      auto output = std::filesystem::path(arguments.at(1));
      output += ".txt";
      write_file(output, "Locally recognized text");
      return 0;
    }
    return 127;
  }

  std::vector<std::string> commands;
};

}  // namespace

TEST_CASE("Poppler and Tesseract adapters use injectable local commands") {
  kc::test::TemporaryDirectory temporary;
  const auto pdf = temporary.path() / "fixture.pdf";
  const auto work = temporary.path() / "work";
  std::filesystem::create_directories(work);
  write_file(pdf, "%PDF fixture");
  auto runner = std::make_shared<FixtureCommandRunner>();

  kc::extraction::PopplerPdfAdapter poppler(
      runner, "fixture-renderer", "fixture-text");
  const auto pages = poppler.render_pages(pdf, work);
  REQUIRE(pages.size() == 2);
  CHECK(pages[0].page_number == 1);
  CHECK(pages[1].page_number == 2);
  const auto native = poppler.extract_native_text(pdf, 1, work);
  REQUIRE(native.has_value());
  CHECK(*native == "Native page text");

  kc::extraction::TesseractOcrProvider tesseract(
      "eng", runner, "fixture-ocr");
  const auto ocr = tesseract.recognize(pages[1].image_path);
  CHECK(ocr.succeeded);
  CHECK(ocr.text == "Locally recognized text");
  CHECK(runner->commands ==
        std::vector<std::string>{
            "fixture-renderer", "fixture-text", "fixture-ocr"});
}
