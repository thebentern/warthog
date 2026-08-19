# Mesh Mode — 802.11s peers, no infrastructure

Instead of associating to an access point, a Warthog node can join an 802.11s
mesh. Every node is a peer. There is nothing to elect, nothing to associate to,
and a node that loses power takes only its own links with it.

This is the mode to use when there is no infrastructure to join — field
deployments, convoys, anything that has to come up on its own.

## Building for mesh

Mesh is chosen at build time, not at runtime, and is mutually exclusive with
station mode:

```bash
pio run -e warthog-mesh-smoke -t upload
```

The env pins the radio to one S1G channel and fixes the mesh identity, so nodes
agree with no configuration at all. **These settings must be identical on every
node in the mesh** — a mismatch is silent, producing no error and no peers:

| Flag | Default |
|---|---|
| `WARTHOG_MESH_ID` | `halowmesh` |
| `WARTHOG_PIN_S1G_CHAN` | `42` |
| `WARTHOG_PIN_S1G_FREQ_HZ` | `923000000` |
| `WARTHOG_PIN_S1G_BW_MHZ` | `2` |
| `WARTHOG_PIN_S1G_GLOBAL_OP_CLASS` | `69` |

Channel 42 at 2 MHz is OpenMANET's default, which is why it is Warthog's — see
[OpenMANET Interop](OpenMANET-Interop).

## Addressing

There is no DHCP on the mesh. Each node derives a static address from its own
MAC:

```
10.77.<mac[4]>.<mac[5]> / 255.255.0.0
```

`3c:1a:cc:4c:83:a5` becomes `10.77.131.165`. The mesh is one flat
`10.77.0.0/16`, so nodes whose third octet differs are still on-link. Ask a node
what it picked with `AT+STATUS?`.

The gateway is the node's own address, so **mesh mode has no upstream route** —
it is a network between peers, not a path to the internet. The address is also
applied on first peer establishment rather than at boot, so a node that has not
peered yet has no mesh address.

## Bringing up a mesh

Flash two or more boards and power them. Peering is automatic. After ~30 s:

```
AT+MPMPEERS?
+MPMPEERS: self=4c83a5 4dc7f8 llid=44921 plid=26523 estab=1 opens=0; ...
```

`estab=1` with a non-zero `plid` means a complete two-way handshake. Then check
data:

```
AT+MPING=10.77.199.248,4
+MPING: reply from 10.77.199.248 seq=1 time=15ms
+MPING: 4 sent, 4 received, 0% loss
```

## Encryption

Warthog has three mesh security levels. Pick one at build time and, for the
legacy shared key, at runtime:

| Mode | Build | What it is |
|---|---|---|
| **SAE/AMPE** | `warthog-mesh-sae` | Real 802.11s security: SAE (Dragonfly, group 19) authentication and AMPE per-link key exchange. This is the mode to use. |
| Shared key | `warthog-mesh-smoke` + `AT+MESHSEC=1` | One hardcoded key baked into every image — obfuscation, not security. Kept for bring-up debugging only. |
| Open | `warthog-mesh-smoke` + `AT+MESHSEC=0` | No keys. Interops with an open OpenMANET mesh. |

### SAE/AMPE

```
pio run -e warthog-mesh-sae -t upload
```

The passphrase defaults to `warthog-mesh` and is set at build time with
`-DWARTHOG_MESH_PASSPHRASE='"your-passphrase"'`. Every node on the mesh needs
the same one.

On boot the node authenticates each SAE peer it discovers (SAE Commit/Confirm,
NIST P-256), then AMPE derives a per-link pairwise key (MTK) and a group key
(MGTK) and installs both in the chip. Peering completes in a single
Open/Confirm exchange and data flows CCMP-encrypted end to end. Verify:

```
AT+SAERX?
+SAERX: ... ESTAB=1 ...
AT+MPMPEERS?
+MPMPEERS: ... ampe_mtk=1 ampe_mgtk=1 ...
AT+KEYINST?
+KEYINST: n=2 [aid=1 pw=1 ...] [aid=0 pw=0 ... hw=1]
```

`ampe_mtk`/`ampe_mgtk` count AMPE-derived keys installed in the chip;
`AT+KEYINST?` shows the pairwise key on the peer's AID and the group key on
AID 0. Bench-measured: peering + keying in one exchange, 8/8 pings at 0% loss,
~16 ms RTT over the keyed link.

A SAE node ignores open-mesh nodes sharing the Mesh ID (and vice versa) — the
Mesh Configuration's Authentication Protocol Identifier must match before a
candidate is even offered to the supplicant. The two security worlds coexist
on air without disturbing each other.

`AT+SAEBRIDGE=0` makes a SAE node deaf to peer candidates (a debugging state);
it defaults on.

### Legacy shared key / open

```
AT+MESHSEC?      → +MESHSEC: 1 (keyed)
AT+MESHSEC=0     → open; re-peers within ~2 s
```

Keyed uses **one hardcoded key, identical on every warthog image** — a counting
sequence, `00 11 22 ... ee ff`. Anyone holding the firmware holds the key.
Against OpenMANET you must run open (or use SAE on both sides — see the
[OpenMANET interop page](OpenMANET-Interop)).

The setting persists in NVS, though a factory flash overwrites that partition,
so a freshly reflashed node is keyed again. It has no effect on a SAE build,
which never installs the hardcoded key.

## How paths work, and why it matters

Worth understanding before debugging a mesh, because the failure is
counter-intuitive.

An 802.11s node will not send a **unicast** frame to a neighbour it has no
*path* to, and a peer link reaching ESTAB does not create one — path discovery
(HWMP) does. Group-addressed frames skip discovery entirely.

So a node that does not answer path requests looks like this: broadcast works,
ARP arrives, unicast never leaves, and every status counter reads healthy.

Warthog participates in both directions — it advertises itself with a path
request every 2 s and answers requests aimed at it:

```
AT+HWMPSTAT?
+HWMPSTAT: rx=234 preq_rx=75 preq_tx=142 prep_rx=159 prep_tx=75 parse_fail=0 not_ours=0
```

`preq_rx` matching `prep_tx` means every request aimed at us was answered.
`parse_fail` should be 0.

## Limits

- **No forwarding.** Warthog answers path requests that target it and ignores
  the rest. Two nodes that cannot hear each other will not relay through a
  Warthog between them.
- SAE/AMPE requires the `warthog-mesh-sae` build; the default smoke build still peers open or with the fixed key.
