/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mudp_classify.h"

int mudp_classify(uint32_t src, int arrived, const struct mudp_nif nif[MUDP_NIF_COUNT])
{
    for (int i = 0; i < MUDP_NIF_COUNT; i++) {
        if (nif[i].ip != 0 && src == nif[i].ip) {
            return MUDP_FROM_SELF;
        }
    }
    if (arrived < 0 || arrived >= MUDP_NIF_COUNT || nif[arrived].ip == 0) {
        return MUDP_FROM_UNKNOWN;
    }
    return arrived;
}
