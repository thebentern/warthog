/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial
 *
 * CCMP framing -- the 8-byte header, the AAD and the nonce. Freestanding
 * (libc only) so the host tests exercise the real code. See the .c for why
 * these bytes are worth pinning.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** CCMP header: PN0 PN1 rsvd KeyID|ExtIV PN2 PN3 PN4 PN5. */
#define UMAC_CCMP_HDR_LEN 8u
/** CCMP MIC length (CCMP-128). */
#define UMAC_CCMP_MIC_LEN 8u
/** Largest AAD: FC(2) + A1..A3(18) + SC(2) + A4(6) + QC(2) = 30. */
#define UMAC_CCMP_AAD_MAXLEN 30u

/** Does this 802.11 header carry a fourth address (ToDS and FromDS both set)? */
bool umac_ccmp_is_4addr(const uint8_t *hdr);
/** Is this a QoS data frame (and so carrying a QoS Control field)? */
bool umac_ccmp_is_qos(const uint8_t *hdr);
/** MAC header length implied by the frame control: 24, 26, 30 or 32. */
uint32_t umac_ccmp_hdr_len(const uint8_t *hdr);

/** Write the 8-byte CCMP header for @p pn and @p key_id (0-3). */
void umac_ccmp_write_header(uint8_t out[UMAC_CCMP_HDR_LEN], const uint8_t pn[6], uint8_t key_id);

/**
 * Read a CCMP header.
 *
 * @returns false if the Ext IV bit is clear, i.e. this is not CCMP framing --
 *          a frame from a WEP/TKIP peer, or a malformed one off the air.
 */
bool umac_ccmp_parse_header(const uint8_t in[UMAC_CCMP_HDR_LEN], uint8_t pn[6], uint8_t *key_id);

/**
 * Build the AAD for @p hdr into @p aad (size UMAC_CCMP_AAD_MAXLEN).
 *
 * Masks exactly the fields 802.11 says may change in flight (Retry, PwrMgt,
 * MoreData, sequence number, and most of the QoS control) so a retransmission
 * still authenticates, and forces Protected on so both ends agree.
 *
 * @returns the AAD length: 22, 24, 28 or 30.
 */
uint32_t umac_ccmp_build_aad(const uint8_t *hdr, uint8_t *aad);

/** Build the 13-byte CCM nonce: priority/management flag, A2, then the PN. */
void umac_ccmp_build_nonce(const uint8_t *hdr, const uint8_t pn[6], uint8_t nonce[13]);
