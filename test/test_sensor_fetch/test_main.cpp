/**
 * Native unit tests for the sensor image download from a URL (M4c S4,
 * Update/SensorFetch.h + the update mode's job rule for it): the URL rule
 * shared with the gateway firmware download, the refusal matrix (job state
 * x queued start x what else runs), the Content-Length and end-of-body
 * checks, the verdict shared by uploads and downloads, the status JSON, and
 * whole downloads through the same image path as an upload (Splitter,
 * classify, verdict) with the signed test fixtures.
 *
 * Run:   pio test -e native -f test_sensor_fetch
 */

#include <ArduinoJson.h>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unity.h>
#include <vector>

#include "../../src/Security/SigVerify.cpp"
#include "../../src/Security/Sha256.h"
#include "../../src/Update/SensorFetch.h"
#include "../../src/Update/UpdateModePolicy.h"
#include "../test_ota_signed/fixtures.h"

using SensorFetch::Activity;
using SensorFetch::Busy;
using SensorFetch::End;
using SensorFetch::Length;
using SensorFetch::Verdict;
using SensorFetch::MAX_FILE;
using SensorFetch::MIN_FILE;
using SensorFetch::busy;
using SensorFetch::busyText;
using SensorFetch::endCheck;
using SensorFetch::lengthCheck;
using SensorFetch::verdict;
using OtaJob::State;
using UpdateModePolicy::Precheck;
using UpdateModePolicy::Reason;
using UpdateModePolicy::Refusal;
using Bytes = std::vector<uint8_t>;

void setUp(void) {}
void tearDown(void) {}

// The same rule for the gateway firmware URL and the sensor image URL
static void test_url_rule(void)
{
    using UpdateModePolicy::urlAccepted;
    TEST_ASSERT_TRUE(urlAccepted("https://pajenicko.cz/webflash/explora/v2/hw0003-0.2.12.bin"));
    TEST_ASSERT_TRUE(urlAccepted("https://192.168.1.126:8443/good/img.bin"));
    TEST_ASSERT_TRUE(urlAccepted("https://[fe80::1]/a.bin"));
    TEST_ASSERT_TRUE(urlAccepted("https://user:pw@example.com/a.bin"));
    TEST_ASSERT_TRUE(urlAccepted("https://example.com"));
    TEST_ASSERT_FALSE(urlAccepted("http://example.com/a.bin"));   // refused before any connection
    TEST_ASSERT_FALSE(urlAccepted("HTTPS://example.com/a.bin"));
    TEST_ASSERT_FALSE(urlAccepted("ftp://example.com/a.bin"));
    TEST_ASSERT_FALSE(urlAccepted(""));
    TEST_ASSERT_FALSE(urlAccepted(nullptr));
    TEST_ASSERT_FALSE(urlAccepted("https://"));
    TEST_ASSERT_FALSE(urlAccepted("https:///a.bin"));
    TEST_ASSERT_FALSE(urlAccepted("https://exa mple.com/a.bin"));
    TEST_ASSERT_FALSE(urlAccepted("https://example.com/a b.bin"));
    TEST_ASSERT_FALSE(urlAccepted("https://example.com/a\r\nX: y"));
    TEST_ASSERT_FALSE(urlAccepted("https://user:p@ss@x.invalid/a.bin")); // odd host (never looked up)
    TEST_ASSERT_FALSE(urlAccepted("https://example.com?x/a.bin"));
    // Length: 256 characters is the limit
    std::string u = "https://example.com/";
    u += std::string(256 - u.size(), 'a');
    TEST_ASSERT_EQUAL(256, (int)u.size());
    TEST_ASSERT_TRUE(urlAccepted(u.c_str()));
    u += "a";
    TEST_ASSERT_FALSE(urlAccepted(u.c_str()));
}

static Precheck okInput(Reason r)
{
    Precheck p{};
    p.uplink = UpdateModePolicy::Link::Ethernet;
    p.clockSet = true;
    p.job = State::None;
    p.reason = r;
    return p;
}

