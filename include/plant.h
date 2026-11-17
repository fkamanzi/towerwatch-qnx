/*
 * plant.h - simulated power plant of a telecom site (-48 V DC system).
 *
 * The model covers AC mains, a standby diesel generator with an automatic
 * transfer switch, rectifier modules, a VRLA battery string, solar input,
 * critical and non-critical DC loads and cabinet temperature. It is a
 * simulation for testing control software, not a replacement for site
 * protection relays.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TOWERWATCH_PLANT_H
#define TOWERWATCH_PLANT_H

#include <stdint.h>

enum gen_state {
    GEN_OFF = 0,
    GEN_CRANKING,
    GEN_WARMUP,
    GEN_ONLINE,
    GEN_COOLDOWN,
    GEN_FAILED
};

enum fault_kind {
    FAULT_NONE = 0,
    FAULT_MAINS_OFF,
    FAULT_MAINS_ON,
    FAULT_GEN_NO_START,
    FAULT_GEN_OK,
    FAULT_RECT_TRIP,
    FAULT_RECT_RESET,
    FAULT_AIRCON_FAIL,
    FAULT_AIRCON_OK,
    FAULT_COUNT
};

/* One snapshot of the plant, as a monitor would read it from the site controller. */
struct plant_sample {
    uint64_t t_ns;          /* monotonic time of the sample */
    uint64_t fault_t_ns;    /* time of the last injected fault, 0 if none */
    uint64_t mains_off_t_ns;/* time the mains supply was cut, 0 while mains is on */
    uint32_t seq;           /* sample counter */
    uint8_t  gen_state;     /* enum gen_state */
    uint8_t  rect_ok;       /* rectifier modules in service */
    uint8_t  rect_total;    /* rectifier modules installed */
    uint8_t  noncrit_on;    /* non-critical load connected (1) or shed (0) */
    uint8_t  site_up;       /* 0 once the battery low-voltage disconnect opened */
    uint8_t  last_fault;    /* enum fault_kind of the last injected fault */
    uint8_t  pad[2];
    float    mains_v;       /* AC mains voltage, V rms */
    float    gen_v;         /* generator AC voltage, V rms */
    float    bus_v;         /* DC bus voltage magnitude, V */
    float    batt_a;        /* battery current, A, positive = discharging */
    float    soc;           /* battery state of charge, 0..1 */
    float    load_crit_w;   /* critical load (radios, transmission), W */
    float    load_noncrit_w;/* non-critical load (cooling, lights), W */
    float    pv_w;          /* solar input, W */
    float    temp_c;        /* cabinet temperature, deg C */
    float    fuel_l;        /* generator fuel used, litres */
    float    e_grid_wh;     /* energy from mains, Wh */
    float    e_gen_wh;      /* energy from the generator, Wh */
    float    e_batt_wh;     /* energy discharged from the battery, Wh */
    float    e_pv_wh;       /* energy from solar, Wh */
};

/* Commands from the controller to the plant. */
struct plant_cmd {
    uint8_t gen_run;        /* 1 = generator run request to the transfer switch */
    uint8_t shed_noncrit;   /* 1 = open the non-critical load contactor */
    uint8_t pad[2];
};

struct plant_config {
    float batt_ah;          /* battery capacity, Ah at 48 V */
    int   rect_modules;     /* rectifier modules installed */
    float rect_module_w;    /* rating of one module, W */
    float load_crit_w;      /* critical load, W */
    float load_noncrit_w;   /* non-critical load, W */
    float pv_peak_w;        /* solar array output now, W (0 = none) */
    float gen_crank_s;      /* generator start time, s */
    float gen_warmup_s;     /* generator warm-up before transfer, s */
    float temp_tau_s;       /* cabinet thermal time constant, s */
};

void plant_default_config(struct plant_config *cfg);
void plant_init(const struct plant_config *cfg, uint64_t t_ns);
void plant_step(uint64_t t_ns, const struct plant_cmd *cmd);
void plant_fault(enum fault_kind f, uint64_t t_ns);
void plant_snapshot(struct plant_sample *out);

const char *gen_state_name(int s);
const char *fault_name(int f);
int fault_from_name(const char *s);   /* -1 if unknown */

/* Battery low-voltage disconnect: below this the battery is cut off and the site goes down. */
#define PLANT_BLVD_V 43.2f

#endif
