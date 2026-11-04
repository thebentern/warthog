/*
 * Copyright 2022 Morse Micro
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#ifndef MORSE_H
#define MORSE_H

#include <assert.h>
#include "utils/includes.h"
#include "utils/common.h"
#include "ap/ap_config.h"
#include "ap/hostapd.h"
#include <sys/stat.h>
struct wpa_supplicant;

#define MORSE_S1G_RETURN_ERROR (-1)
#define MORSE_INVALID_CHANNEL (-2)
#define MORSE_SUCCESS (0)
/** The maximum number of country codes that can be assigned to an S1G class */
#define COUNTRY_CODE_MAX (2)
#define COUNTRY_CODE_LEN (2)
#define S1G_CHAN_ENABLED_FLAG(ch) (1LLU << (ch))
#define NUMBER_OF_BITS(x) (sizeof(x) * 8)

#define MIN_S1G_FREQ_KHZ	750000
#define MAX_S1G_FREQ_KHZ	950000

#define MORSE_JP_HT20_NON_OVERLAP_CHAN_OFFSET 12
#define MORSE_JP_HT20_NON_OVERLAP_CHAN_START 50
#define MORSE_JP_HT20_NON_OVERLAP_CHAN_END 60
#define MORSE_JP_S1G_NON_OVERLAP_CHAN 21

#define S1G_OP_CLASS_IE_LEN 3 /* eid + ie len + op class */
extern unsigned int S1G_OP_CLASSES_LEN;

/* Define Maximum interfaces supported for MBSSID IE */
#define MBSSID_MAX_INTERFACES 2

#define MORSE_OUI	0x0CBF74

#define RAW_PRIORITY_GROUP_MAX  (7)
#define RAW_GROUP_SIZE          (256)
#define RAW_AID_SPILL_RANGE     (512)
#define BITS_PER_WORD           (32)

#define IEEE80211_CHAN_1MHZ (1)
#define IEEE80211_CHAN_2MHZ (2)
#define IEEE80211_CHAN_4MHZ (4)
#define IEEE80211_CHAN_8MHZ (8)
#define IEEE80211_CHAN_16MHZ (16)

#define CHANNELIZATION_SCHEME_NONE 0
/** Channelization per IEEE Std 802.11-2020 */
#define CHANNELIZATION_SCHEME_IEEE80211_2020 1
/** Channelization per IEEE Std 802.11-2024 */
#define CHANNELIZATION_SCHEME_IEEE80211_2024 2
/** Channelization per IEEE Std 802.11-REVmf */
#define CHANNELIZATION_SCHEME_IEEE80211_REVMF 3
#define CHANNELIZATION_SCHEME_DEFAULT CHANNELIZATION_SCHEME_IEEE80211_REVMF

enum morse_vendor_events {
	MORSE_VENDOR_EVENT_BCN_VENDOR_IE_FOUND = 0, /* To be deprecated in a future version */
	MORSE_VENDOR_EVENT_OCS_DONE = 1,
	MORSE_VENDOR_EVENT_MGMT_VENDOR_IE_FOUND = 2,
	MORSE_VENDOR_EVENT_MESH_PEER_ADDR = 3
};

enum morse_vendor_attributes {
	MORSE_VENDOR_ATTR_DATA = 0,
	/* Bitmask of type @ref enum morse_vendor_ie_mgmt_type_flags */
	MORSE_VENDOR_ATTR_MGMT_FRAME_TYPE = 1,
};


struct morse_twt {
	u8 enable;
	u8 flow_id;
	u8 setup_command;
	u32 wake_duration_us;
	u64 wake_interval_us;
	u64 target_wake_time;
};

enum s1g_op_class_type {
	OP_CLASS_INVALID = -1,
	OP_CLASS_S1G_LOCAL = 1,
	OP_CLASS_S1G_GLOBAL = 0,
};

/* Reconstructed from morse.c's static initialisers
 * (us1/us2/eu6/...) and access patterns in morse.c, rrm.c, hostapd.c,
 * acs.c, wpa_supplicant.c. The embedded fork shipped the regulatory data
 * but stripped this declaration — mesh code needs it visible so
 * wpa_supplicant.c can dereference the `const struct ah_class *` returned
 * by morse_s1g_ch_to_op_class. */
struct ah_class {
	int s1g_freq_start;                       /* kHz, e.g. 902000 */
	u8 s1g_op_class;
	u8 s1g_op_class_idx;
	u8 global_op_class;
	int s1g_width;                            /* MHz: 1, 2, 4, 8, 16 */
	const char *cc_list[COUNTRY_CODE_MAX];    /* country codes mapped to this class */
	u64 chans;                                /* S1G_CHAN_ENABLED_FLAG bitmask, ch 1..51 */
};

