#pragma once
#include <stdint.h>
/** Run the on-target AES-CCM known-answer test and benchmark once. Results go
 *  to the g_warthog_ccmp_kat_* / g_warthog_aes_* counters (AT+CCMPKAT?). */
void umac_mesh_ccmp_kat_run(void);
