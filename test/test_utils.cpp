#include "case_fold.h"
#include "catch.hpp"
#include "crypto.h"
#include "myutils.h"
#include "platform.h"

#include <cryptopp/base32.h>

TEST_CASE("Test endian")
{
    using namespace securefs;

    uint32_t a = 0xABCDEF;
    byte raw[4];
    to_little_endian(a, raw);
    REQUIRE(raw[0] == 0xEF);
    REQUIRE(raw[1] == 0xCD);
    REQUIRE(raw[2] == 0xAB);
    REQUIRE(raw[3] == 0);
    REQUIRE(from_little_endian<uint32_t>(raw) == 0xABCDEF);
}

TEST_CASE("Test string")
{
    REQUIRE((securefs::split("/tmp//abcde/123/", '/')
             == std::vector<std::string>{"tmp", "abcde", "123"}));
    REQUIRE((securefs::split("bal/dd9", '/') == std::vector<std::string>{"bal", "dd9"}));
    REQUIRE((securefs::split("cdafadfm", ' ') == std::vector<std::string>{"cdafadfm"}));
    REQUIRE((securefs::split("", 'a')).empty());
    REQUIRE((securefs::split("//////", '/')).empty());
    REQUIRE(securefs::strprintf("%s %04d", "rsy", 9) == "rsy 0009");
    std::string long_string(6000, 'r');
    REQUIRE(securefs::strprintf("%s", long_string.c_str()) == long_string);
}

TEST_CASE("Test conversion of hex")
{
    securefs::id_type id;
    securefs::generate_random(id.data(), id.size());
    auto hex = securefs::hexify(id);
    securefs::id_type id_copy;
    securefs::parse_hex(hex, id_copy.data(), id_copy.size());
    REQUIRE(memcmp(id.data(), id_copy.data(), id.size()) == 0);
}

TEST_CASE("Base32")
{
    CryptoPP::Base32Encoder enc;
    const byte from[] = "hello";
    byte out[256] = {}, out2[256] = {};
    enc.Put(from, sizeof(from));
    enc.MessageEnd();
    enc.Get(out, sizeof(out));

    enc.Initialize();
    enc.Put(from, sizeof(from));
    enc.MessageEnd();
    enc.Get(out2, sizeof(out2));
    CAPTURE(out);
    CAPTURE(out2);
    REQUIRE(strcmp((char*)out, (char*)out2) == 0);
}

TEST_CASE("our base32")
{
    std::string input, output, decoded;
    input.reserve(128);
    for (size_t i = 0; i < 128; ++i)
    {
        if (i > 0)
        {
            input.resize(i, 0);
            securefs::generate_random((byte*)input.data(), i);
        }
        securefs::base32_encode((const byte*)input.data(), i, output);
        CAPTURE(output);
        securefs::base32_decode(output.data(), output.size(), decoded);
        CHECK(input == decoded);
    }
}

TEST_CASE("our base32 against CryptoPP")
{
    std::string input, output;
    char buffer[4000];
    input.reserve(128);
    for (size_t i = 0; i < 128; ++i)
    {
        if (i > 0)
        {
            input.resize(i, 0);
            securefs::generate_random((byte*)input.data(), i);
        }
        securefs::base32_encode((const byte*)input.data(), i, output);

        CryptoPP::Base32Encoder enc;
        enc.Put((const byte*)input.data(), input.size());
        enc.MessageEnd();
        memset(buffer, 0, sizeof(buffer));
        enc.Get((byte*)buffer, sizeof(buffer));
        CHECK(output == buffer);
    }
}

TEST_CASE("case fold")
{
    using securefs::case_fold;

    REQUIRE(case_fold(570) == 11365);
    REQUIRE(case_fold("\xc8\xba") == "\xe2\xb1\xa5");
    REQUIRE(case_fold(
                "AabC\xce\xa3\xce\xaf\xcf\x83\xcf\x85\xcf\x86\xce\xbf\xcf\x82\xef\xac\x81\xc3\x86")
            == "aabc\xcf\x83\xce\xaf\xcf\x83\xcf\x85\xcf\x86\xce\xbf\xcf\x83\xef\xac\x81\xc3\xa6");
}
