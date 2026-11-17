/*
 * plant.c - simulated -48 V telecom site power plant.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "plant.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static struct plant_config C;
static struct plant_sample S;
static uint64_t last_ns;
static double gen_timer_s;
static int mains_ok = 1, gen_will_start = 1, aircon_ok = 1;
static uint32_t rng = 0x2545F491u;

static float noise(float amp)
{
    rng = rng * 1664525u + 1013904223u;
    return amp * ((float)((rng >> 8) & 0xffff) / 32768.0f - 1.0f);
}

/* Open-circuit voltage of a 24-cell VRLA string against state of charge. */
static float ocv(float soc)
{
    return 46.8f + 4.8f * soc;
}

void plant_default_config(struct plant_config *cfg)
{
    cfg->batt_ah = 200.0f;
    cfg->rect_modules = 4;
    cfg->rect_module_w = 2000.0f;
    cfg->load_crit_w = 1800.0f;
    cfg->load_noncrit_w = 1200.0f;
    cfg->pv_peak_w = 0.0f;
    cfg->gen_crank_s = 8.0f;
    cfg->gen_warmup_s = 5.0f;
    cfg->temp_tau_s = 120.0f;
}

void plant_init(const struct plant_config *cfg, uint64_t t_ns)
{
    pthread_mutex_lock(&mtx);
    C = *cfg;
    memset(&S, 0, sizeof S);
    S.t_ns = t_ns;
    S.rect_total = (uint8_t)cfg->rect_modules;
    S.rect_ok = (uint8_t)cfg->rect_modules;
    S.noncrit_on = 1;
    S.site_up = 1;
    S.soc = 1.0f;
    S.bus_v = 54.0f;
    S.temp_c = 26.0f;
    S.gen_state = GEN_OFF;
    last_ns = t_ns;
    gen_timer_s = 0;
    mains_ok = 1;
    gen_will_start = 1;
    aircon_ok = 1;
    pthread_mutex_unlock(&mtx);
}

static void step_generator(double dt, int run)
{
    switch (S.gen_state) {
    case GEN_OFF:
        if (run) { S.gen_state = GEN_CRANKING; gen_timer_s = 0; }
        break;
    case GEN_CRANKING:
        gen_timer_s += dt;
        if (!run) {
            S.gen_state = GEN_OFF;
        } else if (gen_timer_s >= C.gen_crank_s) {
            S.gen_state = gen_will_start ? GEN_WARMUP : GEN_FAILED;
            gen_timer_s = 0;
        }
        break;
    case GEN_WARMUP:
        gen_timer_s += dt;
        if (!run) { S.gen_state = GEN_COOLDOWN; gen_timer_s = 0; }
        else if (gen_timer_s >= C.gen_warmup_s) S.gen_state = GEN_ONLINE;
        break;
    case GEN_ONLINE:
        if (!run) { S.gen_state = GEN_COOLDOWN; gen_timer_s = 0; }
        break;
    case GEN_COOLDOWN:
        gen_timer_s += dt;
        if (run) S.gen_state = GEN_ONLINE;
        else if (gen_timer_s >= 5.0) S.gen_state = GEN_OFF;
        break;
    case GEN_FAILED:
        if (!run) S.gen_state = GEN_OFF;
        break;
    }
}

