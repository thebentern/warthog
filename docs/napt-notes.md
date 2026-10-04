# ESP-IDF lwIP NAPT — Which Netif Gets `napt = 1`

**Date:** 2026-05-22
**TL;DR:** ESP-IDF's lwIP NAPT model is inverted from the conventional "WAN-facing NAT" mental model. Enable NAPT on the **inside** netifs (where clients live), not the **outside** netif (the uplink). Set the outside as the default route. Use `ip_napt_enable_netif()` directly to NAPT more than one inside netif — `esp_netif_napt_enable()` enforces a single-netif exclusivity check that blocks multi-inside configs.

## Symptom

Mac → Warthog (USB ECM) → HaLow → HaLowLink2 → TMO → internet. With NAPT enabled on the HaLow STA netif via `esp_netif_napt_enable()`:

- Local Mac↔Warthog ping works.
- Anything past the Warthog dies — ICMP, TCP, HTTPS all time out.
- `tick` log shows `napt_set=1 default=WIFI_STA_DEF`. Firmware-side state looks correct.
- The HaLowLink2 hands the Warthog a DHCP lease, so L2 bidirectional unicast over HaLow works.

## Root cause — verified by tcpdump on the upstream

`ssh root@192.168.12.1` then `tcpdump -ni any -e '(host 192.168.12.160 or host 192.168.4.2) and not port 22'` while pinging `8.8.8.8` from the Mac via the bridge:

```
wlan0 P  a8:dd:9f:4d:c7:f8 ... 192.168.4.2 > 8.8.8.8: ICMP echo request
br-lan In a8:dd:9f:4d:c7:f8 ... 192.168.4.2 > 8.8.8.8: ICMP echo request
wan   Out 94:83:c4:82:72:ef ... 8.8.8.8 > 192.168.4.2: ICMP echo reply  ← punted to TMO again
```

