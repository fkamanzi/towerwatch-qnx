/*
 * towerwatch.c - real-time power monitor and controller for a telecom site.
 *
 * A high-priority thread samples the site power plant at a fixed rate
 * (1 kHz by default), runs the protection logic on every sample and sends
 * commands back (generator start, non-critical load shedding). It measures
 * its own wake-up latency and execution time on every tick, so the timing
 * claims can be checked on the target.
 *
 * On QNX 8.0 the periodic wake-up is a kernel timer delivering a pulse to a
 * private channel (timer_create + SIGEV_PULSE_INIT + MsgReceivePulse), tick
 * execution time is measured with ClockCycles(), events go to slog2, and the
 * plant can live in a separate process, the pwrsim resource manager, read
 * over QNX message passing at /dev/pwrsim. The Linux build uses
 * clock_nanosleep for the same loop, so the logic can be tested anywhere.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __QNX__
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef __QNX__
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/slog2.h>
#include "pwrsim.h"
#endif

#include "plant.h"
#include "protect.h"

#define LAT_BUCKETS 2001   /* 1 us buckets up to 2 ms, last bucket is overflow */
#define MAX_STEPS 64
#define EV_RING 256

struct options {
    int rate_hz;
    double duration_s;
    const char *scenario;
    const char *json;
    const char *csv;
    int quiet;
    int demo;
    int prio;
    int simulate;
    int resmgr;            /* 1 = read the plant from /dev/pwrsim (QNX) */
    int observe;           /* 1 = raise alarms but send no commands (baseline) */
};

struct step { double t_s; int fault; int done; };

struct lat_stats {
    uint64_t n, sum_ns, max_ns, misses, skipped, work_max_ns;
    uint32_t hist[LAT_BUCKETS];
};

static struct options opt = { 1000, 0.0, NULL, NULL, NULL, 0, 0, 40, 0, 0, 0 };
static struct step steps[MAX_STEPS];
static int nsteps;
static volatile sig_atomic_t stop_flag;

static struct protect_state ps;
static struct lat_stats stats;
static uint64_t t0_ns, period_ns;
static uint64_t down_samples;     /* samples with the site down */

/* Shared with the reporter thread. */
static pthread_mutex_t pub_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct plant_sample pub_sample;
static uint32_t pub_alarms;
static struct protect_event ev_ring[EV_RING];
static unsigned ev_head, ev_tail;

static FILE *json_fp, *csv_fp;

#ifdef __QNX__
static int plant_fd = -1;
static slog2_buffer_t slog_buf;
static int slog_ok;
static uint64_t cycles_per_sec;
#endif

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t work_clock(void)
{
#ifdef __QNX__
    return ClockCycles();
#else
    return mono_ns();
#endif
}

static uint64_t work_to_ns(uint64_t d)
{
#ifdef __QNX__
    return cycles_per_sec ? (d / cycles_per_sec) * 1000000000ull + (d % cycles_per_sec) * 1000000000ull / cycles_per_sec : d;
#else
    return d;
#endif
}

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -r HZ        sampling rate (default 1000)\n"
        "  -d SECONDS   run time, 0 = until Ctrl-C (default 0)\n"
        "  -s FILE      scenario: lines of '<seconds> <fault>'\n"
        "  -j FILE      write one JSON status line per second\n"
        "  -c FILE      write a CSV timeline (every 100 ms)\n"
        "  -p PRIO      SCHED_FIFO priority of the sampling thread (default 40)\n"
        "  -m           demo plant: small battery so a run shows the whole story\n"
        "  -S           simulate: virtual time, no sleeping (tests and charts)\n"
        "  -R           read the plant from the pwrsim resource manager (QNX)\n"
        "  -n           observe only: alarms but no commands (baseline for comparison)\n"
        "  -q           quiet: summary only\n"
        "faults: mains-off mains-on gen-no-start gen-ok rect-trip rect-reset aircon-fail aircon-ok\n",
        argv0);
}

static int load_scenario(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[256], name[64];
    double t;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        if (sscanf(p, "%lf %63s", &t, name) != 2) continue;
        int fk = fault_from_name(name);
        if (fk < 0) { fprintf(stderr, "%s: unknown fault '%s'\n", path, name); fclose(f); return -1; }
        if (nsteps < MAX_STEPS) steps[nsteps++] = (struct step){ t, fk, 0 };
    }
    fclose(f);
    return 0;
}