/* This table is also in the Morse Micro driver */
enum morse_dot11ah_region {
	MORSE_AU,
	MORSE_CA,
	MORSE_EU,
	MORSE_GB,
	MORSE_IN,
	MORSE_JP,
	MORSE_KR,
	MORSE_NZ,
	MORSE_SG,
	MORSE_US,
	REGION_UNSET = 0xFF,
};

/* Used to define buffer size when running a morse_cli command */
#define MORSE_CTRL_COMMAND_LENGTH (256)

/**
 * Return the pointer to the start of a container when a pointer within
 * the container is known
 */
#ifndef container_of
#define container_of(ptr, type, member) ({\
	const typeof(((type *)0)->member)*__mptr = (const typeof(((type *)0)->member) *)(ptr); \
	(type *)((char *)__mptr - offsetof(type, member)); })
#endif

/* RAW limits */
#define MORSE_RAW_MAX_3BIT_SLOTS		(0b111)
#define MORSE_RAW_MIN_SLOT_DUR_US		(500)
#define MORSE_RAW_MAX_SLOT_DUR_US		(MORSE_RAW_MIN_SLOT_DUR_US + (200 * (1 << 11) - 1))
#define MORSE_RAW_MIN_RAW_DUR_US		MORSE_RAW_MIN_SLOT_DUR_US
#define MORSE_RAW_MAX_RAW_DUR_US		(MORSE_RAW_MAX_SLOT_DUR_US * \
							MORSE_RAW_MAX_3BIT_SLOTS)
#define MORSE_RAW_MAX_START_TIME_US		(UINT8_MAX * 2 * 1024)
#define MORSE_RAW_MAX_SLOTS			(63)
#define MORSE_RAW_MAX_PRIORITY			(7)
#define MORSE_RAW_MAX_BEACON_SPREAD		(UINT16_MAX)
#define MORSE_RAW_MAX_NOM_STA_PER_BEACON	(UINT16_MAX)
#define MORSE_RAW_DEFAULT_START_AID		(1)
#define MORSE_RAW_AID_PRIO_SHIFT		(8)
#define MORSE_RAW_AID_DEVICE_MASK		(0xFF)
#define MORSE_MAX_NUM_RAWS_USER_PRIO		(8)	/* Limited by QoS User Priority */
#define MORSE_RAW_ID_HOSTAPD_PRIO_OFFSET	(0x4000)
/* This is an existing limitation which can be removed with native s1g support. */
#define MAX_AID					(2007)


int morse_s1g_validate_csa_params(struct hostapd_iface *iface, struct csa_settings *settings);


/* Return the configured 1 or 2MHz primary channel */
int morse_s1g_get_primary_channel(struct hostapd_config *conf, int bw);


/**
 * morse_is_multi_channelization_country() - Check if country has multiple channelization
 *
 * @alpha: Country code
 *
 * Returns: True if alpha matches a country with multiple channelization.
 */
bool morse_is_multi_channelization_country(const char *alpha);

/**
 * morse_dot11_2020_channelization_is_in_use() - Check if 802.11-2020 channelization is in use
 *
 * Return: true if IEEE802.11-2020 channelization is in use otherwise false.
 */
bool morse_dot11_2020_channelization_is_in_use(void);

/**
 * morse_get_channelization_scheme_in_use() - Get channelization scheme in use
 *
 * Returns: channelization scheme being used.
 */
u32 morse_get_channelization_scheme_in_use(void);

/**
 * morse_set_channelization_scheme() - Set the channelization scheme.
 *
 * @country: Country code
 * @channelization_scheme: Channelization scheme
 *
 * Used to select an alternative set of channels for countries that have more than one scheme.
 */
void morse_set_channelization_scheme(char *country, u32 channelization_scheme);

/**
 * morse_ap_configure_channelization() - Derive channelization and configure it
 *
 * @country: Country code
 * @op_class: Global operating class
 *
 * Returns: 0 on success, else -1
 */
int morse_ap_configure_channelization(char *country, u8 op_class);

/**
 * morse_sta_configure_channelization() - Derive channelization and configure it
 *
 * @ifname: wpa_supplicant structure for a network interface
 * @country: Country code
 *
 * Returns: 0 on success, else -1
 */
int morse_sta_configure_channelization(struct wpa_supplicant *wpa_s, char *country);

/* Defined in driver/driver/h */
enum wnm_oper;

/**
 * morse_s1g_chan_to_ht20_prim_chan() - Convert S1G Primary channel to HT20 channel.
 *
 * @s1g_op_channel: S1G Operating Channel
 * @s1g_prim_1MHz_channel: S1G 1MHz primary channel
 * @cc: Country code
 *
 * Returns: HT20 Channel if mapping is successful, 0 otherwise
 */
int morse_s1g_chan_to_ht20_prim_chan(int s1g_op_channel, int s1g_prim_1MHz_channel, char *cc);