Source IP is **`192.168.4.2`** (the Mac, untranslated) — not `192.168.12.160` (the Warthog's HaLow IP). NAPT was a no-op. The HaLowLink2 NATs the outbound through its own masquerade, but on the return path it has no route for `192.168.4.0/24` and ships the reply back out the WAN. Loop is broken.

## What the ESP-IDF lwIP code actually does

`components/lwip/lwip/src/core/ipv4/ip4.c:334`:

```c
#if ESP_LWIP
#if IP_NAPT
  /* If the output netif uses NAPT, we will not perform NAPT forwarding ... */
  if (!netif->napt) {
    if (ip_napt_forward(p, iphdr, inp, netif) != ERR_OK)
      return;
  }
#endif
#endif
```

And `ip4_napt.c:869`:

```c
err_t ip_napt_forward(struct pbuf *p, struct ip_hdr *iphdr, struct netif *inp, struct netif *outp)
{
  if (!inp->napt)
    return ERR_OK;                       /* skip — only NAPT when INPUT netif is flagged */
  ...
  ip_napt_modify_addr(iphdr, &iphdr->src, ip_2_ip4(&outp->ip_addr)->addr);
                                          /* rewrite src to OUTPUT netif's IP */
}
```

**The `napt = 1` flag must be on the *input* (inside) netif** for the forwarder to translate, and the source is rewritten to the *output* (outside) netif's IP. The canonical IDF example confirms this — `examples/wifi/softap_sta/main/softap_sta.c:253` calls `esp_netif_napt_enable(esp_netif_ap)` on the AP (inside), with the STA (outside) as the default route.

## Fix

In `main/nat.c`:

- **Inside** netifs (`USB`, `WIFI_AP_DEF`) get `napt = 1` via `ip_napt_enable_netif()`.
- **Outside** netif (`WIFI_STA_DEF` — HaLow) is set as default route via `esp_netif_set_default_netif()` and does **not** get `napt = 1`.
- `esp_netif_napt_enable()` won't work for the two-inside-netif case — its internal exclusivity check (`/* Check if other interfaces are up, NAPT is exclusive to one interface */`) rejects the second call. `ip_napt_enable_netif()` from `lwip/lwip_napt.h` is the bypass.

Acquired the underlying `struct netif *` via `esp_netif_get_netif_impl()` (declared in `esp_netif_net_stack.h`, used the same way in `examples/network/vlan_support/main/vlan_support_main.c`).

A polling supervisor task re-enforces the state every 2 s — `esp_netif`'s priority-driven default-netif auto-reselection can revert our explicit `set_default_netif()` whenever the wrong-direction `IP_EVENT_STA_GOT_IP` event fires on another netif, and `ip_napt_enable_netif()` is idempotent so retries are cheap.

## Verification

`tick` log after the fix should show:

```
warthog.nat: NAPT on inside netifs (usb=1 ap=1); HaLow default route: ip=192.168.12.160 gw=192.168.12.1
warthog.nat: tick: halow_ip=192.168.12.160 usb.napt=1 ap.napt=1 default=WIFI_STA_DEF
```

And the upstream tcpdump should show source `192.168.12.160` (translated) instead of `192.168.4.2`.

## Fragments

`ip_napt_recv()` and `ip_napt_forward()` read the transport header at the IP header's end of every packet. A fragment after the first has none there, only payload; and `ip4_input()` calls `ip_napt_recv()` before it reassembles, while forwarded packets are never reassembled. With stock NAPT, from IDF 5.5.4's lwIP and a host run of it (`test_lwip_napt_frag`):

- out (tethered host to HaLow): ICMP fragments after the first keep the host's source address; UDP ones have 8 payload bytes taken for a UDP header, rewritten or dropped.
- in (HaLow to the tethered host): only the first fragment is translated; the rest are addressed to the Warthog and held until the reassembly timer drops them.
- a first TCP fragment shorter than a TCP header: NAPT reads past the end of the pbuf (ASan, heap-buffer-overflow).

`main/nat_frag.c` is lwIP's IPv4 input hook (`LWIP_HOOK_IP4_INPUT`, given to the lwip component as `ESP_IDF_LWIP_HOOK_FILENAME` by `main/CMakeLists.txt`). It reassembles, with `ip4_reass()`, every fragment addressed to one of the Warthog's own addresses, in any mode, or to that of the netif without NAPT it came in on while that netif is down (those `ip_napt_recv()` reads among them), and every one in on a NAPT netif routed out one without (`ip_napt_forward()`), and hands the whole datagram back to `ip4_input()` through the tcpip mailbox; lwIP cuts it again for the outgoing MTU. Each fragment is held as a heap copy with room for a link header (`pbuf_clone()`), so the driver's buffer goes back at once and an echo reply reuses the copies. Fragments to a broadcast or multicast address are dropped. Fragments between two NAPT netifs, and others not for the Warthog, are forwarded as they came. A reassembled datagram whose first fragment does not hold its ports (and TCP flags) is dropped. Counts: `AT+MTU?` `ip_reass`, `ip_reass_drop`. Needs `CONFIG_LWIP_IP4_REASSEMBLY` (`sdkconfig.defaults`).

Measured on air 2026-10-03, both boards: a Mac on the USB link pinged an OpenMANET Pi through NAPT with 100, 1472, 1473, 2000 and 6000 bytes, 3/3 each (one 6000-byte run 2/3), `ip_reass` rising both ways, `ip_reass_drop` 0; the Pi, at MTU 1460 and at 1500, pinged the Warthog with up to 14392 bytes (10 fragments), 5/5.

## Short packets

`ip_napt_recv()` and `ip_napt_forward()` read a whole packet's transport header without checking its length: ports, TCP sequence number and flags, ICMP echo id. On a session match they rewrite the port and the checksum (TCP bytes 16-17, UDP 6-7). A TCP, UDP or ICMP packet shorter than its header is therefore read, and on a match written, past its end; any node on the mesh or a host on USB or the access point can send one. Host run of IDF 5.5.4's lwIP (`test_lwip_napt_frag`, buffers exactly the packet's size): ASan reports a heap-buffer-overflow in `ip_napt_modify_port_udp()`, and the plain build crashes after a TCP session match.

The input hook drops such a packet first: not a fragment, TCP, UDP or ICMP, its IP length or first pbuf ending inside the 20-, 8- or 8-byte header, and one NAPT reads:

- addressed to the address of the netif without NAPT it came in on. This is `ip4_input()`'s whole test for calling `ip_napt_recv()`, so it holds while that netif is down, at 0.0.0.0 or at another netif's subnet broadcast. A lost DHCP lease and batman mode's probe leave the HaLow netif up at 0.0.0.0 with NAPT and its sessions kept (TCP ones 30 minutes), so a packet to 0.0.0.0 that matches a session is rewritten.
- addressed to any other address of the Warthog.
- in on a NAPT netif and routed out one without (`ip_napt_forward()`).

A UDP packet without its destination port (under 4 bytes of header) is dropped wherever it goes: when no netif takes a UDP packet, `ip4_input()` reads that port for DHCP (`IP_ACCEPT_LINK_LAYER_ADDRESSING`, on with `LWIP_DHCP`). Addressed to the Warthog, lwIP's own TCP, UDP and ICMP input would drop all of these anyway. Other short packets are forwarded as they came. A packet that holds its header costs a protocol and a length test; only short ones are classified (one address compare, then netif addresses, then a route). Count: `AT+MTU?` `ip_short_drop`. Host-tested on the package's lwIP built with DHCP as the firmware's, plain and under ASan in CI; not measured on a board.

## References

- ESP-IDF lwIP NAPT: `components/lwip/lwip/src/core/ipv4/ip4_napt.c`
- ESP-IDF lwIP forwarder: `components/lwip/lwip/src/core/ipv4/ip4.c` (around line 334)
- Public API: `components/lwip/lwip/src/include/lwip/lwip_napt.h`
- Canonical example: `examples/wifi/softap_sta/main/softap_sta.c`
- VLAN/lwip_netif example: `examples/network/vlan_support/main/vlan_support_main.c`
