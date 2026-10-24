/**
 * Native unit tests for UpdateModePolicy (Update/UpdateMode.h): the uplink
 * x running interfaces matrix of what is switched off, the refusal reasons
 * and their order, the sensor job states, the memory steps incl. the abort
 * paths, the hard cap and the connectivity check after leaving.
 *
 * Run:   pio test -e native
 */

#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unity.h>
#include "../../src/Update/UpdateModePolicy.h"

using namespace UpdateModePolicy;
using OtaJob::State;

void setUp(void) {}
void tearDown(void) {}

static const uint8_t STA = IF_WIFI_STA;
static const uint8_t AP = IF_WIFI_AP;
static const uint8_t LTE = IF_LTE;

// Every uplink x every combination of running interfaces
static void test_switch_off_matrix(void)
{
    for (uint8_t running = 0; running < 8; running++)
    {
        // Ethernet: everything that runs goes off
        TEST_ASSERT_EQUAL_UINT8(running, switchOffPlan(Link::Ethernet, running));
        // WiFi station carries the uplink: AP and LTE off, the station stays
        TEST_ASSERT_EQUAL_UINT8(running & (AP | LTE), switchOffPlan(Link::WiFi, running));
        // LTE: station and AP off, LTE stays
        TEST_ASSERT_EQUAL_UINT8(running & (STA | AP), switchOffPlan(Link::LTE, running));
        // Never more than what runs
        TEST_ASSERT_EQUAL_UINT8(0, switchOffPlan(Link::Ethernet, running) & (uint8_t)~running);
    }
    // The cases of the design document
    TEST_ASSERT_EQUAL_UINT8(STA | AP | LTE, switchOffPlan(Link::Ethernet, STA | AP | LTE));
    TEST_ASSERT_EQUAL_UINT8(AP | LTE, switchOffPlan(Link::WiFi, STA | AP | LTE));
    TEST_ASSERT_EQUAL_UINT8(STA | AP, switchOffPlan(Link::LTE, STA | AP | LTE));
    // Lite (no LTE): WiFi uplink with the AP still up after boot -> AP only;
    // after the AP timeout nothing can go
    TEST_ASSERT_EQUAL_UINT8(AP, switchOffPlan(Link::WiFi, STA | AP));
    TEST_ASSERT_EQUAL_UINT8(0, switchOffPlan(Link::WiFi, STA));
    // Pro with WiFi switched off: only the boot-grace / recovery AP and LTE
    TEST_ASSERT_EQUAL_UINT8(AP | LTE, switchOffPlan(Link::Ethernet, AP | LTE));
    TEST_ASSERT_EQUAL_UINT8(0, switchOffPlan(Link::Ethernet, 0));
}

static Precheck okInput()
{
    Precheck p{};
    p.active = false;
    p.uplink = Link::Ethernet;
    p.clockSet = true;
    p.uploadRunning = false;
    p.job = State::None;
    p.jobStartPending = false;
    return p;
}

static void test_precheck_ok(void)
{
    Precheck p = okInput();
    TEST_ASSERT_TRUE(precheck(p) == Refusal::None);
    p.uplink = Link::WiFi;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::None);
    p.uplink = Link::LTE;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::None);
}

static void test_refusal_reasons_and_order(void)
{
    Precheck p = okInput();
    p.active = true;
    p.uplink = Link::None;
    p.clockSet = false;
    p.uploadRunning = true;
    p.job = State::Transfer;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::Active);
    p.active = false;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::Offline);
    p.uplink = Link::WiFi;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::ClockNotSet);
    p.clockSet = true;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::UploadRunning);
    p.uploadRunning = false;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::SensorJob);
    p.job = State::Paused;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::None);
    // Every refusal has a text, None has none
    TEST_ASSERT_EQUAL_STRING("", refusalText(Refusal::None));
    for (Refusal r : {Refusal::Active, Refusal::RestartPending, Refusal::Offline, Refusal::ClockNotSet, Refusal::UploadRunning,
                      Refusal::SensorJob})
    {
        TEST_ASSERT_TRUE(refusalText(r)[0] != '\0');
    }
}

