/* pc_sense — read the host's power LED through an optocoupler.
 *
 * Independent of power_btn and USB HID: it shares no lock, task or GPIO with
 * them, and a failure here only leaves the state "unknown". Sampling runs
 * from a periodic esp_timer; readers get the latest classification.
 */
#pragma once

#include "esp_err.h"
#include "pc_power.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the sense GPIO and start sampling. With
 * CONFIG_RW_PC_SENSE_ENABLED=n, succeeds without touching any GPIO. On error
 * nothing keeps running and the state stays unknown; callers may ignore it. */
esp_err_t rw_pc_sense_init(void);

/** Latest classification; RW_PC_UNKNOWN when disabled or undecided. */
rw_pc_power_t rw_pc_sense_get(void);

#ifdef __cplusplus
}
#endif