static void log_event(const struct protect_event *e)
{
    double t = (double)(e->t_ns - t0_ns) / 1e9;
    if (!opt.quiet) printf("[%8.3f s] %s\n", t, e->text);
#ifdef __QNX__
    if (slog_ok) slog2f(slog_buf, 0, e->alarm && e->raised ? SLOG2_WARNING : SLOG2_INFO, "towerwatch %.3f %s", t, e->text);
#endif
}

static void inject_fault(int fk, uint64_t now)
{
#ifdef __QNX__
    if (opt.resmgr) {
        struct pwrsim_fault f = { (uint32_t)fk };
        int rc = devctl(plant_fd, DCMD_PWRSIM_FAULT, &f, sizeof f, NULL);
        if (rc != EOK) fprintf(stderr, "devctl fault: %s\n", strerror(rc));
        return;
    }
#endif
    plant_fault((enum fault_kind)fk, now);
}

static void read_plant(uint64_t now, struct plant_sample *s)
{
#ifdef __QNX__
    if (opt.resmgr) {
        if (pread(plant_fd, s, sizeof *s, 0) != (ssize_t)sizeof *s) memset(s, 0, sizeof *s);
        return;
    }
#endif
    static const struct plant_cmd none;
    plant_step(now, opt.observe ? &none : &ps.cmd);
    plant_snapshot(s);
}

static void send_cmd(const struct plant_cmd *c)
{
#ifdef __QNX__
    if (opt.resmgr) {
        struct plant_cmd copy = *c;
        int rc = devctl(plant_fd, DCMD_PWRSIM_CMD, &copy, sizeof copy, NULL);
        if (rc != EOK) fprintf(stderr, "devctl cmd: %s\n", strerror(rc));
    }
#else
    (void)c;
#endif
}

/* One sampling period: faults due, read the plant, protect, command, measure. */
static void tick(uint64_t now, uint64_t expected, int count_stats)
{
    uint64_t w0 = work_clock();
    double t_rel = (double)(now - t0_ns) / 1e9;
    for (int i = 0; i < nsteps; i++) {
        if (!steps[i].done && t_rel >= steps[i].t_s) {
            steps[i].done = 1;
            inject_fault(steps[i].fault, now);
            struct protect_event e = { now, 0, 1, "" };
            snprintf(e.text, sizeof e.text, "fault injected: %s", fault_name(steps[i].fault));
            pthread_mutex_lock(&pub_mtx);
            ev_ring[ev_head++ % EV_RING] = e;
            pthread_mutex_unlock(&pub_mtx);
        }
    }

    struct plant_sample s;
    struct plant_cmd before = ps.cmd;
    read_plant(now, &s);
    if (!s.site_up) down_samples++;
    struct protect_event ev[8];
    int k = protect_eval(&ps, &s, ev, 8);
    if (!opt.observe && memcmp(&before, &ps.cmd, sizeof before) != 0) send_cmd(&ps.cmd);

    pthread_mutex_lock(&pub_mtx);
    pub_sample = s;
    pub_alarms = ps.alarms;
    for (int i = 0; i < k; i++) ev_ring[ev_head++ % EV_RING] = ev[i];
    pthread_mutex_unlock(&pub_mtx);

    if (!count_stats) return;
    uint64_t work = work_to_ns(work_clock() - w0);
    uint64_t lat = now > expected ? now - expected : 0;
    stats.n++;
    stats.sum_ns += lat;
    if (lat > stats.max_ns) stats.max_ns = lat;
    if (work > stats.work_max_ns) stats.work_max_ns = work;
    if (lat + work > period_ns) stats.misses++;
    uint64_t us = lat / 1000;
    stats.hist[us < LAT_BUCKETS - 1 ? us : LAT_BUCKETS - 1]++;
}

static double percentile_us(const struct lat_stats *st, double p)
{
    if (!st->n) return 0;
    uint64_t want = (uint64_t)ceil(p * (double)st->n), acc = 0;
    for (int i = 0; i < LAT_BUCKETS; i++) {
        acc += st->hist[i];
        if (acc >= want) return (double)i;
    }
    return LAT_BUCKETS - 1;
}

static const char *source_of(const struct plant_sample *s)
{
    if (!s->site_up) return "DOWN";
    if (s->mains_v > 200.0f) return "GRID";
    if (s->gen_state == GEN_ONLINE) return "GEN";
    return "BATTERY";
}

static void drain_events(void)
{
    struct protect_event local[EV_RING];
    unsigned n = 0;
    pthread_mutex_lock(&pub_mtx);
    while (ev_tail != ev_head && n < EV_RING) local[n++] = ev_ring[ev_tail++ % EV_RING];
    pthread_mutex_unlock(&pub_mtx);
    for (unsigned i = 0; i < n; i++) log_event(&local[i]);
}

