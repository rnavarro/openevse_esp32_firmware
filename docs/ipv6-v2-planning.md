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

- Adds a **pioarduino** `platform-espressif32` **alongside** core-2.x, from one tree. Exact pins (from #1091's `platformio.ini`, confirmed 2026-06-08): core-2 default is `espressif32@6.12.0` (Arduino-ESP32 2.0.17 / IDF 4.4.x); core-3 is pioarduino `55.03.38-1` = **Arduino-ESP32 3.3.8 / ESP-IDF 5.5.4**. **No existing core-2 board changes** — IDF5 compat fixes are version-guarded (`ESP_ARDUINO_VERSION`, `MBEDTLS_VERSION_NUMBER`), so core-2 output is byte-for-byte unchanged.
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

### The ArduinoMongoose double-fork — investigated 2026-06-08

Both efforts fork `ArduinoMongoose`. I diffed both forks against their common base to scope the merge. **Result: trivial at the merge-conflict level. The real residual is IDF5 build/behavior verification of our IPv6 patches, not a fork conflict.**

The two forks (both off the same `jeremypoulter/ArduinoMongoose` merge-base `82a6f3b9`):

| Fork | commits | files | what |
|------|---------|-------|------|
| **Ours** (`rnavarro/ArduinoMongoose#fix/ipv6-dual-stack`) | 5 | `src/mongoose.c` (+201/-37), `MongooseCore.cpp` (+56/-6), `MongooseMqttClient.cpp` (+1/-0), `MongooseMqttClient.h` (+8/-1) | IPv6 dual-stack: AAAA-first DNS w/ A fallback, IPv6 nameserver + UDP sendto fix, dual-stack socket path, `setTlsServerName` SNI for pre-resolved-IP connects |
| **RAR's** (`RAR/ArduinoMongoose@cf237c4`) | 3 | `src/mongoose.c` (+40/-3) | mbedTLS 3.x API port (`net_sockets.h`, drop `ssl_internal.h`, port SSL interface) — the IDF5 *build* fix |

**They edit disjoint regions of the only shared file (`mongoose.c`), verified against the same base:**
- Ours: lines ~2382–3971 (`mg_parse_address`, `mg_do_connect`, `resolve_cb`, `mg_connect_opt`, socket send / open-listening-socket) + ~12234 (`mg_resolve_async_opt`) — the **DNS resolver + socket/connect** path.
- RAR's: lines ~4879–5272 — entirely inside the **`mg_ssl_if_*` mbedTLS** block.

No overlap, and RAR doesn't touch the other 3 files we edit. So the union is mechanical: apply both commit sets onto the `jeremypoulter` base (or fold both into a tagged `jeremypoulter/ArduinoMongoose` release). There is **no textual merge conflict** to reconcile. The earlier framing of this as "the one hard dependency / highest-leverage coordination point" overstated the *merge* difficulty.

**The real residual (does NOT go away with the fork merge):** our IPv6 patches were written against the **core-2 socket/DNS APIs**; RAR's fork only makes ArduinoMongoose *build* on IDF5 (mbedTLS 3.x). Whether our IPv6 paths *compile and behave* on IDF5's lwIP is unproven — that is open-Q#3 / "verify on metal," and it is the actual v2 work. Clean source text ≠ working IPv6 on IDF5.

**ESPAL is NOT a double-fork.** We use stock `jeremypoulter/ESPAL@0.0.4`; only RAR forks ESPAL (`RAR/ESPAL@f7678a7`) for IDF5. v2 just consumes RAR's ESPAL (or the folded-back `jeremypoulter` release) — nothing of ours to reconcile there.

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

- **mDNS netif-timing fix (RAR) — investigated 2026-06-08, see dedicated subsection below.** Short version: adopt his netif-lifecycle model; it kills a *second, distinct* boot-crash class; it is **not in #1091** (the base we'd rebase onto), so v2 ports it or coordinates to peel it out of the #1090 WIP; and it needs a `GOT_IP6` restart added for our v6 case.
- **Native test harness (`pio test -e native`, #1091):** RAR added a doctest-based native env. Our standalone `test/test_ipv6_select.cpp` + `run_host_tests.sh` (plain g++) is a natural candidate to **fold into `pio test -e native`** so the selector test runs in their CI matrix.
- **FakeEVSE (`-DFAKE_EVSE`, `/fakeevse`, #1090 group 6):** runs the firmware with **no EVSE hardware attached**, driving charge states. Plus the **OpenEVSE_EV_Simulator** (Jez's dev-board tip) for mode switching. Both let us exercise charge sessions (state 254→3) during IPv6 testing without a vehicle.
- **`http_update.cpp` OTA size guard (#1091):** rejects oversized images before erasing the partition — a safety net relevant to our OTA flashing.

## mDNS startup model: RAR's netif-lifecycle vs ours (investigated 2026-06-08)

This is the second half of the dig. I compared how upstream, our v1, and RAR start mDNS.

**Upstream master + our v1 + #1091 all start mDNS at BOOT.** `MDNS.begin()` runs inside `NetManagerTask::begin()` right after `manageState()`, before any interface is guaranteed up (our `net_manager.cpp:888`; #1091 still does this at its line 539).

**RAR's #1090 replaces boot-start with a netif-lifecycle model:**
- New `_mdnsStarted` bool member; mDNS is **intentionally not started at boot**.
- `startMDNS()` is called when a netif is actually up — on STA/ETH **got-IP** (`haveNetworkConnection()`) and on **SoftAP start**. It does a clean `MDNS.end()` then `MDNS.begin()` + re-adds services, so it re-binds to the current interface if already running.
- `stopMDNS()` is called **before teardown** (WiFi stop) so mDNS async handlers never touch a freed/null netif.
- His comment states the failure it fixes: boot-time start makes the mDNS predefined-interface handler join its multicast group on a **null netif → `esp_netif_is_netif_up(NULL)` load fault → even-cadence reboot loop**, seen on the **ESP32-P4 / ESP-Hosted** build.

### This is a SECOND, distinct boot-crash class from ours

Two different v6/mDNS-adjacent boot loops, different root causes:
- **Ours (fixed, `731c0ae`):** a non-routable `3fff` GUA wins lwIP slot 0 (our slot-0 swap promotes it) → outbound source selection picks it → synchronous `getaddrinfo` stalls past the 5s task watchdog → reboot. Fixed by the loopTask watchdog guard in `mqtt.cpp`.
- **RAR's (fixed in #1090):** boot-time `MDNS.begin()` on a null netif → `esp_netif_is_netif_up(NULL)` fault (P4/ESP-Hosted). Fixed by tying mDNS to the netif lifecycle.

They don't overlap; both are worth carrying.

### Two findings that shape the v2 plan

1. **The netif-timing fix is in #1090, NOT #1091.** #1091 — the foundation we'd rebase onto — still has the boot-time `MDNS.begin()` (line 539). #1090 is the WIP umbrella ("not for merge"). So rebasing onto #1091 does **not** inherit the fix; v2 either ports RAR's netif-lifecycle model itself or coordinates with RAR to peel it out of #1090 into a mergeable PR. Either way we want it — it's a clean replacement for both our boot-time start and our raw-`mdns_init`/`mdns_free` re-init.

2. **RAR's `startMDNS()` has no `GOT_IP6` case.** It is wired to v4 `WIFI_STA_GOT_IP` + `ETH_GOT_IP` + SoftAP only (the `GOT_IP6` tokens appear in his code only in the event-name debug map, not as switch cases). So a global v6 address arriving *after* the v4 got-IP would not re-bind mDNS. Whether IDF5 mdns auto-advertises a late-arriving global AAAA on an already-running responder is **unproven — metal-check** (folds into open-Q#3).

### Recommended v2 mDNS shape

Adopt RAR's netif-lifecycle model as the single startup discipline (start on got-IP / SoftAP, stop before teardown, `_mdnsStarted` guard, no boot start). On core-3 with native global-AAAA mDNS, **our slot-0 swap + the `mdns_init`/`mdns_free` re-init in `onGlobalIPv6Acquired()` delete entirely.** The only v6-specific addition is to make mDNS re-bind once the global v6 address is up: add a `GOT_IP6`-triggered `startMDNS()` (one line — `startMDNS()` already does end+begin), *or* metal-confirm IDF5 mdns picks up late v6 addresses without a restart. That single clean addition replaces our entire slot-0 hack.

Note on the watchdog guard: its *trigger* (a non-routable address winning slot 0 via the swap) deletes with the swap, but a genuinely slow resolve could still stall `getaddrinfo` independent of slot-0, so keep the guard as cheap general insurance (consistent with the "does the watchdog fix still apply on core-3?" note above).

## Headroom / partition reality

- No upstream plan to change the **core-2** partition (Discord search, 2026-06-08 — zero partition-change discussion). The GUI is **embedded in the app image** (per _Jez_), so the new GUI grows the app; #1094 already builds core-2 `openevse_wifi_v1` at **94.8%**, and we're at 94.5% with IPv6 — the two together would blow the 4MB slot.
- The release's implicit answer is **core-3 16MB** (tsdb/energy-history features are 16MB-gated; `openevse_wifi_v1_16mb` has 6.55MB). So headroom is solved *by the migration*, not by repartitioning core-2.
- Repartitioning a *deployed* unit (core-2 → 16MB table) is **not OTA-able** (changes slot offsets) → needs a serial/full flash. But moving to the core-3 `openevse_wifi_v1_16mb` build is itself a full image change; plan a hands-on (or carefully staged) flash for the chargers when the time comes.

## Open questions / next digging

1. ~~**Compare the two ArduinoMongoose forks.**~~ **DONE (2026-06-08)** — see "The ArduinoMongoose double-fork" above. Disjoint regions, same base, no merge conflict; ESPAL isn't a double-fork. The residual is IDF5 build/behavior of our IPv6 patches (rolls into #3).
2. ~~**RAR's mDNS netif-timing fix.**~~ **DONE (2026-06-08)** — see "mDNS startup model" above. Adopt his netif-lifecycle model; it's in #1090 not #1091; add a `GOT_IP6` restart.
3. **IPv6 on core-3 reality check (now the top residual)** — the target is **IDF 5.5.4 / Arduino-ESP32 3.3.8** (pioarduino `55.03.38-1`), not an early 5.1 — so native RDNSS + all-slot global-AAAA mdns are mature; the risk narrows to *our* patches. Confirm on metal, on `openevse_wifi_v1_16mb`: (a) our IPv6 ArduinoMongoose patches *compile and behave* on IDF 5.5 lwIP (they were written against core-2 APIs); (b) IDF5 lwIP RDNSS config works natively (drops our `liblwip.a`); (c) IDF5 mdns advertises the global AAAA — and specifically whether an already-running responder picks up a *late-arriving* v6 address without a restart, or whether we need the `GOT_IP6`-triggered `startMDNS()`. Don't assume; the v1 work taught us to verify on metal.
4. **IPv4 static-IP feature** chris wants — a possible goodwill contribution / follow-up round (we declined v4 focus for now).
5. **Does the watchdog/getaddrinfo trigger exist on core-3?** (see above).

## Recommended v2 sequencing

0. **Wait** for the next release to stabilize (operator decision). Track #1091 + the stack; coordinate with RAR/chris.
1. **Combine ArduinoMongoose patch sets** — mechanical now (disjoint regions, same base): apply our 5 IPv6 commits + RAR's 3 mbedTLS commits onto `jeremypoulter/ArduinoMongoose`, or fold both into a tagged release. Coordinate with RAR/Jez on the fold-back. The *verification* (do our IPv6 paths build/behave on IDF5) is step 4, not here.
2. **Rebase** v1's IPv6 logic onto the #1091 base, targeting `openevse_wifi_v1_16mb`.
3. **Delete the hacks** core-3 makes unnecessary and **adopt RAR's mDNS netif-lifecycle model** in their place: drop `custom_libs/liblwip.a` + `override_lwip.py` (native RDNSS); drop the mDNS slot-0 swap + `mdns_init`/`mdns_free` re-init (native global-AAAA); replace boot-time `MDNS.begin()` with `startMDNS()`/`stopMDNS()` on the netif lifecycle (port from #1090 since #1091 lacks it), plus a `GOT_IP6` restart for v6. Keep the watchdog guard until proven moot.
4. **Re-validate on metal** (the v1 lesson): v6-only, renumber, mDNS AAAA, MQTT-over-v6, on a 16MB core-3 build. Use FakeEVSE/EV-Simulator to exercise sessions.
5. **Fold the selector test** into `pio test -e native`.
6. Re-flash the chargers to the core-3 16MB build (plan the flash method — likely not a simple OTA).

## Findings & correction log

- **ArduinoMongoose / mDNS dig (2026-06-08):** downgraded "the one hard dependency" — the two ArduinoMongoose forks edit disjoint `mongoose.c` regions off the same base (no merge conflict), and ESPAL isn't even a double-fork. The real residual is IDF5 build/behavior of our IPv6 patches. Separately, RAR's mDNS netif-lifecycle fix is a clean replacement for our slot-0 hack but lives in #1090 (WIP), not #1091, and lacks a `GOT_IP6` restart. See the two dedicated sections above.
- **RAR reassessment (2026-06-08):** earlier notes treated RAR's core-3 work as an unsanctioned external proposal "not the project's direction." chris1howell has since confirmed the next release builds from RAR's #1091 and ships his GUI. RAR is the de-facto next-release lead; coordinate accordingly. (`ipv6-implementation-plan.md` Upstreaming section should be read with this correction in mind.)
