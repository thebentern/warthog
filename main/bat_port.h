/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* BATMAN_V member mode (AT+MESHBATMAN=1): the engine in main/bat/ on top of the
 * 802.11s mesh, with the HaLow esp_netif as its soft interface (bat0). */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "bat.h"

/* Mesh bring-up, before mmwlan_mesh_enable(). Allocates nothing unless batman may run. */
esp_err_t warthog_bat_port_start(void);
/* mmwlan_mesh_enable() succeeded: the engine may transmit. */
void      warthog_bat_port_mesh_up(void);
/* It failed: batman stays allocated but idle, reported as not running (mesh-failed). */
void      warthog_bat_port_mesh_failed(void);
bool      warthog_bat_port_running(void);
int       warthog_bat_port_reason(void);   /* enum bat_mode_reason for this boot */
unsigned  warthog_bat_port_routes(void);   /* originators with a route, published by the engine task */
unsigned  warthog_bat_port_neighs(void);
/* bat0 addressing asks the engine task about one client MAC (the lease's router; NULL: none).
 * A new MAC is answered on the engine's next pass, then every 500 ms. */
void      warthog_bat_port_watch(const uint8_t mac[6]);
/* 1: @mac resolves to a routed originator (*r filled), 0: it does not, -1: no answer for @mac yet. */
int       warthog_bat_port_watch_answer(const uint8_t mac[6], struct bat_client_route *r);
/* bat_gw_best's answer, at most 500 ms old: its return capped at 255 (0 when not running), which bat0
 * addressing takes as the number of gateways with a route, and the best one in *out. */
uint8_t   warthog_bat_port_gw(struct bat_gw *out);
void      warthog_bat_port_soft_mac(uint8_t out[6]);
void      warthog_bat_port_hard_mac(uint8_t out[6]);
uint8_t   warthog_bat_port_bcast_copies(void);
uint32_t  warthog_bat_port_tput_override(void);
bool      warthog_bat_port_sae_build(void);
bool      warthog_bat_port_host_ccmp_build(void);

#define WARTHOG_BAT_RENDER_OK          0
#define WARTHOG_BAT_RENDER_NOT_RUNNING (-1)
#define WARTHOG_BAT_RENDER_BUSY        (-2)
/* AT task: renders the chunk of listing @k at *cursor (0 starts it; @mac as bat_render_from's filter,
 * NULL: all) into the port buffer and advances *cursor, to BAT_RENDER_DONE after the last chunk.
 * *out is valid until warthog_bat_port_render_done(), which must follow every WARTHOG_BAT_RENDER_OK. */
int       warthog_bat_port_render(enum bat_render_kind k, const uint8_t *mac, uint32_t *cursor,
                                  const char **out);
void      warthog_bat_port_render_done(void);