// Job state x queued start: the download may run unless the job needs the
// radio right now (Transfer, Verified, Swapping, Confirming); Waiting and a
// queued start are fine for it (not for the gateway download / check)
static void test_job_matrix(void)
{
    const State all[] = {State::None,     State::Waiting,    State::Transfer, State::Paused,
                         State::Verified, State::Swapping,   State::Confirming, State::Done,
                         State::Failed,   State::Aborted,    State::Reverted};
    for (State s : all)
    {
        const bool radio = s == State::Transfer || s == State::Verified || s == State::Swapping ||
                           s == State::Confirming;
        for (bool pending : {false, true})
        {
            Precheck p = okInput(Reason::SensorFetch);
            p.job = s;
            p.jobStartPending = pending;
            TEST_ASSERT_EQUAL_MESSAGE(radio ? (int)Refusal::SensorJob : (int)Refusal::None, (int)precheck(p),
                                      OtaJob::stateName(s));
            // The other sessions keep their rule: only terminal / Paused, no queued start
            Precheck g = okInput(Reason::GatewayUrl);
            g.job = s;
            g.jobStartPending = pending;
            const bool gwOk = !pending && (OtaJob::terminal(s) || s == State::Paused);
            TEST_ASSERT_EQUAL_MESSAGE(gwOk ? (int)Refusal::None : (int)Refusal::SensorJob, (int)precheck(g),
                                      OtaJob::stateName(s));
            Precheck n = g;
            n.reason = Reason::None; // wouldRefuse() without a reason = the old rule
            TEST_ASSERT_TRUE(precheck(n) == precheck(g));
        }
    }
    // The update mode's other refusals come first, as for every session
    Precheck p = okInput(Reason::SensorFetch);
    p.job = State::Transfer;
    p.uploadRunning = true;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::UploadRunning);
    p.active = true;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::Active);
    p = okInput(Reason::SensorFetch);
    p.clockSet = false;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::ClockNotSet);
    p.uplink = UpdateModePolicy::Link::None;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::Offline);
    p.restartPending = true;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::RestartPending);
}

// What else runs: every single activity refuses, in a fixed order
static void test_busy_matrix(void)
{
    for (unsigned m = 0; m < 64; m++)
    {
        Activity a{};
        a.fetch = m & 1;
        a.restart = m & 2;
        a.gatewayUpload = m & 4;
        a.imageUpload = m & 8;
        a.gatewayUrl = m & 16;
        a.updateCheck = m & 32;
        const Busy b = busy(a);
        Busy want = Busy::None;
        if (a.fetch)
            want = Busy::Fetch;
        else if (a.restart)
            want = Busy::Restart;
        else if (a.gatewayUpload)
            want = Busy::GatewayUpload;
        else if (a.imageUpload)
            want = Busy::ImageUpload;
        else if (a.gatewayUrl)
            want = Busy::GatewayUrl;
        else if (a.updateCheck)
            want = Busy::UpdateCheck;
        TEST_ASSERT_EQUAL((int)want, (int)b);
        TEST_ASSERT_EQUAL(m == 0, busyText(b)[0] == '\0');
    }
}

static void test_length_check(void)
{
    TEST_ASSERT_TRUE(lengthCheck(-1) == Length::Missing); // no Content-Length (chunked, close-delimited)
    TEST_ASSERT_TRUE(lengthCheck(0) == Length::TooShort);
    TEST_ASSERT_TRUE(lengthCheck(MIN_FILE - 1) == Length::TooShort);
    TEST_ASSERT_TRUE(lengthCheck(MIN_FILE) == Length::Ok);
    TEST_ASSERT_TRUE(lengthCheck(96584) == Length::Ok);
    TEST_ASSERT_TRUE(lengthCheck(MAX_FILE) == Length::Ok);
    TEST_ASSERT_TRUE(lengthCheck(MAX_FILE + 1) == Length::TooLarge);
    TEST_ASSERT_TRUE(lengthCheck(200000) == Length::TooLarge);
    TEST_ASSERT_EQUAL(114688u + 128u, MAX_FILE);
    // The gateway firmware download's limits (header + 1 .. the OTA slot):
    // the same rule httpsDownload runs for both
    TEST_ASSERT_TRUE(lengthCheck(-1, 257, 1966080) == Length::Missing);
    TEST_ASSERT_TRUE(lengthCheck(256, 257, 1966080) == Length::TooShort);
    TEST_ASSERT_TRUE(lengthCheck(257, 257, 1966080) == Length::Ok);
    TEST_ASSERT_TRUE(lengthCheck(1796400, 257, 1966080) == Length::Ok);
    TEST_ASSERT_TRUE(lengthCheck(1966080, 257, 1966080) == Length::Ok);
    TEST_ASSERT_TRUE(lengthCheck(1966081, 257, 1966080) == Length::TooLarge);
    TEST_ASSERT_TRUE(lengthCheck(0x7FFFFFFFLL, 257, 1966080) == Length::TooLarge);
}

