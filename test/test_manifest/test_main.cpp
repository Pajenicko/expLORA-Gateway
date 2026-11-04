/**
 * Native unit tests for the package manifest (src/Update/Manifest.h): the
 * signature line, the JSON rules, replay, offers and the install / fetch
 * checks. Manifests and images come from the Python release tools
 * (fixtures.h, made by gen_fixtures.py with a throwaway test key); the field
 * matrix parses hand-written JSON directly; the rule corpus checks the
 * firmware against explora_manifest.check_json case by case.
 *
 * Run:   pio test -e native -f test_manifest
 */

#include <ArduinoJson.h>
#include <cstring>
#include <string>
#include <unity.h>
#include <vector>

#include "../../src/Security/SigVerify.cpp"
#include "../../src/Update/Manifest.h"
#include "fixtures.h"

using Manifest::Result;
using Bytes = std::vector<char>;

void setUp(void) {}
void tearDown(void) {}

static ReleaseKeys::Key KEYS[2];
static const Manifest::Limits LIM = {100, 0x1F0000, 64, 114816};
static const char *BASE = "https://example.com/explora/v2/";

static void initKeys()
{
    KEYS[0] = ReleaseKeys::Key{0, {}};
    KEYS[1].keyId = TEST_KEY_ID;
    memcpy(KEYS[1].point, TEST_KEY_POINT, sizeof(TEST_KEY_POINT));
}

static Bytes copy(const uint8_t *p, size_t n)
{
    return Bytes((const char *)p, (const char *)p + n);
}

static Result load(Bytes f, const char *board, uint32_t stored, Manifest::Doc &d, std::string *why = nullptr)
{
    char w[80];
    Result r = Manifest::load(f.data(), f.size(), board, LIM, stored, d, w, sizeof(w), KEYS, 2);
    if (why)
    {
        *why = w;
    }
    return r;
}

static void sha(const uint8_t *p, size_t n, uint8_t out[32])
{
    Sha256 h;
    h.update(p, n);
    TEST_ASSERT_TRUE(h.finish(out));
}

static Manifest::Doc doc; // ~2.8 kB, not on the stack

void test_good_manifest()
{
    TEST_ASSERT_EQUAL(Result::Ok, load(copy(M_GOOD, sizeof(M_GOOD)), "pro", 0, doc));
    TEST_ASSERT_EQUAL_UINT32(5, doc.seq);
    TEST_ASSERT_EQUAL_HEX32(TEST_KEY_ID, doc.keyId);
    TEST_ASSERT_TRUE(doc.hasGateway);
    TEST_ASSERT_EQUAL_HEX32(0x020103, doc.gateway.version);
    TEST_ASSERT_EQUAL_UINT32(sizeof(GW_PRO), doc.gateway.size);
    uint8_t d[32];
    sha(GW_PRO, sizeof(GW_PRO), d);
    TEST_ASSERT_EQUAL_MEMORY(d, doc.gateway.sha256, 32);
    TEST_ASSERT_EQUAL_STRING((std::string(BASE) + "gw-pro-2.1.3.bin").c_str(), doc.gateway.url);
    TEST_ASSERT_EQUAL(2, doc.sensorCount);
    const Manifest::Package *p3 = doc.sensorFor(3), *p4 = doc.sensorFor(4);
    TEST_ASSERT_NOT_NULL(p3);
    TEST_ASSERT_NOT_NULL(p4);
    TEST_ASSERT_NULL(doc.sensorFor(5));
    TEST_ASSERT_EQUAL_HEX32(0x00020C, p3->version);
    TEST_ASSERT_EQUAL_HEX32(0x000301, p4->version);
    TEST_ASSERT_EQUAL_UINT32(sizeof(HW4_IMG), p4->size);
    sha(HW3_IMG, sizeof(HW3_IMG), d);
    TEST_ASSERT_EQUAL_MEMORY(d, p3->sha256, 32);
    TEST_ASSERT_EQUAL_STRING((std::string(BASE) + "hw0004-0.3.1.bin").c_str(), p4->url);
    // The other board takes its own entry
    TEST_ASSERT_EQUAL(Result::Ok, load(copy(M_GOOD, sizeof(M_GOOD)), "lite", 0, doc));
    TEST_ASSERT_EQUAL_HEX32(0x020104, doc.gateway.version);
    TEST_ASSERT_EQUAL_UINT32(sizeof(GW_LITE), doc.gateway.size);
}