static void report(double t_rel)
{
    struct plant_sample s;
    uint32_t alarms;
    struct lat_stats st;
    pthread_mutex_lock(&pub_mtx);
    s = pub_sample;
    alarms = pub_alarms;
    pthread_mutex_unlock(&pub_mtx);
    st.n = stats.n; st.max_ns = stats.max_ns; st.misses = stats.misses;
    memcpy(st.hist, stats.hist, sizeof st.hist);

    char al[160];
    alarm_list(alarms, al, sizeof al);
    float load_kw = (s.load_crit_w + s.load_noncrit_w) / 1000.0f;
    if (!opt.quiet) {
        if (opt.simulate)
            printf("t=%6.1fs  src=%-7s bus=%5.1fV  soc=%3.0f%%  load=%4.2fkW  gen=%-8s temp=%4.1fC  alarms=%s\n",
                   t_rel, source_of(&s), s.bus_v, s.soc * 100.0f, load_kw, gen_state_name(s.gen_state), s.temp_c, al);
        else
            printf("t=%6.1fs  src=%-7s bus=%5.1fV  soc=%3.0f%%  load=%4.2fkW  gen=%-8s temp=%4.1fC  alarms=%s  | latency max %5.1fus p99 %4.0fus miss %" PRIu64 "\n",
                   t_rel, source_of(&s), s.bus_v, s.soc * 100.0f, load_kw, gen_state_name(s.gen_state), s.temp_c, al,
                   (double)st.max_ns / 1000.0, percentile_us(&st, 0.99), st.misses);
        fflush(stdout);
    }
    if (json_fp) {
        fprintf(json_fp,
            "{\"t\":%.1f,\"src\":\"%s\",\"bus_v\":%.2f,\"soc\":%.4f,\"load_w\":%.0f,\"gen\":\"%s\",\"temp_c\":%.2f,"
            "\"alarms\":\"%s\",\"fuel_l\":%.4f,\"lat_max_us\":%.1f,\"lat_p99_us\":%.0f,\"misses\":%" PRIu64 "}\n",
            t_rel, source_of(&s), s.bus_v, s.soc, (s.load_crit_w + s.load_noncrit_w), gen_state_name(s.gen_state),
            s.temp_c, al, s.fuel_l, (double)st.max_ns / 1000.0, percentile_us(&st, 0.99), st.misses);
        fflush(json_fp);
    }
}

static void csv_row(double t_rel)
{
    if (!csv_fp) return;
    struct plant_sample s;
    uint32_t alarms;
    pthread_mutex_lock(&pub_mtx);
    s = pub_sample;
    alarms = pub_alarms;
    pthread_mutex_unlock(&pub_mtx);
    fprintf(csv_fp, "%.1f,%s,%.2f,%.4f,%.0f,%.0f,%s,%.2f,%.4f,%u,%u\n", t_rel, source_of(&s), s.bus_v, s.soc,
            s.load_crit_w, s.load_noncrit_w, gen_state_name(s.gen_state), s.temp_c, s.fuel_l, alarms, s.site_up);
}

static void *reporter(void *arg)
{
    (void)arg;
    uint64_t next = t0_ns + 1000000000ull;
    uint64_t next_csv = t0_ns + 100000000ull;
    while (!stop_flag) {
        struct timespec ts = { 0, 20000000 };   /* 20 ms */
        nanosleep(&ts, NULL);
        drain_events();
        uint64_t now = mono_ns();
        while (now >= next_csv) { csv_row((double)(next_csv - t0_ns) / 1e9); next_csv += 100000000ull; }
        if (now >= next) { report((double)(next - t0_ns) / 1e9); next += 1000000000ull; }
    }
    drain_events();
    return NULL;
}

static int set_rt_priority(pthread_attr_t *attr, int prio)
{
    struct sched_param sp = { .sched_priority = prio };
    pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(attr, SCHED_FIFO);
    return pthread_attr_setschedparam(attr, &sp);
}

static void *sampler(void *arg)
{
    (void)arg;
    uint64_t expected = t0_ns + period_ns;
    uint64_t end = opt.duration_s > 0 ? t0_ns + (uint64_t)(opt.duration_s * 1e9) : UINT64_MAX;
#ifdef __QNX__
    /* Periodic kernel timer delivering a pulse to a private channel. */
    enum { PULSE_TICK = _PULSE_CODE_MINAVAIL };
#ifdef _NTO_CHF_PRIVATE
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);   /* no other process can connect */
#else
    int chid = ChannelCreate(0);
