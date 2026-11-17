/*
 * pwrsim.c - QNX resource manager that owns the simulated site power plant.
 *
 * The plant is stepped by its own periodic timer pulse at a fixed rate and
 * served at /dev/pwrsim through the QNX resource manager framework:
 *   read()    returns the latest struct plant_sample
 *   devctl()  DCMD_PWRSIM_FAULT injects a fault, DCMD_PWRSIM_CMD takes the
 *             controller's commands (generator run, load shed)
 * towerwatch -R reads it over QNX message passing, the way a monitor would
 * read a real site controller through a driver.
 *
 * QNX 8.0 only. SPDX-License-Identifier: Apache-2.0
 */
#ifndef __QNX__
#error "pwrsim is a QNX resource manager; build it with qcc for QNX 8.0"
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "plant.h"
#include "pwrsim.h"

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t attr;

static pthread_mutex_t cmd_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct plant_cmd cmd;
static int rate_hz = 1000;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    int xtype = msg->i.xtype & _IO_XTYPE_MASK;     /* read() and pread() both welcome */
    if (xtype != _IO_XTYPE_NONE && xtype != _IO_XTYPE_OFFSET) return ENOSYS;

    struct plant_sample s;
    plant_snapshot(&s);
    size_t n = msg->i.nbytes < sizeof s ? msg->i.nbytes : sizeof s;
    MsgReply(ctp->rcvid, (long)n, &s, n);
    return _RESMGR_NOREPLY;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) return status;

    void *data = _DEVCTL_DATA(msg->i);
    switch (msg->i.dcmd) {
    case DCMD_PWRSIM_FAULT: {
        if (msg->i.nbytes < sizeof(struct pwrsim_fault)) return EINVAL;
        const struct pwrsim_fault *f = data;
        if (f->kind == FAULT_NONE || f->kind >= FAULT_COUNT) return EINVAL;
        plant_fault((enum fault_kind)f->kind, mono_ns());
        break;
    }
    case DCMD_PWRSIM_CMD: {
        if (msg->i.nbytes < sizeof(struct plant_cmd)) return EINVAL;
        pthread_mutex_lock(&cmd_mtx);
        memcpy(&cmd, data, sizeof cmd);
        pthread_mutex_unlock(&cmd_mtx);
        break;
    }
    default:
        return ENOSYS;
    }
    memset(&msg->o, 0, sizeof msg->o);
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
}

/* Steps the plant on a periodic timer pulse. */
static void *stepper(void *arg)
{
    (void)arg;
    enum { PULSE_TICK = _PULSE_CODE_MINAVAIL };
#ifdef _NTO_CHF_PRIVATE
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);   /* no other process can connect */
#else
    int chid = ChannelCreate(0);
#endif
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent sev;
    SIGEV_PULSE_INIT(&sev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_TICK, 0);
    timer_t tid;
    if (timer_create(CLOCK_MONOTONIC, &sev, &tid) == -1) { perror("timer_create"); exit(1); }
    uint64_t period = 1000000000ull / (uint64_t)rate_hz;
    struct itimerspec its;
    its.it_value.tv_sec = its.it_interval.tv_sec = (time_t)(period / 1000000000ull);
    its.it_value.tv_nsec = its.it_interval.tv_nsec = (long)(period % 1000000000ull);
    if (timer_settime(tid, 0, &its, NULL) == -1) { perror("timer_settime"); exit(1); }
    for (;;) {
        struct _pulse p;
        if (MsgReceivePulse(chid, &p, sizeof p, NULL) == -1) continue;
        if (p.code != PULSE_TICK) continue;
        struct plant_cmd c;
        pthread_mutex_lock(&cmd_mtx);
        c = cmd;
        pthread_mutex_unlock(&cmd_mtx);
        plant_step(mono_ns(), &c);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int c, demo = 0, prio = 41;
    while ((c = getopt(argc, argv, "r:mp:h")) != -1) {
        switch (c) {
        case 'r': rate_hz = atoi(optarg); break;
        case 'm': demo = 1; break;
        case 'p': prio = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-r HZ] [-m demo battery] [-p PRIO]\n", argv[0]);
            return c == 'h' ? 0 : 2;
        }
    }
    if (rate_hz < 1 || rate_hz > 20000) { fprintf(stderr, "rate must be 1..20000 Hz\n"); return 2; }

    struct plant_config cfg;
    plant_default_config(&cfg);
    if (demo) { cfg.batt_ah = 1.0f; cfg.temp_tau_s = 15.0f; }
    plant_init(&cfg, mono_ns());

    dispatch_t *dpp = dispatch_create();
    if (!dpp) { perror("dispatch_create"); return 1; }
    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;
    io_funcs.devctl = io_devctl;
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);
    if (resmgr_attach(dpp, &rattr, PWRSIM_PATH, _FTYPE_ANY, 0, &connect_funcs, &io_funcs, &attr) == -1) {
        perror("resmgr_attach " PWRSIM_PATH);
        return 1;
    }

    pthread_attr_t pa;
    pthread_attr_init(&pa);
    struct sched_param sp = { .sched_priority = prio };
    pthread_attr_setinheritsched(&pa, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&pa, SCHED_FIFO);
    pthread_attr_setschedparam(&pa, &sp);
    pthread_t th;
    if (pthread_create(&th, &pa, stepper, NULL) != 0) pthread_create(&th, NULL, stepper, NULL);

    printf("pwrsim: plant at %s, stepped at %d Hz%s\n", PWRSIM_PATH, rate_hz, demo ? " (demo battery)" : "");
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        if (!(ctp = dispatch_block(ctp))) { perror("dispatch_block"); return 1; }
        dispatch_handler(ctp);
    }
}