void test_replay()
{
    TEST_ASSERT_EQUAL(Result::Ok, load(copy(M_GOOD, sizeof(M_GOOD)), "pro", 5, doc)); // equal: checked again
    TEST_ASSERT_EQUAL(Result::Ok, load(copy(M_SEQ6, sizeof(M_SEQ6)), "pro", 5, doc));
    TEST_ASSERT_EQUAL_UINT32(6, doc.seq);
    std::string why;
    TEST_ASSERT_EQUAL(Result::Replay, load(copy(M_GOOD, sizeof(M_GOOD)), "pro", 6, doc, &why));
    TEST_ASSERT_EQUAL_STRING("seq 5 < 6", why.c_str());
    TEST_ASSERT_EQUAL(Result::Replay, load(copy(M_SEQ6, sizeof(M_SEQ6)), "pro", 0xFFFFFFFFu, doc));
    // Nothing of a replayed manifest is used
    TEST_ASSERT_FALSE(doc.hasGateway);
    TEST_ASSERT_EQUAL(0, doc.sensorCount);
    TEST_ASSERT_EQUAL_HEX32(TEST_KEY_ID, doc.keyId);
    // The replay rule comes after the signature: a bad one never reads as a replay
    Bytes f = copy(M_GOOD, sizeof(M_GOOD));
    f[f.size() - 3] ^= 1;
    TEST_ASSERT_EQUAL(Result::BadSig, load(f, "pro", 6, doc));
}

void test_signature_line()
{
    TEST_ASSERT_EQUAL(Result::NoKey, load(copy(M_OTHER, sizeof(M_OTHER)), "pro", 0, doc));
    TEST_ASSERT_EQUAL_HEX32(OTHER_KEY_ID, doc.keyId);
    TEST_ASSERT_EQUAL(0, doc.seq); // nothing parsed
    // No key at all (the production table is empty today)
    char w[8];
    Bytes g = copy(M_GOOD, sizeof(M_GOOD));
    TEST_ASSERT_EQUAL(Result::NoKey, Manifest::load(g.data(), g.size(), "pro", LIM, 0, doc, w, sizeof(w)));
    const Bytes good = copy(M_GOOD, sizeof(M_GOOD));
    // Every single bit of the JSON is covered
    for (size_t i = Manifest::HEADER_LEN; i < good.size(); i += 37)
    {
        Bytes f = good;
        f[i] ^= 0x20;
        TEST_ASSERT_EQUAL(Result::BadSig, load(f, "pro", 0, doc));
    }
    // A signature digit changed / a byte appended
    Bytes f = good;
    f[20] = f[20] == '0' ? '1' : '0';
    TEST_ASSERT_EQUAL(Result::BadSig, load(f, "pro", 0, doc));
    f = good;
    f.push_back(' ');
    TEST_ASSERT_EQUAL(Result::BadSig, load(f, "pro", 0, doc));
    // Line 1 malformed: uppercase hex, CR LF, extra space, other magic, cut
    f = good;
    f[30] = (char)toupper(f[30]) == f[30] ? 'A' : (char)toupper(f[30]);
    if (f[30] >= '0' && f[30] <= '9')
    {
        f[30] = 'A';
    }
    TEST_ASSERT_EQUAL(Result::Header, load(f, "pro", 0, doc));
    f = good;
    f.insert(f.begin() + Manifest::HEADER_LEN - 1, '\r');
    TEST_ASSERT_EQUAL(Result::Header, load(f, "pro", 0, doc));
    f = good;
    f.insert(f.begin() + 5, ' ');
    TEST_ASSERT_EQUAL(Result::Header, load(f, "pro", 0, doc));
    f = good;
    f[4] = '2';
    TEST_ASSERT_EQUAL(Result::Header, load(f, "pro", 0, doc));
    f = good;
    f.insert(f.begin(), ' ');
    TEST_ASSERT_EQUAL(Result::Header, load(f, "pro", 0, doc));
    TEST_ASSERT_EQUAL(Result::Header, load(Bytes(good.begin(), good.begin() + 100), "pro", 0, doc));
    TEST_ASSERT_EQUAL(Result::Header, load(Bytes(), "pro", 0, doc));
    // Key id 0 is malformed, not a missing key
    f = good;
    memcpy(f.data() + 6, "00000000", 8);
    TEST_ASSERT_EQUAL(Result::KeyIdZero, load(f, "pro", 0, doc));
    // An unknown key (well-formed id)
    f = good;
    memcpy(f.data() + 6, "12345678", 8);
    TEST_ASSERT_EQUAL(Result::NoKey, load(f, "pro", 0, doc));
    TEST_ASSERT_EQUAL_HEX32(0x12345678, doc.keyId);
    // Over 4 kB: refused before anything else
    f = good;
    f.resize(Manifest::MAX_FILE + 1, ' ');
    TEST_ASSERT_EQUAL(Result::TooLarge, load(f, "pro", 0, doc));
    f.resize(Manifest::MAX_FILE);
    TEST_ASSERT_EQUAL(Result::BadSig, load(f, "pro", 0, doc)); // exactly 4096: checked (and not signed so)
}

