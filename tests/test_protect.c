/*
 * test_protect.c - unit tests of the protection logic and an end-to-end
 * scenario on the simulated plant (virtual time, no OS timing involved).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "plant.h"
#include "protect.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } } while (0)
#define MS(x) ((uint64_t)(x) * 1000000ull)

static struct plant_sample healthy(uint64_t t)
{
    struct plant_sample s;
    memset(&s, 0, sizeof s);
    s.t_ns = t; s.mains_v = 230; s.bus_v = 54; s.soc = 1; s.site_up = 1;
    s.rect_ok = 4; s.rect_total = 4; s.noncrit_on = 1; s.temp_c = 26; s.gen_state = GEN_OFF;
    return s;
}

static void test_mains_debounce(void)
{
    struct protect_state ps; struct protect_config c; struct protect_event ev[8];
    protect_default_config(&c); protect_init(&ps, &c);
    uint64_t t = MS(1000);
    struct plant_sample s = healthy(t);
    protect_eval(&ps, &s, ev, 8);
    /* Fault at t, then low samples every 1 ms. */
    uint64_t fault = t;
    for (int i = 1; i <= 4; i++) {
        s = healthy(t + MS(i)); s.mains_v = 0; s.mains_off_t_ns = fault;
        protect_eval(&ps, &s, ev, 8);
        CHECK(!(ps.alarms & ALM_MAINS_FAIL), "no mains alarm before 5 low samples");
    }
    s = healthy(t + MS(5)); s.mains_v = 0; s.mains_off_t_ns = fault;
    int k = protect_eval(&ps, &s, ev, 8);
    CHECK(ps.alarms & ALM_MAINS_FAIL, "mains alarm on the 5th low sample");
    CHECK(ps.last_detect_ns == MS(5), "detection latency is 5 ms at 1 kHz");
    CHECK(k >= 1 && strstr(ev[k - 1].text, "MAINS_FAIL"), "an event names MAINS_FAIL");

    /* A single healthy sample resets the debounce counter before declaration. */
    protect_init(&ps, &c);
    for (int i = 0; i < 4; i++) { s = healthy(t + MS(i)); s.mains_v = 0; protect_eval(&ps, &s, ev, 8); }
    s = healthy(t + MS(4)); protect_eval(&ps, &s, ev, 8);
    for (int i = 5; i < 9; i++) { s = healthy(t + MS(i)); s.mains_v = 0; protect_eval(&ps, &s, ev, 8); }
    CHECK(!(ps.alarms & ALM_MAINS_FAIL), "a short blip does not raise the alarm");
}

static void test_generator_and_shedding(void)
{
    struct protect_state ps; struct protect_config c; struct protect_event ev[8];
    protect_default_config(&c); protect_init(&ps, &c);
    uint64_t t = MS(1000);
    struct plant_sample s;
    for (int i = 0; i < 5; i++) { s = healthy(t + MS(i)); s.mains_v = 0; s.batt_a = 50; s.bus_v = 50.5f; protect_eval(&ps, &s, ev, 8); }
    CHECK(ps.alarms & ALM_MAINS_FAIL, "mains failed");
    CHECK(ps.alarms & ALM_ON_BATTERY, "on battery");
    CHECK(!ps.cmd.gen_run, "generator not started before the ride-through delay");
    s = healthy(t + MS(4) + MS(c.gen_start_delay_ms)); s.mains_v = 0; s.batt_a = 50; s.bus_v = 50.5f;
    protect_eval(&ps, &s, ev, 8);
    CHECK(ps.cmd.gen_run, "generator start requested after the delay");

    /* The generator fails to start: alarm, then shed the non-critical load. */
    s.t_ns += MS(100); s.gen_state = GEN_CRANKING; protect_eval(&ps, &s, ev, 8);
    s.t_ns += MS(100); s.gen_state = GEN_FAILED; protect_eval(&ps, &s, ev, 8);
    CHECK(ps.alarms & ALM_GEN_FAIL, "generator failure raised");
    CHECK(ps.cmd.shed_noncrit, "non-critical load shed when the generator failed");

    /* Generator online: the load comes back only after restore_ms of a healthy bus. */
    s.t_ns += MS(100); s.gen_state = GEN_ONLINE; s.batt_a = -10; s.bus_v = 53.5f; protect_eval(&ps, &s, ev, 8);
    CHECK(!(ps.alarms & ALM_GEN_FAIL), "generator failure cleared once online");
    CHECK(ps.cmd.shed_noncrit, "load still shed right after the generator came online");
    s.t_ns += MS(c.restore_ms) + MS(1); protect_eval(&ps, &s, ev, 8);
    CHECK(!ps.cmd.shed_noncrit, "load restored after the bus stayed healthy");

    /* Mains back for mains_restore_ms: generator stop requested. */
    s.mains_v = 230; protect_eval(&ps, &s, ev, 8);
    s.t_ns += MS(c.mains_restore_ms) + MS(1); protect_eval(&ps, &s, ev, 8);
    CHECK(!(ps.alarms & ALM_MAINS_FAIL), "mains alarm cleared");
    CHECK(!ps.cmd.gen_run, "generator stop requested");
}

