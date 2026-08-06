# Power Delivery — HaLow Association Brownout

**Date:** 2026-05-21
**Hardware:** XIAO ESP32-S3 + Seeed Wi-Fi HaLow add-on (`WI-FI_HALOW_FGH100M_EXT01_V30`, Quectel FGH100M-H / Morse Micro MM6108)
**Outcome:** Boot loop at HaLow association is a power-delivery limitation of the add-on board, not a firmware bug. Fix is a bulk capacitor on the 5 V rail.

## Symptom

With valid HaLow credentials set, the device reboots every ~13 s in a loop:

```
warthog.halow: associating with SSID 'halowlink2-72ef' (SAE)
Morse Micro HaLow NetIF: Attempting to connect to: halowlink2-72ef
<reset>
warthog: last reset reason: POWERON (1)
```

Reproduced identically on two separate XIAO units — not a defective board.

## Diagnosis — ruled out by elimination

A diagnostic build (`CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT=y` + an `esp_register_shutdown_handler` backtrace hook) was flashed to make any software fault impossible to miss:

| Hypothesis | Ruled out by |
|------------|--------------|
| Software crash (exception / `INT_WDT`) | `PANIC_PRINT_HALT` would freeze with a backtrace — never fired |
| Clean `esp_restart()` | shutdown-handler backtrace would print — never fired |
| Detected brownout | no "Brownout detector triggered" line; reset reason is `POWERON`, not `BROWNOUT` |
| Defective unit | second XIAO fails identically |

`POWERON` every cycle means the chip physically lost power — the rail collapsed faster than the brownout detector could react. Not a crash.

## Hardware findings (board schematic)

The add-on has **no onboard voltage regulator**. It passes the XIAO's rails straight through two RF ferrite beads (HCB1608KF-181T20, ~0 Ω at DC):

```
XIAO 3V3 ──[FB1]── MOD_3V3 ── FGH100M-H VBAT      bulk: C7 = 10 uF
XIAO 5V  ──[FB2]── MOD_5V  ── FGH100M-H VDD_FEM   bulk: C3 + C6 = 20 uF
```

The module is dual-rail:
- **VBAT** (radio + digital core, 3.0–3.6 V) → `MOD_3V3` → XIAO 3V3.
- **VDD_FEM** (module pin 4 — the RF power-amplifier front end) → `MOD_5V` → XIAO 5 V pin → **USB VBUS**.

The PA — the heavy TX-current consumer — is on the **5 V rail**, with only 20 µF of local bulk, fed from USB VBUS through the XIAO. The FGH100M-H is rated to 27 dBm output.

## Mechanism

```
PA energizes for SAE association
  → current spike on MOD_5V (20 uF local reservoir only)
  → USB VBUS sags
  → XIAO 3V3 LDO loses input headroom, drops out
  → 3V3 collapses
  → ESP32-S3 POWERON reset
```

## Fix

1. **Bulk cap on the 5 V rail** — 470–1000 µF, low-ESR, ≥16 V. In parallel with C3/C6 near module pin 4, or across the XIAO 5V↔GND pins. This is the primary fix: a local reservoir for the PA turn-on inrush.
2. **Stiff 5 V source** — short thick cable, strong port / powered hub, or external 5 V into the XIAO 5V pin. The inrush path *is* the 5 V/VBUS path, so this helps directly.
3. A 3V3 cap is secondary — holding 5 V up keeps the LDO out of dropout.

## Firmware mitigations already in place — and why they are not enough

- `HALOW_MAX_TX_DBM` cap (`halow.c`): PA *supply* current barely tracks RF *output* backoff, and the failure is a turn-on transient regardless of output level. Capping to 8 dBm did not help.
- USB-OTG bring-up sequenced after HaLow link (`main.c`): de-stacks the USB enumeration inrush from the SAE TX burst. Correct hygiene, but the association inrush alone still browns the rail.

There is no firmware lever that shrinks a PA power-up inrush. This is a hardware fix.

## References

- Board schematic: `WI-FI_HALOW_FGH100M_EXT01_V30_SCH_20241107.pdf`
  (https://files.seeedstudio.com/wiki/wifi_halow/res/WI-FI_HALOW_FGH100M_EXT01_V30_SCH_20241107.pdf)
- Seeed wiki: https://wiki.seeedstudio.com/getting_started_with_wifi_halow_module_for_xiao/
- Quectel FGH100M-H: https://www.quectel.com/product/wi-fi-halow-fgh100m-h/