void test_crlf_extra_fields_missing_board()
{
    // Pretty JSON with CRLF, unknown keys at every level, "issued" negative
    // (informational only), no Pro entry, one sensor
    TEST_ASSERT_EQUAL(Result::Ok, load(copy(M_CRLF, sizeof(M_CRLF)), "pro", 0, doc));
    TEST_ASSERT_FALSE(doc.hasGateway);
    TEST_ASSERT_EQUAL(1, doc.sensorCount);
    TEST_ASSERT_NOT_NULL(doc.sensorFor(3));
    TEST_ASSERT_EQUAL(Manifest::Offer::None, Manifest::gatewayOffer(doc, 0x020000));
    TEST_ASSERT_EQUAL(Result::Ok, load(copy(M_CRLF, sizeof(M_CRLF)), "lite", 0, doc));
    TEST_ASSERT_TRUE(doc.hasGateway);
    TEST_ASSERT_EQUAL_HEX32(0x020104, doc.gateway.version);
}

// Entries skipped by the parse (SkipFn)
static std::vector<std::string> skips;
static void onSkip(const char *what)
{
    skips.push_back(what);
}

void test_signed_but_invalid()
{
    // A sensor entry that breaks a rule: skipped (and reported), the rest used
    std::string why;
    skips.clear();
    Bytes f = copy(M_BADFIELD, sizeof(M_BADFIELD));
    char w[80];
    TEST_ASSERT_EQUAL(Result::Ok, Manifest::load(f.data(), f.size(), "pro", LIM, 0, doc, w, sizeof(w), KEYS, 2, onSkip));
    TEST_ASSERT_EQUAL(1, skips.size());
    TEST_ASSERT_EQUAL_STRING("sensor.0004.sha256", skips[0].c_str());
    TEST_ASSERT_EQUAL(1, doc.skipped);
    TEST_ASSERT_NULL(doc.sensorFor(4));
    TEST_ASSERT_NOT_NULL(doc.sensorFor(3));
    TEST_ASSERT_TRUE(doc.hasGateway);
    TEST_ASSERT_EQUAL(Result::Field, load(copy(M_NOSEQ, sizeof(M_NOSEQ)), "pro", 0, doc, &why));
    TEST_ASSERT_EQUAL_STRING("seq", why.c_str());
}

// A replayed manifest is refused before any entry is looked at: nothing
// reported as skipped, nothing kept (the signed fixture with a bad entry)
void test_replay_reports_no_skips()
{
    skips.clear();
    Bytes f = copy(M_BADFIELD, sizeof(M_BADFIELD));
    char w[80];
    TEST_ASSERT_EQUAL(Result::Ok, Manifest::load(f.data(), f.size(), "pro", LIM, 0, doc, w, sizeof(w), KEYS, 2, onSkip));
    const uint32_t seq = doc.seq;
    skips.clear();
    TEST_ASSERT_EQUAL(Result::Replay,
                      Manifest::load(f.data(), f.size(), "pro", LIM, seq + 1, doc, w, sizeof(w), KEYS, 2, onSkip));
    TEST_ASSERT_EQUAL(0, skips.size());
    TEST_ASSERT_EQUAL(0, doc.skipped);
    TEST_ASSERT_FALSE(doc.hasGateway);
    TEST_ASSERT_EQUAL(0, doc.sensorCount);
    TEST_ASSERT_EQUAL(seq, doc.seq);
}

// More than 255 skipped entries: counted, not wrapped (a uint8_t count
// read 44 for 300)
void test_skip_count_does_not_wrap()
{
    std::string json = "{\"seq\":1,\"sensor\":{";
    for (int i = 0; i < 300; i++)
    {
        char k[16];
        snprintf(k, sizeof(k), "%s\"%04X\":0", i ? "," : "", i + 1);
        json += k;
    }
    json += "}}";
    char w[80];
    skips.clear();
    TEST_ASSERT_EQUAL(Result::Ok, Manifest::parse(json.data(), json.size(), "pro", LIM, doc, w, sizeof(w), onSkip));
    TEST_ASSERT_EQUAL(300, doc.skipped);
    TEST_ASSERT_EQUAL(300, skips.size());
    TEST_ASSERT_EQUAL(0, doc.sensorCount);
}

// The JSON rules on hand-written bodies (parse() after a verified line)
static Result parseText(const std::string &json, const char *board = "pro", std::string *why = nullptr,
                        const Manifest::Limits &lim = LIM)
{
    std::vector<char> buf(json.begin(), json.end());
    char w[80];
    skips.clear();
    Result r = Manifest::parse(buf.data(), buf.size(), board, lim, doc, w, sizeof(w), onSkip);
    if (why)
    {
        *why = w;
    }
    return r;
}

static const char *SHA = "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"";

