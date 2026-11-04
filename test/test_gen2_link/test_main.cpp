/**
 * Native unit tests for the generation 2 link margins and the TX power
 * recommendation (docs/protocol-v2.md 4.3).
 * Run:   pio test -e native
 */

#include <unity.h>
#include "../../src/Protocol/Gen2Link.h"

using namespace Gen2Link;

void setUp(void) {}
void tearDown(void) {}

static const uint32_t T0 = 1000000;
static const int SAT = 11;   // a saturated SNR reading as seen on the bench
static const int CLEAR = 60; // an SNR that never limits (tests of the RSSI rules)

static History filled(int n, int rssi, int8_t tx, uint32_t stepS = 600, int snr = CLEAR, int8_t down = NONE)
{
    History h;
    for (int i = 0; i < n; i++)
    {
        h.add(T0 + (uint32_t)i * stepS, rssi, snr, down, NONE, tx, 0);
    }
    return h;
}

static uint32_t last(int n, uint32_t stepS = 600)
{
    return T0 + (uint32_t)(n - 1) * stepS;
}

static void test_too_few_samples(void)
{
    History h = filled(MIN_UP_SAMPLES - 1, -60, 14);
    Margins m = margins(h, last(4), 14);
    TEST_ASSERT_FALSE(m.upValid());
    TEST_ASSERT_EQUAL((int)Advice::None, (int)recommend(m, 14).advice);
    History e;
    m = margins(e, T0, 14);
    TEST_ASSERT_EQUAL(0, m.upSamples);
    TEST_ASSERT_EQUAL((int)Advice::None, (int)recommend(m, 14).advice);
}

static void test_margins_worst_and_down(void)
{
    History h;
    const int rssi[6] = {-70, -82, -75, -64, -90, -71};
    const int8_t down[6] = {-60, NONE, -65, NONE, -128, NONE};
    for (int i = 0; i < 6; i++)
    {
        h.add(T0 + (uint32_t)i, rssi[i], CLEAR, down[i], NONE, 14, 0);
    }
    Margins m = margins(h, T0 + 6, 14);
    TEST_ASSERT_EQUAL(6, m.upSamples);
    TEST_ASSERT_EQUAL(-90, m.worstUp);
    TEST_ASSERT_EQUAL(39, m.upMargin()); // -90 - (-129)
    TEST_ASSERT_EQUAL(3, m.downSamples);
    TEST_ASSERT_EQUAL(-128, m.worstDown); // a real -128 dBm is a sample, not "absent"
    TEST_ASSERT_EQUAL(1, m.downMargin());
    TEST_ASSERT_TRUE(m.downValid());
}

static void test_lower_to_limit(void)
{
    History h = filled(10, -60, 14); // margin 69 dB: limited to -9 dBm
    Margins m = margins(h, last(10), 14);
    TEST_ASSERT_EQUAL(69, m.upMargin());
    Recommendation r = recommend(m, 14);
    TEST_ASSERT_EQUAL((int)Advice::Lower, (int)r.advice);
    TEST_ASSERT_EQUAL(-9, r.power);
    History h2 = filled(10, -83, -9);
    TEST_ASSERT_EQUAL((int)Advice::None, (int)recommend(margins(h2, last(10), -9), -9).advice);
}

static void test_lower_keeps_fade_margin(void)
{
    History h = filled(8, -109, 14); // margin 20 dB
    Recommendation r = recommend(margins(h, last(8), 14), 14);
    TEST_ASSERT_EQUAL((int)Advice::Lower, (int)r.advice);
    TEST_ASSERT_EQUAL(9, r.power);
    History h2 = filled(8, -112, 14); // 17 dB: 2 dB step only
    TEST_ASSERT_EQUAL((int)Advice::None, (int)recommend(margins(h2, last(8), 14), 14).advice);
    History h3 = filled(8, -111, 14); // 18 dB: 3 dB step
    Recommendation r3 = recommend(margins(h3, last(8), 14), 14);
    TEST_ASSERT_EQUAL((int)Advice::Lower, (int)r3.advice);
    TEST_ASSERT_EQUAL(11, r3.power);
}

static void test_raise_and_maximum(void)
{
    History h = filled(8, -121, 14); // 8 dB: +7 to 21 dBm
    Recommendation r = recommend(margins(h, last(8), 14), 14);
    TEST_ASSERT_EQUAL((int)Advice::Raise, (int)r.advice);
    TEST_ASSERT_EQUAL(21, r.power);
    History h2 = filled(8, -127, 14); // 2 dB: capped at 22
    r = recommend(margins(h2, last(8), 14), 14);
    TEST_ASSERT_EQUAL((int)Advice::Raise, (int)r.advice);
    TEST_ASSERT_EQUAL(22, r.power);
    History h3 = filled(8, -125, 22);
    TEST_ASSERT_EQUAL((int)Advice::AtMaximum, (int)recommend(margins(h3, last(8), 22), 22).advice);
    History h4 = filled(8, -119, 14); // 10 dB: nothing
    TEST_ASSERT_EQUAL((int)Advice::None, (int)recommend(margins(h4, last(8), 14), 14).advice);
}

