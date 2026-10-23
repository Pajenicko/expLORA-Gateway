/**
 * Native unit tests for lib/explora-protocol (protocol generation 2).
 *
 * Known-answer vectors: FIPS-197 (AES), NIST SP 800-38B (CMAC),
 * NIST SP 800-38A F.5.1 (CTR); frame vectors in vectors.h come from an
 * independent Python implementation (lib/explora-protocol/tools/gen_vectors.py).
 * Run:   pio test -e native
 */

#include <unity.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ExploraProtocol.h"
#include "vectors.h"

using namespace ep;

void setUp(void) {}
void tearDown(void) {}

static void fromHex(const char *hex, uint8_t *out)
{
    for (size_t i = 0; hex[2 * i]; i++)
    {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static std::vector<uint8_t> hexv(const char *hex)
{
    std::vector<uint8_t> v(strlen(hex) / 2);
    fromHex(hex, v.data());
    return v;
}

static const char *kNistKey = "2b7e151628aed2a6abf7158809cf4f3c";
static const char *kNistMsg = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                              "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";

// ---------------------------------------------------------------------------
// AES / CMAC / CTR known answers

static void test_aes_fips197_c1(void)
{
    auto key = hexv("000102030405060708090a0b0c0d0e0f");
    auto pt = hexv("00112233445566778899aabbccddeeff");
    auto ct = hexv("69c4e0d86a7b0430d8cdb78070b4c55a");
    uint8_t out[16];
    aesSoftEncrypt(key.data(), pt.data(), out);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ct.data(), out, 16);
}

static void test_aes_sp800_38a_ecb(void)
{
    auto key = hexv(kNistKey);
    auto msg = hexv(kNistMsg);
    auto ct = hexv("3ad77bb40d7a3660a89ecaf32466ef97f5d3d58503b9699de785895a96fdbaaf"
                   "43b1cd7f598ece23881b00e3ed0306887b0c785e27e8ad3f8223207104725dd4");
    for (int b = 0; b < 4; b++)
    {
        uint8_t out[16];
        aesSoftEncrypt(key.data(), msg.data() + 16 * b, out);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(ct.data() + 16 * b, out, 16);
    }
}

static void check_cmac(size_t len, const char *expected)
{
    auto key = hexv(kNistKey);
    auto msg = hexv(kNistMsg);
    auto exp = hexv(expected);
    uint8_t mac[16];
    cmac(aesSoftEncrypt, key.data(), msg.data(), len, mac);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp.data(), mac, 16);
    // Same result fed in odd pieces
    Cmac c(aesSoftEncrypt, key.data());
    size_t pos = 0, step = 1;
    while (pos < len)
    {
        size_t n = (len - pos < step) ? len - pos : step;
        c.update(msg.data() + pos, n);
        pos += n;
        step = step * 2 + 1;
    }
    c.finish(mac);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp.data(), mac, 16);
}

static void test_cmac_sp800_38b_example1_empty(void)
{
    check_cmac(0, "bb1d6929e95937287fa37d129b756746");
}
static void test_cmac_sp800_38b_example2_16(void)
{
    check_cmac(16, "070a16b46b4d4144f79bdd9dd04a287c");
}
static void test_cmac_sp800_38b_example3_40(void)
{
    check_cmac(40, "dfa66747de9ae63030ca32611497c827");
}
static void test_cmac_sp800_38b_example4_64(void)
{
    check_cmac(64, "51f0bebf7e3b9d92fc49741779363cfe");
}

static void test_ctr_sp800_38a_f51(void)
{
    auto key = hexv(kNistKey);
    auto ctr0 = hexv("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    auto data = hexv(kNistMsg);
    auto ct = hexv("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
                   "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
    ctrXor(aesSoftEncrypt, key.data(), ctr0.data(), data.data(), data.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ct.data(), data.data(), ct.size());
    ctrXor(aesSoftEncrypt, key.data(), ctr0.data(), data.data(), data.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(hexv(kNistMsg).data(), data.data(), ct.size());
}

// ---------------------------------------------------------------------------
// Identity and keys

static void test_parse_label_key(void)
{
    uint8_t a[8], b[8];
    TEST_ASSERT_TRUE(parseLabelKey("A1B2-C3D4-E5F6-0718", a));
    TEST_ASSERT_TRUE(parseLabelKey("a1b2c3d4e5f60718", b));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, b, 8);
    TEST_ASSERT_EQUAL_HEX8(0xA1, a[0]);
    TEST_ASSERT_EQUAL_HEX8(0x18, a[7]);
    TEST_ASSERT_FALSE(parseLabelKey("A1B2-C3D4-E5F6-071", a));    // 15 digits
    TEST_ASSERT_FALSE(parseLabelKey("A1B2-C3D4-E5F6-07189", a));  // 17 digits
    TEST_ASSERT_FALSE(parseLabelKey("A1B2-C3D4-E5F6-07G8", a));   // not hex
    TEST_ASSERT_FALSE(parseLabelKey(nullptr, a));
}

static void test_parse_devid(void)
{
    uint32_t id = 0;
    TEST_ASSERT_TRUE(parseDevId("3EA4191D", id));
    TEST_ASSERT_EQUAL_HEX32(0x3EA4191D, id);
    TEST_ASSERT_TRUE(parseDevId("3ea4191d", id));
    TEST_ASSERT_FALSE(parseDevId("3EA4-191D", id));
    TEST_ASSERT_FALSE(parseDevId("3EA4191", id));
}

static void test_key_derivation_vectors(void)
{
    uint8_t label[8], k[16];
    TEST_ASSERT_TRUE(parseLabelKey(kVecLabelText, label));
    deriveRootKey(aesSoftEncrypt, label, kVecDevId, k);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecRootKey, k, 16);
    KeyPair j, s;
    deriveJoinKeys(aesSoftEncrypt, k, kVecDevId, j);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecJoinEnc, j.enc, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecJoinMic, j.mic, 16);
    deriveSessionKeys(aesSoftEncrypt, k, kVecDevId, kVecDevNonce, kVecGwNonce, s);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecSessEnc, s.enc, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecSessMic, s.mic, 16);
}

static KeyPair joinKeys()
{
    KeyPair j;
    memcpy(j.enc, kVecJoinEnc, 16);
    memcpy(j.mic, kVecJoinMic, 16);
    return j;
}

static KeyPair sessKeys()
{
    KeyPair s;
    memcpy(s.enc, kVecSessEnc, 16);
    memcpy(s.mic, kVecSessMic, 16);
    return s;
}

// ---------------------------------------------------------------------------
// Frames against the reference vectors + round trips