void plant_step(uint64_t t_ns, const struct plant_cmd *cmd)
{
    pthread_mutex_lock(&mtx);
    double dt = (t_ns > last_ns) ? (double)(t_ns - last_ns) / 1e9 : 0.0;
    last_ns = t_ns;
    if (dt > 0.1) dt = 0.1;   /* clamp after a stall so the physics stays stable */

    step_generator(dt, cmd ? cmd->gen_run : 0);

    S.mains_v = mains_ok ? 230.0f + noise(2.0f) : fabsf(noise(0.5f));
    if (S.gen_state == GEN_ONLINE || S.gen_state == GEN_COOLDOWN) S.gen_v = 231.0f + noise(3.0f);
    else if (S.gen_state == GEN_WARMUP) S.gen_v = 225.0f + noise(5.0f);
    else S.gen_v = 0.0f;

    int ac_mains = mains_ok;
    int ac_gen = !ac_mains && S.gen_state == GEN_ONLINE;   /* transfer switch prefers mains */
    int ac = ac_mains || ac_gen;

    S.noncrit_on = !(cmd && cmd->shed_noncrit);
    float crit = S.site_up ? C.load_crit_w * (1.0f + noise(0.02f)) : 0.0f;
    float nonc = (S.site_up && S.noncrit_on) ? C.load_noncrit_w * (aircon_ok ? 1.0f : 0.15f) : 0.0f;
    S.load_crit_w = crit;
    S.load_noncrit_w = nonc;
    float load = crit + nonc;

    float pv = C.pv_peak_w > 0 ? C.pv_peak_w * (1.0f + noise(0.05f)) : 0.0f;
    S.pv_w = pv;
    float rect_cap = ac ? (float)S.rect_ok * C.rect_module_w : 0.0f;
    float supply = rect_cap + pv;

    float bus, batt_w, ac_w = 0.0f;   /* batt_w > 0: discharging */
    if (supply >= load) {
        float max_charge = 0.1f * C.batt_ah * 54.0f;   /* 0.1 C charge limit */
        float charge = S.soc < 0.999f ? fminf(supply - load, max_charge) : 0.0f;
        batt_w = -charge;
        bus = (S.soc < 0.98f ? 53.2f + 0.8f * S.soc : 54.0f) + noise(0.03f);
        ac_w = fmaxf(0.0f, load + charge - pv);
    } else {
        float deficit = load - supply;
        float v = ocv(S.soc);
        bus = v - (deficit / v) * 0.02f + noise(0.02f);   /* 20 mOhm string resistance */
        batt_w = deficit;
        ac_w = rect_cap;
    }
    if (S.site_up && (bus < PLANT_BLVD_V || (S.soc <= 0.0f && batt_w > 0))) {
        S.site_up = 0;   /* battery low-voltage disconnect opened: the site is down */
    } else if (!S.site_up && ac && S.rect_ok > 0) {
        S.site_up = 1;   /* an AC source is back: the disconnect recloses */
    }
    if (!S.site_up) { batt_w = 0.0f; bus = ac ? 54.0f : 0.0f; }

    S.soc -= (float)(batt_w * dt / 3600.0 / (C.batt_ah * 48.0));
    if (S.soc < 0.0f) S.soc = 0.0f;
    if (S.soc > 1.0f) S.soc = 1.0f;

    S.bus_v = bus;
    S.batt_a = bus > 1.0f ? batt_w / bus : 0.0f;

    if (ac_mains) S.e_grid_wh += (float)(ac_w * dt / 3600.0);
    if (ac_gen) S.e_gen_wh += (float)(ac_w * dt / 3600.0);
    if (batt_w > 0) S.e_batt_wh += (float)(batt_w * dt / 3600.0);
    S.e_pv_wh += (float)(pv * dt / 3600.0);

    /* Diesel: about 0.8 L/h idle plus 0.27 L per kWh delivered. */
    if (S.gen_state == GEN_ONLINE && ac_gen) S.fuel_l += (float)((0.8 + 0.27 * ac_w / 1000.0) * dt / 3600.0);
    else if (S.gen_state == GEN_WARMUP || S.gen_state == GEN_COOLDOWN || S.gen_state == GEN_ONLINE)
        S.fuel_l += (float)(0.8 * dt / 3600.0);

    /* Cabinet temperature: cooling is part of the non-critical load. */
    float target = (S.noncrit_on && aircon_ok && S.site_up) ? 26.0f : 26.0f + 0.009f * crit;
    S.temp_c += (float)((target - S.temp_c) * dt / (C.temp_tau_s > 1.0f ? C.temp_tau_s : 1.0f));

    S.t_ns = t_ns;
    S.seq++;
    pthread_mutex_unlock(&mtx);
}

void plant_fault(enum fault_kind f, uint64_t t_ns)
{
    pthread_mutex_lock(&mtx);
    switch (f) {
    case FAULT_MAINS_OFF:    if (mains_ok) S.mains_off_t_ns = t_ns; mains_ok = 0; break;
    case FAULT_MAINS_ON:     mains_ok = 1; S.mains_off_t_ns = 0; break;
    case FAULT_GEN_NO_START: gen_will_start = 0; break;
    case FAULT_GEN_OK:
        gen_will_start = 1;
        if (S.gen_state == GEN_FAILED) S.gen_state = GEN_OFF;   /* repaired: a held run request cranks again */
        break;
    case FAULT_RECT_TRIP:    if (S.rect_ok > 0) S.rect_ok--; break;
    case FAULT_RECT_RESET:   S.rect_ok = S.rect_total; break;
    case FAULT_AIRCON_FAIL:  aircon_ok = 0; break;
    case FAULT_AIRCON_OK:    aircon_ok = 1; break;
    default: break;
    }
    if (f > FAULT_NONE && f < FAULT_COUNT) {
        S.fault_t_ns = t_ns;
        S.last_fault = (uint8_t)f;
    }
    pthread_mutex_unlock(&mtx);
}

void plant_snapshot(struct plant_sample *out)
{
    pthread_mutex_lock(&mtx);
    *out = S;
    pthread_mutex_unlock(&mtx);
}

static const char *const gen_names[] = { "off", "cranking", "warmup", "online", "cooldown", "failed" };
static const char *const fault_names[] = {
    "none", "mains-off", "mains-on", "gen-no-start", "gen-ok",
    "rect-trip", "rect-reset", "aircon-fail", "aircon-ok"
};

const char *gen_state_name(int s)
{
    return (s >= 0 && s <= GEN_FAILED) ? gen_names[s] : "?";
}

const char *fault_name(int f)
{
    return (f >= 0 && f < FAULT_COUNT) ? fault_names[f] : "?";
}

int fault_from_name(const char *s)
{
    for (int i = 1; i < FAULT_COUNT; i++)
        if (strcmp(s, fault_names[i]) == 0) return i;
    return -1;
}