// The time budget of a sensor image download: 30 s plus the body at 2 kB/s,
// from the request
static void test_budget(void)
{
    using SensorFetch::budgetMs;
    TEST_ASSERT_EQUAL_UINT32(30000, budgetMs(0));
    TEST_ASSERT_EQUAL_UINT32(31000, budgetMs(2048));
    TEST_ASSERT_EQUAL_UINT32(31001, budgetMs(2049)); // rounded up
    TEST_ASSERT_EQUAL_UINT32(30250, budgetMs(MIN_FILE));
    TEST_ASSERT_EQUAL_UINT32(86063, budgetMs(MAX_FILE));
    TEST_ASSERT_EQUAL_UINT32(77161, budgetMs(96584)); // a 0.2.12 image with its signature
    // Monotonic, no overflow at the largest Content-Length any caller passes
    uint32_t prev = 0;
    for (uint32_t t = 0; t <= MAX_FILE; t += 997)
    {
        TEST_ASSERT_TRUE(budgetMs(t) >= prev);
        prev = budgetMs(t);
    }
    // A server at exactly 2 kB/s with 30 s for the connection fits; the
    // measured rates (WiFi / Ethernet ~50 kB/s, LTE ~10 kB/s) have a wide
    // margin; a trickle (1 kB every 10 s, under the 15 s stall limit) is cut
    // after 87 s instead of 15 minutes
    const uint32_t total = MAX_FILE;
    TEST_ASSERT_TRUE(30000 + (uint64_t)total * 1000 / 2048 <= budgetMs(total));
    TEST_ASSERT_TRUE(5000 + (uint64_t)total * 1000 / 10240 < budgetMs(total) / 5);
    TEST_ASSERT_TRUE((uint64_t)total * 1000 / 100 > budgetMs(total)); // 100 B/s: cut
    TEST_ASSERT_TRUE(budgetMs(total) < 900000 / 10);
    // The whole session's radio pause the Waiting rule assumes
    TEST_ASSERT_TRUE(budgetMs(MAX_FILE) + 20000 <= UpdateModePolicy::SENSOR_FETCH_PAUSE_MAX_S * 1000);
}

// A Waiting job and the download: refused with a start out, or when the
// "no accept" deadline leaves less than the longest pause plus two of the
// sensor's intervals; other sessions keep refusing Waiting
static void test_waiting_rule(void)
{
    using UpdateModePolicy::SENSOR_FETCH_PAUSE_MAX_S;
    using UpdateModePolicy::waitingAllows;
    OtaJob::WaitView w;
    TEST_ASSERT_TRUE(waitingAllows(w)); // no deadline armed, no start
    w.deadlineArmed = true;
    w.intervalS = 600;
    w.deadlineLeftS = SENSOR_FETCH_PAUSE_MAX_S + 1200;
    TEST_ASSERT_TRUE(waitingAllows(w));
    w.deadlineLeftS--;
    TEST_ASSERT_FALSE(waitingAllows(w));
    w.deadlineLeftS = 0;
    TEST_ASSERT_FALSE(waitingAllows(w));
    w.deadlineLeftS = 0xFFFFFFFFu;
    w.intervalS = 0xFFFFFFFFu; // no overflow
    TEST_ASSERT_FALSE(waitingAllows(w));
    w.intervalS = 600;
    TEST_ASSERT_TRUE(waitingAllows(w));
    w.startOut = true; // a start out: refused whatever the deadline
    TEST_ASSERT_FALSE(waitingAllows(w));
    w.deadlineArmed = false;
    TEST_ASSERT_FALSE(waitingAllows(w));

    // Through precheck: its own refusal for the download, the old one for the
    // other sessions; Transfer keeps SensorJob
    Precheck p = okInput(Reason::SensorFetch);
    p.job = State::Waiting;
    p.jobWait = w; // start out
    TEST_ASSERT_TRUE(precheck(p) == Refusal::JobWaiting);
    TEST_ASSERT_TRUE(UpdateModePolicy::refusalText(Refusal::JobWaiting)[0] != '\0');
    p.jobWait = OtaJob::WaitView();
    TEST_ASSERT_TRUE(precheck(p) == Refusal::None);
    p.job = State::Transfer;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::SensorJob);
    Precheck g = okInput(Reason::GatewayUrl);
    g.job = State::Waiting;
    TEST_ASSERT_TRUE(precheck(g) == Refusal::SensorJob);
    // Paused / terminal / a queued start are unaffected by the wait view
    p.jobWait.startOut = true;
    p.job = State::Paused;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::None);
    p.job = State::Done;
    p.jobStartPending = true;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::None);

    // Real jobs (OtaJob's own deadline rules)
    OtaJob::Now n;
    n.unixS = 1760000000;
    n.uptimeS = 1000;
    for (uint32_t interval : {60u, 120u, 300u, 600u, 3600u})
    {
        OtaJob::Job j;
        j.intervalS = interval;
        OtaJob::setState(j, State::Waiting, n);
        const uint32_t full = OtaJob::waitingDeadlineS(j);
        OtaJob::WaitView v = OtaJob::waitView(j, n);
        TEST_ASSERT_FALSE(v.startOut);
        TEST_ASSERT_TRUE(v.deadlineArmed);
        TEST_ASSERT_EQUAL_UINT32(full, v.deadlineLeftS);
        TEST_ASSERT_EQUAL(full >= SENSOR_FETCH_PAUSE_MAX_S + 2 * interval, waitingAllows(v));
        // Later in the window: refused once the deadline gets close
        OtaJob::Now later = n;
        later.unixS += full - (SENSOR_FETCH_PAUSE_MAX_S + 2 * interval) + 1;
        TEST_ASSERT_FALSE(waitingAllows(OtaJob::waitView(j, later)));
        // A start went out (its accepting UP is the sensor's next UP): refused
        OtaJob::startSent(j, 41, 7, n);
        TEST_ASSERT_FALSE(waitingAllows(OtaJob::waitView(j, n)));
    }
    // 600 s interval (the default): a fresh Waiting job admits the download
    OtaJob::Job j;
    j.intervalS = 600;
    OtaJob::setState(j, State::Waiting, n);
    TEST_ASSERT_TRUE(waitingAllows(OtaJob::waitView(j, n)));
}