/**
 * morse_s1g_get_start_freq_for_country() - Get starting frequency for given country.
 *
 * @cc: Country code
 * @freq: S1G Frequency
 * @bw: Bandwidth
 *
 * Returns: S1G Starting frequency for given country.
 */
int morse_s1g_get_start_freq_for_country(char *cc, int freq, int bw);

/**
 * morse_calculate_primary_s1g_channel_au() - Derive primary S1G channel from given inputs
 *
 * @op_bw_mhz: Operating channel bandwidth in MHz
 * @pr_bw_mhz: Primary channel bandwidth in MHz
 * @s1g_op_chan: S1G channel for operating center frequency
 * @pr_1mhz_chan_idx: 1MHz channel index of primary channel
 *
 * Returns: derived channel on success, else an error code.
 */
int morse_calculate_primary_s1g_channel_au(int op_bw_mhz, int pr_bw_mhz, int s1g_op_chan,
								int pr_1mhz_chan_idx);

/**
 * morse_ht_chan_offset_au () - For "AU" get offset value to derive HT channel
 *
 * @chan: S1G/HT primary channel
 * @primary_chan: S1G primary channel when chan is S1G channel - ignored otherwise
 * @ht: true to use HT/S1G channel as input (for testing only)
 *
 * Returns: Offset value
 */
int morse_ht_chan_offset_au(int chan, int primary_chan, bool ht);

/**
 * Determine if a channel has a disabled primary channel confiuration
 *
 * This function checks if a HT-mapped operating channel has a disabled HT primary or HT secondary
 * channel. The function is only valid to be called on a channel that has the same bandwidth as the
 * configs operating class, as it uses the primary channel index from the config.
 *
 * @param conf Hostapd config, including configuration containing primary channel parameters
 * @param mode Supported HW mode information, including a list of enabled/disabled channels
 * @param s1g_op_chan S1G operating channel
 *
 * @return True if HT primary or HT secondary is disabled
 */
bool morse_s1g_is_chan_conf_primary_disabled(struct hostapd_config *conf,
					     struct hostapd_hw_modes *mode, int s1g_op_chan);

/**
 * Configure the chip during setup interface sequence.
 *
 * Handles the morse_cli calls to set channel and operating class, including details that are not
 * otherwise mapped to 5GHz channels (e.g. primary channel width)
 *
 * @param iface Hostapd interface struct
 *
 * @return 0 upon success, MORSE_S1G_RETURN_ERROR on failure
 */
int morse_set_interface(struct hostapd_iface *iface);

/* -------------------------------------------------------------------------
 * Prototypes the embedded fork stripped from morse.h but
 * kept the definitions of in morse.c — re-added so wpa_supplicant.c / mesh.c
 * / acs.c compile clean (-Wimplicit-function-declaration + return-type fixes).
 * ------------------------------------------------------------------------- */
enum s1g_op_class_type morse_s1g_op_class_valid(u8 s1g_op_class,
		const struct ah_class **class);
int morse_s1g_chan_to_ht_chan(int s1g_chan);
int morse_s1g_op_class_chan_to_freq(u8 s1g_op_class, int s1g_chan);
int morse_cc_get_primary_s1g_channel(int op_bw_mhz, int pr_bw_mhz,
		int s1g_op_chan, int pr_1mhz_chan_idx, char *cc);
const struct ah_class *morse_s1g_ch_to_op_class(u8 s1g_bw, char *cc, int s1g_chan,
		bool report_error);
int morse_s1g_verify_op_class_country_channel(u8 s1g_op_class, char *cc, int s1g_chan,
		u8 s1g_1mhz_prim_index);
int morse_validate_ht_channel_with_idx(u8 s1g_op_class, int ht_center_channel,
		int *oper_chwidth, int s1g_prim_1mhz_chan_index,
		struct hostapd_config *conf);

/* CLI bridges — these shell out to a userspace morse_cli tool on Linux. On
 * embedded (no fork/exec) they're stubbed in morse_stubs.c to return 0.
 * Functional config goes through the umac/driver path (mmdrv_mesh_config etc.). */
int morse_set_channel(const char *ifname, int oper_freq, int oper_chwidth,
		u8 prim_chwidth, u8 prim_1mhz_ch_idx);
int morse_set_s1g_op_class(const char *ifname, u8 opclass, u8 prim_opclass);
int morse_set_mesh_config(const char *ifname, u8 *mesh_id, u8 mesh_id_len,
		u8 beaconless_mode, u8 max_plinks);
int morse_mbca_conf(const char *ifname, u8 mbca_config, u8 min_beacon_gap,
		u8 tbtt_adj_interval, u8 beacon_timing_report_interval,
		u16 mbss_start_scan_duration);
int morse_set_mesh_dynamic_peering(const char *ifname, bool enabled,
		u8 rssi_margin, u32 blacklist_timeout);

#endif /* MORSE_H */
