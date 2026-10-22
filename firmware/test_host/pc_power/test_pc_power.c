/* Host tests for the power-LED classifier (main/pc_power.c).
 *
 * Expectations come from the classifier contract in pc_power.h and the
 * RW-POWER spec, not from the implementation. Time is synthetic: every
 * sample carries an explicit timestamp, so nothing here sleeps or reads a
 * clock. Each scenario runs at several time bases (0, just below the 32-bit
 * wrap, and near 2^40) because the driver's monotonic clock may start
 * anywhere.
 *
 * Debounce is judged from sample timestamps: a raw level "held for N ms"
 * means it was seen on a sample at t and still seen on a sample at t + N.
 */
#include "pc_power.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U     RW_PC_UNKNOWN
#define OFF   RW_PC_OFF
#define ON    RW_PC_ON
#define SLEEP RW_PC_SLEEP
#define LIT   true
#define DARK  false

#define STEADY RW_PC_STEADY_MS
#define DEB    RW_PC_DEBOUNCE_MS
#define BIT(s) (1u << (s))

/* The scenarios spell out four edges and 10 ms sampling, and assume a blink
 * half-period of 500 ms is well above debounce and well below steady. */
_Static_assert(RW_PC_BLINK_EDGES == 4, "scenarios spell out four edges");
_Static_assert(RW_PC_SAMPLE_MS == 10, "scenarios assume 10 ms sampling");
_Static_assert(DEB % 10 == 0 && DEB > 30 && DEB < 100, "debounce range assumed below");
_Static_assert(STEADY % 10 == 0 && STEADY > 2000, "steady range assumed below");

typedef struct {
    rw_pc_classifier_t c;
    const char *name;
    uint64_t base;
    uint64_t t; /* timestamp of the next 10 ms sample */
} sim_t;

static void start(sim_t *s, const char *name, uint64_t base)
{
    rw_pc_classifier_init(&s->c);
    s->name = name;
    s->base = base;
    s->t = base;
}

static void check(const sim_t *s, uint64_t now, rw_pc_power_t got, unsigned allowed,
                  int line)
{
    if ((unsigned)got < 4u && (allowed & BIT(got))) return;
    fprintf(stderr, "FAIL %s (base %" PRIu64 ", line %d): at +%" PRIu64
            " ms got %d (%s), allowed mask 0x%x\n",
            s->name, s->base, line, now - s->base, (int)got, rw_pc_power_name(got),
            allowed);
    exit(1);
}

/* Feed `lit` every 10 ms for dur_ms (samples at t, t+10, ..., t+dur-10) and
 * require every result to be in `allowed`. */
static void hold_(sim_t *s, bool lit, uint64_t dur_ms, unsigned allowed, int line)
{
    if (dur_ms == 0 || dur_ms % 10) {
        fprintf(stderr, "test bug: hold of %" PRIu64 " ms (line %d)\n", dur_ms, line);
        exit(2);
    }
    for (uint64_t end = s->t + dur_ms; s->t < end; s->t += 10)
        check(s, s->t, rw_pc_classifier_step(&s->c, lit, s->t), allowed, line);
}

/* One sample at base + off_ms (any spacing); later holds continue 10 ms on. */
static void at_(sim_t *s, uint64_t off_ms, bool lit, unsigned allowed, int line)
{
    uint64_t now = s->base + off_ms;
    check(s, now, rw_pc_classifier_step(&s->c, lit, now), allowed, line);
    s->t = now + 10;
}

#define HOLD(s, lit, dur, st)       hold_((s), (lit), (dur), BIT(st), __LINE__)
#define HOLD_ANY(s, lit, dur, mask) hold_((s), (lit), (dur), (mask), __LINE__)
#define AT(s, off, lit, st)         at_((s), (off), (lit), BIT(st), __LINE__)
#define AT_ANY(s, off, lit, mask)   at_((s), (off), (lit), (mask), __LINE__)

/* Steady lit from boot: UNKNOWN until exactly STEADY after the first sample.
 * A single shutdown edge keeps "on" (no sleep), and "off" arrives STEADY
 * after the raw change, not STEADY + DEB after debounce accepted it. */
static void boot_on_then_shutdown(uint64_t base)
{
    sim_t s;
    start(&s, "boot_on_then_shutdown", base);
    HOLD(&s, LIT, STEADY, U);   /* +0 .. +STEADY-10 */
    HOLD(&s, LIT, 3000, ON);    /* decided at exactly +STEADY */
    HOLD(&s, DARK, STEADY, ON); /* raw off at +STEADY+3000 */
    HOLD(&s, DARK, 1000, OFF);  /* off at raw change + STEADY */
}

/* Steady off from boot with irregular spacing (boundary at STEADY-1 vs
 * STEADY), then a single power-on edge keeps "off" until STEADY after it. */
static void boot_off_then_power_on(uint64_t base)
{
    sim_t s;
    start(&s, "boot_off_then_power_on", base);
    AT(&s, 0, DARK, U);
    AT(&s, STEADY - 1, DARK, U);
    AT(&s, STEADY, DARK, OFF);
    AT(&s, STEADY + 1000, LIT, OFF);  /* raw on */
    HOLD(&s, LIT, STEADY - 10, OFF);
    HOLD(&s, LIT, 1000, ON);          /* raw change + STEADY */
}

