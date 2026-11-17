/*
 * pwrctl.c - inject faults into pwrsim or print the plant state (QNX only).
 *
 *   pwrctl mains-off        cut the mains supply
 *   pwrctl gen-no-start     make the next generator start fail
 *   pwrctl status           print the latest plant sample
 *
 * QNX 8.0 only. SPDX-License-Identifier: Apache-2.0
 */
#ifndef __QNX__
#error "pwrctl talks to the pwrsim resource manager; build it with qcc for QNX 8.0"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <devctl.h>

#include "plant.h"
#include "pwrsim.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s status | mains-off | mains-on | gen-no-start | gen-ok | rect-trip | rect-reset | aircon-fail | aircon-ok\n", argv[0]);
        return 2;
    }
    int fd = open(PWRSIM_PATH, O_RDWR);
    if (fd == -1) { perror(PWRSIM_PATH " (is pwrsim running?)"); return 1; }

    if (strcmp(argv[1], "status") == 0) {
        struct plant_sample s;
        if (read(fd, &s, sizeof s) != (ssize_t)sizeof s) { perror("read"); return 1; }
        printf("mains %.0f V  gen %s  bus %.2f V  soc %.1f%%  battery %.1f A  load %.0f W (+%.0f W non-critical)  temp %.1f C  site %s\n",
               s.mains_v, gen_state_name(s.gen_state), s.bus_v, s.soc * 100.0f, s.batt_a,
               s.load_crit_w, s.load_noncrit_w, s.temp_c, s.site_up ? "up" : "DOWN");
        close(fd);
        return 0;
    }

    int fk = fault_from_name(argv[1]);
    if (fk < 0) { fprintf(stderr, "unknown fault '%s'\n", argv[1]); return 2; }
    struct pwrsim_fault f = { (uint32_t)fk };
    int rc = devctl(fd, DCMD_PWRSIM_FAULT, &f, sizeof f, NULL);
    if (rc != EOK) { fprintf(stderr, "devctl: %s\n", strerror(rc)); return 1; }
    printf("injected %s\n", argv[1]);
    close(fd);
    return 0;
}