static void test_job_states(void)
{
    // Allowed: no job, terminal, Paused
    for (State s : {State::None, State::Done, State::Failed, State::Aborted, State::Reverted, State::Paused})
    {
        TEST_ASSERT_TRUE(jobAllows(s, false));
        TEST_ASSERT_FALSE(jobAllows(s, true)); // a queued start counts as active
    }
    // Refused: the job needs the radio
    for (State s : {State::Waiting, State::Transfer, State::Verified, State::Swapping, State::Confirming})
    {
        TEST_ASSERT_FALSE(jobAllows(s, false));
        Precheck p = okInput();
        p.job = s;
        TEST_ASSERT_TRUE(precheck(p) == Refusal::SensorJob);
    }
}

static void test_fits(void)
{
    using HeapGuard::TLS_MIN_BLOCK;
    using HeapGuard::TLS_MIN_FREE;
    TEST_ASSERT_TRUE(fits({TLS_MIN_FREE, TLS_MIN_BLOCK}, 0));
    TEST_ASSERT_FALSE(fits({TLS_MIN_FREE - 1, TLS_MIN_BLOCK}, 0));
    TEST_ASSERT_FALSE(fits({TLS_MIN_FREE + 100000, TLS_MIN_BLOCK - 1}, 0));
    // The download's own needs come on top
    TEST_ASSERT_FALSE(fits({TLS_MIN_FREE + 8191, 40000}, 8192));
    TEST_ASSERT_TRUE(fits({TLS_MIN_FREE + 8192, 40000}, 8192));
    // Bench figures (RAM stage): Pro all interfaces up 99 kB fits a download,
    // the Pro before it (43.9 kB) did not, with WiFi off it did (72.8 kB)
    TEST_ASSERT_TRUE(fits({99292, 90100}, 8192));
    TEST_ASSERT_FALSE(fits({43960, 38900}, 8192));
    TEST_ASSERT_TRUE(fits({72796, 38900}, 8192));
}

static void test_memory_steps_and_abort_paths(void)
{
    // Fits: nothing is switched off
    TEST_ASSERT_TRUE(memoryStep(true, STA | AP | LTE, false) == MemStep::Proceed);
    TEST_ASSERT_TRUE(memoryStep(true, 0, false) == MemStep::Proceed);
    // Does not fit: switch the plan off once
    TEST_ASSERT_TRUE(memoryStep(false, STA | AP | LTE, false) == MemStep::SwitchOff);
    TEST_ASSERT_TRUE(memoryStep(false, AP, false) == MemStep::SwitchOff);
    // Fits after switching off
    TEST_ASSERT_TRUE(memoryStep(true, STA | AP | LTE, true) == MemStep::Proceed);
    // Abort: still does not fit after switching off
    TEST_ASSERT_TRUE(memoryStep(false, STA | AP | LTE, true) == MemStep::Abort);
    // Abort: nothing to switch off (Lite on WiFi after the AP timeout)
    TEST_ASSERT_TRUE(memoryStep(false, 0, false) == MemStep::Abort);
}

static void test_hard_cap(void)
{
    const uint32_t CAP = 15UL * 60 * 1000;
    TEST_ASSERT_FALSE(capReached(1000, 1000, CAP));
    TEST_ASSERT_FALSE(capReached(1000 + CAP - 1, 1000, CAP));
    TEST_ASSERT_TRUE(capReached(1000 + CAP, 1000, CAP));
    // millis() wrap
    TEST_ASSERT_FALSE(capReached(100, 0xFFFFFF00u, CAP));
    TEST_ASSERT_TRUE(capReached(CAP, 0xFFFFFF00u, CAP));
}

static Watch armedWith(uint8_t off, const LinkState &before, uint32_t at)
{
    Watch w{};
    armWatch(w, off, before, at);
    return w;
}

static const Wanted ALL_WANTED{true, true, true};

