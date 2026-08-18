# Client Mode — a 2.4 GHz AP for everything else

Not everything can take a USB host. Warthog runs a 2.4 GHz Wi-Fi access point
beside the USB surface so phones, cameras and IoT clients reach the same uplink.

**Still the simplest path for phones and tablets**, though no longer the only
one: warthog's USB surface is now CDC-NCM, which iOS and iPadOS bind natively
(see [Host Mode](Host-Mode)). Wi-Fi needs no cable, no OTG adapter, and does not
ask the phone to power the board.

Both surfaces are live simultaneously and share the uplink through NAPT.

## Defaults

| | |
|---|---|
| SSID | `warthog` |
| Passphrase | `warthog-default` |
| Warthog | `192.168.5.1` |
| Clients | `192.168.5.2` and up, by DHCP |
| Netmask | `/24` |

**Change the passphrase before deploying anything.** The default is public
knowledge — it is in this page.

## Reconfiguring at runtime

```
AT+WIFIAP=mynetwork,a-better-secret,11
AT+WIFIAP?
+WIFIAP: ssid="mynetwork" chan=11 (psk hidden)
OK
```

Arguments are SSID, passphrase, channel. Stored in NVS, so it survives reboots
and outranks the build-time default.

## Build-time defaults

| Macro | Default |
|---|---|
| `WARTHOG_AP_SSID` | `warthog` |
| `WARTHOG_AP_PSK` | `warthog-default` |
| `WARTHOG_AP_GW_IP` | `192.168.5.1` |
| `WARTHOG_AP_NETMASK` | `255.255.255.0` |

## Verifying

Associate a phone, then from it:

- gets `192.168.5.2`
- `ping 192.168.5.1` succeeds
- traffic to the wider network resolves and routes

If association works but nothing routes, check DNS first — see
[Host Mode](Host-Mode#dns). The same DHCP-offered resolver serves both surfaces
and `AT+DNS=` changes it for both.
