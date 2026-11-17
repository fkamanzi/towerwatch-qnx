/*
 * protect.h - site power protection and control logic.
 *
 * Evaluated once per sample (1 kHz by default). Pure C with no OS calls, so
 * the same logic runs in the QNX real-time loop, in the Linux build and in
 * the unit tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TOWERWATCH_PROTECT_H
#define TOWERWATCH_PROTECT_H

#include <stdint.h>
#include "plant.h"

#define ALM_MAINS_FAIL  0x01u
#define ALM_ON_BATTERY  0x02u
#define ALM_GEN_FAIL    0x04u
#define ALM_RECT_FAIL   0x08u
#define ALM_BATT_LOW    0x10u
#define ALM_HIGH_TEMP   0x20u
#define ALM_LOAD_SHED   0x40u
#define ALM_SITE_DOWN   0x80u
#define ALM_COUNT       8

struct protect_config {
    int   mains_debounce;       /* consecutive low samples before mains failure is declared */
    float mains_fail_v;         /* below this, mains is failed, V */
    float mains_ok_v;           /* above this, mains is healthy, V */
    int   mains_restore_ms;     /* healthy time before mains is declared back */
    int   gen_start_delay_ms;   /* ride-through before the generator is started */
    int   gen_start_timeout_ms; /* generator must be online within this time */
    float shed_soc;             /* shed non-critical load below this state of charge on battery */
    float shed_bus_v;           /* ... or below this bus voltage on battery */
    float restore_bus_v;        /* restore load when an AC source holds the bus above this */
    int   restore_ms;           /* ... for this long */
    float batt_low_soc;         /* battery low alarm */
    float temp_alarm_c;         /* high temperature alarm */
    float temp_hyst_c;          /* alarm clears this far below the threshold */
};

struct protect_event {
    uint64_t t_ns;
    uint32_t alarm;             /* alarm bit, or 0 for a control action */
    int      raised;            /* 1 raised, 0 cleared */
    char     text[96];
};

struct protect_state {
    struct protect_config cfg;
    struct plant_cmd cmd;       /* commands to send to the plant */
    uint32_t alarms;            /* active alarm bits */
    int      mains_low_count;
    uint64_t mains_fail_t, mains_ok_since, gen_cmd_t, ac_good_since;
    uint8_t  prev_gen_state;
    uint64_t last_detect_ns;    /* last mains-failure detection latency (fault to alarm), ns */
    uint64_t worst_detect_ns;   /* worst detection latency seen */
    uint32_t detections;        /* mains failures detected */
};

void protect_default_config(struct protect_config *cfg);
void protect_init(struct protect_state *ps, const struct protect_config *cfg);

/* Evaluate one sample. Writes up to n events to ev and returns how many were written. */
int protect_eval(struct protect_state *ps, const struct plant_sample *s, struct protect_event *ev, int n);

const char *alarm_name(uint32_t bit);
/* Writes the names of the active alarms, comma separated, into buf. */
void alarm_list(uint32_t alarms, char *buf, int len);

#endif