static void test_join_request_vector_and_roundtrip(void)
{
    JoinRequest req = {kVecDevId, kVecDevNonce, 0x0003, 0, 1, 0, kCapRx2, 0};
    uint8_t f[kJoinReqLen];
    TEST_ASSERT_EQUAL(kJoinReqLen, buildJoinRequest(aesSoftEncrypt, joinKeys(), req, f, sizeof(f)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecJoinReq, f, sizeof(kVecJoinReq));

    FrameInfo info;
    TEST_ASSERT_TRUE(peekFrame(f, sizeof(f), info));
    TEST_ASSERT_EQUAL((int)FrameType::JoinReq, (int)info.type);
    TEST_ASSERT_EQUAL_HEX32(kVecDevId, info.devId);
    JoinRequest back;
    TEST_ASSERT_TRUE(parseJoinRequest(f, sizeof(f), back));
    TEST_ASSERT_EQUAL_UINT32(kVecDevNonce, back.devNonce);
    TEST_ASSERT_EQUAL_UINT16(3, back.hwType);
    TEST_ASSERT_EQUAL_UINT8(1, back.fwMinor);
    TEST_ASSERT_TRUE(verifyJoinRequest(aesSoftEncrypt, joinKeys(), f, sizeof(f)));
    f[8] ^= 1; // devNonce changed
    TEST_ASSERT_FALSE(verifyJoinRequest(aesSoftEncrypt, joinKeys(), f, sizeof(f)));
    TEST_ASSERT_FALSE(parseJoinRequest(f, sizeof(f) - 1, back)); // fixed length
}

static void test_join_accept_vector_and_roundtrip(void)
{
    uint8_t f[64];
    size_t n = buildJoinAccept(aesSoftEncrypt, joinKeys(), kVecDevId, kVecDevNonce, kVecGwNonce, 1, kVecJoinAccTlv,
                               sizeof(kVecJoinAccTlv), f, sizeof(f));
    TEST_ASSERT_EQUAL(sizeof(kVecJoinAcc), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecJoinAcc, f, n);

    uint32_t gw = 0;
    uint8_t ver = 0, tlv[64];
    size_t tlvLen = 0;
    TEST_ASSERT_TRUE(openJoinAccept(aesSoftEncrypt, joinKeys(), f, n, kVecDevNonce, gw, ver, tlv, tlvLen));
    TEST_ASSERT_EQUAL_HEX32(kVecGwNonce, gw);
    TEST_ASSERT_EQUAL_UINT8(1, ver);
    TEST_ASSERT_EQUAL(sizeof(kVecJoinAccTlv), tlvLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecJoinAccTlv, tlv, tlvLen);
    // Bound to the devNonce of the request
    TEST_ASSERT_FALSE(openJoinAccept(aesSoftEncrypt, joinKeys(), f, n, kVecDevNonce + 1, gw, ver, tlv, tlvLen));
    // Session keys from the accept equal the reference
    uint8_t label[8], k[16];
    parseLabelKey(kVecLabelText, label);
    deriveRootKey(aesSoftEncrypt, label, kVecDevId, k);
    KeyPair s;
    deriveSessionKeys(aesSoftEncrypt, k, kVecDevId, kVecDevNonce, gw, s);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecSessMic, s.mic, 16);
}

static void test_up_vector_and_roundtrip(void)
{
    uint8_t f[64];
    size_t n = buildData(aesSoftEncrypt, sessKeys(), FrameType::Up, kVecDevId, kVecUpFcnt, 0, kFctrlAck | 1,
                         kVecUpTlv, sizeof(kVecUpTlv), f, sizeof(f));
    TEST_ASSERT_EQUAL(sizeof(kVecUp), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecUp, f, n);

    FrameInfo info;
    TEST_ASSERT_TRUE(peekFrame(f, n, info));
    TEST_ASSERT_EQUAL((int)FrameType::Up, (int)info.type);
    TEST_ASSERT_EQUAL_UINT16(kVecUpFcnt, info.fcnt16);
    TEST_ASSERT_EQUAL_HEX8(kFctrlAck | 1, info.fctrl);
    uint8_t p[64];
    size_t pl = 0;
    TEST_ASSERT_TRUE(openData(aesSoftEncrypt, sessKeys(), FrameType::Up, f, n, kVecUpFcnt, 0, p, pl));
    TEST_ASSERT_EQUAL(sizeof(kVecUpTlv), pl);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecUpTlv, p, pl);
    // Wrong high counter bits, flipped bit anywhere, wrong key: MIC fails
    TEST_ASSERT_FALSE(openData(aesSoftEncrypt, sessKeys(), FrameType::Up, f, n, kVecUpFcnt + 0x10000, 0, p, pl));
    for (size_t i = 0; i < n; i++)
    {
        f[i] ^= 0x01;
        TEST_ASSERT_FALSE(openData(aesSoftEncrypt, sessKeys(), FrameType::Up, f, n, kVecUpFcnt, 0, p, pl));
        f[i] ^= 0x01;
    }
    TEST_ASSERT_FALSE(openData(aesSoftEncrypt, joinKeys(), FrameType::Up, f, n, kVecUpFcnt, 0, p, pl));
}

static void test_down_vector_bound_to_acked_uplink(void)
{
    uint8_t f[64];
    size_t n = buildData(aesSoftEncrypt, sessKeys(), FrameType::Down, kVecDevId, kVecDownFcnt, kVecUpFcnt,
                         kFctrlAck | 1, kVecDownTlv, sizeof(kVecDownTlv), f, sizeof(f));
    TEST_ASSERT_EQUAL(sizeof(kVecDown), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecDown, f, n);
    uint8_t p[64];
    size_t pl = 0;
    TEST_ASSERT_TRUE(openData(aesSoftEncrypt, sessKeys(), FrameType::Down, f, n, kVecDownFcnt, kVecUpFcnt, p, pl));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecDownTlv, p, pl);
    // Same DOWN presented as the answer to another uplink: rejected
    TEST_ASSERT_FALSE(openData(aesSoftEncrypt, sessKeys(), FrameType::Down, f, n, kVecDownFcnt, kVecUpFcnt + 1, p, pl));
}

static void test_up_and_down_keystreams_differ(void)
{
    // Same counter, same plaintext, different direction -> different ciphertext
    uint8_t pt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t u[32], d[32];
    buildData(aesSoftEncrypt, sessKeys(), FrameType::Up, kVecDevId, 9, 0, 0, pt, sizeof(pt), u, sizeof(u));
    buildData(aesSoftEncrypt, sessKeys(), FrameType::Down, kVecDevId, 9, 0, 0, pt, sizeof(pt), d, sizeof(d));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(u + kDataHeaderLen, d + kDataHeaderLen, sizeof(pt)));
}