static std::string pkg(const std::string &version = "2.0.1", const std::string &url = "https://h.example/f.bin",
                       const std::string &size = "1000", const std::string &sha = SHA)
{
    return "{\"version\":\"" + version + "\",\"url\":\"" + url + "\",\"size\":" + size + "," + sha + "}";
}

static void field(const std::string &json, const char *expect, const char *board = "pro")
{
    std::string why;
    Result r = parseText(json, board, &why);
    TEST_ASSERT_EQUAL_MESSAGE(Result::Field, r, json.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expect, why.c_str(), json.c_str());
}

// A gateway entry that breaks a rule: this board's = no gateway offer (why
// names the field, nothing skipped); the other board's = skipped. Either
// way the manifest is accepted
static void gatewayEntry(const std::string &json, const char *b, const char *expect)
{
    std::string why;
    TEST_ASSERT_EQUAL_MESSAGE(Result::Ok, parseText(json, b, &why), json.c_str());
    TEST_ASSERT_TRUE_MESSAGE(doc.gatewayInvalid, json.c_str());
    TEST_ASSERT_FALSE(doc.hasGateway);
    TEST_ASSERT_EQUAL(Manifest::Offer::None, Manifest::gatewayOffer(doc, 0x010000));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expect, why.c_str(), json.c_str());
    TEST_ASSERT_EQUAL(0, doc.skipped);
    const char *other = strcmp(b, "pro") == 0 ? "lite" : "pro";
    TEST_ASSERT_EQUAL_MESSAGE(Result::Ok, parseText(json, other, &why), json.c_str());
    TEST_ASSERT_FALSE(doc.gatewayInvalid);
    TEST_ASSERT_FALSE(doc.hasGateway);
    TEST_ASSERT_EQUAL(1, doc.skipped);
    TEST_ASSERT_EQUAL(1, skips.size());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expect, skips[0].c_str(), json.c_str());
}

// A sensor entry that breaks a rule: skipped, never offered
static void sensorEntry(const std::string &json, uint16_t hw, const char *expect)
{
    TEST_ASSERT_EQUAL_MESSAGE(Result::Ok, parseText(json), json.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(1, doc.skipped, json.c_str());
    TEST_ASSERT_EQUAL(0, doc.sensorCount);
    if (hw)
    {
        TEST_ASSERT_NULL(doc.sensorFor(hw));
    }
    TEST_ASSERT_EQUAL(1, skips.size());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expect, skips[0].c_str(), json.c_str());
}