static void test_end_check(void)
{
    TEST_ASSERT_TRUE(endCheck(1000, 1000, false) == End::Ok);
    TEST_ASSERT_TRUE(endCheck(1000, 999, false) == End::Short);
    TEST_ASSERT_TRUE(endCheck(1000, 0, false) == End::Short);
    TEST_ASSERT_TRUE(endCheck(1000, 1000, true) == End::Long);
    TEST_ASSERT_TRUE(endCheck(1000, 1001, false) == End::Long);
}

static void test_verdict(void)
{
    using OtaImageSigned::Class;
    for (bool busyHw : {false, true})
    {
        TEST_ASSERT_TRUE(verdict(Class::Verified, false, busyHw) == (busyHw ? Verdict::Busy : Verdict::Store));
        TEST_ASSERT_TRUE(verdict(Class::Verified, true, busyHw) == (busyHw ? Verdict::Busy : Verdict::Store));
        TEST_ASSERT_TRUE(verdict(Class::Unsigned, false, busyHw) == (busyHw ? Verdict::Busy : Verdict::Store));
        TEST_ASSERT_TRUE(verdict(Class::NoKey, false, busyHw) == (busyHw ? Verdict::Busy : Verdict::Store));
        // "Signed images only" first: the reason the admin can act on
        TEST_ASSERT_TRUE(verdict(Class::Unsigned, true, busyHw) == Verdict::SignedOnly);
        TEST_ASSERT_TRUE(verdict(Class::NoKey, true, busyHw) == Verdict::SignedOnly);
        // A refused classification never gets this far; it would not be stored either
        TEST_ASSERT_TRUE(verdict(Class::Refused, false, busyHw) == Verdict::SignedOnly);
    }
}