static void test_empty_up_is_poll(void)
{
    uint8_t f[16], p[4];
    size_t pl = 99;
    size_t n = buildData(aesSoftEncrypt, sessKeys(), FrameType::Up, kVecDevId, 1, 0, kFctrlAck, nullptr, 0, f,
                         sizeof(f));
    TEST_ASSERT_EQUAL(kDataOverhead, n);
    TEST_ASSERT_TRUE(openData(aesSoftEncrypt, sessKeys(), FrameType::Up, f, n, 1, 0, p, pl));
    TEST_ASSERT_EQUAL(0, pl);
}

static void test_rejoin_vector_bound_to_uplink(void)
{
    const uint8_t *upMic = kVecUp + sizeof(kVecUp) - kMicLen;
    uint8_t f[kRejoinLen];
    TEST_ASSERT_EQUAL(kRejoinLen,
                      buildRejoin(aesSoftEncrypt, joinKeys(), kVecDevId, (uint16_t)kVecUpFcnt, upMic, f, sizeof(f)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecRejoin, f, sizeof(kVecRejoin));
    FrameInfo info;
    TEST_ASSERT_TRUE(peekFrame(f, sizeof(f), info));
    TEST_ASSERT_EQUAL((int)JoinAccKind::Rejoin, (int)info.joinKind);
    TEST_ASSERT_TRUE(verifyRejoin(aesSoftEncrypt, joinKeys(), f, sizeof(f), (uint16_t)kVecUpFcnt, upMic));
    uint8_t otherMic[4] = {upMic[0], upMic[1], upMic[2], (uint8_t)(upMic[3] ^ 1)};
    TEST_ASSERT_FALSE(verifyRejoin(aesSoftEncrypt, joinKeys(), f, sizeof(f), (uint16_t)kVecUpFcnt, otherMic));
    TEST_ASSERT_FALSE(verifyRejoin(aesSoftEncrypt, joinKeys(), f, sizeof(f), (uint16_t)(kVecUpFcnt + 1), upMic));
}

static void test_peek_rejects_gen1_and_other_versions(void)
{
    // A gen1 SHT40 packet (13 bytes, XOR obfuscated) - byte 0 is random; with
    // a non-matching marker it is not gen2
    uint8_t gen1[13] = {0x13, 0x05, 0x53, 0x55, 0x78, 0x0c, 0x12, 0x02, 0x08, 0x5c, 0x17, 0x44, 0x00};
    FrameInfo info;
    TEST_ASSERT_FALSE(peekFrame(gen1, sizeof(gen1), info));
    uint8_t f[sizeof(kVecUp)];
    memcpy(f, kVecUp, sizeof(f));
    f[0] = (uint8_t)((f[0] & ~0x07) | 2); // protocol version 2
    TEST_ASSERT_FALSE(peekFrame(f, sizeof(f), info));
    TEST_ASSERT_FALSE(peekFrame(kVecUp, 11, info)); // shorter than header + MIC
    TEST_ASSERT_FALSE(peekFrame(kVecJoinReq, sizeof(kVecJoinReq) - 1, info));
}

static void test_build_limits(void)
{
    uint8_t big[kMaxPayload + 1] = {0};
    uint8_t f[kMaxFrame];
    TEST_ASSERT_EQUAL(0, buildData(aesSoftEncrypt, sessKeys(), FrameType::Up, 1, 0, 0, 0, big, sizeof(big), f,
                                   sizeof(f)));
    TEST_ASSERT_EQUAL(kMaxPayload + kDataOverhead, buildData(aesSoftEncrypt, sessKeys(), FrameType::Up, 1, 0, 0, 0,
                                                             big, kMaxPayload, f, sizeof(f)));
    TEST_ASSERT_EQUAL(0, buildData(aesSoftEncrypt, sessKeys(), FrameType::Up, 1, 0, 0, 0, big, 10, f, 21)); // cap
    TEST_ASSERT_EQUAL(0, buildData(aesSoftEncrypt, sessKeys(), FrameType::JoinReq, 1, 0, 0, 0, big, 1, f,
                                   sizeof(f)));
}

static void test_open_rejects_wrong_direction(void)
{
    // A replayed UP played into the sensor's RX1 window: same session keys,
    // must not verify as a DOWN
    uint8_t p[64];
    size_t pl = 0;
    TEST_ASSERT_FALSE(
        openData(aesSoftEncrypt, sessKeys(), FrameType::Down, kVecUp, sizeof(kVecUp), kVecUpFcnt, 0, p, pl));
    TEST_ASSERT_FALSE(openData(aesSoftEncrypt, sessKeys(), FrameType::Up, kVecDown, sizeof(kVecDown), kVecDownFcnt,
                               kVecUpFcnt, p, pl));
    // Join frames are never data frames
    TEST_ASSERT_FALSE(
        openData(aesSoftEncrypt, sessKeys(), FrameType::JoinAcc, kVecUp, sizeof(kVecUp), kVecUpFcnt, 0, p, pl));
}

static void test_open_rejects_fcnt16_mismatch(void)
{
    uint8_t p[64];
    size_t pl = 0;
    TEST_ASSERT_FALSE(
        openData(aesSoftEncrypt, sessKeys(), FrameType::Up, kVecUp, sizeof(kVecUp), kVecUpFcnt + 1, 0, p, pl));
}

static void test_peek_length_limits(void)
{
    FrameInfo info;
    uint8_t f[300] = {0};
    // UP with 223 payload bytes (one over kMaxPayload)
    f[0] = makeMhdr(FrameType::Up);
    TEST_ASSERT_TRUE(peekFrame(f, kDataOverhead + kMaxPayload, info));
    TEST_ASSERT_FALSE(peekFrame(f, kDataOverhead + kMaxPayload + 1, info));
    TEST_ASSERT_FALSE(peekFrame(f, kMaxFrame + 1, info));
    // ACCEPT shorter than the fixed part
    f[0] = makeMhdr(FrameType::JoinAcc);
    f[5] = (uint8_t)JoinAccKind::Accept;
    TEST_ASSERT_TRUE(peekFrame(f, kJoinAcceptMinLen, info));
    TEST_ASSERT_FALSE(peekFrame(f, kJoinAcceptMinLen - 1, info));
    // REJOIN only exactly 10 bytes
    f[5] = (uint8_t)JoinAccKind::Rejoin;
    TEST_ASSERT_TRUE(peekFrame(f, kRejoinLen, info));
    TEST_ASSERT_FALSE(peekFrame(f, kRejoinLen + 1, info));
    TEST_ASSERT_FALSE(peekFrame(f, kRejoinLen - 1, info));
    f[5] = 7; // unknown kind
    TEST_ASSERT_FALSE(peekFrame(f, kRejoinLen, info));
}

static void test_join_accept_size_limits(void)
{
    static uint8_t tlv[256] = {0};
    static uint8_t f[300];
    const size_t maxTlv = kMaxFrame - kJoinAcceptMinLen; // 240
    TEST_ASSERT_EQUAL(240, maxTlv);
    TEST_ASSERT_EQUAL(kMaxFrame, buildJoinAccept(aesSoftEncrypt, joinKeys(), 1, 1, 1, 1, tlv, maxTlv, f, sizeof(f)));
    TEST_ASSERT_EQUAL(0, buildJoinAccept(aesSoftEncrypt, joinKeys(), 1, 1, 1, 1, tlv, maxTlv + 1, f, sizeof(f)));
    // A length that would wrap kJoinAcceptMinLen + tlvLen
    TEST_ASSERT_EQUAL(0, buildJoinAccept(aesSoftEncrypt, joinKeys(), 1, 1, 1, 1, tlv, SIZE_MAX - 5, f, sizeof(f)));
    // Output buffer too small
    TEST_ASSERT_EQUAL(0, buildJoinAccept(aesSoftEncrypt, joinKeys(), 1, 1, 1, 1, tlv, 10, f, kJoinAcceptMinLen + 9));
}

// ---------------------------------------------------------------------------
// Counter window and devNonce

static void test_fcnt_first_frame_of_session(void)
{
    uint32_t v;
    TEST_ASSERT_TRUE(expandFcnt(false, 0, 0, v));
    TEST_ASSERT_EQUAL_UINT32(0, v);
    TEST_ASSERT_TRUE(expandFcnt(false, 0, kMaxFcntGap - 1, v));
    TEST_ASSERT_FALSE(expandFcnt(false, 0, kMaxFcntGap, v));
}

static void test_fcnt_window_edges(void)
{
    uint32_t v;
    TEST_ASSERT_FALSE(expandFcnt(true, 100, 100, v)); // replay
    TEST_ASSERT_FALSE(expandFcnt(true, 100, 99, v));  // older -> would be +65535
    TEST_ASSERT_TRUE(expandFcnt(true, 100, 101, v));
    TEST_ASSERT_EQUAL_UINT32(101, v);
    TEST_ASSERT_TRUE(expandFcnt(true, 100, 100 + kMaxFcntGap, v));
    TEST_ASSERT_FALSE(expandFcnt(true, 100, 100 + kMaxFcntGap + 1, v));
    // 16-bit roll-over
    TEST_ASSERT_TRUE(expandFcnt(true, 0x0001FFFE, 0x0003, v));
    TEST_ASSERT_EQUAL_HEX32(0x00020003, v);
    TEST_ASSERT_TRUE(expandFcnt(true, 0x0001FFFF, 0x0000, v));
    TEST_ASSERT_EQUAL_HEX32(0x00020000, v);
    // 32-bit end: no wrap
    TEST_ASSERT_TRUE(expandFcnt(true, 0xFFFFFFFE, 0xFFFF, v));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, v);
    TEST_ASSERT_FALSE(expandFcnt(true, 0xFFFFFFFF, 0x0000, v));
}

