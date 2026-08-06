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

## References

- ESP-IDF lwIP NAPT: `components/lwip/lwip/src/core/ipv4/ip4_napt.c`
- ESP-IDF lwIP forwarder: `components/lwip/lwip/src/core/ipv4/ip4.c` (around line 334)
- Public API: `components/lwip/lwip/src/include/lwip/lwip_napt.h`
- Canonical example: `examples/wifi/softap_sta/main/softap_sta.c`
- VLAN/lwip_netif example: `examples/network/vlan_support/main/vlan_support_main.c`
