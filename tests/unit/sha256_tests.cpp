#include "kc/import/sha256.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

TEST_CASE("SHA-256 matches standard test vectors") {
  kc::test::TemporaryDirectory temporary;
  const auto empty_path = temporary.path() / "empty.txt";
  const auto abc_path = temporary.path() / "abc.txt";
  const auto multi_block_path = temporary.path() / "multi-block.txt";
  {
    std::ofstream empty(empty_path, std::ios::binary);
    std::ofstream abc(abc_path, std::ios::binary);
    std::ofstream multi_block(multi_block_path, std::ios::binary);
    abc << "abc";
    multi_block << "abcdbcdecdefdefgefghfghighijhijk"
                   "ijkljklmklmnlmnomnopnopq";
  }

  CHECK(kc::source_import::sha256_file(empty_path) ==
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855");
  CHECK(kc::source_import::sha256_file(abc_path) ==
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
  CHECK(kc::source_import::sha256_text("abc") ==
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
  CHECK(kc::source_import::sha256_file(multi_block_path) ==
        "248d6a61d20638b8e5c026930c3e6039"
        "a33ce45964ff2167f6ecedd419db06c1");
}