void test_field_matrix()
{
    TEST_ASSERT_EQUAL(Result::Ok, parseText("{\"seq\":0}"));
    TEST_ASSERT_FALSE(doc.hasGateway);
    TEST_ASSERT_EQUAL(Result::Ok, parseText("{\"seq\":4294967295,\"gateway\":{\"pro\":" + pkg() + "}}"));
    TEST_ASSERT_EQUAL_UINT32(4294967295u, doc.seq);
    TEST_ASSERT_TRUE(doc.hasGateway);
    TEST_ASSERT_EQUAL_UINT32(1000, doc.gateway.size);
    // seq
    field("{}", "seq");
    field("{\"seq\":-1}", "seq");
    field("{\"seq\":4294967296}", "seq");
    field("{\"seq\":1.5}", "seq");
    field("{\"seq\":\"5\"}", "seq");
    field("{\"seq\":null}", "seq");
    // structure
    field("{\"seq\":1,\"gateway\":[]}", "gateway");
    gatewayEntry("{\"seq\":1,\"gateway\":{\"pro\":5}}", "pro", "gateway.pro (not an object)");
    field("{\"seq\":1,\"sensor\":\"x\"}", "sensor");
    field("{\"seq\":1,\"sensor\":[],\"gateway\":{\"pro\":" + pkg() + "}}", "sensor");
    TEST_ASSERT_EQUAL(Result::Json, parseText("[1]"));
    TEST_ASSERT_EQUAL(Result::Json, parseText("garbage"));
    TEST_ASSERT_EQUAL(Result::Json, parseText(""));
    TEST_ASSERT_EQUAL(Result::Json, parseText("{\"seq\":1,"));
    // package fields: this board's entry = no gateway offer, the other's skipped
    for (const char *b : {"pro", "lite"})
    {
        const std::string w = std::string("gateway.") + b;
        auto gw = [&](const std::string &p) { return "{\"seq\":1,\"gateway\":{\"" + std::string(b) + "\":" + p + "}}"; };
        auto field = [&](const std::string &json, const std::string &expect) { gatewayEntry(json, b, expect.c_str()); };
        field(gw(pkg("2.0")), (w + ".version"));
        field(gw(pkg("2.0.0-dev")), (w + ".version"));
        field(gw(pkg("256.0.0")), (w + ".version"));
        field(gw(pkg(" 2.0.0")), (w + ".version"));
        field(gw("{\"url\":\"https://h/f\",\"size\":1000," + std::string(SHA) + "}"), (w + ".version"));
        field(gw(pkg("2.0.1", "http://h.example/f.bin")), (w + ".url"));
        field(gw(pkg("2.0.1", "https://h.example/a b.bin")), (w + ".url"));
        field(gw(pkg("2.0.1", "https:///f.bin")), (w + ".url"));
        field(gw(pkg("2.0.1", "https://h.example/" + std::string(240, 'a'))), (w + ".url"));
        field(gw(pkg("2.0.1", "https://h.example/f", "99")), (w + ".size")); // below the gateway minimum
        field(gw(pkg("2.0.1", "https://h.example/f", "2031617")), (w + ".size"));
        field(gw(pkg("2.0.1", "https://h.example/f", "\"1000\"")), (w + ".size"));
        field(gw(pkg("2.0.1", "https://h.example/f", "-1")), (w + ".size"));
        field(gw(pkg("2.0.1", "https://h.example/f", "1000", "\"sha256\":\"0123\"")), (w + ".sha256").c_str());
        field(gw(pkg("2.0.1", "https://h.example/f", "1000",
                     "\"sha256\":\"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\"")),
              (w + ".sha256"));
        field(gw(pkg("2.0.1", "https://h.example/f", "1000",
                     "\"sha256\":\"g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"")),
              (w + ".sha256"));
        field(gw(pkg("2.0.1", "https://h.example/f", "1000",
                     "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0\"")),
              (w + ".sha256"));
    }
    // URL of exactly 256 characters is fine
    const std::string u256 = "https://h.example/" + std::string(256 - 18, 'a');
    TEST_ASSERT_EQUAL(256, u256.size());
    TEST_ASSERT_EQUAL(Result::Ok, parseText("{\"seq\":1,\"gateway\":{\"pro\":" + pkg("2.0.1", u256) + "}}"));
    TEST_ASSERT_EQUAL_STRING(u256.c_str(), doc.gateway.url);
    // sensor keys and sizes (sensor limits)
    auto sn = [](const std::string &k, const std::string &p) { return "{\"seq\":1,\"sensor\":{\"" + k + "\":" + p + "}}"; };
    TEST_ASSERT_EQUAL(Result::Ok, parseText(sn("00A3", pkg("0.2.13", "https://h/s.bin", "96584"))));
    TEST_ASSERT_NOT_NULL(doc.sensorFor(0xA3));
    sensorEntry(sn("00a3", pkg()), 0xA3, "sensor.00a3 (HW type key)");
    sensorEntry(sn("3", pkg()), 3, "sensor.3 (HW type key)");
    sensorEntry(sn("00003", pkg()), 3, "sensor.00003 (HW type key)");
    sensorEntry(sn("0000", pkg()), 0, "sensor.0000 (HW type key)");
    sensorEntry(sn("0x03", pkg()), 3, "sensor.0x03 (HW type key)");
    sensorEntry(sn("0\\\"<x>&", pkg()), 0, "sensor.0??x?? (HW type key)"); // shown safely
    sensorEntry(sn("0003", pkg("0.2.13", "https://h/s.bin", "63")), 3, "sensor.0003.size");
    sensorEntry(sn("0003", pkg("0.2.13", "https://h/s.bin", "114817")), 3, "sensor.0003.size");
    sensorEntry(sn("0003", pkg("0.2.256", "https://h/s.bin", "1000")), 3, "sensor.0003.version");
    sensorEntry(sn("0003", pkg("0.2.1", "https://a@b@h/s.bin", "1000")), 3, "sensor.0003.url");
    sensorEntry(sn("0003", "[]"), 3, "sensor.0003 (not an object)");
    // At most 8 sensor packages: the first 8 valid ones are kept
    std::string many = "{\"seq\":1,\"sensor\":{";
    for (int i = 1; i <= 9; i++)
    {
        char k[8];
        snprintf(k, sizeof(k), "%04X", i);
        many += std::string(i > 1 ? "," : "") + "\"" + k + "\":" + pkg("0.2.1", "https://h/s.bin", "1000");
    }
    std::string eight = many.substr(0, many.rfind(",\"0009\"")) + "}}";
    TEST_ASSERT_EQUAL(Result::Ok, parseText(eight));
    TEST_ASSERT_EQUAL(8, doc.sensorCount);
    std::string why;
    TEST_ASSERT_EQUAL(Result::Ok, parseText(many + "}}", "pro", &why));
    TEST_ASSERT_EQUAL(8, doc.sensorCount);
    TEST_ASSERT_EQUAL(1, doc.skipped);
    TEST_ASSERT_NULL(doc.sensorFor(9));
    TEST_ASSERT_EQUAL_STRING("sensor.0009 (more than 8 sensor packages)", skips[0].c_str());
    // Unknown keys: ignored at the top, in "gateway" and inside packages
    TEST_ASSERT_EQUAL(Result::Ok,
                      parseText("{\"x\":[1,2,{\"y\":3}],\"seq\":2,\"issued\":\"never compared\",\"gateway\":{\"pro\":"
                                "{\"version\":\"2.0.1\",\"url\":\"https://h/f\",\"size\":1000," +
                                std::string(SHA) + ",\"beta\":true,\"notes\":\"x\"},\"esp32c6\":{}},\"sensor\":{}}"));
    TEST_ASSERT_EQUAL_UINT32(2, doc.seq);
    TEST_ASSERT_TRUE(doc.hasGateway);
    // Escapes in strings are decoded before the checks ("\/" is "/")
    TEST_ASSERT_EQUAL(Result::Ok, parseText("{\"seq\":1,\"gateway\":{\"pro\":" + pkg("2.0.1", "https:\\/\\/h\\/f") + "}}"));
    TEST_ASSERT_EQUAL_STRING("https://h/f", doc.gateway.url);
}

// Skipping never turns an invalid entry into an offered one, and never
// hides a valid one: each entry stands alone
void test_skip_is_never_an_offer()
{
    // Invalid own entry next to a valid one of the other board: no offer
    const std::string both = "{\"seq\":3,\"gateway\":{\"pro\":" + pkg("9.9.9", "https://h/f", "50") +
                             ",\"lite\":" + pkg("9.9.9") + "}}";
    std::string why;
    TEST_ASSERT_EQUAL(Result::Ok, parseText(both, "pro", &why));
    TEST_ASSERT_FALSE(doc.hasGateway);
    TEST_ASSERT_TRUE(doc.gatewayInvalid);
    TEST_ASSERT_EQUAL(Manifest::Offer::None, Manifest::gatewayOffer(doc, 0x010000));
    TEST_ASSERT_EQUAL_UINT32(0, doc.gateway.version); // nothing of it kept
    TEST_ASSERT_EQUAL_STRING("", doc.gateway.url);
    TEST_ASSERT_EQUAL(Result::Ok, parseText(both, "lite", &why));
    TEST_ASSERT_TRUE(doc.hasGateway);
    TEST_ASSERT_FALSE(doc.gatewayInvalid);
    TEST_ASSERT_EQUAL(1, doc.skipped);
    // A skipped sensor entry is not offered even though a valid one of
    // another HW type follows, and does not take one of the 8 places
    std::string s = "{\"seq\":3,\"sensor\":{";
    for (int i = 1; i <= 9; i++)
    {
        char k[8];
        snprintf(k, sizeof(k), "%04X", i);
        s += std::string(i > 1 ? "," : "") + "\"" + k + "\":" + pkg("0.2.1", "https://h/s.bin", i == 2 ? "5" : "1000");
    }
    s += "}}";
    TEST_ASSERT_EQUAL(Result::Ok, parseText(s));
    TEST_ASSERT_EQUAL(8, doc.sensorCount);
    TEST_ASSERT_EQUAL(1, doc.skipped);
    TEST_ASSERT_NULL(doc.sensorFor(2));
    TEST_ASSERT_NOT_NULL(doc.sensorFor(9));
    TEST_ASSERT_EQUAL(Manifest::Offer::None, Manifest::sensorOffer(doc.sensorFor(2), false, 0, false));
    // Duplicate keys (the tool refuses them): ArduinoJson keeps the last
    // value, so an invalid duplicate hides nothing the tool would publish
    TEST_ASSERT_EQUAL(Result::Ok,
                      parseText("{\"seq\":1,\"sensor\":{\"0003\":" + pkg("0.2.1", "https://h/s", "1000") + ",\"0003\":" +
                                pkg("0.2.1", "https://h/s", "5") + "}}"));
    TEST_ASSERT_NULL(doc.sensorFor(3));
    // The gateway's invalid entry keeps the sensors usable
    TEST_ASSERT_EQUAL(Result::Ok,
                      parseText("{\"seq\":1,\"gateway\":{\"pro\":{}},\"sensor\":{\"0003\":" +
                                pkg("0.2.1", "https://h/s", "1000") + "}}", "pro", &why));
    TEST_ASSERT_TRUE(doc.gatewayInvalid);
    TEST_ASSERT_EQUAL_STRING("gateway.pro.version", why.c_str());
    TEST_ASSERT_NOT_NULL(doc.sensorFor(3));
}

// The firmware against explora_manifest.check_json, case by case (fixtures
// made by the tool): refused alike, and per entry the same verdict
void test_rule_corpus()
{
    TEST_ASSERT_EQUAL_UINT32(PY_GATEWAY_MIN, Manifest::FIRMWARE_LIMITS.gatewayMin);
    TEST_ASSERT_EQUAL_UINT32(PY_GATEWAY_MAX, Manifest::FIRMWARE_LIMITS.gatewayMax);
    TEST_ASSERT_EQUAL_UINT32(PY_SENSOR_MIN, Manifest::FIRMWARE_LIMITS.sensorMin);
    TEST_ASSERT_EQUAL_UINT32(PY_SENSOR_MAX, Manifest::FIRMWARE_LIMITS.sensorMax);
    TEST_ASSERT_EQUAL(PY_MAX_NODES, Manifest::MAX_NODES);
    TEST_ASSERT_EQUAL(PY_MAX_SENSORS, Manifest::MAX_PACKAGES);
    for (const CorpusCase &c : CORPUS)
    {
        const std::string json((const char *)c.data, c.len);
        for (const char *b : {"pro", "lite"})
        {
            std::string why;
            const Result r = parseText(json, b, &why, Manifest::FIRMWARE_LIMITS);
            const int entry = strcmp(b, "pro") == 0 ? c.pro : c.lite;
            const std::string m = std::string(c.name) + " / " + b + ": " + why;
            TEST_ASSERT_EQUAL_MESSAGE(c.refused != 0, r != Result::Ok, m.c_str());
            if (r != Result::Ok)
            {
                continue;
            }
            TEST_ASSERT_EQUAL_MESSAGE(c.sensorsKept, doc.sensorCount, m.c_str());
            TEST_ASSERT_EQUAL_MESSAGE(entry == 1, doc.hasGateway, m.c_str());
            TEST_ASSERT_EQUAL_MESSAGE(entry == 0, doc.gatewayInvalid, m.c_str());
            TEST_ASSERT_EQUAL_MESSAGE(c.skips - (entry == 0 ? 1 : 0), doc.skipped, m.c_str());
            TEST_ASSERT_EQUAL_MESSAGE(doc.skipped, skips.size(), m.c_str());
        }
    }
}

void test_offers()
{
    Manifest::clear(doc);
    TEST_ASSERT_EQUAL(Manifest::Offer::None, Manifest::gatewayOffer(doc, 0x020000));
    doc.hasGateway = true;
    doc.gateway.version = 0x020001;
    TEST_ASSERT_EQUAL(Manifest::Offer::Install, Manifest::gatewayOffer(doc, 0x020000));
    TEST_ASSERT_EQUAL(Manifest::Offer::Current, Manifest::gatewayOffer(doc, 0x020001));
    TEST_ASSERT_EQUAL(Manifest::Offer::Older, Manifest::gatewayOffer(doc, 0x020100));
    TEST_ASSERT_EQUAL(Manifest::Offer::Unknown, Manifest::gatewayOffer(doc, GatewayImage::VERSION_UNKNOWN));
    TEST_ASSERT_EQUAL(GatewayImage::VERSION_UNKNOWN, GatewayImage::runningVersionOf("2.1.0-rc1"));
    Manifest::Package p;
    p.version = 0x00020D;
    TEST_ASSERT_EQUAL(Manifest::Offer::None, Manifest::sensorOffer(nullptr, true, 0, true));
    TEST_ASSERT_EQUAL(Manifest::Offer::Install, Manifest::sensorOffer(&p, false, 0, false)); // nothing stored
    TEST_ASSERT_EQUAL(Manifest::Offer::Install, Manifest::sensorOffer(&p, true, 0x00020C, true)); // newer
    TEST_ASSERT_EQUAL(Manifest::Offer::Install, Manifest::sensorOffer(&p, true, 0x00020D, false)); // same, signed now
    TEST_ASSERT_EQUAL(Manifest::Offer::Current, Manifest::sensorOffer(&p, true, 0x00020D, true));
    TEST_ASSERT_EQUAL(Manifest::Offer::Older, Manifest::sensorOffer(&p, true, 0x00020E, false));
    TEST_ASSERT_EQUAL_STRING("install", Manifest::offerName(Manifest::Offer::Install));
}

void test_install_checks()
{
    TEST_ASSERT_EQUAL(Result::Ok, load(copy(M_GOOD, sizeof(M_GOOD)), "pro", 0, doc));
    const Manifest::Expect e = Manifest::expectOf(doc.gateway);
    uint8_t d[32];
    sha(GW_PRO, sizeof(GW_PRO), d);
    const uint32_t running = 0x020000;
    TEST_ASSERT_NULL(Manifest::gatewayInstallRefusal(e, sizeof(GW_PRO), d, true, 0x020103, running));
    TEST_ASSERT_EQUAL_STRING("the size differs from the manifest",
                             Manifest::gatewayInstallRefusal(e, sizeof(GW_PRO) - 1, d, true, 0x020103, running));
    uint8_t bad[32];
    memcpy(bad, d, 32);
    bad[31] ^= 1;
    TEST_ASSERT_EQUAL_STRING("the SHA-256 differs from the manifest",
                             Manifest::gatewayInstallRefusal(e, sizeof(GW_PRO), bad, true, 0x020103, running));
    TEST_ASSERT_NOT_NULL(Manifest::gatewayInstallRefusal(e, sizeof(GW_PRO), d, false, 0x020103, running));
    TEST_ASSERT_EQUAL_STRING("the signed version differs from the manifest",
                             Manifest::gatewayInstallRefusal(e, sizeof(GW_PRO), d, true, 0x020104, running));
    TEST_ASSERT_EQUAL_STRING("not newer than the running firmware",
                             Manifest::gatewayInstallRefusal(e, sizeof(GW_PRO), d, true, 0x020103, 0x020103));
    TEST_ASSERT_NOT_NULL(Manifest::gatewayInstallRefusal(e, sizeof(GW_PRO), d, true, 0x020103, 0x030000));
    TEST_ASSERT_NOT_NULL(
        Manifest::gatewayInstallRefusal(e, sizeof(GW_PRO), d, true, 0x020103, GatewayImage::VERSION_UNKNOWN));
    // Sensor image
    const Manifest::Expect s = Manifest::expectOf(*doc.sensorFor(4), 4);
    TEST_ASSERT_TRUE(s.set);
    sha(HW4_IMG, sizeof(HW4_IMG), d);
    TEST_ASSERT_NULL(Manifest::sensorFetchRefusal(s, sizeof(HW4_IMG), d, true, 4, 0x000301));
    TEST_ASSERT_EQUAL_STRING("the HW type differs from the manifest",
                             Manifest::sensorFetchRefusal(s, sizeof(HW4_IMG), d, true, 3, 0x000301));
    TEST_ASSERT_EQUAL_STRING("the version differs from the manifest",
                             Manifest::sensorFetchRefusal(s, sizeof(HW4_IMG), d, true, 4, 0x000302));
    TEST_ASSERT_NOT_NULL(Manifest::sensorFetchRefusal(s, sizeof(HW4_IMG), d, false, 4, 0x000301));
    TEST_ASSERT_NOT_NULL(Manifest::sensorFetchRefusal(s, sizeof(HW4_IMG) + 1, d, true, 4, 0x000301));
    sha(HW3_IMG, sizeof(HW3_IMG), d);
    TEST_ASSERT_EQUAL_STRING("the SHA-256 differs from the manifest",
                             Manifest::sensorFetchRefusal(s, sizeof(HW4_IMG), d, true, 4, 0x000301));
}

void test_budget_and_sizes()
{
    TEST_ASSERT_EQUAL_UINT32(22000, Manifest::budgetMs(Manifest::MAX_FILE));
    TEST_ASSERT_TRUE(Manifest::budgetMs(Manifest::MIN_FILE) < Manifest::budgetMs(Manifest::MAX_FILE));
    TEST_ASSERT_EQUAL(144, Manifest::HEADER_LEN);
    TEST_ASSERT_TRUE(sizeof(M_GOOD) <= Manifest::MAX_FILE);
    // A full manifest (2 boards + 8 sensors, 256-character URLs) still parses
    std::string u = "https://h.example/" + std::string(256 - 18, 'a');
    std::string big = "{\"seq\":1,\"gateway\":{\"pro\":" + pkg("2.0.1", u) + ",\"lite\":" + pkg("2.0.1", u) +
                      "},\"sensor\":{";
    for (int i = 1; i <= 8; i++)
    {
        char k[8];
        snprintf(k, sizeof(k), "%04X", i);
        big += std::string(i > 1 ? "," : "") + "\"" + k + "\":" + pkg("0.2.1", u, "1000");
    }
    big += "}}";
    TEST_ASSERT_TRUE(big.size() + Manifest::HEADER_LEN <= Manifest::MAX_FILE);
    TEST_ASSERT_EQUAL(Result::Ok, parseText(big));
    TEST_ASSERT_EQUAL(8, doc.sensorCount);
}

int main(int, char **)
{
    initKeys();
    UNITY_BEGIN();
    RUN_TEST(test_good_manifest);
    RUN_TEST(test_replay);
    RUN_TEST(test_signature_line);
    RUN_TEST(test_crlf_extra_fields_missing_board);
    RUN_TEST(test_signed_but_invalid);
    RUN_TEST(test_replay_reports_no_skips);
    RUN_TEST(test_skip_count_does_not_wrap);
    RUN_TEST(test_field_matrix);
    RUN_TEST(test_skip_is_never_an_offer);
    RUN_TEST(test_rule_corpus);
    RUN_TEST(test_offers);
    RUN_TEST(test_install_checks);
    RUN_TEST(test_budget_and_sizes);
    return UNITY_END();
}