/* 1 Hz blink (500/500) from boot. The first sample is not an edge, so SLEEP
 * comes exactly when the 4th real edge is accepted. Blinking keeps SLEEP;
 * waking to steady lit gives ON STEADY after the last edge; a reboot-style
 * flicker after that is not sleep (the steady decision ended the run). */
static void blink_1hz_then_wake(uint64_t base)
{
    sim_t s;
    start(&s, "blink_1hz_then_wake", base);
    HOLD(&s, LIT, 1000, U);
    /* edges dated +1000, +1500, +2000, +2500 */
    HOLD(&s, DARK, 500, U);
    HOLD(&s, LIT, 500, U);
    HOLD(&s, DARK, 500, U);
    HOLD(&s, LIT, DEB, U);         /* 4th edge still pending */
    HOLD(&s, LIT, 500 - DEB, SLEEP);
    for (int i = 0; i < 10; ++i) {
        HOLD(&s, DARK, 500, SLEEP);
        HOLD(&s, LIT, 500, SLEEP);
    }
    /* stays lit; the last edge was the raw change to lit 500 ms ago */
    HOLD(&s, LIT, STEADY - 500, SLEEP);
    HOLD(&s, LIT, 500, ON);
    HOLD(&s, DARK, 300, ON);
    HOLD(&s, LIT, STEADY + 1000, ON);
}

/* Slow blink with half-period h just under STEADY, starting from ON: no steady
 * decision between edges, SLEEP at the 4th edge, then sleep -> off. With
 * h = STEADY - 10 each next raw change is still pending (not yet debounced)
 * when STEADY passes; it must not be mistaken for a steady level. */
static void slow_blink_then_off(uint64_t base, uint64_t h)
{
    sim_t s;
    start(&s, "slow_blink_then_off", base);
    HOLD(&s, LIT, STEADY, U);
    HOLD(&s, LIT, 1000, ON);
    HOLD(&s, DARK, h, ON);
    HOLD(&s, LIT, h, ON);
    HOLD(&s, DARK, h, ON);
    HOLD(&s, LIT, DEB, ON);
    HOLD(&s, LIT, h - DEB, SLEEP);
    for (int i = 0; i < 2; ++i) {
        HOLD(&s, DARK, h, SLEEP);
        HOLD(&s, LIT, h, SLEEP);
    }
    HOLD(&s, DARK, STEADY, SLEEP);
    HOLD(&s, DARK, 1000, OFF);
}

/* Three edges 500 ms apart from ON, then a 4th edge `gap` ms after the 3rd.
 * The 4th edge's raw change is sampled at exactly +gap and accepted DEB
 * later, with no samples in between. */
static void gap_boundary(uint64_t base, uint64_t gap)
{
    const uint64_t e3 = STEADY + 2000;
    sim_t s;
    start(&s, gap < STEADY ? "gap_just_under_steady" : "gap_exactly_steady", base);
    HOLD(&s, LIT, STEADY, U);
    HOLD(&s, LIT, 1000, ON);
    HOLD(&s, DARK, 500, ON);
    HOLD(&s, LIT, 500, ON);
    HOLD(&s, DARK, STEADY - 10, ON); /* 3rd edge dated e3; last sample e3+STEADY-10 */
    if (gap < STEADY) {
        AT(&s, e3 + gap, LIT, ON);
        AT(&s, e3 + gap + DEB, LIT, SLEEP); /* run of four, each gap < STEADY */
        HOLD(&s, LIT, 500, SLEEP);
    } else {
        /* A gap of exactly STEADY breaks the run. Whether "off" is reported
         * at +STEADY while the new raw level is still pending is not pinned
         * (see report); either way it is never SLEEP, and ON follows STEADY
         * after the 4th edge. */
        const unsigned on_or_off = BIT(ON) | BIT(OFF);
        AT_ANY(&s, e3 + gap, LIT, on_or_off);
        AT_ANY(&s, e3 + gap + DEB, LIT, on_or_off);
        HOLD_ANY(&s, LIT, STEADY - DEB - 10, on_or_off);
        HOLD(&s, LIT, 1000, ON);
    }
}

/* A reboot flicker (2 edges) and a later 2-edge group, separated by a
 * steady stretch, never make SLEEP. The control at the end shows the same
 * two groups separated by less than STEADY do form a run of four. */
static void flicker_groups(uint64_t base)
{
    sim_t s;
    start(&s, "flicker_groups", base);
    HOLD(&s, LIT, STEADY, U);
    HOLD(&s, LIT, 1000, ON);
    HOLD(&s, DARK, 2000, ON);          /* reboot: off 2 s, back on */
    HOLD(&s, LIT, STEADY + 1000, ON);
    HOLD(&s, DARK, 300, ON);           /* two more edges */
    HOLD(&s, LIT, STEADY + 1000, ON);
    /* control: 2 edges, STEADY - 1000 lit, 2 edges -> one run of four */
    HOLD(&s, DARK, 300, ON);
    HOLD(&s, LIT, STEADY - 1000, ON);
    HOLD(&s, DARK, 300, ON);
    HOLD(&s, LIT, DEB, ON);
    HOLD(&s, LIT, 500, SLEEP);
}

