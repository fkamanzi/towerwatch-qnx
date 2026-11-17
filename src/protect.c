/*
 * protect.c - site power protection and control logic.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "protect.h"

#include <stdio.h>
#include <string.h>

#define MS(x) ((uint64_t)(x) * 1000000ull)

void protect_default_config(struct protect_config *cfg)
{
    cfg->mains_debounce = 5;
    cfg->mains_fail_v = 180.0f;
    cfg->mains_ok_v = 200.0f;
    cfg->mains_restore_ms = 2000;
    cfg->gen_start_delay_ms = 3000;
    cfg->gen_start_timeout_ms = 20000;
    cfg->shed_soc = 0.5f;
    cfg->shed_bus_v = 47.5f;
    cfg->restore_bus_v = 52.0f;
    cfg->restore_ms = 10000;
    cfg->batt_low_soc = 0.3f;
    cfg->temp_alarm_c = 40.0f;
    cfg->temp_hyst_c = 2.0f;
}

void protect_init(struct protect_state *ps, const struct protect_config *cfg)
{
    memset(ps, 0, sizeof *ps);
    ps->cfg = *cfg;
    ps->prev_gen_state = GEN_OFF;
}

static const char *const names[ALM_COUNT] = {
    "MAINS_FAIL", "ON_BATTERY", "GEN_FAIL", "RECT_FAIL",
    "BATT_LOW", "HIGH_TEMP", "LOAD_SHED", "SITE_DOWN"
};

const char *alarm_name(uint32_t bit)
{
    for (int i = 0; i < ALM_COUNT; i++)
        if (bit == (1u << i)) return names[i];
    return "?";
}

void alarm_list(uint32_t alarms, char *buf, int len)
{
    int used = 0;
    buf[0] = '\0';
    for (int i = 0; i < ALM_COUNT && used < len - 1; i++) {
        if (!(alarms & (1u << i))) continue;
        int w = snprintf(buf + used, (size_t)(len - used), "%s%s", used ? "," : "", names[i]);
        if (w < 0) break;
        used += w;
    }
    if (!used && len > 1) snprintf(buf, (size_t)len, "-");
}

static void emit(struct protect_event *ev, int n, int *k, uint64_t t, uint32_t alarm, int raised, const char *text)
{
    if (*k >= n) return;
    ev[*k].t_ns = t;
    ev[*k].alarm = alarm;
    ev[*k].raised = raised;
    snprintf(ev[*k].text, sizeof ev[*k].text, "%s", text);
    (*k)++;
}

static void set_alarm(uint32_t *a, uint32_t bit, int on)
{
    if (on) *a |= bit; else *a &= ~bit;
}

int protect_eval(struct protect_state *ps, const struct plant_sample *s, struct protect_event *ev, int n)
{
    const struct protect_config *c = &ps->cfg;
    uint32_t a = ps->alarms;
    int k = 0;
    char text[96];

    /* Mains failure, debounced over consecutive samples. */
    if (s->mains_v < c->mains_fail_v) {
        if (!(a & ALM_MAINS_FAIL) && ++ps->mains_low_count >= c->mains_debounce) {
            a |= ALM_MAINS_FAIL;
            ps->mains_fail_t = s->t_ns;
            ps->mains_ok_since = 0;
            ps->detections++;
            ps->last_detect_ns = 0;
            if (s->mains_off_t_ns && s->mains_off_t_ns <= s->t_ns) {
                ps->last_detect_ns = s->t_ns - s->mains_off_t_ns;
                if (ps->last_detect_ns > ps->worst_detect_ns) ps->worst_detect_ns = ps->last_detect_ns;
            }
        }
    } else {
        ps->mains_low_count = 0;
    }

    /* Mains restore, after it stays healthy for mains_restore_ms. */
    if ((a & ALM_MAINS_FAIL) && s->mains_v > c->mains_ok_v) {
        if (!ps->mains_ok_since) ps->mains_ok_since = s->t_ns;
        if (s->t_ns - ps->mains_ok_since >= MS(c->mains_restore_ms)) {
            a &= ~ALM_MAINS_FAIL;
            ps->mains_ok_since = 0;
        }
    } else if (s->mains_v <= c->mains_ok_v) {
        ps->mains_ok_since = 0;
    }

    int on_batt = s->site_up && s->batt_a > 1.0f;
    set_alarm(&a, ALM_ON_BATTERY, on_batt);

    /* Generator: start after the ride-through delay, stop when mains is back. */
    if ((a & ALM_MAINS_FAIL) && !ps->cmd.gen_run && s->t_ns - ps->mains_fail_t >= MS(c->gen_start_delay_ms)) {
        ps->cmd.gen_run = 1;
        ps->gen_cmd_t = s->t_ns;
        emit(ev, n, &k, s->t_ns, 0, 1, "generator start requested");
    }
    if (!(a & ALM_MAINS_FAIL) && ps->cmd.gen_run) {
        ps->cmd.gen_run = 0;
        emit(ev, n, &k, s->t_ns, 0, 1, "mains back: generator stop requested");
    }
    if (s->gen_state == GEN_CRANKING && ps->prev_gen_state != GEN_CRANKING)
        ps->gen_cmd_t = s->t_ns;   /* a new start attempt restarts the timeout */
    if (ps->cmd.gen_run) {
        if (s->gen_state == GEN_ONLINE) a &= ~ALM_GEN_FAIL;
        else if (s->gen_state == GEN_FAILED || s->t_ns - ps->gen_cmd_t > MS(c->gen_start_timeout_ms)) a |= ALM_GEN_FAIL;
    } else {
        a &= ~ALM_GEN_FAIL;
    }
    ps->prev_gen_state = s->gen_state;

    /* Non-critical load shedding keeps the radios on air longer. */
    if (!ps->cmd.shed_noncrit && on_batt &&
        (s->soc < c->shed_soc || s->bus_v < c->shed_bus_v || (a & ALM_GEN_FAIL))) {
        ps->cmd.shed_noncrit = 1;
        ps->ac_good_since = 0;
        snprintf(text, sizeof text, "non-critical load shed (SoC %.0f%%, bus %.1f V)", s->soc * 100.0f, s->bus_v);
        emit(ev, n, &k, s->t_ns, 0, 1, text);
    }
    if (ps->cmd.shed_noncrit) {
        if (!on_batt && s->bus_v > c->restore_bus_v) {
            if (!ps->ac_good_since) ps->ac_good_since = s->t_ns;
            if (s->t_ns - ps->ac_good_since >= MS(c->restore_ms)) {
                ps->cmd.shed_noncrit = 0;
                ps->ac_good_since = 0;
                emit(ev, n, &k, s->t_ns, 0, 1, "non-critical load restored");
            }
        } else {
            ps->ac_good_since = 0;
        }
    }
    set_alarm(&a, ALM_LOAD_SHED, ps->cmd.shed_noncrit);

    set_alarm(&a, ALM_RECT_FAIL, s->rect_ok < s->rect_total);

    if (s->soc < c->batt_low_soc) a |= ALM_BATT_LOW;
    else if (s->soc > c->batt_low_soc + 0.05f) a &= ~ALM_BATT_LOW;

    if (s->temp_c > c->temp_alarm_c) a |= ALM_HIGH_TEMP;
    else if (s->temp_c < c->temp_alarm_c - c->temp_hyst_c) a &= ~ALM_HIGH_TEMP;

    set_alarm(&a, ALM_SITE_DOWN, !s->site_up);

    /* One event per alarm change. */
    uint32_t changed = a ^ ps->alarms;
    for (int i = 0; i < ALM_COUNT; i++) {
        uint32_t bit = 1u << i;
        if (!(changed & bit)) continue;
        if (bit == ALM_MAINS_FAIL && (a & bit) && ps->last_detect_ns && s->t_ns - ps->mains_fail_t == 0)
            snprintf(text, sizeof text, "RAISE %s (%.3f ms after the fault)", names[i], (double)ps->last_detect_ns / 1e6);
        else
            snprintf(text, sizeof text, "%s %s", (a & bit) ? "RAISE" : "CLEAR", names[i]);
        emit(ev, n, &k, s->t_ns, bit, (a & bit) != 0, text);
    }
    ps->alarms = a;
    return k;
}
