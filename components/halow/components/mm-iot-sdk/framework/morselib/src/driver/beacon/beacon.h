/*
 * Copyright 2025 Morse Micro
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 */



#pragma once

#include <stdbool.h>
#include <stdint.h>

struct driver_data;


int morse_beacon_start(struct driver_data *driverd, uint16_t vif_id, uint32_t period_ms);


int morse_beacon_stop(struct driver_data *driverd);


int morse_beacon_work(struct driver_data *driverd);


void morse_beacon_irq_handle(struct driver_data *driverd, uint32_t status1_reg);


/* warthog mesh fork: whether a host beacon tick yields to the chip. It does when the chip
 * raised a beacon IRQ since the last tick, once it has raised two since the start (its one
 * kickoff IRQ, then a TBTT it scheduled itself, as a Linux MESH VIF's does). Freestanding. */
static inline bool morse_beacon_host_tick_yields(uint32_t chip_irqs, uint32_t *seen)
{
    const bool yields = chip_irqs >= 2u && chip_irqs != *seen;
    *seen = chip_irqs;
    return yields;
}