static void test_temperature_hysteresis(void)
{
    struct protect_state ps; struct protect_config c; struct protect_event ev[8];
    protect_default_config(&c); protect_init(&ps, &c);
    struct plant_sample s = healthy(MS(1));
    s.temp_c = c.temp_alarm_c + 0.5f; protect_eval(&ps, &s, ev, 8);
    CHECK(ps.alarms & ALM_HIGH_TEMP, "high temperature raised");
    s.temp_c = c.temp_alarm_c - 1.0f; protect_eval(&ps, &s, ev, 8);
    CHECK(ps.alarms & ALM_HIGH_TEMP, "still raised inside the hysteresis band");
    s.temp_c = c.temp_alarm_c - c.temp_hyst_c - 0.1f; protect_eval(&ps, &s, ev, 8);
    CHECK(!(ps.alarms & ALM_HIGH_TEMP), "cleared below the hysteresis band");
}

/* Runs the plant and the protection together in virtual time. */
static struct plant_sample run_scenario(int with_control, int gen_repair_s, struct protect_state *out, int *went_down)
{
    struct plant_config pc; plant_default_config(&pc); pc.batt_ah = 1.0f; pc.temp_tau_s = 15.0f;
    struct protect_config c; protect_default_config(&c);
    struct protect_state ps; protect_init(&ps, &c);
    struct protect_event ev[8];
    uint64_t t0 = MS(1000);
    plant_init(&pc, t0);
    struct plant_sample s;
    *went_down = 0;
    for (int i = 1; i <= 120000; i++) {   /* 120 s at 1 kHz */
        uint64_t t = t0 + MS(i);
        if (i == 10000) { plant_fault(FAULT_MAINS_OFF, t); plant_fault(FAULT_GEN_NO_START, t); }
        if (gen_repair_s && i == gen_repair_s * 1000) plant_fault(FAULT_GEN_OK, t);
        if (i == 100000) plant_fault(FAULT_MAINS_ON, t);
        struct plant_cmd none = { 0, 0, { 0, 0 } };
        plant_step(t, with_control ? &ps.cmd : &none);
        plant_snapshot(&s);
        if (!s.site_up) *went_down = 1;
        protect_eval(&ps, &s, ev, 8);
    }
    if (out) *out = ps;
    return s;
}

static void test_end_to_end(void)
{
    struct protect_state ps;
    int down_with, down_without;
    struct plant_sample with = run_scenario(1, 60, &ps, &down_with);
    CHECK(!down_with && with.site_up, "with towerwatch the site stays up through the outage");
    CHECK(ps.detections == 1, "one mains failure detected");
    CHECK(ps.worst_detect_ns >= MS(4) && ps.worst_detect_ns <= MS(6), "mains failure detected 4 to 6 ms after the fault");
    struct plant_sample without = run_scenario(0, 60, NULL, &down_without);
    CHECK(down_without, "without control the same outage takes the site down");
    CHECK(without.site_up, "the site comes back once mains returns");
}

int main(void)
{
    test_mains_debounce();
    test_generator_and_shedding();
    test_temperature_hysteresis();
    test_end_to_end();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all protection tests passed\n");
    return 0;
}
