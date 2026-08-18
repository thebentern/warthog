# Host Mode — USB Ethernet for the machine it is plugged into

Plug a Warthog into a laptop and the laptop gains a USB Ethernet adapter whose
uplink is a kilometre-class HaLow link. No drivers on macOS or Linux; Windows
needs RNDIS, which is not implemented yet.

## What the host sees

Warthog presents **CDC-ECM** and serves DHCP on it.

| | |
|---|---|
| Warthog | `192.168.4.1` |
| Host | `192.168.4.2` and up, by DHCP |
| Netmask | `/24` |
| DNS offered | `1.1.1.1` by default |

Traffic is NAPT'd out the HaLow uplink, so the host does not need a route
installed on the far side.

## Bring-up

macOS:

```bash
sudo ipconfig set en10 DHCP && sleep 3
ifconfig en10 | grep "inet "
ping -c 3 192.168.4.1
```

Linux:

```bash
sudo dhclient enp0s20u1
ping -c 3 192.168.4.1
```

Expect `192.168.4.2`, replies under 5 ms.

## Changing the subnet

Build-time, in `platformio.ini`:

| Macro | Default |
|---|---|
| `WARTHOG_USB_GW_IP` | `192.168.4.1` |
| `WARTHOG_USB_NETMASK` | `255.255.255.0` |

## DNS

The single most common "the internet is broken" cause here is DNS, not routing.
If `ping 8.8.8.8` works and `curl example.com` hangs, the DNS handed out by DHCP
is unreachable from wherever the uplink lands.

Change it at runtime — no rebuild, persists in NVS:

```
AT+DNS=8.8.8.8
AT+DNS?
+DNS: 8.8.8.8
OK
```

Malformed input is rejected without touching NVS. To go back to the build-time
default, `AT+ERASE` clears the whole `warthog` namespace — which also clears
HaLow and AP credentials, so you will re-enter those.

## The console shares the cable

The same USB connection carries a CDC-ACM console alongside the Ethernet
interface, so you can reconfigure the node without unplugging it. See
[AT Command Reference](AT-Command-Reference).