static void test_watch_missing(void)
{
    LinkState all{true, true, true, true};
    LinkState down{false, false, false, false};
    // Nothing switched off: no watch at all
    Watch none = armedWith(0, all, 0);
    TEST_ASSERT_FALSE(none.armed);
    // Station switched off and up before: must have its address again
    Watch w = armedWith(STA | AP | LTE, all, 0);
    TEST_ASSERT_TRUE(w.armed);
    TEST_ASSERT_EQUAL_UINT8(STA | AP | LTE, w.expect);
    TEST_ASSERT_EQUAL_UINT8(STA, watchMissing(w, LinkState{true, false, true, true}, ALL_WANTED));
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(w, all, ALL_WANTED));
    // ... not if it had none before (station retrying)
    Watch noSta = armedWith(STA | AP | LTE, LinkState{true, false, true, true}, 0);
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(noSta, LinkState{true, false, true, true}, ALL_WANTED));
    // AP: only when it ran before and the WiFi policy still wants it
    Watch ap = armedWith(AP, all, 0);
    TEST_ASSERT_EQUAL_UINT8(AP, watchMissing(ap, LinkState{true, true, false, true}, ALL_WANTED));
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(ap, LinkState{true, true, false, true}, Wanted{true, false, true}));
    // ... and stays expected (the policy may want it again)
    TEST_ASSERT_EQUAL_UINT8(AP, ap.expect);
    // LTE: only when it was online before
    Watch lte = armedWith(LTE, all, 0);
    TEST_ASSERT_EQUAL_UINT8(LTE, watchMissing(lte, LinkState{true, true, true, false}, ALL_WANTED));
    Watch lteRetrying = armedWith(LTE, LinkState{true, true, true, false}, 0);
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(lteRetrying, LinkState{true, true, true, false}, ALL_WANTED));
    // The uplink: only if it was online before the session
    Watch st = armedWith(STA, all, 0);
    TEST_ASSERT_EQUAL_UINT8(IF_UPLINK, watchMissing(st, LinkState{false, true, true, true}, ALL_WANTED));
    Watch offline = armedWith(AP, LinkState{false, false, true, false}, 0);
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(offline, LinkState{false, false, true, false}, ALL_WANTED));
    TEST_ASSERT_EQUAL_UINT8(IF_UPLINK | STA | AP | LTE, watchMissing(w, down, ALL_WANTED));
}

static void test_watch_settings_changed(void)
{
    LinkState all{true, true, true, true};
    LinkState noLte{true, true, true, false};
    Watch w = armedWith(STA | AP | LTE, all, 0);
    // LTE disabled / changed in the settings: dropped for good, nothing missing
    uint8_t dropped = 0;
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(w, noLte, Wanted{true, true, false}, &dropped));
    TEST_ASSERT_EQUAL_UINT8(LTE, dropped);
    TEST_ASSERT_EQUAL_UINT8(STA | AP, w.expect);
    // ... even when wanted again later (the watch is about the old settings)
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(w, noLte, ALL_WANTED, &dropped));
    TEST_ASSERT_EQUAL_UINT8(0, dropped);
    // Station off in the settings: not expected
    Watch s = armedWith(STA, all, 0);
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(s, LinkState{true, false, false, true}, Wanted{false, false, true}));
    // The uplink is never dropped
    TEST_ASSERT_EQUAL_UINT8(IF_UPLINK, watchMissing(s, LinkState{false, false, false, true}, Wanted{false, false, false}));
}

static void test_watch_merge(void)
{
    LinkState all{true, true, true, true};
    // Session 1 switched the station and LTE off; session 2 starts while
    // they are still reattaching (its "before" has neither) and switches
    // the AP off: the earlier interfaces stay expected
    Watch w = armedWith(STA | LTE, all, 1000);
    LinkState reattaching{true, false, true, false};
    armWatch(w, AP, reattaching, 5000);
    TEST_ASSERT_EQUAL_UINT8(STA | AP | LTE, w.expect);
    TEST_ASSERT_EQUAL_UINT32(5000, w.leftAt); // the window starts again
    TEST_ASSERT_TRUE(w.needOnline);
    // A session that switched nothing off keeps what the watch expects,
    // but restarts its window (it blocked the loop: LTE could not reattach)
    armWatch(w, 0, LinkState{false, false, false, false}, 9000);
    TEST_ASSERT_TRUE(w.armed);
    TEST_ASSERT_EQUAL_UINT8(STA | AP | LTE, w.expect);
    TEST_ASSERT_EQUAL_UINT32(9000, w.leftAt);
    TEST_ASSERT_TRUE(w.needOnline);
    // ... and arms none when no watch runs
    Watch idle{};
    armWatch(idle, 0, all, 9000);
    TEST_ASSERT_FALSE(idle.armed);
    // A new watch after the old one ended starts from scratch
    w.armed = false;
    armWatch(w, LTE, all, 20000);
    TEST_ASSERT_EQUAL_UINT8(LTE, w.expect);
}