static void test_status_json(void)
{
    using FS = SensorFetch::State;
    char buf[320];
    SensorFetch::statusJson(buf, sizeof(buf), FS::Running, 4096, 96584, "");
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"downloading\",\"bytes\":4096,\"total\":96584,\"result\":\"\"}", buf);
    // Quotes, backslashes and control characters cannot break the JSON
    SensorFetch::statusJson(buf, sizeof(buf), FS::Failed, 0, 0, "not stored: \"Signed images only\" \\ x\r\n");
    StaticJsonDocument<512> d;
    TEST_ASSERT_FALSE(deserializeJson(d, buf));
    TEST_ASSERT_EQUAL_STRING("failed", d["state"]);
    TEST_ASSERT_EQUAL_STRING("not stored: 'Signed images only' ' x''", d["result"]);
    // Worst case: the largest numbers and a result longer than the buffer
    std::string longText(1000, 'x');
    for (FS s : {FS::Idle, FS::Queued, FS::Running, FS::Stored, FS::Failed})
    {
        const size_t n = SensorFetch::statusJson(buf, sizeof(buf), s, 0xFFFFFFFFu, 0xFFFFFFFFu, longText.c_str());
        TEST_ASSERT_TRUE(n < sizeof(buf));
        TEST_ASSERT_EQUAL(n, strlen(buf));
        StaticJsonDocument<512> w;
        TEST_ASSERT_FALSE(deserializeJson(w, buf));
        TEST_ASSERT_EQUAL_STRING(SensorFetch::stateName(s), w["state"]);
        TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, w["total"].as<uint32_t>());
    }
}

// --- Whole downloads through the image path of an upload -----------------

static const size_t IMAGE_LEN = sizeof(SIGNED_FILE) - SigBlock::SIZE; // 1024

static ReleaseKeys::Key makeKey(uint32_t id, const uint8_t *point)
{
    ReleaseKeys::Key k{id, {}};
    memcpy(k.point, point, ReleaseKeys::POINT_SIZE);
    return k;
}

static const ReleaseKeys::Key TEST_KEYS[2] = {{0, {}}, makeKey(TEST_KEY_ID, TEST_KEY_POINT)};
static const ReleaseKeys::Key NO_KEYS[2] = {{0, {}}, {0, {}}}; // the production table today

struct Result
{
    bool refusedEarly = false; // before any byte was stored (length)
    bool cut = false;          // short / long body: never classified
    OtaImageSigned::Class cls = OtaImageSigned::Class::Refused;
    Verdict v = Verdict::SignedOnly;
    Bytes stored;
};

// contentLength: what the server announces; body: what it sends; chunk:
// the read size. As httpsDownload + ImageSink + finishSensorImage do it
static Result download(long long contentLength, const Bytes &body, size_t chunk, bool signedOnly,
                       const ReleaseKeys::Key *keys = TEST_KEYS, bool hwBusy = false)
{
    Result r;
    if (lengthCheck(contentLength) != Length::Ok)
    {
        r.refusedEarly = true;
        return r;
    }
    OtaImageSigned::Splitter<Sha256> split;
    Sha256 hash;
    uint32_t received = 0;
    while (received < (uint32_t)contentLength && received < body.size())
    {
        size_t n = body.size() - received;
        n = n < chunk ? n : chunk;
        n = n < (uint32_t)contentLength - received ? n : (uint32_t)contentLength - received;
        size_t payload = 0;
        if (split.feed(body.data() + received, n, payload, hash) != OtaImageSigned::Early::None)
        {
            r.cut = true;
            return r;
        }
        r.stored.insert(r.stored.end(), body.data() + received, body.data() + received + payload);
        received += n;
    }
    if (endCheck((uint32_t)contentLength, received, body.size() > received) != End::Ok)
    {
        r.cut = true;
        return r;
    }
    const OtaImageSigned::Outcome o = OtaImageSigned::classify(split, hash, keys, 2);
    r.cls = o.cls;
    r.v = verdict(o.cls, signedOnly, hwBusy);
    return r;
}

static void test_download_signed_and_unsigned(void)
{
    using OtaImageSigned::Class;
    const Bytes signedFile(SIGNED_FILE, SIGNED_FILE + sizeof(SIGNED_FILE));
    const Bytes image(SIGNED_FILE, SIGNED_FILE + IMAGE_LEN);
    for (size_t chunk : {1u, 7u, 128u, 1000u, 2048u})
    {
        // Signed with a key the firmware carries: stored byte-identical to the image
        Result r = download(signedFile.size(), signedFile, chunk, true);
        TEST_ASSERT_TRUE(r.cls == Class::Verified);
        TEST_ASSERT_TRUE(r.v == Verdict::Store);
        TEST_ASSERT_TRUE(r.stored == image);
        // Unsigned: stored when "Signed images only" is off (confirm at the job start), refused when on
        r = download(image.size(), image, chunk, false);
        TEST_ASSERT_TRUE(r.cls == Class::Unsigned && r.v == Verdict::Store);
        r = download(image.size(), image, chunk, true);
        TEST_ASSERT_TRUE(r.cls == Class::Unsigned && r.v == Verdict::SignedOnly);
        // Signed with a key the firmware does not carry (NoKey) = unsigned
        r = download(signedFile.size(), signedFile, chunk, false, NO_KEYS);
        TEST_ASSERT_TRUE(r.cls == Class::NoKey && r.v == Verdict::Store);
        r = download(signedFile.size(), signedFile, chunk, true, NO_KEYS);
        TEST_ASSERT_TRUE(r.cls == Class::NoKey && r.v == Verdict::SignedOnly);
        // An unfinished job uses this HW type
        r = download(signedFile.size(), signedFile, chunk, false, TEST_KEYS, true);
        TEST_ASSERT_TRUE(r.v == Verdict::Busy);
    }
}

