#pragma once

/*
 * One-shot mesh diagnostic. Attempts mmwlan_mesh_enable() and logs whether the
 * MM6108 firmware accepts an 802.11s mesh VIF.
 *
 * This is the decision gate for Phases 3-6 of the mesh port: morselib's chip
 * ABI header carries the MESH interface-type enum value but none of the mesh
 * commands, so firmware mesh support is unverifiable from source. Flashing the
 * `warthog-mesh-smoke` env and reading the serial log answers it. See
 * docs/mesh-port-scope.md ("Decision gate").
 *
 * Built unconditionally; only invoked from warthog_halow_start() when
 * WARTHOG_MESH_SMOKE is defined (the warthog-mesh-smoke PlatformIO env).
 */
void warthog_mesh_smoke_test(void);