// Every watched interface disabled by the user: "watch cancelled", not
// "connectivity restored"
static void test_watch_cancelled(void)
{
    LinkState all{true, true, false, true}; // no AP before: STA off expects no AP
    LinkState noSta{true, false, false, false};
    Watch w = armedWith(STA | LTE, all, 0);
    TEST_ASSERT_FALSE(watchCancelled(w));
    // One of two dropped: still watching the other
    TEST_ASSERT_EQUAL_UINT8(STA, watchMissing(w, noSta, Wanted{true, true, false}));
    TEST_ASSERT_FALSE(watchCancelled(w));
    // Both dropped: nothing missing, and cancelled
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(w, noSta, Wanted{false, true, false}));
    TEST_ASSERT_TRUE(watchCancelled(w));
    TEST_ASSERT_EQUAL_UINT8(STA | LTE, w.dropped);
    // Back on its own (nothing dropped): restored, not cancelled
    Watch r = armedWith(STA | LTE, all, 0);
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(r, all, ALL_WANTED));
    TEST_ASSERT_FALSE(watchCancelled(r));
    // One dropped, the other back: restored (something did come back)
    Watch m = armedWith(STA | LTE, all, 0);
    TEST_ASSERT_EQUAL_UINT8(0, watchMissing(m, LinkState{true, true, true, false}, Wanted{true, true, false}));
    TEST_ASSERT_FALSE(watchCancelled(m));
    // A new watch forgets the drops of the old one
    w.armed = false;
    armWatch(w, LTE, all, 100);
    TEST_ASSERT_EQUAL_UINT8(0, w.dropped);
    TEST_ASSERT_FALSE(watchCancelled(w));
    // The diag JSON names it
    DiagView v{};
    v.lastResult = "";
    v.refusedText = "";
    v.restore = 3;
    char out[DIAG_JSON_MAX];
    diagJson(v, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"restore\":\"cancelled\""));
}

static void test_restart_pending_and_interrupt(void)
{
    Precheck p = okInput();
    p.restartPending = true;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::RestartPending);
    p.uplink = Link::None;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::RestartPending); // before Offline
    p.active = true;
    TEST_ASSERT_TRUE(precheck(p) == Refusal::Active);
    TEST_ASSERT_TRUE(refusalText(Refusal::RestartPending)[0] != '\0');
    // A watch restart waits for an upload, a radio job and a queued start
    TEST_ASSERT_FALSE(restartWouldInterrupt(false, State::None, false));
    TEST_ASSERT_FALSE(restartWouldInterrupt(false, State::Paused, false));
    TEST_ASSERT_TRUE(restartWouldInterrupt(true, State::None, false));
    TEST_ASSERT_TRUE(restartWouldInterrupt(false, State::Done, true));
    TEST_ASSERT_TRUE(restartWouldInterrupt(false, State::Transfer, false));
}

