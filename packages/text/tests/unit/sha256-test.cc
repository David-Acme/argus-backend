#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <shared/utils/sha256/sha256.hxx>
#include <string>

TEST_CASE("sha256 matches the standard known-answer vectors")
{
  CHECK(argus::hash::sha256Hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(argus::hash::sha256Hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(argus::hash::sha256Hex(
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256 handles the 56 and 64 byte block boundaries")
{
  const std::string fiftyFive(55, 'a');
  const std::string fiftySix(56, 'a');
  const std::string sixtyFour(64, 'a');
  CHECK(argus::hash::sha256Hex(fiftyFive) ==
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  CHECK(argus::hash::sha256Hex(fiftySix) ==
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  CHECK(argus::hash::sha256Hex(sixtyFour) ==
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST_CASE("chunked updates equal a single update")
{
  argus::hash::Sha256 chunked;
  const std::string payload = "the quick brown fox jumps over the lazy dog";
  for (const char byte : payload)
    chunked.update(&byte, 1);
  CHECK(argus::hash::hex(chunked.digest()) == argus::hash::sha256Hex(payload));
}

TEST_CASE("length-prefixed fields never collide across boundaries")
{
  CHECK(argus::hash::sha256Hex(std::string("a\nb") + "c") !=
        argus::hash::sha256Hex(std::string("a") + "b\nc"));
}
