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
    TEST_ASSERT_TRUE(openData(aesSoftEncrypt, sessKeys(), f, n, kVecUpFcnt, 0, p, pl));
    TEST_ASSERT_EQUAL(sizeof(kVecUpTlv), pl);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecUpTlv, p, pl);
    // Wrong high counter bits, flipped bit anywhere, wrong key: MIC fails
    TEST_ASSERT_FALSE(openData(aesSoftEncrypt, sessKeys(), f, n, kVecUpFcnt + 0x10000, 0, p, pl));
    for (size_t i = 0; i < n; i++)
    {
        f[i] ^= 0x01;
        TEST_ASSERT_FALSE(openData(aesSoftEncrypt, sessKeys(), f, n, kVecUpFcnt, 0, p, pl));
        f[i] ^= 0x01;
    }
    TEST_ASSERT_FALSE(openData(aesSoftEncrypt, joinKeys(), f, n, kVecUpFcnt, 0, p, pl));
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
    TEST_ASSERT_TRUE(openData(aesSoftEncrypt, sessKeys(), f, n, kVecDownFcnt, kVecUpFcnt, p, pl));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kVecDownTlv, p, pl);
    // Same DOWN presented as the answer to another uplink: rejected
    TEST_ASSERT_FALSE(openData(aesSoftEncrypt, sessKeys(), f, n, kVecDownFcnt, kVecUpFcnt + 1, p, pl));
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
    TEST_ASSERT_TRUE(openData(aesSoftEncrypt, sessKeys(), f, n, 1, 0, p, pl));
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
    RUN_TEST(test_fcnt_first_frame_of_session);
    RUN_TEST(test_fcnt_window_edges);
    RUN_TEST(test_devnonce_monotonic);
    RUN_TEST(test_tlv_write_read_with_channels);
    RUN_TEST(test_tlv_unknown_types_are_skipped);
    RUN_TEST(test_tlv_truncated_and_writer_errors);
    RUN_TEST(test_tlv_downlink_config);
    return UNITY_END();
}