static void test_devnonce_monotonic(void)
{
    TEST_ASSERT_TRUE(devNonceAcceptable(false, 0, 0));
    TEST_ASSERT_TRUE(devNonceAcceptable(true, 7, 8));
    TEST_ASSERT_FALSE(devNonceAcceptable(true, 7, 7));
    TEST_ASSERT_FALSE(devNonceAcceptable(true, 7, 3));
}

// ---------------------------------------------------------------------------
// TLV

static void test_tlv_write_read_with_channels(void)
{
    uint8_t buf[64];
    TlvWriter w(buf, sizeof(buf));
    TEST_ASSERT_TRUE(w.addI16(up::kTemperature, 2140));
    TEST_ASSERT_TRUE(w.addU16(up::kHumidity, 5956));
    TEST_ASSERT_TRUE(w.addU16(up::kBattery, 3090));
    TEST_ASSERT_TRUE(w.addI16(up::kTemperature, kInvalidI16, 2));
    const uint8_t board[3] = {0xde, 0xad, 0xbe};
    TEST_ASSERT_TRUE(w.add(up::kBoardData, 0, board, 3));
    TEST_ASSERT_TRUE(w.ok());
    TEST_ASSERT_EQUAL(sizeof(kVecUpTlv), w.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecUpTlv, buf, w.size());

    TlvReader r(buf, w.size());
    Tlv t;
    TEST_ASSERT_EQUAL(1, r.next(t));
    TEST_ASSERT_EQUAL_HEX8(up::kTemperature, t.type);
    TEST_ASSERT_EQUAL_INT16(2140, (int16_t)tlvU16(t));
    TEST_ASSERT_EQUAL(1, r.next(t));
    TEST_ASSERT_EQUAL(1, r.next(t));
    TEST_ASSERT_EQUAL_UINT16(3090, tlvU16(t));
    TEST_ASSERT_EQUAL(1, r.next(t));
    TEST_ASSERT_EQUAL_HEX8(up::kTemperature, t.type);
    TEST_ASSERT_EQUAL_UINT8(2, t.channel);
    TEST_ASSERT_EQUAL_INT16(kInvalidI16, (int16_t)tlvU16(t));
    TEST_ASSERT_EQUAL(1, r.next(t));
    TEST_ASSERT_EQUAL_UINT8(3, t.len);
    TEST_ASSERT_EQUAL_HEX8(0xbe, t.value[2]);
    TEST_ASSERT_EQUAL(0, r.next(t));
}

static void test_tlv_unknown_types_are_skipped(void)
{
    // Unknown ids in every length class, with and without channel, between known ones
    const uint8_t buf[] = {
        tlvType(LenClass::One, 0x1F), 0xAA,                                // unknown 1 byte
        (uint8_t)(tlvType(LenClass::Two, 0x1E) | kTlvChannelBit), 3, 1, 2, // unknown 2 bytes, channel 3
        up::kBattery, 0x0c, 0x12,                                          // known
        tlvType(LenClass::Four, 0x1D), 1, 2, 3, 4,                         // unknown 4 bytes
        tlvType(LenClass::Var, 0x1C), 5, 9, 9, 9, 9, 9,                    // unknown, 5 bytes
        up::kHumidity, 0x17, 0x44,                                         // known
    };
    TlvReader r(buf, sizeof(buf));
    Tlv t;
    int known = 0, total = 0;
    uint16_t batt = 0, rh = 0;
    while (r.next(t) == 1)
    {
        total++;
        if (t.type == up::kBattery)
        {
            batt = tlvU16(t);
            known++;
        }
        else if (t.type == up::kHumidity)
        {
            rh = tlvU16(t);
            known++;
        }
    }
    TEST_ASSERT_EQUAL(6, total);
    TEST_ASSERT_EQUAL(2, known);
    TEST_ASSERT_EQUAL_UINT16(3090, batt);
    TEST_ASSERT_EQUAL_UINT16(5956, rh);
}

