/* pc_power — classify the host's front-panel power LED into on/off/sleep.
 *
 * Pure logic, no ESP-IDF dependencies, so host tests drive it with synthetic
 * time. The pc_sense driver feeds it one sample every RW_PC_SAMPLE_MS.
 *
 * A raw level must hold for RW_PC_DEBOUNCE_MS to count as an edge. With no
 * edge for RW_PC_STEADY_MS the state is the steady level ("on"/"off").
 * RW_PC_BLINK_EDGES edges in a row, each less than RW_PC_STEADY_MS after the
 * previous one, mean the LED is blinking or breathing ("sleep"). Anything in
 * between keeps the previous answer, so a single shutdown edge or a brief
 * reset flicker never reports "sleep". Before the first decision the state
 * is "unknown".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RW_PC_SAMPLE_MS    10u
#define RW_PC_DEBOUNCE_MS  50u
#define RW_PC_STEADY_MS    5000u
#define RW_PC_BLINK_EDGES  4u

typedef enum {
    RW_PC_UNKNOWN = 0,
    RW_PC_OFF,
    RW_PC_ON,
    RW_PC_SLEEP,
} rw_pc_power_t;

typedef struct {
    bool          started;
    bool          level;          /* debounced: LED lit */
    bool          candidate;      /* latest raw level */
    uint64_t      candidate_ms;   /* when the raw level last changed */
    uint64_t      last_edge_ms;   /* last debounced edge (or first sample) */
    uint8_t       run;            /* edges in the current blink run */
    rw_pc_power_t state;
} rw_pc_classifier_t;

void rw_pc_classifier_init(rw_pc_classifier_t *c);

/** Feed one sample. @p lit is the LED state after the active-level mapping;
 * @p now_ms must not go backwards. Returns the current classification. */
rw_pc_power_t rw_pc_classifier_step(rw_pc_classifier_t *c, bool lit, uint64_t now_ms);

/** "on", "off", "sleep" or "unknown" — the protocol's `pc_power` values. */
const char *rw_pc_power_name(rw_pc_power_t state);

#ifdef __cplusplus
}
#endif