#endif
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (chid == -1 || coid == -1) { perror("ChannelCreate/ConnectAttach"); stop_flag = 1; return NULL; }
    struct sigevent sev;
    SIGEV_PULSE_INIT(&sev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_TICK, 0);
    timer_t tid;
    if (timer_create(CLOCK_MONOTONIC, &sev, &tid) == -1) { perror("timer_create"); stop_flag = 1; return NULL; }
    struct itimerspec its;
    its.it_value.tv_sec = (time_t)(expected / 1000000000ull);
    its.it_value.tv_nsec = (long)(expected % 1000000000ull);
    its.it_interval.tv_sec = (time_t)(period_ns / 1000000000ull);
    its.it_interval.tv_nsec = (long)(period_ns % 1000000000ull);
    if (timer_settime(tid, TIMER_ABSTIME, &its, NULL) == -1) { perror("timer_settime"); stop_flag = 1; return NULL; }
    while (!stop_flag) {
        struct _pulse pulse;
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) { if (errno == EINTR) continue; perror("MsgReceivePulse"); break; }
        if (pulse.code != PULSE_TICK) continue;
        uint64_t now = mono_ns();
        while (expected + period_ns <= now) {   /* a whole period was missed: count it, do not queue it */
            expected += period_ns;
            stats.misses++;
            stats.skipped++;
        }
        tick(now, expected, 1);
        expected += period_ns;
        if (now >= end) break;
    }
    timer_delete(tid);
    ConnectDetach(coid);
    ChannelDestroy(chid);
#else
    struct timespec next = { (time_t)(expected / 1000000000ull), (long)(expected % 1000000000ull) };
    while (!stop_flag) {
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        if (rc == EINTR) continue;
        uint64_t now = mono_ns();
        while (expected + period_ns <= now) {
            expected += period_ns;
            stats.misses++;
            stats.skipped++;
        }
        tick(now, expected, 1);
        expected += period_ns;
        next.tv_sec = (time_t)(expected / 1000000000ull);
        next.tv_nsec = (long)(expected % 1000000000ull);
        if (now >= end) break;
    }
#endif
    stop_flag = 1;
    return NULL;
}

