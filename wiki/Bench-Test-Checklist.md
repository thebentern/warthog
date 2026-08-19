# Bench-test checklist

Bring-up flow for a fresh board. Each step has a clear pass signal — stop and debug if any fails.

## 1. Build + flash

```bash
pio run -e warthog-us -t upload    # hold BOOT, tap RESET, release BOOT first
```

## 2. USB-Serial-JTAG boot logs (~1 s window before USB-OTG takes over)

```bash
scripts/watch.sh
```

✅ Expected: HaLow chip ID `0x0306`, BCF version, factory MAC printed within ~1 s.

## 3. USB CDC-ACM console comes up after USB-OTG handoff

`watch.sh` automatically reattaches to the new `/dev/cu.usbmodemXXXX` port.

✅ Expected: stable port (no cycling back to the JTAG `usbmodem101`). LED enters the HaLow-state pattern with a 100 ms wink every 2 s once macOS opens the CDC.

## 4. USB Ethernet adapter

```bash
# en10 is an example -- yours will differ. Find it with:
#   networksetup -listallhardwareports | grep -A1 Warthog
sudo ipconfig set en10 DHCP && sleep 3
ifconfig en10 | grep "inet "
ping -c 3 192.168.4.1
```

✅ Expected: `inet 192.168.4.2`, three echo replies, <5 ms RTT.

## 5. 2.4 GHz Wi-Fi AP

Phone → Settings → Wi-Fi → look for `warthog`. Default PSK is `warthog-default`.

✅ Expected: associates, gets `192.168.5.2`, can `ping 192.168.5.1`.

## 6. AT command surface

In a serial terminal on `/dev/cu.usbmodemXXXX`:

```
AT
AT+STATUS?
AT+VERSION?
```

✅ Expected: each replies `OK`. `STATUS?` shows real IPs for USB + AP, zeros for HaLow if no AP is associated.

## 7. HaLow STA (requires upstream HaLow AP)

```
AT+HALOW=YourHaLowAP,yourpsk
AT+RESET
```

On reboot, watch the CDC console for `warthog.halow: DHCP lease: ip=...`.

✅ Expected: HaLow STA gets a DHCP lease from the upstream AP within ~30 s.

## 8. NAT bridge (depends on step 7)

From the USB-attached laptop or the Wi-Fi-AP-attached phone:

```bash
ping -c 3 -b en10 8.8.8.8       # IP-only — proves L3 forwarding + NAPT
curl -s -o /dev/null -w "%{http_code}\n" https://example.com   # proves DNS too
```

✅ Expected: three echo replies (~90 ms each — HaLow RTT), then `200` from the `curl`. CDC console shows `warthog.nat: NAPT up on HaLow STA`.

⚠️ If `ping` works but `curl` hangs, the DHCP-DNS option isn't reaching the host — see [Troubleshooting](#ping-8888-works-but-curl-examplecom-hangs).

## 9. Mesh (mesh build only, needs a second node)

Flash `warthog-mesh-smoke` to two boards, or one board plus an OpenMANET node.
After ~30 s:

```
AT+MPMPEERS?
```

✅ Expected: each peer listed with `estab=1` and a non-zero `plid`. Then:

```
AT+MPING=<peer's 10.77.x.y>,4
```

✅ Expected: four replies. Against an OpenMANET peer, `AT+MESHSEC=0` first
— stock OpenMANET is unencrypted. See [OpenMANET Gateway](OpenMANET-Gateway).

## 10. Encrypted mesh — SAE/AMPE (two boards)

Flash `warthog-mesh-sae` to two boards (same build → same passphrase). After
~30 s, on either board:

```
AT+SAERX?
AT+MPMPEERS?
AT+KEYINST?
```

✅ Expected:
- `AT+SAERX?` shows `ESTAB=1` and low single-digit `act=`/`rxact=` counts —
  peering completes in one Open/Confirm exchange, not a retransmit storm.
- `AT+MPMPEERS?` shows `ampe_mtk=1 ampe_mgtk=1` — AMPE-derived keys installed.
- `AT+KEYINST?` shows `n=2`: a pairwise key on the peer's AID and the group
  key on AID 0.

```
AT+MPING=<peer's 10.77.x.y>,8
```

✅ Expected: 8/8 replies over the CCMP-encrypted link (bench: ~16 ms RTT).

If an open-mesh node shares the Mesh ID (an OpenMANET box in stock config),
the SAE boards must still key up beside it, and `AT+SAERX?` must show
`status=0` / `rxfail_sa=000000` — no failure statuses from the open node,
because it is never offered as a candidate.