/* 10 ms sampling from ON. Off seen on samples t..t+40 (held 40 ms) adds no
 * edges, so four such glitch transitions do not make SLEEP; off seen on
 * samples t..t+50 (held 50 ms) is two edges per glitch, so two glitches
 * make SLEEP when the 4th edge (the second return to lit) is accepted. */
static void debounce_10ms(uint64_t base)
{
    sim_t s;
    start(&s, "debounce_10ms", base);
    HOLD(&s, LIT, STEADY, U);
    HOLD(&s, LIT, 1000, ON);
    for (int i = 0; i < 2; ++i) {
        HOLD(&s, DARK, DEB, ON);       /* seen for DEB - 10 */
        HOLD(&s, LIT, 500 - DEB, ON);
    }
    HOLD(&s, LIT, STEADY + 1000, ON);
    HOLD(&s, DARK, DEB + 10, ON);      /* seen for DEB: edges 1, 2 */
    HOLD(&s, LIT, 500 - DEB - 10, ON);
    HOLD(&s, DARK, DEB + 10, ON);      /* edge 3 */
    HOLD(&s, LIT, DEB, ON);            /* edge 4 pending */
    HOLD(&s, LIT, 500, SLEEP);
}

/* Irregular spacing before the first decision. Excursions held DEB-1 ms,
 * including a chattering one whose second half restarts the hold, neither
 * change the level nor reset the steady timer: ON at exactly STEADY. An
 * excursion held exactly DEB is two edges, and the steady timer runs from
 * the raw change of the second one (+1060), not its acceptance (+1110). */
static void debounce_irregular(uint64_t base)
{
    sim_t s;
    start(&s, "debounce_irregular_ignored", base);
    AT(&s, 0, LIT, U);
    AT(&s, 1000, DARK, U);
    AT(&s, 1000 + DEB - 1, DARK, U);
    AT(&s, 1000 + DEB, LIT, U);
    AT(&s, 2000, DARK, U);              /* chatter: off 30 ms, lit, off again */
    AT(&s, 2030, LIT, U);
    AT(&s, 2040, DARK, U);
    AT(&s, 2040 + DEB - 1, DARK, U);    /* 89 ms after the first off */
    AT(&s, 2040 + DEB, LIT, U);
    AT(&s, STEADY - 1, LIT, U);
    AT(&s, STEADY, LIT, ON);

    start(&s, "debounce_irregular_accepted", base);
    AT(&s, 0, LIT, U);
    AT(&s, 1000, DARK, U);
    AT(&s, 1000 + DEB, DARK, U);        /* edge dated +1000 */
    AT(&s, 1060, LIT, U);
    AT(&s, 1060 + DEB, LIT, U);         /* edge dated +1060 */
    AT(&s, 1060 + STEADY - 1, LIT, U);
    AT(&s, 1060 + STEADY, LIT, ON);
}

/* A short dark excursion straddling the first steady decision must not
 * make the decision "off": the debounced level is lit. Whether ON is
 * reported at +STEADY or once the excursion ends is not pinned. */
static void excursion_at_decision(uint64_t base)
{
    sim_t s;
    start(&s, "excursion_at_decision", base);
    HOLD(&s, LIT, STEADY - 20, U);
    HOLD(&s, DARK, 20, U);                       /* +STEADY-20, -10 */
    HOLD_ANY(&s, DARK, 20, BIT(U) | BIT(ON));    /* +STEADY, +10: held 30 ms */
    HOLD(&s, LIT, 1000, ON);
}

static void names(void)
{
    const struct { rw_pc_power_t st; const char *name; } cases[] = {
        {RW_PC_OFF, "off"}, {RW_PC_ON, "on"}, {RW_PC_SLEEP, "sleep"},
        {RW_PC_UNKNOWN, "unknown"}, {(rw_pc_power_t)4, "unknown"},
        {(rw_pc_power_t)1000, "unknown"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const char *got = rw_pc_power_name(cases[i].st);
        if (!got || strcmp(got, cases[i].name) != 0) {
            fprintf(stderr, "FAIL names: %d -> %s, want %s\n", (int)cases[i].st,
                    got ? got : "(null)", cases[i].name);
            exit(1);
        }
    }
}

int main(void)
{
    const uint64_t bases[] = {0, (UINT64_C(1) << 32) - 3000, (UINT64_C(1) << 40) - 7};
    names();
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; ++i) {
        const uint64_t b = bases[i];
        boot_on_then_shutdown(b);
        boot_off_then_power_on(b);
        blink_1hz_then_wake(b);
        slow_blink_then_off(b, STEADY - 100);
        slow_blink_then_off(b, STEADY - 10);
        gap_boundary(b, STEADY - 1);
        gap_boundary(b, STEADY);
        flicker_groups(b);
        debounce_10ms(b);
        debounce_irregular(b);
        excursion_at_decision(b);
    }
    puts("pc_power: all tests passed");
    return 0;
}