static void summary(void)
{
    struct plant_sample s;
    pthread_mutex_lock(&pub_mtx);
    s = pub_sample;
    pthread_mutex_unlock(&pub_mtx);
    printf("\n--- towerwatch summary ---\n");
    if (opt.simulate) {
        printf("simulated %.1f s at %d Hz (virtual time)\n", (double)stats.n / opt.rate_hz, opt.rate_hz);
    } else {
        printf("ticks: %" PRIu64 " at %d Hz, deadline misses: %" PRIu64 " (%" PRIu64 " whole periods skipped)\n",
               stats.n, opt.rate_hz, stats.misses, stats.skipped);
        printf("wake-up latency: mean %.1f us, p99 %.0f us, p99.9 %.0f us, max %.1f us\n",
               stats.n ? (double)stats.sum_ns / (double)stats.n / 1000.0 : 0.0,
               percentile_us(&stats, 0.99), percentile_us(&stats, 0.999), (double)stats.max_ns / 1000.0);
        printf("tick work: max %.1f us\n", (double)stats.work_max_ns / 1000.0);
    }
    if (ps.detections)
        printf("mains failures detected: %u, worst detection %.3f ms after the fault\n",
               ps.detections, (double)ps.worst_detect_ns / 1e6);
    if (down_samples)
        printf("site downtime: %.1f s (battery disconnect)\n", (double)down_samples / opt.rate_hz);
    else
        printf("site stayed up the whole run\n");
    printf("energy: grid %.0f Wh, generator %.0f Wh, battery %.0f Wh, solar %.0f Wh; diesel %.3f L\n",
           s.e_grid_wh, s.e_gen_wh, s.e_batt_wh, s.e_pv_wh, s.fuel_l);
}

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "r:d:s:j:c:p:mSRnqh")) != -1) {
        switch (c) {
        case 'r': opt.rate_hz = atoi(optarg); break;
        case 'd': opt.duration_s = atof(optarg); break;
        case 's': opt.scenario = optarg; break;
        case 'j': opt.json = optarg; break;
        case 'c': opt.csv = optarg; break;
        case 'p': opt.prio = atoi(optarg); break;
        case 'm': opt.demo = 1; break;
        case 'S': opt.simulate = 1; break;
        case 'R': opt.resmgr = 1; break;
        case 'n': opt.observe = 1; break;
        case 'q': opt.quiet = 1; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (opt.rate_hz < 1 || opt.rate_hz > 20000) { fprintf(stderr, "rate must be 1..20000 Hz\n"); return 2; }
    if (opt.scenario && load_scenario(opt.scenario) != 0) return 2;
    if (opt.simulate && opt.duration_s <= 0) opt.duration_s = 120.0;
    period_ns = 1000000000ull / (uint64_t)opt.rate_hz;

    if (opt.json && !(json_fp = fopen(opt.json, "w"))) { perror(opt.json); return 1; }
    if (opt.csv) {
        if (!(csv_fp = fopen(opt.csv, "w"))) { perror(opt.csv); return 1; }
        fprintf(csv_fp, "t,src,bus_v,soc,load_crit_w,load_noncrit_w,gen,temp_c,fuel_l,alarms,site_up\n");
    }

    struct protect_config pc;
    protect_default_config(&pc);
    protect_init(&ps, &pc);

    struct plant_config cfg;
    plant_default_config(&cfg);
    if (opt.demo) {
        cfg.batt_ah = 1.0f;        /* tiny battery: minutes become seconds */
        cfg.temp_tau_s = 15.0f;
    }

#ifdef __QNX__
    cycles_per_sec = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    slog2_buffer_set_config_t scfg;
    memset(&scfg, 0, sizeof scfg);
    scfg.buffer_set_name = "towerwatch";
    scfg.num_buffers = 1;
    scfg.verbosity_level = SLOG2_INFO;
    scfg.buffer_config[0].buffer_name = "events";
    scfg.buffer_config[0].num_pages = 4;
    slog_ok = slog2_register(&scfg, &slog_buf, 0) == 0;
    if (opt.resmgr) {
        plant_fd = open(PWRSIM_PATH, O_RDWR);
        if (plant_fd == -1) { perror(PWRSIM_PATH " (start pwrsim first)"); return 1; }
    }
#else
    if (opt.resmgr) { fprintf(stderr, "-R needs QNX and the pwrsim resource manager\n"); return 2; }
#endif

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (opt.simulate) {
        /* Virtual time: same plant, same protection, no sleeping. */
        t0_ns = 1000000000ull;
        plant_init(&cfg, t0_ns);
        uint64_t ticks = (uint64_t)(opt.duration_s * opt.rate_hz);
        for (uint64_t i = 1; i <= ticks && !stop_flag; i++) {
            uint64_t now = t0_ns + i * period_ns;
            tick(now, now, 0);
            stats.n++;
            drain_events();
            if (i % (uint64_t)(opt.rate_hz / 10 ? opt.rate_hz / 10 : 1) == 0) csv_row((double)(now - t0_ns) / 1e9);
            if (i % (uint64_t)opt.rate_hz == 0) report((double)(now - t0_ns) / 1e9);
        }
        summary();
        if (json_fp) fclose(json_fp);
        if (csv_fp) fclose(csv_fp);
        return 0;
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0 && !opt.quiet)
        fprintf(stderr, "note: mlockall failed (%s); continuing without locked memory\n", strerror(errno));

    t0_ns = mono_ns();
    if (!opt.resmgr) plant_init(&cfg, t0_ns);

    pthread_t rt, rep;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int rc = set_rt_priority(&attr, opt.prio);
    if (rc == 0) rc = pthread_create(&rt, &attr, sampler, NULL);
    if (rc != 0) {
        fprintf(stderr, "note: SCHED_FIFO priority %d not available (%s); using the default policy\n", opt.prio, strerror(rc));
        pthread_attr_destroy(&attr);
        pthread_attr_init(&attr);
        rc = pthread_create(&rt, &attr, sampler, NULL);
        if (rc != 0) { fprintf(stderr, "pthread_create: %s\n", strerror(rc)); return 1; }
    }
    pthread_attr_destroy(&attr);
    if (!opt.quiet)
        printf("towerwatch: sampling at %d Hz, plant %s%s\n", opt.rate_hz,
               opt.resmgr ? "from " "/dev/pwrsim" : "in-process", opt.demo ? " (demo battery)" : "");
    pthread_create(&rep, NULL, reporter, NULL);
    pthread_join(rt, NULL);
    stop_flag = 1;
    pthread_join(rep, NULL);
    summary();
    if (json_fp) fclose(json_fp);
    if (csv_fp) fclose(csv_fp);
    return 0;
}