static void test_download_bad_files(void)
{
    using OtaImageSigned::Class;
    const Bytes signedFile(SIGNED_FILE, SIGNED_FILE + sizeof(SIGNED_FILE));
    // Bad signature with a known key: refused, never stored as unsigned
    Bytes bad(SIGNED_FILE, SIGNED_FILE + IMAGE_LEN);
    bad.insert(bad.end(), BLOCK_BAD_SIG, BLOCK_BAD_SIG + SigBlock::SIZE);
    Result r = download(bad.size(), bad, 2048, false);
    TEST_ASSERT_TRUE(r.cls == Class::Refused);
    // A block for another HW type: refused, also without the key (binding first)
    Bytes hw(SIGNED_FILE, SIGNED_FILE + IMAGE_LEN);
    hw.insert(hw.end(), BLOCK_WRONG_HW, BLOCK_WRONG_HW + SigBlock::SIZE);
    TEST_ASSERT_TRUE(download(hw.size(), hw, 2048, false).cls == Class::Refused);
    TEST_ASSERT_TRUE(download(hw.size(), hw, 2048, false, NO_KEYS).cls == Class::Refused);
    // Not a sensor image (e.g. a web page or another file of the server)
    Bytes page(4096, 'x');
    memcpy(page.data(), "<!DOCTYPE html>", 15);
    r = download(page.size(), page, 2048, false);
    TEST_ASSERT_TRUE(r.cut || r.cls == Class::Refused);
}

static void test_download_cut_or_long_body(void)
{
    const Bytes signedFile(SIGNED_FILE, SIGNED_FILE + sizeof(SIGNED_FILE));
    const Bytes image(SIGNED_FILE, SIGNED_FILE + IMAGE_LEN);
    // The server announces the signed file, the connection ends right after
    // the image: a short body, never "unsigned"
    Result r = download(signedFile.size(), image, 2048, false);
    TEST_ASSERT_TRUE(r.cut);
    for (size_t cut : {(size_t)1, (size_t)500, IMAGE_LEN - 1, IMAGE_LEN + 1, signedFile.size() - 1})
    {
        Bytes b(signedFile.begin(), signedFile.begin() + cut);
        TEST_ASSERT_TRUE(download(signedFile.size(), b, 2048, false).cut);
    }
    // More bytes than the Content-Length: refused
    Bytes longer = signedFile;
    longer.push_back(0);
    TEST_ASSERT_TRUE(download(signedFile.size(), longer, 2048, false).cut);
    // Content-Length smaller than the file (the image only): the rest is
    // more data than announced - refused, not stored as unsigned
    TEST_ASSERT_TRUE(download(IMAGE_LEN, signedFile, 2048, false).cut);
    // No Content-Length / too large: refused before anything is stored
    TEST_ASSERT_TRUE(download(-1, signedFile, 2048, false).refusedEarly);
    TEST_ASSERT_TRUE(download(MAX_FILE + 1, signedFile, 2048, false).refusedEarly);
    TEST_ASSERT_TRUE(download(200000, signedFile, 2048, false).refusedEarly);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_url_rule);
    RUN_TEST(test_job_matrix);
    RUN_TEST(test_busy_matrix);
    RUN_TEST(test_length_check);
    RUN_TEST(test_budget);
    RUN_TEST(test_waiting_rule);
    RUN_TEST(test_end_check);
    RUN_TEST(test_verdict);
    RUN_TEST(test_status_json);
    RUN_TEST(test_download_signed_and_unsigned);
    RUN_TEST(test_download_bad_files);
    RUN_TEST(test_download_cut_or_long_body);
    return UNITY_END();
}
