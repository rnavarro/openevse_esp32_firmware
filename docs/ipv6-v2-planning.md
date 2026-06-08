# IPv6 v2 Planning — onto core-3 / IDF5 / 16MB

Status: planning (2026-06-08). Companion to `ipv6-implementation-plan.md` (which covers v1 — the shipped core-2/IDF4 implementation). This doc plans the *next* phase: landing our IPv6 work on the upstream next-release base (Arduino-ESP32 **core-3.x / ESP-IDF 5**), where the v1 hacks delete themselves and the flash-headroom squeeze disappears.

Nothing here is started. Per operator decision (2026-06-08) we **wait and see how the next release lands** before rebasing; this is the map for when we do.

## Thesis (one paragraph)

v1 works and is deployed (both 16MB chargers live on dual-stack, IPv6 as the active MQTT transport — see `ipv6-implementation-plan.md`). But v1 rides three core-2/IDF4 workarounds (custom RDNSS `liblwip.a`, the mDNS slot-0 swap, and the loopTask-watchdog guard around a blocking `getaddrinfo`) and sits at ~94.5% of a 1.88MB app slot. The upstream next release introduces a **two-core build** where 16MB boards move to **core-3 / IDF5**, which natively provides RDNSS + global-AAAA mDNS (deleting two hacks) and a **6.55MB app slot** (deleting the headroom problem). Our chargers are 16MB → eligible. So v2 = **rebase our IPv6 work onto the core-3 `openevse_wifi_v1_16mb` target**, shedding the hacks. The single hard dependency is reconciling two divergent `ArduinoMongoose` forks (ours = IPv6, RAR's = IDF5).

## The upstream next-release landscape (as of 2026-06-08)

### Who

- **chris1howell (Chris Howell)** — active maintainer, orchestrating the release, merges substantive code. Confirmed on Discord: *"We have a bunch of branches on openevse_esp32_firmware active with PRs in. They start with master and build from PR #1091."*
- **RAR / RARNC (Andrew Rankin, @Validic)** — the **de-facto lead for the next release**. His modernization work (#1090 umbrella, peeled into #1091–#1098) is the spine of the release; chris is building on it and shipping his new GUI. NOTE: this **corrects an earlier (wrong) assessment** in `ipv6-implementation-plan.md` that called RAR an unsanctioned outsider — at the time he had `author_association = NONE`, zero merged PRs, and no Discord presence, but chris has since confirmed the release builds from his #1091 and ships his GUI. Treat RAR as the lead collaborator to coordinate with.
- **jeremypoulter / _Jez_ (Jeremy Poulter)** — historical maintainer, now mostly deps/maintenance; owns `jeremypoulter/ArduinoMongoose` and `jeremypoulter/ESPAL` (the registry homes our and RAR's forks must fold back into).

### chris's stated roadmap (Discord, 2026-06-08)

A **big-bang release**, not incremental: safety-controller dev **9.0.0** (universal EU/US, features toggled by RAPI commands), many new RAPI commands, a **new GUI from RARNC with local energy monitoring**, power/temperature graphs, day/month/year charts, dark/light themes. chris also wants to add IPv4 Static IP / Netmask / Gateway / DNS and asked if we'd done v4 work (we said no; offered a follow-up round).

### #1091 — the foundation everything builds from

`feature/core3-two-core-build` (RAR, **ready, not draft**, 11 files, +344/-29). The load-bearing change:

- Adds a **pioarduino** `platform-espressif32` (Arduino-ESP32 3.x / IDF5) **alongside** core-2.x, from one tree. **No existing core-2 board changes** — IDF5 compat fixes are version-guarded (`ESP_ARDUINO_VERSION`, `MBEDTLS_VERSION_NUMBER`), so core-2 output is byte-for-byte unchanged.
- Adds **`[env:openevse_wifi_v1_16mb]`** — our chargers' feature set (PN532 + MCP9808 + NeoPixel) on core-3 with a **16MB partition table: 6.55MB app slot, validated at 31.8% used (2.09MB)**.
- `scripts/pio` wrapper isolates the two cores' package caches locally; `[env:native]` doctest harness (`pio test -e native`); `http_update.cpp` rejects oversized OTA images before erasing the partition; CI matrix + native test job; `docs/building.md`.
- **Forks `ArduinoMongoose` + `ESPAL`** at git pins carrying IDF5/core-3 fixes (to be folded back into `jeremypoulter/*` before merge).

### The PR stack (all base `master`, conceptually stacked on #1091)

| PR | branch | author | what | size |
|----|--------|--------|------|------|
| #1089 | `NTP_Fixes` | chris1howell | NTP reliability re-do (stuck `_fetchingTime`, stale Mongoose `_nc`, sntp_hostname handler, manual sync, status panel). The narrower successor to the reverted #1087. | 22f |
| #1090 | `feature/esp32-modernization` | RAR | **WIP umbrella, not for merge** — 132 commits, 8 groups (core-3, ESP32-P4 touchscreen, security, Home Assistant, energy/CSV, FakeEVSE, tsdb, more boards) | 172f |
| #1091 | `feature/core3-two-core-build` | RAR | the two-core foundation (above) | 11f |
| #1092 | `feature/gui-nightshift-default` | RAR | the new GUI (pure `web_static`/`gui-v2`) | 37f |
| #1093 | `feature/tls-hw-rng` | RAR | replace dummy TLS RNG with HW RNG (mbedTLS 3.x) | 2f |
| #1094 | `feature/energy-logging-1083` | RAR | energy logging + temp throttle (all boards) + tsdb history (core-3 16MB) — **stacked on #1091** | 45f |
| #1096 | `feature/display-feeds` | RAR | home-battery + vehicle plugged/charging state feeds | 8f |
| #1097/#1098 | `feature/vehicle-charge-limit[-mqtt]` | RAR | vehicle charge limit over the dataset / MQTT | 3f/6f |
| #1038 | `copilot/add-ipv6-support` | bot | dead 28-line link-local-only IPv6 stub — supersede, ignore | 4f |

## What this means for our IPv6 work

### Target: `openevse_wifi_v1_16mb` (core-3)

This is where v2 lives. On it we get, for free:
- **6.55MB app slot** (vs our 1.88MB) — the headroom problem is gone.
- **Native RDNSS** (IDF5 lwIP config) — the custom `liblwip.a` blob in `custom_libs/` **deletes**.
- **Native global-AAAA mDNS** (IDF5 mdns reads all v6 slots) — the **slot-0 swap deletes**, and with it the source-selection side effect that forced the watchdog guard.
- Our chargers are 16MB → eligible. The 4MB Olimex bench unit stays core-2 (the v1 implementation remains correct for it).

### The integration crux: the ArduinoMongoose (+ ESPAL) double-fork

**This is the one hard dependency.** Both efforts fork `ArduinoMongoose`:
- **Ours:** `rnavarro/ArduinoMongoose#fix/ipv6-dual-stack` — IPv6 dual-stack listener/connect patches (against the older core-2 ArduinoMongoose).
- **RAR's:** a git-pinned fork carrying **IDF5/core-3** fixes.

For v2, ArduinoMongoose must carry **both** patch sets. Options: port our IPv6 patches onto RAR's IDF5 fork, or fold both into `jeremypoulter/ArduinoMongoose` as a tagged release. This is the highest-leverage coordination point with RAR. (ESPAL is also double-relevant but lighter.)

### Conflict surface (rebasing v1's diff onto the next-release base)

| Our file | Upstream PRs touching it | Risk |
|----------|--------------------------|------|
| `platformio.ini` | #1091 (two-core restructure), #1090, #1094 | **High** — re-fit our `MG_ENABLE_IPV6` / ArduinoMongoose dep / `override_lwip` extra_script / `custom_libs` / env wiring into the two-core layout. On core-3 several of these *go away* (native RDNSS → drop `override_lwip`/`custom_libs`). |
| `mqtt.cpp`/`.h` | #1094, #1096, #1098 | **Medium-high** — features edit the same file as our watchdog guard + IPv6 DNS path |
| `web_server.cpp` | #1094, #1096, #1098 | **Medium** — handlers near our `/debug/ipv6` route |
| `app_config.cpp/.h` | #1089, #1090, #1094, #1096, #1098 | Low for us (we barely touch it) |
| `net_manager.cpp/.h` | none in the active stack | **Low — good.** Our biggest IPv6 file lands clean |
| `ipv6_select.h`, `test/` | none (new files) | None — and the selector is a candidate to move into `pio test -e native` (see below) |

So the rebase is bounded: build config + two shared files. The core IPv6 logic (`net_manager.cpp`, the selector) is conflict-free.

### Does the watchdog fix still apply on core-3?

Revisit. Our `731c0ae` exempts loopTask from the task watchdog around a **synchronous `getaddrinfo`** in the `#if MG_ENABLE_IPV6` cold-resolve path. On core-3 the DNS/source-selection situation differs (native RDNSS, no slot-0 swap → no non-routable address winning slot 0), so the *trigger* may not exist. Keep the guard until proven unnecessary on core-3, then decide whether to drop it. It's cheap insurance regardless.

## Overlaps & opportunities with RAR's work

- **mDNS netif-timing fix (RAR, #1090):** *"mDNS started on netif-up rather than at boot (null-netif boot crash)."* Adjacent to our mDNS work (slot-0 swap + the boot-loop we fixed). On core-3 our slot-0 swap deletes, but his netif-*timing* fix may still be the right startup model — compare notes before re-implementing.
- **Native test harness (`pio test -e native`, #1091):** RAR added a doctest-based native env. Our standalone `test/test_ipv6_select.cpp` + `run_host_tests.sh` (plain g++) is a natural candidate to **fold into `pio test -e native`** so the selector test runs in their CI matrix.
- **FakeEVSE (`-DFAKE_EVSE`, `/fakeevse`, #1090 group 6):** runs the firmware with **no EVSE hardware attached**, driving charge states. Plus the **OpenEVSE_EV_Simulator** (Jez's dev-board tip) for mode switching. Both let us exercise charge sessions (state 254→3) during IPv6 testing without a vehicle.
- **`http_update.cpp` OTA size guard (#1091):** rejects oversized images before erasing the partition — a safety net relevant to our OTA flashing.

## Headroom / partition reality

- No upstream plan to change the **core-2** partition (Discord search, 2026-06-08 — zero partition-change discussion). The GUI is **embedded in the app image** (per _Jez_), so the new GUI grows the app; #1094 already builds core-2 `openevse_wifi_v1` at **94.8%**, and we're at 94.5% with IPv6 — the two together would blow the 4MB slot.
- The release's implicit answer is **core-3 16MB** (tsdb/energy-history features are 16MB-gated; `openevse_wifi_v1_16mb` has 6.55MB). So headroom is solved *by the migration*, not by repartitioning core-2.
- Repartitioning a *deployed* unit (core-2 → 16MB table) is **not OTA-able** (changes slot offsets) → needs a serial/full flash. But moving to the core-3 `openevse_wifi_v1_16mb` build is itself a full image change; plan a hands-on (or carefully staged) flash for the chargers when the time comes.

## Open questions / next digging

1. **Compare the two ArduinoMongoose forks** — diff RAR's IDF5 fork against ours (`fix/ipv6-dual-stack`) to scope the IPv6+IDF5 merge. (Highest priority for v2.)
2. **RAR's mDNS netif-timing fix** — read it; decide if it replaces/augments our startup handling on core-3.
3. **IPv6 on core-3 reality check** — confirm IDF5 lwIP RDNSS config + mdns global-AAAA actually behave as expected on `openevse_wifi_v1_16mb` (don't assume; the v1 work taught us to verify on metal).
4. **IPv4 static-IP feature** chris wants — a possible goodwill contribution / follow-up round (we declined v4 focus for now).
5. **Does the watchdog/getaddrinfo trigger exist on core-3?** (see above).

## Recommended v2 sequencing

0. **Wait** for the next release to stabilize (operator decision). Track #1091 + the stack; coordinate with RAR/chris.
1. **Reconcile ArduinoMongoose** — get IPv6 + IDF5 patches into one fork/release (the hard dependency).
2. **Rebase** v1's IPv6 logic onto the #1091 base, targeting `openevse_wifi_v1_16mb`.
3. **Delete the hacks** that core-3 makes unnecessary: `custom_libs/liblwip.a` + `override_lwip.py` (native RDNSS), the mDNS slot-0 swap (native global-AAAA). Keep the watchdog guard until proven moot.
4. **Re-validate on metal** (the v1 lesson): v6-only, renumber, mDNS AAAA, MQTT-over-v6, on a 16MB core-3 build. Use FakeEVSE/EV-Simulator to exercise sessions.
5. **Fold the selector test** into `pio test -e native`.
6. Re-flash the chargers to the core-3 16MB build (plan the flash method — likely not a simple OTA).

## Correction log

- **RAR reassessment (2026-06-08):** earlier notes treated RAR's core-3 work as an unsanctioned external proposal "not the project's direction." chris1howell has since confirmed the next release builds from RAR's #1091 and ships his GUI. RAR is the de-facto next-release lead; coordinate accordingly. (`ipv6-implementation-plan.md` Upstreaming section should be read with this correction in mind.)