static void test_url_host(void)
{
    char h[64];
    bool lit = true;
    TEST_ASSERT_TRUE(urlHost("https://pajenicko.cz/webflash/x.json", h, sizeof(h), lit));
    TEST_ASSERT_EQUAL_STRING("pajenicko.cz", h);
    TEST_ASSERT_FALSE(lit);
    TEST_ASSERT_TRUE(urlHost("https://host.example:8443/a?x=1", h, sizeof(h), lit));
    TEST_ASSERT_EQUAL_STRING("host.example", h);
    // Split exactly like HTTPClient: the authority ends at the first '/', the
    // user info at its FIRST '@' - '?', '#', '@' after the first '/' are path
    TEST_ASSERT_TRUE(urlHost("https://user@files.example:443/a@b.bin?q=@x#@y", h, sizeof(h), lit));
    TEST_ASSERT_EQUAL_STRING("files.example", h);
    TEST_ASSERT_TRUE(urlHost("https://user:pw@files.example/a.bin", h, sizeof(h), lit));
    TEST_ASSERT_EQUAL_STRING("files.example", h);
    // '?' or '#' before the first '/': still the authority for HTTPClient,
    // which connects to the host after the first '@'
    TEST_ASSERT_TRUE(urlHost("https://a.example?x@b.example/fw", h, sizeof(h), lit));
    TEST_ASSERT_EQUAL_STRING("b.example", h);
    TEST_ASSERT_TRUE(urlHost("https://files.example#frag@x/a", h, sizeof(h), lit));
    TEST_ASSERT_EQUAL_STRING("x", h);
    // An odd host HTTPClient would take ('?', '#' or '@' left in it, e.g. a
    // raw '@' in the password): no lookup (and so never logged)
    TEST_ASSERT_FALSE(urlHost("https://user:p@ss@files.example/a.bin", h, sizeof(h), lit));
    TEST_ASSERT_FALSE(urlHost("https://a.example?token=1", h, sizeof(h), lit));
    TEST_ASSERT_FALSE(urlHost("https://a.example#x", h, sizeof(h), lit));
    // Literals: no lookup
    TEST_ASSERT_TRUE(urlHost("https://[2001:db8::1]:443/a.bin", h, sizeof(h), lit));
    TEST_ASSERT_TRUE(lit);
    TEST_ASSERT_TRUE(urlHost("https://u:p@[::1]/a.bin", h, sizeof(h), lit));
    TEST_ASSERT_TRUE(lit);
    TEST_ASSERT_TRUE(urlHost("https://192.168.1.10:8443/a.bin", h, sizeof(h), lit));
    TEST_ASSERT_TRUE(lit);
    TEST_ASSERT_EQUAL_STRING("192.168.1.10", h);
    // No host / not https / too long
    TEST_ASSERT_FALSE(urlHost("https:///a.bin", h, sizeof(h), lit));
    TEST_ASSERT_FALSE(urlHost("https://user@/a.bin", h, sizeof(h), lit));
    TEST_ASSERT_FALSE(urlHost("http://host/a.bin", h, sizeof(h), lit));
    TEST_ASSERT_FALSE(urlHost("https://abcdefghij.example/", h, 8, lit));
}

static void test_url_for_log(void)
{
    char o[160];
    struct
    {
        const char *in, *want;
    } cases[] = {
        {"https://pajenicko.cz/webflash/explora/v1/fw.bin", "https://pajenicko.cz/webflash/explora/v1/fw.bin"},
        {"https://user:pw@files.example:8443/a.bin?token=abc#f", "https://files.example:8443/a.bin"},
        {"https://files.example/a@b.bin?q=@x", "https://files.example/a@b.bin"},
        {"https://a.example?x@b.example/fw", "https://b.example/fw"},
        {"https://files.example#frag@x/a", "https://x/a"},
        {"https://u:p@[2001:db8::1]:443/a.bin", "https://[2001:db8::1]:443/a.bin"},
        {"https://files.example", "https://files.example"},
        // odd hosts: never shown (may hold a part of the password or a token)
        {"https://user:p@ss@files.example/a.bin", "https://(invalid host)/a.bin"},
        {"https://a.example?token=secret", "https://(invalid host)"},
    };
    for (auto &c : cases)
    {
        urlForLog(c.in, o, sizeof(o));
        TEST_ASSERT_EQUAL_STRING(c.want, o);
    }
    // Truncated to the buffer, always terminated
    urlForLog("https://files.example/a.bin", o, 10);
    TEST_ASSERT_EQUAL_STRING("https://f", o);
}