static void test_normalised_to_current_power(void)
{
    History h = filled(6, -100, 8);
    Margins m = margins(h, last(6), 14);
    TEST_ASSERT_EQUAL(-94, m.worstUp);
    TEST_ASSERT_EQUAL(35, m.upMargin());
    History h2;
    h2.add(T0, -80, CLEAR, NONE, NONE, 14, 0);
    h2.add(T0 + 1, -95, CLEAR, NONE, NONE, 0, 0);
    h2.add(T0 + 2, -70, CLEAR, NONE, NONE, 22, 0);
    TEST_ASSERT_EQUAL(-81, margins(h2, T0 + 3, 14).worstUp);
}

static void test_reviewer_noise_floor_scenarios(void)
{
    // noise floor about -98 dBm: RSSI -92, reported SNR +6 at 14 dBm.
    // RSSI margin 37 dB, SNR margin 18 dB: at most a 3 dB step
    History a = filled(6, -92, 14, 600, 6);
    Margins m = margins(a, last(6), 14);
    TEST_ASSERT_EQUAL(18, m.upMargin());
    TEST_ASSERT_TRUE(m.upSnrLimits());
    Recommendation r = recommend(m, 14);
    TEST_ASSERT_TRUE(r.advice != Advice::Lower || 14 - r.power <= 3);
    // reported +8: at most 5 dB
    History b = filled(6, -92, 14, 600, 8);
    r = recommend(margins(b, last(6), 14), 14);
    TEST_ASSERT_EQUAL((int)Advice::Lower, (int)r.advice);
    TEST_ASSERT_EQUAL(9, r.power);
    // saturated +11 at the current power: one step of at most 8 dB
    History c = filled(6, -60, 14, 600, SAT);
    r = recommend(margins(c, last(6), 14), 14);
    TEST_ASSERT_EQUAL((int)Advice::Lower, (int)r.advice);
    TEST_ASSERT_EQUAL(6, r.power);
}

static void test_snr_bound_normalisation(void)
{
    // a saturated reading normalised up stays a bound: +11 at 14 -> +19 at 22
    History up = filled(6, -60, 14, 600, SAT);
    Margins m = margins(up, last(6), 22);
    TEST_ASSERT_EQUAL(6, m.upSnrSamples);
    TEST_ASSERT_EQUAL(19, m.worstUpSnr);
    // ... normalised down it is skipped (22 -> -9)
    History down = filled(6, -41, 22, 600, SAT);
    m = margins(down, last(6), -9);
    TEST_ASSERT_EQUAL(0, m.upSnrSamples);
    TEST_ASSERT_FALSE(m.upSnrLimits());
    TEST_ASSERT_EQUAL((int)Advice::None, (int)recommend(m, -9).advice); // no Lower without a bound
    // a reading below saturation normalised down counts: +4 at 14 -> -2 at 8
    History low = filled(6, -100, 14, 600, 4);
    m = margins(low, last(6), 8);
    TEST_ASSERT_EQUAL(6, m.upSnrSamples);
    TEST_ASSERT_EQUAL(-2, m.worstUpSnr);
    TEST_ASSERT_EQUAL(10, m.upMargin());
    // SNR-limited at a noisy site: raise
    History noisy = filled(6, -111, 14, 600, -6);
    m = margins(noisy, last(6), 14);
    TEST_ASSERT_EQUAL(6, m.upMargin());
    Recommendation r = recommend(m, 14);
    TEST_ASSERT_EQUAL((int)Advice::Raise, (int)r.advice);
    TEST_ASSERT_EQUAL(22, r.power);
}

static void test_downlink_snr_lower_bound(void)
{
    History h;
    for (int i = 0; i < 4; i++)
    {
        h.add(T0 + (uint32_t)i, -70, CLEAR, -100, (int8_t)(i == 2 ? -4 : 11), 14, 0);
    }
    Margins m = margins(h, T0 + 4, 14);
    TEST_ASSERT_EQUAL(4, m.downSnrSamples); // every report counts, saturated ones too
    TEST_ASSERT_EQUAL(-4, m.worstDownSnr);
    TEST_ASSERT_TRUE(m.downSnrLimits());
    TEST_ASSERT_EQUAL(8, m.downMargin());
}

