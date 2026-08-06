/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host unit test for the ported dot11ah s1g_ieee80211 frequency math. */
#include <stdio.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

int failures = 0;
static void expect_eq(const char *what, int got, int want) {
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); failures++; }
    else             { printf("ok   %s = %d\n", what, got); }
}

int main(void) {
    /* US S1G: 902 MHz base, 500 kHz channel spacing (freq % 500 == 0). */
    expect_eq("US 902.5MHz -> ch1", __ieee80211_freq_khz_to_channel(902500), 1);
    expect_eq("US 903.0MHz -> ch2", __ieee80211_freq_khz_to_channel(903000), 2);
    expect_eq("US 909.0MHz -> ch14", __ieee80211_freq_khz_to_channel(909000), 14);
    /* EU S1G: 863 MHz base path (freq <= 902000). */
    expect_eq("EU 863.5MHz -> ch1", __ieee80211_freq_khz_to_channel(863500), 1);
    /* 2.4 GHz fallback (freq in MHz path). */
    expect_eq("2412 MHz -> ch1", __ieee80211_freq_khz_to_channel(2412000), 1);
    expect_eq("2484 MHz -> ch14", __ieee80211_freq_khz_to_channel(2484000), 14);

    printf(failures ? "\n%d FAILURES\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
