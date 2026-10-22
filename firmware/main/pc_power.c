#include "pc_power.h"

#include <string.h>

void rw_pc_classifier_init(rw_pc_classifier_t *c)
{
    memset(c, 0, sizeof(*c));
    c->state = RW_PC_UNKNOWN;
}

rw_pc_power_t rw_pc_classifier_step(rw_pc_classifier_t *c, bool lit, uint64_t now_ms)
{
    if (!c->started) {
        /* The first sample sets the level; it is not an edge. */
        c->started      = true;
        c->level        = lit;
        c->candidate    = lit;
        c->candidate_ms = now_ms;
        c->last_edge_ms = now_ms;
        return c->state;
    }

    if (lit != c->candidate) {
        c->candidate    = lit;
        c->candidate_ms = now_ms;
    }
    if (c->candidate != c->level && now_ms - c->candidate_ms >= RW_PC_DEBOUNCE_MS) {
        /* Date the edge from when the raw level changed, not from when the
         * debounce accepted it, so blink gaps are measured faithfully. */
        uint64_t edge = c->candidate_ms;
        if (edge - c->last_edge_ms >= RW_PC_STEADY_MS) {
            c->run = 0; /* a steady stretch ends any earlier blink run */
        }
        c->level        = c->candidate;
        c->last_edge_ms = edge;
        if (c->run < UINT8_MAX) {
            ++c->run;
        }
    }

    /* Wait out a pending raw change before calling the level steady: it may
     * be the next edge of a slow blink, dated before the steady boundary. */
    if (c->candidate == c->level && now_ms - c->last_edge_ms >= RW_PC_STEADY_MS) {
        c->run   = 0;
        c->state = c->level ? RW_PC_ON : RW_PC_OFF;
    } else if (c->run >= RW_PC_BLINK_EDGES) {
        c->state = RW_PC_SLEEP;
    }
    return c->state;
}

const char *rw_pc_power_name(rw_pc_power_t state)
{
    switch (state) {
    case RW_PC_OFF:   return "off";
    case RW_PC_ON:    return "on";
    case RW_PC_SLEEP: return "sleep";
    default:          return "unknown";
    }
}