static void test_no_flip_flop_after_lower(void)
{
    const int snrs[4] = {SAT, 8, 4, -2};
    for (int si = 0; si < 4; si++)
    {
        for (int rssi = -125; rssi <= -40; rssi += 5)
        {
            for (int tx = -9; tx <= 22; tx++)
            {
                History h = filled(6, rssi, (int8_t)tx, 600, snrs[si]);
                Recommendation r = recommend(margins(h, last(6), tx), tx);
                if (r.advice != Advice::Lower)
                {
                    continue;
                }
                // the operator follows the hint; before new samples arrive
                // the old ones must not ask for another change
                Recommendation again = recommend(margins(h, last(6), r.power), r.power);
                TEST_ASSERT_EQUAL((int)Advice::None, (int)again.advice);
            }
        }
    }
}

// Physically consistent histories: one noise floor and one path loss per
// history, a fade per sample, the reading = min(true SNR, saturation). After
// a Lower hint every sample - counted or skipped - keeps its SNR bound
// >= +3 dB (true SNR >= -12 dB + fade margin) and its RSSI >= -114 dBm at the
// new power; the same data never turns a Lower into a Raise.
static void test_brute_force_lower_physical(void)
{
    uint32_t seed = 12345;
    auto rnd = [&seed](int lo, int hi) {
        seed = seed * 1103515245u + 12345u;
        return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
    };
    int lowers = 0, lowersToMin = 0;
    for (int round = 0; round < 60000; round++)
    {
        const int noise = rnd(-125, -90);
        const int loss = rnd(50, 150);
        const int spread = rnd(0, 20);
        const int sat = rnd(10, 12);
        const int p0 = rnd(-9, 22);
        const int n = rnd(5, 20);
        History h;
        int txs[20], rssis[20], truths[20], readings[20];
        int k = 0;
        for (int tries = 0; k < n && tries < 200; tries++)
        {
            const int tx = (k < n - 3 && rnd(0, 4) == 0) ? rnd(-9, 22) : p0; // the newest 3 at p0
            const int rssi = tx - loss - rnd(0, spread);
            const int truth = rssi - noise;
            if (rssi < SENSITIVITY_DBM || truth < SNR_LIMIT_DB)
            {
                continue; // not received
            }
            txs[k] = tx;
            rssis[k] = rssi;
            truths[k] = truth;
            readings[k] = truth < sat ? truth : sat;
            h.add(T0 + (uint32_t)k * 600, rssi, readings[k], NONE, NONE, (int8_t)tx, 0);
            k++;
        }
        if (k < MIN_UP_SAMPLES)
        {
            continue;
        }
        const Margins m = margins(h, last(k), p0);
        const Recommendation r = recommend(m, p0);
        if (r.advice != Advice::Lower)
        {
            continue;
        }
        lowers++;
        lowersToMin += r.power == Gen2Config::MIN_TX_POWER_DBM;
        for (int i = 0; i < k; i++)
        {
            const int step = r.power - txs[i];
            TEST_ASSERT_TRUE(readings[i] + step >= FADE_MARGIN_DB + SNR_LIMIT_DB);
            TEST_ASSERT_TRUE(truths[i] + step >= SNR_LIMIT_DB);
            TEST_ASSERT_TRUE(rssis[i] + step >= SENSITIVITY_DBM + FADE_MARGIN_DB);
        }
        TEST_ASSERT_TRUE(recommend(margins(h, last(k), r.power), r.power).advice != Advice::Raise);
    }
    TEST_ASSERT_TRUE(lowers >= 2000);
    TEST_ASSERT_TRUE(lowersToMin > 0);
}

// Reviewer chain (round 3): floor -100 dBm, 4 normal UPs and one deep fade
// at 14 dBm; following every Lower hint with one new UP at the new power must
// never push the fade below the demodulation limit while it is in the window
static void test_chain_keeps_old_fade(void)
{
    const int noise = -100;
    History h;
    uint32_t t = T0;
    int tx = 14;
    struct S
    {
        int tx, truth;
    } win[HISTORY];
    int count = 0;
    auto add = [&](int rssi) {
        const int truth = rssi - noise;
        h.add(t, rssi, truth < SAT ? truth : SAT, NONE, NONE, (int8_t)tx, 0);
        win[count % HISTORY] = {tx, truth};
        count++;
        t += 600;
    };
    for (int i = 0; i < 4; i++)
    {
        add(tx - 84); // -70 dBm at 14 dBm: true SNR 30, reading 11
    }
    add(tx - 104); // the fade: -90 dBm, true SNR 10
    int steps = 0;
    for (int up = 0; up < 60; up++)
    {
        Recommendation r = recommend(margins(h, t - 600, tx), tx);
        if (r.advice == Advice::Lower)
        {
            tx = r.power;
            steps++;
        }
        const int inWindow = count < HISTORY ? count : HISTORY;
        for (int i = 0; i < inWindow; i++)
        {
            TEST_ASSERT_TRUE(win[i].truth + (tx - win[i].tx) >= SNR_LIMIT_DB);
        }
        add(tx - 84); // the next normal UP at the current power
    }
    TEST_ASSERT_TRUE(steps >= 2); // the hint still makes progress, one step per window turnover
    TEST_ASSERT_EQUAL(-9, tx);
}

