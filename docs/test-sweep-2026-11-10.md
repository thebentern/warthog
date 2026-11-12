# Full test sweep: bugs and gaps

**Date:** 2026-11-10
**Firmware:** `v0.1.0-12-g0082fe0` (main at `0082fe0`), ESP-IDF 5.5.6, esp_tinyusb 3.0.0, morselib 2.12.3, chip firmware 1.17.9
**Bench:** three XIAO ESP32-S3 + FGH100M-H boards (`…BA51`, `…738D`, `…823D`) on a macOS host, and two OpenMANET 1.8.0 Raspberry Pi nodes (plain 802.11s SAE mesh `halowmesh`, US ch 42, 2 MHz, no `bat0`) that also serve DHCP as the gateway `10.41.254.1`

Every one of the nine images was flashed and exercised on hardware, along with the whole AT command set, the crash and hang guards, the download paths, USB networking, NAT, the bench scripts and the host test suite. A read-only code audit ran alongside, with every finding re-checked by a second independent reader.

**Headline:** the two release mesh images work well in plain mesh mode, and the host suite and CI checks all pass. Five serious defects were reproduced on hardware, all in less-travelled modes: bridge mode, batman mode, `AT+MESHSEC` on an SAE build, `AT+MPING`, and open meshes. One board (`…BA51`) has a power-delivery problem that the firmware cannot currently recover from.

