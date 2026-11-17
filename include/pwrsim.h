/*
 * pwrsim.h - interface of the pwrsim resource manager (QNX only).
 *
 * pwrsim owns the simulated plant and serves it at /dev/pwrsim:
 *   read()   returns the latest struct plant_sample
 *   devctl() DCMD_PWRSIM_FAULT injects a fault, DCMD_PWRSIM_CMD sends controller commands
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TOWERWATCH_PWRSIM_H
#define TOWERWATCH_PWRSIM_H

#include <stdint.h>
#include <devctl.h>
#include "plant.h"

#define PWRSIM_PATH "/dev/pwrsim"

struct pwrsim_fault {
    uint32_t kind;   /* enum fault_kind */
};

#define DCMD_PWRSIM_FAULT __DIOT(_DCMD_MISC, 0x51, struct pwrsim_fault)
#define DCMD_PWRSIM_CMD   __DIOT(_DCMD_MISC, 0x52, struct plant_cmd)

#endif