static void test_tlv_truncated_and_writer_errors(void)
{
    const uint8_t cut[] = {up::kBattery, 0x0c};
    TlvReader r(cut, sizeof(cut));
    Tlv t;
    TEST_ASSERT_EQUAL(-1, r.next(t));
    const uint8_t cutVar[] = {up::kBoardData, 4, 1, 2};
    TlvReader r2(cutVar, sizeof(cutVar));
    TEST_ASSERT_EQUAL(-1, r2.next(t));
    const uint8_t cutChan[] = {(uint8_t)(up::kBattery | kTlvChannelBit)};
    TlvReader r3(cutChan, sizeof(cutChan));
    TEST_ASSERT_EQUAL(-1, r3.next(t));

    uint8_t buf[4];
    TlvWriter w(buf, sizeof(buf));
    TEST_ASSERT_FALSE(w.addU8(up::kBattery, 1)); // length class mismatch
    TEST_ASSERT_FALSE(w.ok());
    TlvWriter w2(buf, sizeof(buf));
    TEST_ASSERT_TRUE(w2.addU16(up::kBattery, 1));
    TEST_ASSERT_FALSE(w2.addU16(up::kBattery, 2)); // no room
    TEST_ASSERT_EQUAL(3, w2.size());
}

static void test_tlv_downlink_config(void)
{
    uint8_t buf[32];
    TlvWriter w(buf, sizeof(buf));
    uint8_t iv[4];
    putU16(iv, 600);
    putU16(iv + 2, 123);
    w.add(down::kInterval, 0, iv, 4);
    w.addI8(down::kTxPower, 14);
    w.addU32(down::kTime, 1792265000);
    TEST_ASSERT_EQUAL(sizeof(kVecJoinAccTlv), w.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecJoinAccTlv, buf, w.size());
}


// --- OTA transfer (spec 14, ep_ota.h) -------------------------------------------

static void test_ota_down_payload_limit_and_binding(void)
{
    const uint32_t acceptFcnt = 41; // FCnt32 of the UP that accepted the OTA start
    uint8_t data[ota::kBlockSize];
    for (size_t i = 0; i < sizeof(data); i++)
    {
        data[i] = (uint8_t)i;
    }
    uint8_t value[2 + ota::kBlockSize];
    TEST_ASSERT_EQUAL(sizeof(value), ota::encodeBlock(388, data, sizeof(data), value, sizeof(value)));
    uint8_t tlv[256];
    TlvWriter w(tlv, sizeof(tlv));
    TEST_ASSERT_TRUE(w.add(down::kOtaBlock, 0, value, sizeof(value)));
    TEST_ASSERT_EQUAL(236, w.size()); // type, length, index, 232 bytes
    uint8_t f[kMaxFrame];
    size_t n = buildData(aesSoftEncrypt, sessKeys(), FrameType::Down, kVecDevId, 0x10005, acceptFcnt,
                         kFctrlOtaSession, tlv, w.size(), f, sizeof(f));
    TEST_ASSERT_EQUAL(248, n);
    FrameInfo info;
    TEST_ASSERT_TRUE(peekFrame(f, n, info));
    uint8_t p[kMaxPayloadOta];
    size_t pl = 0;
    TEST_ASSERT_TRUE(openData(aesSoftEncrypt, sessKeys(), FrameType::Down, f, n, 0x10005, acceptFcnt, p, pl));
    TEST_ASSERT_EQUAL(236, pl);
    TlvReader r(p, pl);
    Tlv t;
    TEST_ASSERT_EQUAL(1, r.next(t));
    TEST_ASSERT_EQUAL_HEX8(0xC2, t.type);
    uint16_t idx = 0;
    const uint8_t *d = nullptr;
    size_t dl = 0;
    TEST_ASSERT_TRUE(ota::decodeBlock(t.value, t.len, idx, d, dl));
    TEST_ASSERT_EQUAL(388, idx);
    TEST_ASSERT_EQUAL(ota::kBlockSize, dl);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, d, dl);
    // bound to the accepting UP: another binding fails
    TEST_ASSERT_FALSE(openData(aesSoftEncrypt, sessKeys(), FrameType::Down, f, n, 0x10005, acceptFcnt + 1, p, pl));
    // the OTA limit only with the OTA bit in a DOWN
    uint8_t big[kMaxPayloadOta] = {};
    TEST_ASSERT_EQUAL(kMaxPayloadOta + kDataOverhead,
                      buildData(aesSoftEncrypt, sessKeys(), FrameType::Down, kVecDevId, 7, acceptFcnt,
                                kFctrlOtaSession, big, kMaxPayloadOta, f, sizeof(f)));
    TEST_ASSERT_EQUAL(0, buildData(aesSoftEncrypt, sessKeys(), FrameType::Down, kVecDevId, 7, acceptFcnt,
                                   kFctrlOtaSession, big, kMaxPayloadOta + 1, f, sizeof(f)));
    TEST_ASSERT_EQUAL(0, buildData(aesSoftEncrypt, sessKeys(), FrameType::Down, kVecDevId, 7, 3, kFctrlAck, big,
                                   kMaxPayload + 1, f, sizeof(f)));
    TEST_ASSERT_EQUAL(0, buildData(aesSoftEncrypt, sessKeys(), FrameType::Up, kVecDevId, 7, 0, kFctrlClassC, big,
                                   kMaxPayload + 1, f, sizeof(f)));
    // a received normal DOWN / UP longer than 222 bytes of payload is rejected by peekFrame
    n = buildData(aesSoftEncrypt, sessKeys(), FrameType::Down, kVecDevId, 7, acceptFcnt, kFctrlOtaSession, big,
                  kMaxPayload + 1, f, sizeof(f));
    TEST_ASSERT_TRUE(peekFrame(f, n, info));
    f[7] &= (uint8_t)~kFctrlOtaSession;
    TEST_ASSERT_FALSE(peekFrame(f, n, info));
    f[0] = makeMhdr(FrameType::Up);
    f[7] = kFctrlClassC;
    TEST_ASSERT_FALSE(peekFrame(f, n, info));
    TEST_ASSERT_EQUAL(1, (kCapOtaMask >> kCapOtaShift) & ota::kTransferVersion);
}