**Update, 2026-11-11:** fixes for H1 to H5 are in the working tree, uncommitted, and each was checked on the boards ([section 9](#9-fixes-for-h1-to-h5-2026-11-11)). Verifying them turned up a sixth high-severity bug, in the USB stack that came in with the ESP-IDF upgrade ([H6](#h6-the-usb-function-stops-when-the-console-port-is-opened-and-closed-under-network-load)); it is fixed in the working tree too, by vendoring the USB component with two changes, and checked on the boards.

Each item below says how it was established:

- **Hardware** — reproduced on a board in this sweep.
- **Audit** — found by reading the code and confirmed by a second reader; not run on a board.

---

## 1. Results by image

| Image | Boards | Result |
|---|---|---|
| `warthog-station` | BA51, 823D | USB NCM, console, Wi-Fi AP, region and `AT+ERASE` all work. Run as a mesh (`AT+MESHEN=1`) it peers and pings with a `mesh-smoke` board. The HaLow access-point uplink could not be tested (no HaLow AP on the bench). |
| `warthog-station-ecm` | BA51 | macOS binds its ECM driver, issues a lease, pings pass including 1472-byte unfragmented and 4000-byte fragmented. |
| `warthog-mesh-smoke` | 738D | Open mesh peers and passes traffic (6/8, 7/8 pings). Ignores the mesh ID ([H5](#h5-an-open-mesh-peers-with-any-node-whatever-its-mesh-id)). |
| `warthog-mesh-sae` | 738D, 823D | Peers with three neighbours but carries no data at all; works only as an isolated pair, with 10–50% loss ([M1](#m1-warthog-mesh-sae-carries-no-data-once-it-has-more-than-one-peer)). |
| `warthog-mesh-sae-nochipkey` | 738D | Behaves as its build note describes (diagnostic only). |
| `warthog-mesh-sae-swccmp` | 738D | No data until `AT+SWCCMP=1`, then lease, pings and NAT work. |
| `warthog-mesh-sae-swccmp-on` | 823D | Armed at boot, lease and pings work. Replies of 1000 bytes or more from the gateway are lost, which is the documented RTS/CTS limit of the STA chip interface. |
| `warthog-mesh-sae-swccmp-meshvif` | 738D, 823D | Plain mesh: everything passes. Batman mode: runs out of memory and crashes ([H2](#h2-batman-mode-runs-out-of-heap-on-warthog-mesh-sae-swccmp-meshvif)). |
| `warthog-mesh-sae-meshvif` | 738D, 823D | Everything passes: ping matrix, NAT at every size, HTTP, the end-to-end suite's traffic checks, throughput, soak and a power-cycle rejoin. |

Numbers for the `meshvif` pair, for reference:

| Check | Result |
|---|---|
| Board to board ping | 10/10, about 20 ms |
| Board to gateway ping | 10/10 both boards |
| Mac → USB → NAT → gateway | 10/10; 1400 B, 1472 B unfragmented and 3000 B fragmented all pass; HTTP 200 in about 0.1 s |
| UDP throughput (delivered) | 134 / 525 / 919 / 1483 kbps at 64 / 256 / 512 / 1024 B (0 / 0 / 6 / 8% loss) |
| Soak, 3 min | 11 rounds, no dead pair, 0% and 3% loss |
| Rejoin after 45 s power-off | lease and pings back within 70 s |

---

## 2. Bugs reproduced on hardware

### High

#### H1. `AT+MESHBRIDGE=1` never forms a bridge, and takes networking down with it

- **What happens (mesh image, `warthog-mesh-sae-meshvif`):** after `AT+MESHBRIDGE=1` and a reboot, `AT+MESHCFG?` reports `l2=bridge multicast=all(bridged)`, but the node never gets a mesh address (`+HALOW: ip=0.0.0.0` for over 90 s with two peers up). The tethered Mac gets no DHCP lease (it fell back to `169.254.131.58`) and cannot ping `192.168.4.1` or the gateway. Other nodes cannot ping the board. `AT+MUDP?` shows all addresses `0.0.0.0`.
- **Then it crashes:** the first `AT+MPING` panics the board in the lwIP thread (`pc=0xfffffffd`; backtrace `ip4_output_if_opt` ip4.c:892 ← `raw_sendto` ← `lwip_netconn_do_send`): lwIP sends through a bridge netif that was never added.
- **It also applies on the station image:** a stored `AT+MESHBRIDGE=1` with `AT+MESHEN=0` stops the USB and AP DHCP servers and skips NAT. The Mac got no lease (`169.254.37.208`) while `AT+STATUS?` still showed `+USB: ip=192.168.4.1`.
- **Where:** `main/main.c:99-106` starts the Wi-Fi AP before `warthog_mesh_bridge_start()`, and gates only on radio-up and the stored flag; `main/mesh_bridge.c:86-100` records ports and stops DHCP, but IDF's bridge glue only attaches ports on an Ethernet start event or `WIFI_EVENT_AP_START`, neither of which arrives.
- **Recover:** `AT+MESHBRIDGE=0`, `AT+RESET` (the console keeps working).
- **Fix direction:** attach the ports explicitly or start the glue before the AP; gate on `AT+MESHEN`; fall back to NAT when the bridge has no ports, as `wiki/Mesh-Mode.md` already promises.
- Bridge mode was added on 2026-09-21, before the ESP-IDF upgrade. Whether it ever worked on the earlier IDF was not checked.

#### H2. Batman mode runs out of heap on `warthog-mesh-sae-swccmp-meshvif`

This is the image the README recommends for OpenMANET with `bat0`.

- **Measured** (`AT+BATSTAT?`, the `port` line):

  | Board state | `heap_free` | `heap_min` | `heap_largest` |
  |---|---|---|---|
  | Batman up, no address yet, 2–3 peers | 28–32 KB | 22.9 KB | 17–22 KB |
  | Batman up, address taken, 3 peers | 3.5–12.8 KB | **56 bytes** | 2.5–7.7 KB |

  Each peer costs about 4.5 KB. Taking the address allocates NAT's tables, about 24 KB.
- **Consequences seen:**
  - A panic on both boards, once each in about nine batman boots: `assert failed: ip_napt_init ip4_napt.c:224 (ip_portmap_table != NULL && ip_napt_table != NULL)`, reached from `nat_napt_on_` at `main/nat.c:46`. NAT's tables are allocated lazily when the uplink first gets an address, and lwIP asserts when that allocation fails. 738D's was seen live at 192 s of uptime; 823D's was found afterwards in its core-dump record. Four later controlled reboots did not repeat it.
  - On 823D, 23 HaLow chip restarts in about ten minutes, an `MMOSAL_ASSERT` record at `umac_mmdrv_shim.c:169` (a chip restart that could not restore the mesh), peer count flapping between 3 and 2, and batman neighbours lost on both boards.
- **Not caught by the boot guard:** the crash comes after more than 60 s of uptime, by which time the crash-boot count has been cleared.
- **Fix direction:** allocate the NAT tables at boot while memory is plentiful, and handle failure without asserting; look at enabling the XIAO's 8 MB PSRAM (`CONFIG_SPIRAM` is not set); size the NAT table to the device.
- The bench's OpenMANET nodes do not run batman, so this was Warthog-to-Warthog batman only.

#### H3. `AT+MESHSEC=` on an SAE build drops every link until reboot

- **Reproduce:** on a working SAE mesh, send `AT+MESHSEC=1` (the value already stored). The reply is `+MESHSEC: keyed, stored, re-peering (~2s)`.
- **Result:** `AT+PEERS?` goes from 3 to 0 and stays there (checked at 6 s, 60 s and 2 min) while `AT+SAERX?` still shows `ESTAB=4`. All traffic stops: board to board 0/4, board to gateway 0/4, Mac through NAT 0/10, and the other board cannot ping it either. Only `AT+RESET` recovers.
- **Where:** `main/at.c:2775` requests a re-peer on every build; `umac_datapath_mesh.c:3379` then deletes all datapath peers without telling hostap, which refuses to re-add stations it still holds.
- The AT reference says this command has no effect on the SAE build. The web flasher's mesh Save button sends it.

#### H4. `AT+MPING` frees its semaphore while the ping task is still running

- **Reproduce:** from the host, `ping -i 0.2 192.168.4.1`; on the board, `AT+MPING=192.168.4.250,2` (any address that does not answer).
- **Result:** after 4.5 s the board prints `+MPING: 0 sent, 0 received, 100% loss` and `OK`. Seven seconds later, unsolicited, `+MPING: seq=1 timeout` and `+MPING: seq=2 timeout` appear, and then the board panics: `assert failed: xQueueGenericSend queue.c:937`, task `ping`, from `mping_on_end` (`main/at.c:1322`).
- **Where:** `main/at.c:1363-1365` ignores the result of the wait, never stops the session, and deletes the semaphore the ping task later gives.
- **Fix direction:** stop the session and wait for its end callback before deleting anything.

#### H5. An open mesh peers with any node whatever its mesh ID

- **Reproduce:** two non-SAE boards (`warthog-mesh-smoke`, or the station image with `AT+MESHEN=1`) on the same channel with different `AT+MESHID` values (`halowmesh` and `auditmismatch`).
- **Result:** both reach `estab=1`, `AT+MESHCFG?` says `peered`, and pings pass (3/8 and 6/8).
- **Where:** `umac_mesh.c:2421` (`umac_mesh_handle_mpm`) never reads the Mesh ID element.
- Non-SAE builds share a compiled-in key, so two open meshes an operator configured as separate will merge. The open-mesh board also kept sending peering Opens to one of the SAE OpenMANET nodes.

#### H6. The USB function stops when the console port is opened and closed under network load

Found on 2026-11-11 while verifying the fixes for H1 to H5. It is not caused by them: it reproduces on `0082fe0` unmodified.

- **Reproduce:** tether a host and push traffic through the USB network function (here 1.5 Mbit/s of UDP plus ten 1400-byte pings a second), and at the same time open and close the console serial port a few times a second, sending nothing. Each open sends the CDC control requests `SET_LINE_CODING` and `SET_CONTROL_LINE_STATE`.
- **Result:** the whole USB function stops: the console is dead, the USB network is dead, and control requests on endpoint 0 time out. The board stays attached (the hub still reports the port connected and enabled), does not reset and leaves no core dump. The rest of the firmware keeps running: other nodes still hear it and it answers pings over the mesh. Two boards stayed like that for 7.5 hours. A host-initiated port reset does not bring it back (the device then fails to enumerate); only a power cycle does. Neither watchdog nor the hang guard fires, because every task is alive.
- **Time to failure:** 18 s (3 port opens), 41 s and 55 s (33 and 43 opens) on the mesh boards; 96 s (318 opens) on BA51 on the station image with its radio idle. It also happened once with no deliberate load, after several console sessions in a row, and once at the start of a ping test.
- **What does not trigger it:** the same load with the console closed; the same load with one console session held open and commands running over it (80 to 112 command rounds); port opens with no network load (237 opens, and 176 command sessions in another run); 40,000 `SET_LINE_CODING` requests or 20,000 DTR toggles sent under load without opening the port.
- **Where:** the USB device controller driver in DMA mode (a second fault in slave mode is below), TinyUSB 0.21's `dcd_dwc2.c`. esp_tinyusb 3.0.0 uses nothing else (its changelog: "Removed DCD Slave/IRQ mode. Only Buffer DMA mode is supported"), and 3.0.0 came in with the ESP-IDF 5.5.6 upgrade (`93cdec9`, 2026-11-04); v0.1.0 was built on esp_tinyusb 2.2. A bench image that streams the controller's registers over the mesh showed the dead state: a SETUP packet stored on endpoint 0 (`DOEPTSIZ0` 0x60000000 to 0x40000078, `DOEPDMA0` advanced by 8) whose "setup done" interrupt is never raised (`DOEPINT0` stays 0), the receive FIFO permanently non-empty with a network packet at its head, and the network OUT endpoint's packet count down by two with no bytes moved. On a host bus reset the driver's `handle_bus_reset()` calls `dma_setup_prepare()`, which returns early because endpoint 0 OUT is still enabled (`dcd_dwc2.c:171-175`), so endpoint 0 is never re-armed and the enumeration SETUPs are stored but never delivered. What puts the controller into that state in the first place is not established.
- **Fix, part 1: slave mode.** `components/esp_tinyusb` is now a vendored copy of the registry's esp_tinyusb 3.0.0 (`main/idf_component.yml` points at it with `override_path`), because the component manager silently restores an edited managed file at the next configure. Its `include/tusb_config.h` sets `CFG_TUD_DWC2_DMA_ENABLE` to 0, so TinyUSB builds its slave (interrupt) mode controller driver. Three boards then ran the reproducing test for four minutes each, 549, 939 and 939 port opens under load, with no failure. Host-to-board throughput over USB was the same in both modes (4.1 Mbit/s at 4 offered, 5.9 Mbit/s at 9 offered).
- **A second fault, in slave mode only.** With port opens once a second and three commands sent in each session, under the same load, the console alone went silent: about one 70-second run in four at first, later one in ten to one in twenty-five (it failed after 21 clean runs, and again after 23). The USB network and endpoint 0 kept working, the AT task was alive, and a host port reset brought the console back without a reboot. The register stream showed the console's IN endpoint enabled with one packet outstanding, no bytes left to load and its transmit FIFO empty (`DIEPCTL2` 0x80898040, `DIEPTSIZ2` 0x00080000, `DTXFSTS2` 16 words free), and, with the port held open, the "IN token received with the FIFO empty" and NAK flags setting again each time they were cleared: the host was reading, and the controller had counted a packet as loaded that was not there. The stuck transfer was a two-packet one each time (87 and 88 bytes).
- **Narrowing it down.** A bench build with a task writing a line to the console every tick, and the host sending about 2,000 refused control requests a second, reproduces it in 2 to 60 seconds. Measured with that: 90-byte lines with no control traffic survive (240 s); 40-byte lines with accepted control requests survive (100 s, two boards); 90-byte lines with refused requests fail (every run), whether or not esp_tinyusb's warning log for the refused request is compiled in. Ruled out by building each variant and failing: a lock around the interrupt handler's FIFO refill and its `DIEPEMPMSK` update (this passed 21 ordinary runs first, by luck), the transmit FIFO flush in the stall path (no flush had run when it failed), a global IN NAK around that flush, the FIFO-empty interrupt level, and each of the two endpoint 0 stall writes on its own. The host sent no clear-halt. So the fault needs a console transfer longer than one packet, whose later packets the driver's interrupt handler loads from the CDC transmit FIFO, together with control requests that end in a stall. What happens inside the driver or the controller is not established.
- **Fix, part 2: one packet per transfer.** `tusb.c`'s `tu_edpt_stream_write_xfer()` now caps a FIFO-backed IN transfer at one packet (four lines marked `Warthog H6`), so every console packet is loaded when its transfer starts and the interrupt-driven refill is never used for it. NCM is not affected: it transfers from a plain buffer and did not show the fault. Console output still ran at about 120 KB/s. On the bench build with the cap, two boards survived the stress for 200 s and then 500 s each (62 MB of console output per board in the 500 s), where every build without it stopped within 150 s.
- **Final check, on the images the boards now run:** 738D and 823D ran 12 of the 70-second command runs under load, with a host program sending refused control requests to both at the same time (how many got through was not recorded), and no command failed; then 237 port opens each under load in four minutes, no failure; no asserts, three peers each, no failed allocations. Host-to-board throughput over USB was unchanged. All nine images build, pass `check-image.sh` and the host suite passes with the strict guards. The 12-run test was not run against an unfixed image with the same extra control traffic, so its failure rate there is not known; the stress above is the test with a measured before and after.
- **What enforces it:** glue guard 87 (the define; the four marked lines of `tusb.c`, and that the rest of it and every other file is the registry's, by `CHECKSUMS.json`), `scripts/ci/check-lock.sh` (the component resolves to the local copy) and `scripts/ci/check-image.sh` (the ELF holds the slave mode driver, not the DMA one). The tests to rerun after touching the component are described in `tools/bench/README.md`.
- **Left as it is:** neither driver defect was root-caused; both are avoided. The DMA failure may share a cause with the second fault (both need control requests during bulk traffic), which was not tested. Any other FIFO-backed stream class added later gets the same one packet cap. `components/esp_tinyusb/` is untracked until it is committed, and must go in with the manifest change. On an image built between `93cdec9` and this fix, keep one console session open rather than opening the port repeatedly, or leave the console alone while tethered traffic flows. The bench notes' "boards also drop off USB on their own occasionally" (`tools/bench/README.md`) fits the DMA fault.

### Medium

#### M1. `warthog-mesh-sae` carries no data once it has more than one peer

- With the two OpenMANET nodes also in the mesh, both boards showed `peers=3`, `ESTAB=3` and keys installed, yet both fell back to `10.77.x.y` addresses and every ping failed (board to board 0/10 both ways, gateway 0/10, NAT dead).
- Isolated on a private mesh ID so they had only each other, traffic passed but with heavy loss: 8/10, 18/20, 16/20 one way and 5/10, 15/20, 10/20 the other. The same two boards on the `meshvif` images were at 0–5%.
- The build notes in `platformio.ini` explain why (this chip interface keeps one pairwise key, the last installed). The user-facing docs say only "Warthog-to-Warthog only", and the README recommends this image for a "Warthog SAE mesh" with no node-count caveat. It is also not in the release bundle or the web flasher.

#### M2. Wi-Fi AP settings that silently stop the AP

`AT+WIFIAP` answers `OK` and stores each of these; after the reboot the log says `warthog.wifi_ap: esp_wifi_set_mode/config/start failed` and no AP is on air:

| Setting | Driver's objection | Documented? |
|---|---|---|
| Passphrase of 1–7 characters | rejected | Yes, in Client Mode ("or the AP does not start") |
| Channel 12 or 13 | `Channel number 13 is out of range from 1 to 11 in country code cc=01` | No: the docs and the command say 1–13 |
| 64 characters that are not hex | `password convert fail` | No |

In every case `AT+STATUS?` still reports `+AP: ip=192.168.5.1` and `AT+WIFIAP?` shows the stored values, so nothing on the console says the AP is down. `main/wifi_ap.c` returns after the netif already has its address, and `main/main.c` ignores the result.

An empty passphrase makes the AP open, which is documented, but the web flasher's Save AP button sends an empty passphrase whenever the box is left blank (`web-flasher/index.html:530`, audit).

#### M3. Several `0|1` setters store garbage

`AT+MESHEN`, `AT+MESHFWD`, `AT+MESHGRP`, `AT+MESHPMF`, `AT+MESHBRIDGE` and `AT+MESHDHCP` parse with `strtoul(args, NULL, 10)` and no end check (`main/at.c:3375, 3432, 3491, 3502, 3513, 3530`):

| Input | Stored |
|---|---|
| `abc`, empty, `0x` | 0, with `OK` |
| `1x` | 1 |
| `AT+MESHEN=256` | 0 |
| `AT+MESHEN=257` | 1 |

`AT+MESHSEC`, `AT+MESHBATMAN`, `AT+AMPDU`, `AT+TIDPARAMS`, `AT+GTKPERSTA`, `AT+CRYPTOHOST` and `AT+MCAST` validate correctly, so the fix is to reuse their parser.

#### M4. The boot guard cannot see two kinds of reset loop

- **Power-on and brown-out loops.** Only panics and watchdog resets count as crash boots, and the count lives in RAM that a power loss clears. BA51 looped on `POWERON` every 1–2 seconds indefinitely ([section 4](#4-ba51-power-delivery-and-what-it-exposed)). Because USB starts only after the HaLow start, the board never offered a console during the loop.
- **Crashes after 60 s of uptime.** The count is cleared at 60 s, so a fault that recurs every few minutes (H2 is one) never reaches safe mode.

#### M5. The station image run as a mesh beacons ten times as often

With `AT+MESHEN=1` the station image sent 926 beacons in 96 s; `warthog-mesh-smoke` sent 105 in 109 s. The station build compiles a 100 TU beacon interval where every mesh image uses 1000 TU, and it is not settable. Its default mesh ID also differs (`warthog-mesh-test`, which Mesh Mode does document).

### Low

| Finding | Detail |
|---|---|
| `AT+SEALFIT=<bad>` has no terminator line | The usage text overflows `reply_error()`'s 128-byte buffer (`main/at.c:88-94`, literal at `:3835`), so the reply ends `…(deERROR` on one line. A host waiting for `ERROR` hangs. |
| `AT+DNS` accepts malformed addresses | `1.2.3` is stored as typed; `255.255.255.255`, `224.0.0.1` and `127.0.0.1` are accepted. The reference says malformed input is rejected. |
| `AT+SAEBRIDGE=3`, `=abc` or empty | Answers `OK` and sets 0, which makes the node deaf to SAE candidates until reboot (`main/at.c:3597`). |
| `AT+SWCCMP=1` on builds without host CCMP | Reports `host ccmp ON` and `on=1`. `=2` turns it on and `=abc` turns it off. |
| Other lax parsing | `AT+REKEY=99` is queued with no slot check; `AT+MACSTATS=abc`, `AT+SAESTAGE=abc`, `AT+FRAG=99999999`, a sixth field on `AT+MESHCHAN`, `AT+VERSION??` and `AT+STATUS?junk` are all accepted. |
| `AT+MINJECT` cap | Documented as 300 bytes; the 192-character line limit caps it at 90 (91 answers `line too long`). |
| Console needs DTR | With DTR low the console sends nothing at all; Troubleshooting says any terminal works. |
| `AT+MESHCFG?` in safe mode | Prints "the channel list was REJECTED … fix that before reading anything else" when the radio is simply held. |
| `AT+VERSION?` does not name the image | All nine images report the same string, so there is no way to tell which variant a board runs. |
| `AT+HOSTFRAG` on the station image | `=auto` reports `mode=auto … applies now`; after a reboot it reads `mode=off stored=auto`. |
| `scripts/dl_trigger.py` | Prints "chip should be in download mode" on the station image, which ignores the 1200-baud touch (verified: the board stayed in the app). |
| Reset reason naming | The boot log calls reset reason 11 `OTHER (11)`; `AT+ASSERT?` calls it `USB`. |
| `AT+BATN?` | Occasionally returned no `+BATN:` line within 2.5 s while batman was under memory pressure. |

---

## 3. Bugs found by the code audit (not run on hardware)

All confirmed by a second reader unless marked uncertain.

| Severity | Area | Finding | Where |
|---|---|---|---|
| High (uncertain) | Web flasher | Release assets are fetched cross-origin from github.com, whose download redirect carries no CORS header; needs a check of the deployed page | `web-flasher/index.html:337` |
| Medium | NAT | IDF's NAPT limits are inherited unchanged: UDP and ICMP mappings expire after 2 s idle, ICMP errors are not translated, source ports below 1024 are refused | `main/nat.c:46` |
| Medium | Mesh security | Management frame protection is enforced on receive only for path selection; an unprotected Deauth or Disassoc in a peer's name frees that peer | `umac_datapath_mesh.c:288` |
| Medium | Mesh status | Under SAE, stations still authenticating count as peers, so `AT+MESHCFG?` says `peered` and the "not peered" report is delayed on a passphrase mismatch | `main/mesh.c:537`, `umac_datapath_mesh.c:2887` |
| Medium | Path selection | A leaf answers every copy of a flooded PREQ and raises its sequence number each time, so the last-arriving path wins over the best | `umac_mesh.c:2300` |
| Low | Path selection | The path metric is added without saturation; a neighbour's maximum metric wraps to the best one | `umac_mesh_hwmp_relay.c:58` |
| Low | Mesh | A leaf never consults the duplicate cache, so a re-flooded group frame is delivered once per neighbour | `umac_datapath.c:923` |
| Low | Mesh | Under SAE a probe response from another mesh is offered to hostap with no Mesh ID check, starting SAE and taking one of four peer slots | `umac_datapath_mesh.c:261` |
| Low | Mesh | Open-mesh link expiry ignores data and path-selection frames; a peer that does not beacon or probe for 30 s is dropped mid-traffic | `umac_mesh.c:1750` |
| Low | Mesh | An SAE authenticator start failure is treated as non-fatal but leaves a NULL `mesh_rsn` that the first peer offer dereferences | `supplicant_core_mesh.c:319` |
| Low | Mesh diagnosis | The no-peers diagnosis uses the since-boot beacon total, so it can never again report "nothing audible" | `main/mesh.c:127` |
| Low | Batman | Two nodes probing the same static fallback address at once both take it, and nothing detects the duplicate | `main/mesh.c:227` |
| Low | USB | The NCM build advertises remote wakeup but ignores suspend; frames for a suspended host are dropped | `main/usb_net.c:454` |
| Low | USB | ECM build: after a host bus reset the TX pump can submit on endpoint 0 | `main/usbnet_core.c:98` |
| Low | Region | A corrupted region partition does not hold the radio; the entry is erased and the board boots on the US default | `main/cfg.c:375` |
| Low | Release | Three of the four released images carry the 1200-baud download trigger, so any program opening the console at 1200 baud drops a field node into download mode | `release.yml:26`, `main/usb_net.c:232` |
| Low | Console | The over-long-line discard flag survives a reconnect, so the next session's first command is swallowed | `main/at.c:3910` |
| Low | Console | `AT+CRYPTOHOST?` overwrites a pending `AT+CRYPTOHOST=` request | `main/at.c:2743` |
| Low | Console | `AT+STATUS?` `+DNS: offered=` prints the stored value, not what DHCP is handing out before the reboot | `main/at.c:263` |
| Low | API | The public mesh API documents a callback that never fires, and `max_peers` and disable are unimplemented | `mmwlan_mesh.c:113` |

---

## 4. BA51: power delivery, and what it exposed

BA51 was found in safe mode at the start of the sweep. After a reset it never came back as a Warthog.

- **Symptom:** every boot logged `last reset reason: POWERON (1)` and died 1–2 seconds in, right after `mmwlan_scan_request` or the first probe transmit. A 25-second VBUS power-off did not clear it. This is the supply-collapse signature in `docs/power-notes.md`.
- **What the host sees:** the board shows up as "USB JTAG/serial debug unit" (`303a:1001`) and never as "Warthog HaLow Bridge". The boot log is readable on that port between resets, which is the only diagnostic available.
- **On the station image** (radio idle, no transmit) the same board is stable, which is how the rest of the sweep used it.
- **How it reached safe mode earlier:** three crash boots. One core dump decodes to `hw_restart_evt_handler` at `umac_mmdrv_shim.c:169`, the deliberate reset when a chip restart cannot restore the mesh. The other shows the `drv` task jumping to `0x24918a27`. That value is exactly the flash literal at `0x42040c4c` (`0x4212a978`) read one nibble out of alignment, checked against the board's flash image: the rail sagged far enough to corrupt a flash read without tripping the brown-out detector. It is not a driver bug.
- **In one boot** only the HaLow chip died. Commands timed out (`Failed to write page: -32768`), yet the firmware still logged `mesh: interface up (vif_id=0, chip VIF type 1)` and carried on.

What to act on:

- **Hardware:** check BA51's bulk capacitor, cable and hub port. It sits on the first hub; the two healthy boards sit on a second hub behind it.
- **Firmware:** see [M4](#m4-the-boot-guard-cannot-see-two-kinds-of-reset-loop). The brown-out detector is at its lowest level (level 7, about 2.44 V), below what the flash needs at 80 MHz; a higher level would turn silent corruption into a clean reset.
- **Docs:** Troubleshooting has no entry for "appears as USB JTAG/serial debug unit, never as Warthog".

---

## 5. Gaps

### Test coverage

- **Nothing tests the default image's main functions.** No host test, CI check or bench script exercises the HaLow station uplink, the 2.4 GHz AP, or the client DNS option.
- **No release gate on hardware behaviour.** `release.yml` publishes after image and pin checks alone, and CI does not run for tag pushes.
- **No heap telemetry outside batman mode.** `heap_free`, `heap_min` and `heap_largest` exist only on `AT+BATSTAT?`. The memory margin of the plain mesh images, with four peers and AP clients, is unknown.

### Bench scripts

- **Hard-coded bench.** All four `mesh_*.py` scripts hard-code three board serials. `mesh_churn.py` hard-codes `uhubctl -l 0-1.3 -p 1` for the victim; today that hub is `1-1.3` and port 1 is a different board.
- **`mesh_churn.py` needs exactly three boards** and raises `IndexError` with two.
- **Link counters are blind on SAE builds.** `mesh_churn.py` and `mesh_soak.py` count `estab=` from tables hostap does not drive, so they always print 0. `mesh_e2e.py` has the `AT+PEERS?` fallback; the others do not.
- **`mesh_e2e.py`** fails its peer-count checks whenever a legitimate extra peer is on air, passes vacuously with one board, and always exits 0 (it exited 0 on a 2/16 run).
- **`openmanet_interop.py --audit-only`** prints "nothing will be changed" and then stores mesh settings on the Warthog (seen: the "warthog config" step ran). It also never resets the Warthog, so each run tests the previous boot's settings.
- **`scripts/autoflash.sh` and `scripts/flash.sh`** write a factory image to whatever serial port they find, with esptool's auto-reset. Another ESP32-S3 on the bench, such as a Meshtastic node, would be flashed. Neither was run without an explicit port for that reason.
- **`tools/bench/flash.sh`** leaves boards in download mode when a flash fails and exits 0.
- **`mesh_e2e.py` needs the `meshtastic` package**, which is not installed here; a stand-in encoder was used.

### Documentation

- **Six verbs are undocumented:** `AT+BCNDUMP?`, `AT+BCNRX?`, `AT+PRQRX?`, `AT+PLINKTX?`, `AT+PLINKRX?`, `AT+PLINKSTAT?`, plus the query forms `AT+MESHDHCP?` and `AT+SAEBRIDGE?`.
- **Dead links.** The README points readers at `wiki/*.md` in the repo, where the wiki-style cross-links (no `.md`) do not resolve.
- **Bench checklist step 3** expects a HaLow-state LED pattern that an unconfigured board never shows.
- **SAE peers are not expired.** With one board powered off for 45 s the survivor still reported `PEERS: count=3`. Mesh Mode notes this; with a four-peer limit it means departed nodes hold slots.

---

## 6. What passed

- **Host side:** host suite plain and ASan/UBSan (9039 checks each), lwIP NAT 40/40, the USB network host tests, the strict glue guard, and the CI scripts (platform pin, image check on all nine images, sdkconfig landing, lock, clean tree).
- **Console:** all 79 query commands on all three boards; CR, LF and CRLF terminators; backspace; lines up to 5000 characters; binary input; 40 pipelined commands with every reply in order.
- **Validation that is correct:** region (including a reboot into JP and back, with the pinned-channel warning), `AT+ERASE` (region kept), the forwarding/bridge/batman conflict checks, and the mesh ID, passphrase, RSSI floor, channel, throughput, fragmentation, rate and capture setters.
- **Crash guard:** `AT+ASSERTTEST=at|crit|loop|hang`, safe mode after three crash boots with USB, console and AP still up, `AT+RESET` out of it, the 60-second clear, `AT+ASSERT=0` and `AT+COREDUMP=0`.
- **Hang guard:** all eight `AT+HANGTEST` probes reset with the documented reason and timing (61 s for `spin` and `main`; 114–203 s for the rest).
- **Download paths:** `AT+DLMODE` (about 20 uses), the 1200-baud touch on a mesh image, esptool with the documented `--before no-reset --after watchdog-reset`, and `scripts/flash.sh` with a factory image (settings erased, region kept, as documented).
- **USB networking:** NCM on all three boards and ECM on one; lease `192.168.4.2`, router, DNS `1.1.1.1`, MTU 1500.

---

## 7. Not tested

- The HaLow access-point uplink, and anything that depends on it.
- Wi-Fi AP client association, DHCP and NAT from the AP side. The Mac's only Wi-Fi interface is its internet link, so the AP was checked by passive scan only (present, on the configured channel, WPA2 or open as configured).
- NAT to the internet and client DNS resolution: the bench mesh has no internet behind it.
- Batman against real batman-adv, relay and forwarding mode (`AT+MESHFWD=1`), more than four peers, multi-hop paths.
- Regions other than US on air, Windows, Linux and iOS hosts, soaks longer than three minutes, and the web flasher.

Observations that could not be attributed:

- Gateway-to-board pings were usually clean but twice showed a 60–66% loss burst in a 15-second window. The nodes sit centimetres apart at −4 to −17 dBm, so receiver overload is a plausible cause.
- One 8 MB esptool write to 738D failed its MD5 check; the retry verified.
- The boards' serial port names changed during the day when the hub re-enumerated, which is what breaks the hard-coded bench scripts.

---

## 8. State the boards were left in

After the sweep (2026-11-10) all three boards were restored byte for byte from flash dumps taken at the start, except BA51, which was left on the station image so that it would not return to its power-on reset loop.

After the fix work (2026-11-11):

| Board | Image | Notes |
|---|---|---|
| 738D | fixed `warthog-mesh-sae-meshvif` (this tree) | Its original settings partitions were written back. Meshing with three peers, lease `10.41.0.160`. |
| 823D | fixed `warthog-mesh-sae-meshvif` (this tree) | **Changed.** It was found on `v0.1.0-9-g06c036a` with `AT+SWCCMP=1` sent by hand. Its original settings partitions were written back. Meshing with three peers, lease `10.41.0.124`. |
| BA51 | fixed `warthog-station` (this tree) | Mesh off, no HaLow SSID, as before. Still not to be given a mesh image until its supply is sorted (section 4). |

All three run the build that also carries the H6 fix. The fixed images all read `v0.1.0-12-g0082fe0-dirty`; `AT+HEAP?` answers on an image with the H1 to H5 fixes and is an unknown command on one without. Nothing on the console tells an image with the H6 fix from one without: `scripts/ci/check-image.sh` reads it from the ELF. The flash dumps taken at the start of the sweep are in `tools/bench/backup/` (`738D-full.bin`, `823D-full.bin`, `BA51-full.bin`, 8 MB each, git-ignored): writing one back at offset 0 returns that board to exactly what it was.

The repository has the fixes, their tests and the documentation changes as uncommitted work, and this report; `components/esp_tinyusb/` (the vendored USB component, 284 files) is untracked and has to be added with them. The OpenMANET gateway had its existing mesh settings rewritten with the same values and its radio bounced once by `openmanet_interop.py` during the sweep; nothing on the Pis was changed during the fix work.

---

## 9. Fixes for H1 to H5 (2026-11-11)

All five are in the working tree, uncommitted. (The fix for H6 came after this section was written and is recorded in the H6 entry.) Each was designed, reviewed against the code by a second reader before any line was written, implemented with host tests, and then measured on the boards. An adversarial review of the whole change found no high-severity defect; the two findings worth acting on (below) were fixed and re-tested.

### What changed, and what the boards showed

| Bug | Change | Measured afterwards |
|---|---|---|
| **H1** bridge mode | `main/mesh_bridge.c` rewritten without ESP-IDF's bridge glue. The bridge is built whole or not at all, on a running mesh only; anything short of that leaves NAT mode untouched and `AT+MESHBRIDGE?` says why (`running=`, `reason=`, `ports=`, `mesh_link=`, `grp_rx=`). `AT+MPING` checks for a route before sending. | Bridge up one second after boot with ports `usb,ap,mesh`; the bridge took the mesh's lease and the tethered Mac got its own address from the mesh's DHCP server (16 to 57 s after boot). Through the bridge: gateway pings at 1000, 1472 (unfragmented) and 3000 bytes, the other Warthog, HTTP. With both boards bridged, a host's own multicast and broadcast cross both bridges with the host's address. Station image with the setting stored: `reason=mesh-off`, NAT, DHCP and console intact, no crash. A bench build that refuses the last port fell back to NAT cleanly (`reason=port-refused`). |
| **H2** batman memory | The XIAO's 8 MB PSRAM is mapped; `main/ext_ram.c` tests its first megabyte at boot (250 ms) and makes it a separate heap that holds batman's tables (52 KB) and the frame copies queued to and from USB. NAT's tables are taken at boot behind a memory check, so lwIP's assert is not reached. Wi-Fi code moved out of IRAM and its buffer counts cut. New `AT+HEAP?` and `AT+EXTRAM`. Batman refuses to start without the PSRAM heap (`reason=no-psram`). | Batman mode with an address and three peers: 82 KB of internal RAM free (was 3.5 to 12.8 KB, low-water 56 bytes). 26 batman boots without a panic (was about one in nine). 15-minute soak: no chip restart (823D had 23 in ten minutes), no failed allocation. Two minutes at twice the link's capacity from both hosts: no reset, no failed allocation, low-water mark 23 KB. Panic path and safe mode behave; safe mode leaves the PSRAM unused. |
| **H3** `AT+MESHSEC` | On the six SAE images the command only stores the value and says so; morselib refuses the teardown under SAE as a second layer; on a non-SAE mesh only a changed value re-peers. The web flasher asks `AT+MPMPEERS?` first and sends nothing to a board on the SAE mesh, or when that query is not answered (released v0.1.0 still has the bug). | Three SAE peers before, during and after `AT+MESHSEC=1`, `=0`, `=1`; pings pass throughout (was: zero peers until a reboot). |
| **H4** `AT+MPING` | The command pings on the AT task over one raw ICMP socket; no second task, no callback, nothing left running after the `OK`. | 30 commands to a dead address while the host pinged the board five times a second: each ended cleanly, nothing printed afterwards, no panic, every host ping answered. |
| **H5** mesh ID | An open mesh accepts and starts peerings only inside its Mesh ID and Mesh Configuration profile; a stranger's Open is answered with one Close and takes no slot. Under SAE a probe response naming another mesh is not offered to the supplicant. New counters `foreign=` (`AT+MPMSTAT?`) and `prq_other=` (`AT+PRSPSTAT?`). | Two boards on different mesh IDs: no peering in six minutes (was: peered in about ten seconds). Same ID: peered in 20 to 40 s and pinged. An unfixed neighbour on another ID: 35 Opens refused in a minute, each with a Close; it logged `close_reason=54`. The SAE mesh with the two OpenMANET nodes still forms three peers; an SAE board moved to another mesh ID was never counted as a peer. |

All nine images build and boot on the boards with the PSRAM heap up. The host suite passes plain and under ASan/UBSan (about 9,300 checks; glue guard 118, 119 since guard 87 was added for H6, with four new guards and several extended; a new lwIP bridge test of 31 checks; flasher 35), as do the image and sdkconfig checks.

### From the review

- **Fixed:** in bridge mode a host that enumerated in the first second or two after boot could take a `192.168.4.x` lease from the node's own DHCP server before the bridge stopped it. The USB network link is now held down until the bridge is built or has failed. Re-measured: the Mac got the mesh's lease 16 s after boot.
- **Fixed:** the flasher's guard sent `AT+MESHSEC=` anyway when its `AT+MPMPEERS?` query timed out.
- **Left, low:** the memory check before lwIP allocates its NAT tables is not atomic with the allocation. It matters only with internal RAM nearly exhausted, which now takes `AT+EXTRAM=0` or safe mode under load.

### Limits and what was not measured

- **`AT+EXTRAM=0` is a diagnostic setting, not an operating mode.** It returns the node to the old memory budget: under the double overload a plain mesh leaf logged thousands of failed allocations and one board asserted and rebooted. That is how the unfixed firmware behaves under the same load.
- **Headroom under overload is 23 KB, not generous.** A fourth peer costs about 4.5 KB more, and a busy Wi-Fi AP client could take up to 38 KB of driver buffers. If that proves too tight, the levers are a 256-session NAT table (8 KB), a smaller HaLow event-loop stack (8 KB; it uses about 5 KB of its 24 KB) and fewer Wi-Fi dynamic buffers. None was applied.
- **Bridge mode limits that hold:** a NAT-mode Warthog cannot open a connection to a host behind a bridged one (measured 0/3; the host must speak first), and a bridged node does not relay for others, so it needs a direct peer with a DHCP server.
- **H5 is half done by design.** An open node still sends peering Opens toward an SAE node that shares its mesh ID (Part B of the design), and the no-peers diagnosis can still blame the channel when the cause is a mesh-ID mismatch.
- **`AT+MPING` to an on-link address that does not answer ARP** can report `1 sent` or `0 sent`: lwIP's ARP queue refuses probes after the first few. The old code counted the same way.
- **Not measured:** anything with a 2.4 GHz Wi-Fi client (the AP as a bridge port, PSRAM-backed frames handed to the Wi-Fi driver), a phone or a Linux or Windows host, the station uplink, a fourth peer, a board whose PSRAM does not answer, and CI itself: the new tests ran only on macOS, and Ubuntu with gcc has not built them (a reviewer read them for portability and found nothing).
- **The Pis were not inspected from their side** this time: `ssh root@10.41.254.1` now fails host-key verification against the entry in `known_hosts`, and that was left alone.