static void test_losses_block_lower(void)
{
    History h = filled(6, -60, 14);
    h.add(last(7), -60, CLEAR, NONE, NONE, 14, 2);
    Margins m = margins(h, last(7), 14);
    TEST_ASSERT_EQUAL(2, (int)m.lost);
    TEST_ASSERT_EQUAL((int)Advice::None, (int)recommend(m, 14).advice);
    History w = filled(6, -122, 14);
    w.add(last(7), -122, CLEAR, NONE, NONE, 14, 3);
    TEST_ASSERT_EQUAL((int)Advice::Raise, (int)recommend(margins(w, last(7), 14), 14).advice);
    History r = filled(1, -60, 14);
    r.lost[0] = 5;
    for (int i = 1; i <= HISTORY; i++)
    {
        r.add(T0 + (uint32_t)i, -60, CLEAR, NONE, NONE, 14, 0);
    }
    TEST_ASSERT_EQUAL(0, (int)margins(r, T0 + HISTORY, 14).lost);
}

static void test_lost_between(void)
{
    TEST_ASSERT_EQUAL(0, (int)lostBetween(true, 5, 6));
    TEST_ASSERT_EQUAL(3, (int)lostBetween(true, 5, 9));
    TEST_ASSERT_EQUAL(0, (int)lostBetween(true, 9, 9));
    TEST_ASSERT_EQUAL(0, (int)lostBetween(false, 40, 0));
    TEST_ASSERT_EQUAL(2, (int)lostBetween(false, 40, 2));
    History h;
    h.add(T0, -60, SAT, NONE, NONE, 14, 1000);
    TEST_ASSERT_EQUAL(255, h.lost[0]);
}

static void test_window_long_interval(void)
{
    History h = filled(5, -80, 14, 12 * 3600);
    Margins m = margins(h, last(5, 12 * 3600), 14);
    TEST_ASSERT_EQUAL(5, m.upSamples);
    TEST_ASSERT_TRUE(m.upValid());
    h.add(last(5, 12 * 3600) + 24 * 3600, -80, SAT, NONE, NONE, 14, 1);
    TEST_ASSERT_EQUAL(6, h.count);
    History w = filled(20, -70, 14, 12 * 3600);
    TEST_ASSERT_EQUAL(15, margins(w, last(20, 12 * 3600), 14).upSamples);
    History s = filled(10, -110, 14, 600);
    s.add(last(10) + WINDOW_S + 1, -60, SAT, NONE, NONE, 14, 0);
    TEST_ASSERT_EQUAL(1, s.count);
    History r = filled(30, -70, 14, 600);
    TEST_ASSERT_EQUAL(HISTORY, margins(r, last(30), 14).upSamples);
}

static void test_clamp_and_none(void)
{
    History h;
    h.add(T0, -140, 127, NONE, NONE, 14, 0);
    TEST_ASSERT_EQUAL(-128, h.up[0]);
    TEST_ASSERT_EQUAL(126, h.upSnr[0]);
    TEST_ASSERT_EQUAL(127, NONE);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_too_few_samples);
    RUN_TEST(test_margins_worst_and_down);
    RUN_TEST(test_lower_to_limit);
    RUN_TEST(test_lower_keeps_fade_margin);
    RUN_TEST(test_raise_and_maximum);
    RUN_TEST(test_normalised_to_current_power);
    RUN_TEST(test_reviewer_noise_floor_scenarios);
    RUN_TEST(test_snr_bound_normalisation);
    RUN_TEST(test_downlink_snr_lower_bound);
    RUN_TEST(test_no_flip_flop_after_lower);
    RUN_TEST(test_brute_force_lower_physical);
    RUN_TEST(test_chain_keeps_old_fade);
    RUN_TEST(test_losses_block_lower);
    RUN_TEST(test_lost_between);
    RUN_TEST(test_window_long_interval);
    RUN_TEST(test_clamp_and_none);
    return UNITY_END();
}