static void test_ota_session_tlv_rules(void)
{
    uint8_t buf[32];
    TlvWriter w(buf, sizeof(buf));
    const uint8_t blk[2 + 8] = {0, 5, 1, 2, 3, 4, 5, 6, 7, 8};
    w.add(down::kOtaBlock, 0, blk, sizeof(blk));
    uint8_t type = 0;
    const uint8_t *v = nullptr;
    size_t vl = 0;
    TEST_ASSERT_TRUE(ota::sessionTlv(buf, w.size(), type, v, vl));
    TEST_ASSERT_EQUAL_HEX8(0xC2, type);
    TEST_ASSERT_EQUAL(10, vl);
    // two TLVs, another type, a channel, empty: dropped
    w.addU8(down::kReboot, 0xB0);
    TEST_ASSERT_FALSE(ota::sessionTlv(buf, w.size(), type, v, vl));
    TlvWriter w2(buf, sizeof(buf));
    w2.addU32(down::kTime, 1);
    TEST_ASSERT_FALSE(ota::sessionTlv(buf, w2.size(), type, v, vl));
    TlvWriter w3(buf, sizeof(buf));
    const uint8_t end = 1;
    w3.add(down::kOtaEnd, 1, &end, 1);
    TEST_ASSERT_FALSE(ota::sessionTlv(buf, w3.size(), type, v, vl));
    TlvWriter w4(buf, sizeof(buf));
    w4.add(down::kOtaEnd, 0, &end, 1);
    TEST_ASSERT_TRUE(ota::sessionTlv(buf, w4.size(), type, v, vl));
    TEST_ASSERT_EQUAL_HEX8(0xC3, type);
    TEST_ASSERT_FALSE(ota::sessionTlv(buf, 0, type, v, vl));
    TEST_ASSERT_FALSE(ota::sessionTlv(buf, 2, type, v, vl)); // truncated
}

static ota::Start goodStart()
{
    ota::Start s;
    s.imageLen = 90112;
    s.imageCrc = 0xA1B2C3D4;
    s.fwVersion = 0x000600;
    s.hwType = 3;
    s.k = 32;
    s.cadence100ms = 260;
    return s;
}

static void test_ota_start_codec_and_check(void)
{
    ota::Start s = goodStart();
    uint8_t v[ota::kStartLen + 2] = {};
    TEST_ASSERT_EQUAL(22, ota::encodeStart(s, v));
    const uint8_t expected[22] = {0x01, 0x00, 0x01, 0x60, 0x00, 0xA1, 0xB2, 0xC3, 0xD4, 0x00, 0x00, 0x06, 0x00,
                                  0x00, 0x03, 0xE8, 0x20, 0x01, 0x04, 0x09, 0x0F, 0xA0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, v, 22);
    ota::Start d;
    TEST_ASSERT_TRUE(ota::decodeStart(v, sizeof(v), d)); // longer values: extra bytes ignored
    TEST_ASSERT_EQUAL_UINT32(s.imageLen, d.imageLen);
    TEST_ASSERT_EQUAL_HEX32(s.imageCrc, d.imageCrc);
    TEST_ASSERT_EQUAL(260, d.cadence100ms);
    TEST_ASSERT_EQUAL(4000, d.startDelayMs);
    TEST_ASSERT_FALSE(ota::decodeStart(v, 21, d));
    TEST_ASSERT_EQUAL(0, ota::checkStart(s));
    ota::Start b = s;
    b.version = 2;
    TEST_ASSERT_EQUAL(ota::kRefuseVersion, ota::checkStart(b));
    b = s;
    b.imageLen = 90113; // not a multiple of 8
    TEST_ASSERT_EQUAL(ota::kRefuseImage, ota::checkStart(b));
    b.imageLen = ota::kSlotSize + 8;
    TEST_ASSERT_EQUAL(ota::kRefuseImage, ota::checkStart(b));
    b = s;
    b.blockSize = 224;
    TEST_ASSERT_EQUAL(ota::kRefuseImage, ota::checkStart(b));
    b = s;
    b.k = 31;
    TEST_ASSERT_EQUAL(ota::kRefuseImage, ota::checkStart(b));
    b.k = 65;
    TEST_ASSERT_EQUAL(ota::kRefuseImage, ota::checkStart(b));
    b = s;
    b.cadence100ms = 15; // 1.5 s: shorter than the block plus the windows
    TEST_ASSERT_EQUAL(ota::kRefuseImage, ota::checkStart(b));
    b = s;
    b.sf = 7; // version 1 receivers: SF9 only
    TEST_ASSERT_EQUAL(ota::kRefuseImage, ota::checkStart(b));
    b = s;
    b.startDelayMs = 2000;
    TEST_ASSERT_EQUAL(ota::kRefuseImage, ota::checkStart(b));
    b = s;
    b.cadence100ms = 210; // 33 x 21 s = 693 s: more than 5 statuses per hour (D7)
    TEST_ASSERT_EQUAL(ota::kRefuseImage, ota::checkStart(b));
    b.cadence100ms = 220; // 726 s
    TEST_ASSERT_EQUAL(0, ota::checkStart(b));
    b.k = 64;
    b.cadence100ms = 111; // 65 x 11.1 s = 721.5 s
    TEST_ASSERT_EQUAL(0, ota::checkStart(b));
    TEST_ASSERT_EQUAL(2 * 33 * 26000, ota::silenceMs(s)); // 2 rounds, ~29 min
    // the start fits a normal DOWN together with the time TLV
    uint8_t tlv[40];
    TlvWriter w(tlv, sizeof(tlv));
    TEST_ASSERT_TRUE(w.add(down::kOtaStart, 0, v, ota::kStartLen));
    TEST_ASSERT_TRUE(w.addU32(down::kTime, 1792265000));
    TEST_ASSERT_EQUAL(29, w.size());
}

