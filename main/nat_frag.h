#pragma once

#include <stdint.h>

/* IPv4 datagrams the input hook reassembled and handed back to lwIP, fragments or datagrams it dropped,
 * and whole TCP, UDP or ICMP packets it dropped as too short for the header NAPT reads. */
void warthog_nat_frag_counts(uint32_t *reass, uint32_t *drop, uint32_t *cut_short);