static void test_after_leave(void)
{
    const uint32_t LIMIT = 5UL * 60 * 1000;
    TEST_ASSERT_TRUE(afterLeave(0, 5000, 1000, LIMIT, false) == After::Restored);
    TEST_ASSERT_TRUE(afterLeave(0, 1000 + LIMIT + 1, 1000, LIMIT, true) == After::Restored);
    TEST_ASSERT_TRUE(afterLeave(LTE, 1000 + LIMIT - 1, 1000, LIMIT, false) == After::Watching);
    TEST_ASSERT_TRUE(afterLeave(LTE, 1000 + LIMIT, 1000, LIMIT, false) == After::Reboot);
    TEST_ASSERT_TRUE(afterLeave(IF_UPLINK, 1000 + LIMIT, 1000, LIMIT, false) == After::Reboot);
    // Work in progress defers the restart, it does not cancel it
    TEST_ASSERT_TRUE(afterLeave(LTE, 1000 + LIMIT, 1000, LIMIT, true) == After::Deferred);
    TEST_ASSERT_TRUE(afterLeave(LTE, 1000 + LIMIT - 1, 1000, LIMIT, true) == After::Watching);
    TEST_ASSERT_TRUE(afterLeave(STA, 10, 0xFFFFFFF0u, LIMIT, false) == After::Watching);
    // Which jobs block a restart
    TEST_ASSERT_TRUE(jobBlocksRestart(State::Transfer));
    TEST_ASSERT_TRUE(jobBlocksRestart(State::Swapping));
    TEST_ASSERT_TRUE(jobBlocksRestart(State::Confirming));
    for (State st : {State::None, State::Waiting, State::Paused, State::Verified, State::Done, State::Failed,
                     State::Aborted, State::Reverted})
    {
        TEST_ASSERT_FALSE(jobBlocksRestart(st));
    }
}

// Multi-pass sessions (S6): a session the hard cap ended must not end a
// newer one, and its owner sees it as over
static void test_generations(void)
{
    uint32_t current = 0;
    uint32_t first = nextGeneration(current);
    current = first;
    TEST_ASSERT_TRUE(first != 0);
    TEST_ASSERT_TRUE(ownsSession(first, current, true));
    // process() ended it (hard cap): active false - its leave() is a no-op
    TEST_ASSERT_FALSE(ownsSession(first, current, false));
    // A newer session: the stale owner does not own it
    uint32_t second = nextGeneration(current);
    current = second;
    TEST_ASSERT_TRUE(second != first);
    TEST_ASSERT_FALSE(ownsSession(first, current, true));
    TEST_ASSERT_TRUE(ownsSession(second, current, true));
    // "No session" is never owned, the counter skips 0 on wrap
    TEST_ASSERT_FALSE(ownsSession(0, 0, true));
    TEST_ASSERT_EQUAL_UINT32(1, nextGeneration(0xFFFFFFFFu));
}