static void test_ota_status_codec(void)
{
    ota::Status s;
    s.base = 300;
    s.window = 0x8000000000000005ULL;
    s.slot = 329;
    uint8_t v[16];
    TEST_ASSERT_EQUAL(12, ota::encodeStatus(s, v, sizeof(v)));
    uint8_t tlv[16];
    TlvWriter w(tlv, sizeof(tlv));
    TEST_ASSERT_TRUE(w.add(up::kOtaStatus, 0, v, 12));
    TEST_ASSERT_EQUAL(14, w.size()); // base, window, slot index (spec 14.2)
    ota::Status d;
    TEST_ASSERT_TRUE(ota::decodeStatus(v, 12, d));
    TEST_ASSERT_EQUAL((int)ota::StatusKind::Progress, (int)d.kind);
    TEST_ASSERT_EQUAL(300, d.base);
    TEST_ASSERT_EQUAL_HEX64(0x8000000000000005ULL, d.window);
    TEST_ASSERT_EQUAL(329, d.slot);
    // a progress value is never read as an event, other lengths are ignored
    TEST_ASSERT_FALSE(ota::decodeStatus(v, 10, d));
    TEST_ASSERT_FALSE(ota::decodeStatus(v, 3, d));
    ota::Status e;
    e.kind = ota::StatusKind::Refused;
    e.arg = ota::kRefuseUnconfirmed;
    TEST_ASSERT_EQUAL(2, ota::encodeStatus(e, v, sizeof(v)));
    TEST_ASSERT_TRUE(ota::decodeStatus(v, 2, d));
    TEST_ASSERT_EQUAL((int)ota::StatusKind::Refused, (int)d.kind);
    TEST_ASSERT_EQUAL(ota::kRefuseUnconfirmed, d.arg);
    const uint8_t unknown[2] = {9, 0}, zero[2] = {0, 0};
    TEST_ASSERT_FALSE(ota::decodeStatus(unknown, 2, d));
    TEST_ASSERT_FALSE(ota::decodeStatus(zero, 2, d)); // kind 0 is the 12-byte progress form only
    TEST_ASSERT_FALSE(ota::decodeStatus(v, 1, d));
    TEST_ASSERT_EQUAL(0, ota::encodeStatus(s, v, 11));
    const uint8_t battery[2] = {6, 23}; // verified, battery 2.3 V
    TEST_ASSERT_TRUE(ota::decodeStatus(battery, 2, d));
    TEST_ASSERT_EQUAL((int)ota::StatusKind::BatteryLow, (int)d.kind);
    TEST_ASSERT_EQUAL(23, d.arg);
}

static void test_ota_block_codec_limits(void)
{
    uint8_t data[ota::kBlockSize] = {}, v[2 + ota::kBlockSize + 8];
    TEST_ASSERT_EQUAL(0, ota::encodeBlock(1, data, ota::kBlockSize + 8, v, sizeof(v)));
    TEST_ASSERT_EQUAL(0, ota::encodeBlock(1, data, ota::kBlockSize, v, ota::kBlockSize + 1));
    TEST_ASSERT_EQUAL(18, ota::encodeBlock(5, data, 16, v, sizeof(v))); // a short last block
    uint16_t idx;
    const uint8_t *d;
    size_t dl;
    TEST_ASSERT_TRUE(ota::decodeBlock(v, 18, idx, d, dl));
    TEST_ASSERT_EQUAL(16, dl);
    TEST_ASSERT_FALSE(ota::decodeBlock(v, 17, idx, d, dl));                   // not a multiple of 8
    TEST_ASSERT_FALSE(ota::decodeBlock(v, 2, idx, d, dl));                    // empty
    TEST_ASSERT_FALSE(ota::decodeBlock(v, 2 + ota::kBlockSize + 8, idx, d, dl)); // too long
}

static void test_ota_block_math(void)
{
    TEST_ASSERT_EQUAL(389, ota::blockCount(90112, 232));  // 88 kB
    TEST_ASSERT_EQUAL(442, ota::blockCount(102400, 232)); // 100 kB
    TEST_ASSERT_EQUAL(495, ota::blockCount(ota::kSlotSize, 232));
    TEST_ASSERT_EQUAL(232, ota::blockLength(0, 90112, 232));
    TEST_ASSERT_EQUAL(90112 - 388 * 232, ota::blockLength(388, 90112, 232)); // 96
    TEST_ASSERT_EQUAL(0, ota::blockLength(389, 90112, 232));
    TEST_ASSERT_EQUAL(0, (90112 - 388 * 232) % 8);
    TEST_ASSERT_EQUAL(44, ota::pageCount(90112, 2048));
    TEST_ASSERT_EQUAL(45, ota::pageCount(90120, 2048));
    uint16_t first, last;
    TEST_ASSERT_TRUE(ota::blocksInPage(0, 90112, 232, 2048, first, last));
    TEST_ASSERT_EQUAL(0, first);
    TEST_ASSERT_EQUAL(8, last); // 2047 / 232 = 8
    TEST_ASSERT_TRUE(ota::blocksInPage(1, 90112, 232, 2048, first, last));
    TEST_ASSERT_EQUAL(8, first); // block 8 spans pages 0 and 1
    TEST_ASSERT_EQUAL(17, last);
    TEST_ASSERT_TRUE(ota::blocksInPage(43, 90112, 232, 2048, first, last));
    TEST_ASSERT_EQUAL(379, first); // 43 * 2048 / 232
    TEST_ASSERT_EQUAL(388, last);
    TEST_ASSERT_FALSE(ota::blocksInPage(44, 90112, 232, 2048, first, last));
}

static void test_ota_bitmap_window(void)
{
    ota::Bitmap b(389);
    TEST_ASSERT_EQUAL(0, b.firstMissing());
    for (uint16_t i = 0; i < 100; i++)
    {
        b.set(i);
    }
    b.set(101);
    b.set(163); // base + 63
    b.set(164); // outside the window of base 100
    b.set(400); // past the end: ignored
    TEST_ASSERT_EQUAL(103, b.count());
    TEST_ASSERT_EQUAL(100, b.firstMissing());
    const uint64_t w = b.window(100);
    TEST_ASSERT_EQUAL_HEX64(0x8000000000000002ULL, w);
    // the gateway applies the status to its view
    ota::Bitmap g(389);
    g.applyStatus(100, w);
    TEST_ASSERT_EQUAL(102, g.count());
    TEST_ASSERT_TRUE(g.test(99) && g.test(101) && g.test(163) && !g.test(164) && !g.test(100));
    // the sensor's bitmap shrinks (resume check erased pages): the status
    // replaces the gateway's view, so blocks 88..99 are sent again
    for (uint16_t i = 0; i < 200; i++)
    {
        g.set(i);
    }
    g.applyStatus(88, 0x0ULL);
    TEST_ASSERT_EQUAL(88, g.count());
    TEST_ASSERT_EQUAL(88, g.firstMissing());
    TEST_ASSERT_FALSE(g.test(95) || g.test(160) || g.test(199)); // inside and above the window cleared
    g.applyStatus(0, 0);
    TEST_ASSERT_EQUAL(0, g.count()); // a new transfer: nothing confirmed
    g.set(7);
    TEST_ASSERT_FALSE(g.applyStatus(400, 0)); // base past the end: ignored, view unchanged
    TEST_ASSERT_EQUAL(1, g.count());
    TEST_ASSERT_TRUE(g.applyStatus(389, 0));  // base = block count: complete
    TEST_ASSERT_TRUE(g.complete());
    // window near the end
    ota::Bitmap e(389);
    for (uint16_t i = 0; i < 389; i++)
    {
        if (i != 380)
        {
            e.set(i);
        }
    }
    TEST_ASSERT_EQUAL(380, e.firstMissing());
    TEST_ASSERT_EQUAL_HEX64(0x1FEULL, e.window(380)); // 381..388, nothing past the end
    e.set(380);
    TEST_ASSERT_TRUE(e.complete());
    TEST_ASSERT_EQUAL(389, e.firstMissing());
    // load from a checkpoint clears bits past n
    uint8_t raw[ota::Bitmap::kBytes];
    memset(raw, 0xFF, sizeof(raw));
    ota::Bitmap l;
    l.load(10, raw);
    TEST_ASSERT_EQUAL(10, l.count());
    TEST_ASSERT_EQUAL(0, l.bytes()[2]);
    b.clear(5);
    TEST_ASSERT_EQUAL(5, b.firstMissing());
}

static void test_ota_schedule_and_airtime(void)
{
    // the sensor's formula (spec 7): JOIN_REQ 239 ms, largest normal frame 1.20 s
    TEST_ASSERT_EQUAL(238592, ota::airtimeUs(22, 9));
    TEST_ASSERT_EQUAL(177152, ota::airtimeUs(12, 9)); // POLL / REJOIN 177 ms
    TEST_ASSERT_EQUAL(1201152, ota::airtimeUs(234, 9));
    TEST_ASSERT_EQUAL(248, ota::kBlockFrameLen);
    const uint32_t block = ota::airtimeUs(ota::kBlockFrameLen, 9);
    TEST_ASSERT_EQUAL(1262592, block); // 1.26 s
    TEST_ASSERT_EQUAL(238592, ota::airtimeUs(24, 9));
    TEST_ASSERT_EQUAL(0, ota::airtimeUs(20, 6));
    TEST_ASSERT_EQUAL(9117696, ota::airtimeUs(248, 12)); // low data rate optimisation at SF12
    // cadence within 60 % of the 80 % data share: 26 s at SF9, K = 32
    TEST_ASSERT_EQUAL(260, ota::cadenceFor(block, 32));
    const double perHour = 3600.0 / 26 * 32 / 33 * block / 1e6;
    TEST_ASSERT_TRUE(perHour <= 0.1 * 0.48 * 3600);
    TEST_ASSERT_EQUAL(220, ota::cadenceFor(ota::airtimeUs(248, 7), 32)); // SF7: 9 s by airtime, 22 s by the 720 s round
    TEST_ASSERT_EQUAL(120, ota::cadenceFor(ota::airtimeUs(248, 7), 64)); // 65 x 12 s = 780 s
    // rounds: 32 block slots, then the status slot
    TEST_ASSERT_FALSE(ota::isStatusSlot(0, 32));
    TEST_ASSERT_FALSE(ota::isStatusSlot(31, 32));
    TEST_ASSERT_TRUE(ota::isStatusSlot(32, 32));
    TEST_ASSERT_FALSE(ota::isStatusSlot(33, 32));
    TEST_ASSERT_TRUE(ota::isStatusSlot(65, 32));
    ota::Start s = goodStart();
    TEST_ASSERT_EQUAL(4000, ota::slotOffsetMs(0, s));
    TEST_ASSERT_EQUAL(4000 + 33 * 26000, ota::slotOffsetMs(33, s));
    TEST_ASSERT_EQUAL(6, ota::driftMs(26000));   // 200 ppm of 26 s, rounded up
    TEST_ASSERT_EQUAL(60, ota::driftMs(300000));
    // re-anchoring is limited to +-(20 ms + drift)
    TEST_ASSERT_EQUAL(7, ota::anchorStep(7, 26000));
    TEST_ASSERT_EQUAL(26, ota::anchorStep(90, 26000));
    TEST_ASSERT_EQUAL(-26, ota::anchorStep(-500, 26000));
    TEST_ASSERT_EQUAL(-80, ota::anchorStep(-500, 300000));
    TEST_ASSERT_EQUAL(26, ota::anchorStep(26, 26000)); // exactly at the limit
    TEST_ASSERT_EQUAL(279552, ota::airtimeUs(35, 9)); // SHT40 report (9 B TLVs) + 14-byte status TLV
    TEST_ASSERT_EQUAL(259072, ota::airtimeUs(30, 9)); // accepting UP: accepted 4 B + progress 14 B
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_aes_fips197_c1);
    RUN_TEST(test_aes_sp800_38a_ecb);
    RUN_TEST(test_cmac_sp800_38b_example1_empty);
    RUN_TEST(test_cmac_sp800_38b_example2_16);
    RUN_TEST(test_cmac_sp800_38b_example3_40);
    RUN_TEST(test_cmac_sp800_38b_example4_64);
    RUN_TEST(test_ctr_sp800_38a_f51);
    RUN_TEST(test_parse_label_key);
    RUN_TEST(test_parse_devid);
    RUN_TEST(test_key_derivation_vectors);
    RUN_TEST(test_join_request_vector_and_roundtrip);
    RUN_TEST(test_join_accept_vector_and_roundtrip);
    RUN_TEST(test_up_vector_and_roundtrip);
    RUN_TEST(test_down_vector_bound_to_acked_uplink);
    RUN_TEST(test_up_and_down_keystreams_differ);
    RUN_TEST(test_empty_up_is_poll);
    RUN_TEST(test_rejoin_vector_bound_to_uplink);
    RUN_TEST(test_peek_rejects_gen1_and_other_versions);
    RUN_TEST(test_build_limits);
    RUN_TEST(test_open_rejects_wrong_direction);
    RUN_TEST(test_open_rejects_fcnt16_mismatch);
    RUN_TEST(test_peek_length_limits);
    RUN_TEST(test_join_accept_size_limits);
    RUN_TEST(test_fcnt_first_frame_of_session);
    RUN_TEST(test_fcnt_window_edges);
    RUN_TEST(test_devnonce_monotonic);
    RUN_TEST(test_tlv_write_read_with_channels);
    RUN_TEST(test_tlv_unknown_types_are_skipped);
    RUN_TEST(test_tlv_truncated_and_writer_errors);
    RUN_TEST(test_tlv_downlink_config);
    RUN_TEST(test_ota_down_payload_limit_and_binding);
    RUN_TEST(test_ota_start_codec_and_check);
    RUN_TEST(test_ota_session_tlv_rules);
    RUN_TEST(test_ota_status_codec);
    RUN_TEST(test_ota_block_codec_limits);
    RUN_TEST(test_ota_block_math);
    RUN_TEST(test_ota_bitmap_window);
    RUN_TEST(test_ota_schedule_and_airtime);
    return UNITY_END();
}