static void test_interfaces_text(void)
{
    char t[INTERFACES_TEXT_MAX];
    interfacesText(0, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("nothing", t);
    interfacesText(STA | AP | LTE, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("WiFi station, access point, LTE", t);
    interfacesText(IF_UPLINK | LTE, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("the uplink, LTE", t);
    interfacesText(0xFF, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("the uplink, WiFi station, access point, LTE", t);
    char safe[8];
    jsonSafeCopy("a\"b\\c\nde", safe, sizeof(safe));
    TEST_ASSERT_EQUAL_STRING("a'b'c d", safe);
}

static void fill(char *buf, size_t size, char c)
{
    memset(buf, c, size - 1);
    buf[size - 1] = 0;
}

// The worst case of /diag/heap "updateMode": an active session after a
// failed one with the longest texts, every number at 10 digits. Must fit
// DIAG_JSON_MAX and parse (the reviews found the old 448 B buffer cut it)
static void test_diag_json_worst_case(void)
{
    static char result[RESULT_MAX], refused[REFUSAL_MAX];
    fill(result, sizeof(result), 'r');
    result[5] = '"'; // replaced, never escaped longer
    result[6] = '\\';
    fill(refused, sizeof(refused), 'f');
    DiagView v{};
    v.active = true;
    v.reason = Reason::GatewayUrl;
    v.startedAt = v.runningS = v.freeAtEntry = v.largestAtEntry = v.minFree = 0xFFFFFFFFu;
    v.switchedOff = v.lastSwitchedOff = v.watchExpect = 0xFF;
    v.sessions = v.aborts = v.refusals = 0xFFFFFFFFu;
    v.lastReason = Reason::GatewayUrl;
    v.lastResult = result;
    v.lastDurationS = v.lastMinFree = v.restoreS = v.refusedAgoS = 0xFFFFFFFFu;
    v.restore = 1;
    v.refusedReason = Reason::GatewayUrl;
    v.refusedText = refused;
    static char out[DIAG_JSON_MAX];
    int n = diagJson(v, out, sizeof(out));
    printf("worst-case updateMode JSON: %d B of %u\n", n, (unsigned)DIAG_JSON_MAX);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(out));
    TEST_ASSERT_EQUAL_size_t((size_t)n, strlen(out));
    // Parsed as part of /diag/heap
    std::string doc = std::string("{\"x\":1,\"updateMode\":") + out + ",\"radioPaused\":true}";
    DynamicJsonDocument d(4096);
    TEST_ASSERT_TRUE(deserializeJson(d, doc) == DeserializationError::Ok);
    TEST_ASSERT_TRUE(d["updateMode"]["active"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("gateway firmware download", d["updateMode"]["reason"].as<const char *>());
    TEST_ASSERT_EQUAL_size_t(RESULT_MAX - 1, strlen(d["updateMode"]["last"]["result"].as<const char *>()));
    TEST_ASSERT_EQUAL_size_t(REFUSAL_MAX - 1, strlen(d["updateMode"]["lastRefusal"]["text"].as<const char *>()));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, d["updateMode"]["minFree8bit"].as<uint32_t>());
    TEST_ASSERT_TRUE(d["radioPaused"].as<bool>());
    // Idle, nothing yet: short and valid, no lastRefusal
    DiagView idle{};
    idle.lastResult = "";
    idle.refusedText = "";
    n = diagJson(idle, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(out));
    DynamicJsonDocument d2(2048);
    TEST_ASSERT_TRUE(deserializeJson(d2, out) == DeserializationError::Ok);
    TEST_ASSERT_FALSE(d2["active"].as<bool>());
    TEST_ASSERT_TRUE(d2["lastRefusal"].isNull());
    // A too small buffer reports the length needed (the firmware then
    // prints a minimal object instead of a cut one)
    char small[64];
    int need = diagJson(v, small, sizeof(small));
    TEST_ASSERT_TRUE(need >= (int)sizeof(small));
    TEST_ASSERT_EQUAL_size_t(sizeof(small) - 1, strlen(small));
}

static void test_reason_names(void)
{
    TEST_ASSERT_EQUAL_STRING("gateway firmware download", reasonName(Reason::GatewayUrl));
    TEST_ASSERT_EQUAL_STRING("update check", reasonName(Reason::UpdateCheck));
    for (Reason r : {Reason::None, Reason::UpdateCheck, Reason::GatewayUrl, Reason::SensorFetch, Reason::Manifest,
                     Reason::Scheduled})
    {
        TEST_ASSERT_TRUE(reasonName(r)[0] != '\0');
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_switch_off_matrix);
    RUN_TEST(test_precheck_ok);
    RUN_TEST(test_refusal_reasons_and_order);
    RUN_TEST(test_job_states);
    RUN_TEST(test_fits);
    RUN_TEST(test_memory_steps_and_abort_paths);
    RUN_TEST(test_hard_cap);
    RUN_TEST(test_watch_missing);
    RUN_TEST(test_watch_settings_changed);
    RUN_TEST(test_watch_merge);
    RUN_TEST(test_watch_cancelled);
    RUN_TEST(test_restart_pending_and_interrupt);
    RUN_TEST(test_url_host);
    RUN_TEST(test_url_for_log);
    RUN_TEST(test_after_leave);
    RUN_TEST(test_generations);
    RUN_TEST(test_interfaces_text);
    RUN_TEST(test_diag_json_worst_case);
    RUN_TEST(test_reason_names);
    return UNITY_END();
}
