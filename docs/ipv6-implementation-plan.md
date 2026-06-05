# IPv6 Support Implementation Plan

**Date:** 2026-06-03 (updated 2026-06-04 with consult findings)
**Branch target:** `fix/ipv6-support` (from `master`)
**Goal:** Enable dual-stack IPv4+IPv6 on OpenEVSE ESP32 firmware, test on local hardware, upstream if successful

### Build Target Priority

| Priority | Build Environment | Board | Notes |
|---|---|---|---|
| **P0 — Must test** | `openevse_wifi_v1` | ESP32 WROOM-32 | Both openevse-single (172.16.5.113) and openevse-double (172.16.5.187) run this buildenv |
| **P1 — Must compile** | `openevse_wifi_tft_v1` | ESP32 WROOM-32 + TFT | Next most common variant; must not regress |
| **P2 — Must compile** | `olimex_esp32-gateway-f` | Olimex ESP32-Gateway | ETH+WiFi; used for ETH code path testing during dev |
| **P3 — Best effort** | All other 17 environments | Various | Compile check only, no runtime testing |

---

## Problem Statement

The OpenEVSE ESP32 firmware is IPv4-only. Our local network is heavily IPv6 and we want the EVSE units accessible via IPv6. The ESP32 hardware and LwIP stack support IPv6 natively, but the firmware never enables it and the Mongoose networking library has significant IPv6 gaps.

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│  Application Layer                                       │
│  web_server.cpp · mqtt.cpp · emoncms.cpp · ocpp.cpp     │
│  All use Mongoose for HTTP/MQTT/WebSocket connections    │
└──────────────┬───────────────────────────────────────────┘
               │
┌──────────────▼───────────────────────────────────────────┐
│  ArduinoMongoose 0.0.22 (Mongoose 6.18)                  │
│  HTTP server · MQTT client · HTTP client · WebSocket     │
│  ⚠️ Uses MG_NET_IF_SOCKET path on ESP32                  │
│  ⚠️ MG_ENABLE_IPV6 defaults to 0 on ESP32               │
└──────────────┬───────────────────────────────────────────┘
               │
┌──────────────▼───────────────────────────────────────────┐
│  LwIP POSIX Socket Layer (MG_NET_IF_SOCKET)              │
│  socket() · bind() · listen() · accept() · connect()    │
│  ✅ Dual-stack: AF_INET6 + V6ONLY=0 → IPADDR_TYPE_ANY   │
│  ✅ IPv4 clients handled via mapped addresses            │
└──────────────┬───────────────────────────────────────────┘
               │
┌──────────────▼───────────────────────────────────────────┐
│  ESP-IDF LwIP Stack                                       │
│  ✅ CONFIG_LWIP_IPV6=y (enabled by default)              │
│  ✅ SLAAC via Router Advertisements                       │
│  ✅ socket(AF_INET6) auto-creates IPADDR_TYPE_ANY PCBs  │
│  ✅ RDNSS DNS-from-RA (custom liblwip.a, Phase 0)       │
│  ❌ No stateful DHCPv6 for address assignment             │
└──────────────────────────────────────────────────────────┘
```

## Critical Discovery: ESP32 Uses Socket Path, Not Raw LwIP

The ESP32 platform section in mongoose.h (line 557) unconditionally sets:
```c
#define MG_NET_IF MG_NET_IF_SOCKET
```

**Note:** `MG_ENABLE_IPV6` defaults to **0** for ESP32 (line 3246). The NRF51/NRF52 sections (lines 1155, 1198) set it to 1, but ESP32 does not. The build flag `-D MG_ENABLE_IPV6=1` in platformio.ini is required.

This means Mongoose uses the POSIX socket API (`socket()`, `bind()`, `listen()`, `accept()`) on ESP32, NOT the raw LwIP API (`tcp_new_ip6()`, `tcp_bind_ip6()`, etc.). The raw LwIP code at lines 14800-15762 is compiled but the vtable points to `mg_socket_iface_vtable`.

**This is actually good news.** The LwIP socket layer on ESP32 supports dual-stack out of the box:

| Feature | Socket Path | Raw LwIP Path |
|---|---|---|
| Dual-stack PCB | ✅ `socket(AF_INET6)` → `IPADDR_TYPE_ANY` automatically | ❌ `tcp_new_ip6()` → `IPADDR_TYPE_V6` (IPv6-only) |
| V6ONLY support | ✅ `setsockopt()` works, default=0 (dual-stack) | ❌ Not applicable |
| IPv4 clients | ✅ Via IPv4-mapped addresses (`::ffff:x.x.x.x`) | ❌ Blocked by type mismatch |
| Minimal patch | ~5 lines in core Mongoose | ~20+ lines in LwIP integration |

---

## Key Findings from Research

### ESP32 Layer: Ready (with v2.x API corrections)

The ESP32 Arduino core shipped with `espressif32@6.12.0` is **v2.0.17** (based on ESP-IDF v4.4.7). This is NOT v3.x — the IPv6 API names are different:

| v2.x (what we have) | v3.x (what the plan initially assumed) |
|---|---|
| `WiFi.enableIpV6()` | `WiFi.enableIPv6()` |
| `WiFi.localIPv6()` (link-local only; no global getter) | `WiFi.linkLocalIPv6()` + `WiFi.globalIPv6()` (separate methods) |
| `IPv6Address` (separate class) | Unified `IPAddress` (holds both) |
| `WiFi.softAPenableIpV6()` / `WiFi.softAPIPv6()` | `WiFi.softAPenableIPv6()` |
| No `hasGlobalIPv6()` | `WiFi.hasGlobalIPv6()` |

**Critical: `WiFi.localIPv6()` returns link-local only** — it calls `esp_netif_get_ip6_linklocal()`. There is no Arduino wrapper for global IPv6 in v2.x.

**Getting the global IPv6 address — use event payload, NOT ifkey lookup.** The `ip_event_got_ip6_t` struct carried by `ARDUINO_EVENT_WIFI_STA_GOT_IP6` / `ARDUINO_EVENT_ETH_GOT_IP6` already contains both the `esp_netif_t*` handle AND the IPv6 address. The firmware already stores the full `arduino_event_info_t` in `NetworkEventMessage` (net_manager.h lines 81-89). No `esp_netif_get_handle_from_ifkey()` call is needed — it would use unstable ESP-IDF internal strings (`WIFI_STA_DEF`, `ETH_DEF`) that could break across versions.

```cpp
// In the GOT_IP6 event handler:
case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
{
  // Event payload has the address directly — no ifkey lookup needed
  esp_ip6_addr_t addr = info.got_ip6.ip6_info.ip;
  esp_ip6_addr_type_t addr_type = esp_netif_ip6_get_addr_type(&addr);
  if (addr_type == ESP_IP6_ADDR_IS_LINK_LOCAL) {
    _ipv6address_linklocal = IPv6Address(addr.addr).toString();
    DBUGF("WiFi STA IPv6 link-local: %s", _ipv6address_linklocal.c_str());
  } else if (addr_type == ESP_IP6_ADDR_IS_GLOBAL || addr_type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL) {
    // IS_GLOBAL and IS_UNIQUE_LOCAL (fc00::/7) are SEPARATE enum values — both are routable
    _ipv6address_global = IPv6Address(addr.addr).toString();
    DBUGF("WiFi STA IPv6 global: %s", _ipv6address_global.c_str());
  }
  Mongoose.ipConfigChanged();
} break;
```

`esp_netif_ip6_get_addr_type()` classifies addresses as link-local, global (including ULA), or invalid without needing a netif handle. The event payload `info.got_ip6.ip6_info.ip` is the authoritative source.

Other v2.x notes:
- `IPv6Address` has `.toString()` method — confirmed
- `GOT_IP6` event payload contains the address directly (`info.got_ip6.ip6_info.ip`) — use `esp_netif_ip6_get_addr_type()` to classify
- LwIP 2.x IPv6 stack compiled into firmware (`liblwip.a` link confirmed)
- `tcp_new_ip_type()`, `IPADDR_TYPE_ANY` available in LwIP 2.x

### LwIP Socket Layer: Dual-Stack Ready

Confirmed from esp-lwip source analysis:

1. **`socket(AF_INET6, SOCK_STREAM, 0)`** creates a `NETCONN_TCP_IPV6` netconn, which in `pcb_new()` creates a PCB with `IPADDR_TYPE_ANY` — dual-stack by default
2. **`IPV6_V6ONLY` defaults to 0** — `netconn_alloc()` never sets `NETCONN_FLAG_IPV6_V6ONLY`
3. **`setsockopt(IPV6_V6ONLY=0)`** is supported in `lwip_setsockopt_impl()` for explicit clarity
4. **`lwip_netconn_do_listen()`** has a safety belt: if bound to `IP6_ADDR_ANY` with V6ONLY=0, it promotes PCB type to `IPADDR_TYPE_ANY`
5. **IPv4 clients** are handled via IPv4-mapped IPv6 addresses: `ip4_2_ipv4_mapped_ipv6()` in `lwip_accept()`

### Mongoose Socket Path: IPv4-Only by Default

Three hardcoded `AF_INET` prevent dual-stack:

| Location | Line | Code | Problem |
|---|---|---|---|
| `mg_parse_address()` | 2645 | `sa->sin.sin_family = AF_INET;` | Bare port "80" → AF_INET → IPv4-only socket |
| `mg_socket_if_connect_tcp()` | ~3685 | `socket(AF_INET, SOCK_STREAM, proto)` | Outbound connections always IPv4 |
| `mg_socket_if_connect_udp()` | ~3700 | `socket(AF_INET, SOCK_DGRAM, 0)` | Outbound UDP always IPv4 |

Note: `mg_open_listening_socket()` already does the right thing — it uses `socket(sa->sa.sa_family, ...)`. The problem is upstream: `mg_parse_address("80")` sets AF_INET, so the socket is created IPv4-only.

### Mongoose DNS Resolver: A-Record Only

- `resolve_cb()` (line 3052) only handles `MG_DNS_A_RECORD`, ignoring AAAA
- `mg_connect_opt()` (line 3182) hardcodes `MG_DNS_A_RECORD` query type
- `mg_dns_parse_record_data()` already supports AAAA when `MG_ENABLE_IPV6=1` (line 11437)
- **No DNS cache** — parallel A+AAAA queries won't collide
- `query` parameter is a plain `int`, not a bitmask — must send separate queries
- **Recommended approach**: Family-first (query AAAA if device has global IPv6, A if IPv4-only), add fallback later

### Mongoose Accept Path: Missing sa_family

In the **LwIP low-level path** (`mg_lwip_handle_accept`, line 15228):
- Never sets `sa.sa.sa_family` — leaves it at 0
- Always passes `sizeof(sa.sin)` even for IPv6 connections
- This causes `mg_sock_addr_to_str()` to format IPv6 connections as IPv4

**However, this only affects the LwIP low-level path.** In the socket path, `accept()` returns a proper `sockaddr` with correct family. No fix needed for our ESP32 build.

### Mongoose UDP: Even More Broken Than TCP

- **No `UDP_NEW` macro** — `udp_new()` called directly, always IPv4 PCB
- `mg_lwip_if_udp_send()` hardcodes `.type = 0` (IPv4) and reads from `sa->sin.sin_addr`
- `mg_lwip_udp_recv_cb()` always stores IPv4 source address
- **BUT** — mDNS uses ESP-IDF's ESPmDNS library, not Mongoose. UDP bugs don't affect service discovery.

For the socket path, `mg_socket_if_connect_udp()` creates IPv4 sockets. Fix: use `sa->sa.sa_family`.

### Copilot PR #1038 Assessment

The existing Copilot PR is **incomplete and has bugs**:
- Uses `WiFi.localIPv6()` — this **does exist in v2.x** but returns link-local only. The Copilot PR doesn't attempt to get the global address at all, and the v3.x `globalIPv6()` method doesn't exist in our v2.x core
- Only adds address reporting, doesn't fix Mongoose
- No dual-stack server binding
- No DNS AAAA resolution
- **Verdict: Do not merge.**

---

## Agent Research Findings (2026-06-03)

Three parallel agents investigated unresolved technical questions. All findings are incorporated into the phase sections above; this section summarizes the major results.

### Heap Budget: Non-Issue

`MG_ENABLE_IPV6=1` grows `union socket_address` from 16 → 28 bytes (+12). `sizeof(struct mg_connection)` grows from ~112 → ~124 bytes. With 10-20 typical concurrent connections, total heap delta is +120–240 bytes — under 0.3% of the ~80-140KB free heap after full init. No fixed connection pool; connections are heap-allocated dynamically. The Phase 1 `String` fields for IPv6 addresses add ~16 bytes empty, ~80-100 bytes populated. **Verdict: Negligible, no concern.**

### mDNS AAAA: Fully Automatic

ESPmDNS does **not** need any code changes. The ESP-IDF mDNS component queries addresses from the netif dynamically at response time (not at registration time). When `IP_EVENT_GOT_IP6` fires, mDNS automatically enables the IPv6 PCB, triggers announcements, and begins responding to AAAA queries. `esp_netif_get_all_ip6()` returns all IPv6 addresses (link-local + global) on the interface. No `addIP6Address()` API exists because it's unnecessary. The only prerequisite is calling `WiFi.enableIpV6()` before `WiFi.begin()`. Order of `enableIpV6()` vs `MDNS.begin()` does not matter. **Verdict: Zero mDNS code changes needed.**

### ETH.enableIpV6(): Confirmed in v2.0.17

`ETH.enableIpV6()` (capital V — v2.x naming) exists at `ETH.h:93`. Implementation uses the deprecated `tcpip_adapter` API internally (`tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_ETH)`), but that's an implementation detail. `ETH.localIPv6()` also exists and returns link-local only (same as WiFi). No fallback needed. **Verdict: Use `ETH.enableIpV6()` directly.**

### esp_netif ifkey Strings: Don't Use Them

The ifkey strings (`WIFI_STA_DEF`, `ETH_DEF`, etc.) are ESP-IDF internal details, stable within a major version but not guaranteed across versions. **The `ip_event_got_ip6_t` struct in the GOT_IP6 event payload already carries both `esp_netif_t*` and the IPv6 address.** The Arduino core classifies events into `ARDUINO_EVENT_WIFI_STA_GOT_IP6` / `ARDUINO_EVENT_ETH_GOT_IP6` automatically. The firmware already stores the full `arduino_event_info_t` in `NetworkEventMessage`. Use `esp_netif_ip6_get_addr_type()` on the event payload address to classify link-local vs global. **Verdict: Use event payload, not ifkey lookup.**

### WebSocket/HTTP Client IP: Zero Risk

No firmware code extracts, inspects, or logs client IP addresses from WebSocket or HTTP connections. `MongooseHttpWebSocketConnection::getRemoteAddress()` exists but is never called. Access control uses HTTP Basic Auth (username/password), not IP-based allowlists. OCPP uses an outbound WebSocket client, not server-side. **Verdict: No changes needed. Dual-stack WebSocket upgrades "just work".**

### Phase 3 (AAAA DNS): 8 Mongoose Bugs Identified

Full inventory in the Phase 3 section above. Key takeaway: the DNS resolver only sends A-record queries, `resolve_cb()` only processes A responses, both `mg_socket_if_connect_tcp()` and `mg_socket_if_connect_udp()` hardcode `AF_INET`, and UDP send uses `sizeof(nc->sa.sin)` (16 bytes) instead of the correct IPv6 size (28 bytes). The `inet_ntop` macro for `AF_INET6` always returns `NULL`. **Verdict: Phase 3 requires 8 patches minimum. Recommend fixing socket creation bugs (1-4) as part of Phase 2, adding zero-regression AAAA parsing in Phase 3a, deferring AAAA query emission to Phase 3b.**

---

## Implementation Strategy

### Option chosen: Patch Mongoose socket path + enable IPv6

**Why not switch to raw LwIP path?**
- The socket path already creates `IPADDR_TYPE_ANY` PCBs automatically
- The raw LwIP path uses `tcp_new_ip6()` which is IPv6-only — needs MORE patches
- The socket path is simpler: `accept()` returns proper sockaddr, no manual pbuf management
- Less invasive changes, lower regression risk

**Why not Mongoose 7.x migration?**
- Completely rewritten API, weeks of work, high regression risk
- Not justified when the socket path needs ~5 lines of changes

### Fork strategy

1. Fork `jeremypoulter/ArduinoMongoose` to `rnavarro/ArduinoMongoose`
2. Create branch `fix/ipv6-dual-stack` from the 0.0.22 tag
3. Patch the socket path in mongoose.c
4. Point OpenEVSE `platformio.ini` at our fork
5. Test, iterate, upstream if it works

## Why Fix Both Socket and Raw LwIP Paths

Even though ESP32 uses `MG_NET_IF_SOCKET`, we're fixing IPv6 bugs in the raw LwIP low-level path too:

1. **Other platforms use it** — NRF51/NRF52 already hardcode `MG_ENABLE_IPV6=1` with `MG_NET_IF_LWIP_LOW_LEVEL`. Our fork would still be broken for them.

2. **No extra cost** — we're already in mongoose.c patching IPv6. The raw LwIP fixes are small (mostly adding `sa_family` checks and using existing macros correctly).

3. **Future-proof** — if anyone (upstream or us) ever switches ESP32 to the raw LwIP path, these bugs would surface immediately.

4. **Responsible forking** — if we ship "IPv6 dual-stack support" in our ArduinoMongoose fork, leaving known IPv6 bugs in half the code paths is a landmine.

The raw LwIP path patches (7-14 in the inventory) are separate from the socket path patches (1-6) and can be validated independently.

**Branch strategy: PR 1 = ESP32 socket path only. PR 2 = raw LwIP cleanup.** The raw LwIP patches cannot be tested on ESP32 hardware (the code path is dead when `MG_NET_IF_SOCKET` is active). They should go in a separate branch and PR, not gated on ESP32 validation. The ESP32 PR ships first; the raw LwIP PR follows after review by anyone running NRF51/NRF52 targets. Some raw LwIP patches also need updating for LwIP 2.x (e.g., `udp_new_ip6()` should be `udp_new_ip_type(IPADDR_TYPE_ANY)` for dual-stack, not `udp_new_ip6()` which is IPv6-only).

---

## Implementation Phases

### Phase 0: Custom LwIP Build — Enable RDNSS for IPv6 DNS Auto-Learning

**Goal:** Replace the prebuilt `liblwip.a` with a custom rebuild that enables RDNSS (RFC 8106) so the device auto-learns IPv6 DNS resolvers from Router Advertisements.

**Why this is needed:** The Arduino ESP32 framework's prebuilt `liblwip.a` has `CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS=0`, which means the RDNSS option handler in `nd6.c` is completely compiled out (`#if`-guarded at three locations). Without RDNSS, the device can get an IPv6 address via SLAAC but cannot learn an IPv6 DNS server — making IPv6-only networks non-functional.

**Feasibility: Fully verified and implemented.** Using `esp32-arduino-lib-builder` on its `release/v4.4` branch to rebuild `liblwip.a`, then LIBPATH preemption via a PlatformIO extra script.

#### What changes in the custom build

Only one sdkconfig line changes:
```
CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS=2   # was 0
```

This enables three `#if`-guarded code blocks in `nd6.c` (from `espressif/esp-lwip` at commit `a45be9e4`):
1. **Line 100-101**: The `rdnss` member in `union ra_options` — file-scope static, not referenced by other .o files
2. **Line 559-562**: The `rdnss_server_idx` tracking variable — stack-local inside `nd6_input()`
3. **Line 757-794**: The `case ND6_OPTION_TYPE_RDNSS:` handler — calls `pbuf_copy_partial()`, `dns_setserver()`, `dns_getserver()`, `ip_addr_cmp()`, `htonl()` — all already linked

**Zero ABI impact.** No struct layouts change. No function signatures change. No shared global data changes. The `dns_setserver()` call writes to the same `dns_servers[]` array DHCPv4 uses — no new storage or subsystem interaction.

**No OTA breakage.** `liblwip.a` is statically linked into the firmware binary at compile time. OTA replaces the entire app partition (a single `.bin` file). The custom `liblwip.a` just means different bytes in the same firmware format. Flashing stock firmware over custom works — user loses RDNSS (feature regression, not a breakage).

#### Implemented approach: `esp32-arduino-lib-builder` + LIBPATH preemption

**Status: Complete (June 2026).** Built using `esp32-arduino-lib-builder` on its `release/v4.4` branch with `CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS=2` appended to `configs/defconfig.common`. The rebuilt `liblwip.a` is stored in `custom_libs/` and linked via a PlatformIO extra script that prepends it to `LIBPATH`.

**Why LIBPATH preemption instead of a full custom framework package:**
- The builder's `release/v4.4` branch only rebuilds `liblwip.a`, not the entire framework. The framework hosting approach required the master branch which defaults to IDF v5.5 and has component incompatibilities with v4.4 (esp_littlefs, esp32-camera require IDF ≥5.0).
- For a single library replacement, LIBPATH preemption is proportional to the change. A full framework fork (~200MB+ with LFS) for one `.a` file is overkill.
- The `esp32-arduino-lib-builder` ensures the rebuilt library uses the same compiler flags and sdkconfig as the official build. Only the RDNSS config line differs.
- The extra script (`scripts/override_lwip.py`) makes the override explicit and visible — not a hidden SCons hack.

#### Build process (reproducible)

```bash
# 1. Clone the lib builder and check out its v4.4 branch
cd ~/workspace
git clone https://github.com/espressif/esp32-arduino-lib-builder.git
cd esp32-arduino-lib-builder
git checkout release/v4.4

# 2. Add RDNSS config
echo 'CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS=2' >> configs/defconfig.common

# 3. Remove components that require IDF >= 5.0 (not needed for lwip rebuild)
rm -rf components/esp32-camera components/esp_littlefs components/espressif__esp-dsp components/esp-rainmaker

# 4. First run: clone IDF + install tools (takes 10-20 min)
./build.sh -t esp32 -I release/v4.4
# Will fail at cmake due to leftover IDF >= 5.0 component checks. That's OK.

# 5. Subsequent runs: skip env setup, just build the IDF libraries
source esp-idf/export.sh
./build.sh -s -t esp32 -I release/v4.4 -b idf_libs

# Output: build/esp-idf/lwip/liblwip.a
# Verify RDNSS: grep RDNSS build/config/sdkconfig.cmake
```

**Build time:** ~10-20 minutes for IDF/tools download, ~5 minutes for the library build itself.

#### PlatformIO integration

`scripts/override_lwip.py` prepends `custom_libs/` to `LIBPATH`:

```python
from os.path import join
Import("env")
env.Prepend(LIBPATH=[join("$PROJECT_DIR", "custom_libs")])
```

Added to `[common]` in `platformio.ini` as a pre-action extra script. Verified via `firmware.map` that `custom_libs/liblwip.a` is linked instead of the framework default.

**Rebuild verification:**
- `nd6.c.obj` in the rebuilt library is 5,084 bytes larger than prebuilt (158,464 vs 153,380) — the RDNSS code paths are compiled in
- Full `liblwip.a` is ~10KB smaller than prebuilt (4,291,360 vs 4,301,420) due to different component set in the builder's v4.4 branch
- Both `openevse_wifi_v1` and `olimex_esp32-gateway-f` targets build and link successfully

#### RDNSS handler internals

When a Router Advertisement arrives at `nd6_input()`:
1. The RA option loop encounters option type 25 (`ND6_OPTION_TYPE_RDNSS`)
2. Parses header: type, length, reserved, lifetime
3. Calculates address count: `num = (length - 1) / 2`
4. For each address: reads 16 bytes via `pbuf_copy_partial()`, then:
   - Lifetime > 0: `dns_setserver(rdnss_server_idx++, &address)` — adds to LwIP DNS table
   - Lifetime == 0: searches `dns_getserver()` for match, removes via `dns_setserver(s, NULL)`
5. The `rdnss_server_idx` counter is capped by `DNS_MAX_SERVERS` (= 3 in Arduino build)

After Phase 0, LwIP's `dns_servers[]` array is populated with both DHCPv4-supplied IPv4 DNS **and** RDNSS-supplied IPv6 DNS servers. `WiFi.hostByName()`, `getaddrinfo()`, and `netconn_gethostbyname()` all use this array. **Mongoose's** DNS resolver remains a separate problem (hardcoded `8.8.8.8`) — Phase 3 scope.

#### Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Full framework build takes 30-60 min | Informational | One-time cost; alternative is LIBPATH hack (fragile) |
| Wrong compiler flags produce incompatible .o | Low | `esp32-arduino-lib-builder` uses the same build system as the official release |
| Framework update changes lwipopts.h incompatibly | Very Low | Arduino ESP32 v2.0.x is EOL — unlikely to change; pin platform version |
| Someone flashes stock firmware and loses RDNSS | Informational | Document in release notes |
| GitHub-hosted framework repo goes down | Very Low | PlatformIO caches packages locally after first download |
| `esp32-arduino-lib-builder` doesn't support IDF v4.4 | Low | Verify branch compatibility; fallback to standalone IDF build if needed |

---

### Phase 1: WiFi Layer — Enable IPv6 on the ESP32 interface

**Goal:** Get an IPv6 address on the ESP32 and report it in the API.

**Changes in `openevse_esp32_firmware`:**

1. **`platformio.ini`** — Add build flag:
   ```ini
   build_flags = -D MG_ENABLE_IPV6=1  # MUST stay in [common] so ArduinoMongoose + MicroOcppMongoose + firmware share one mg_connection ABI
   ```

2. **`src/net_manager.cpp`** — After WiFi/Ethernet connects, enable IPv6:
   ```cpp
   // In wifiClientConnect(), after WiFi.begin():
   WiFi.enableIpV6();  // Note: v2.x API (uppercase V)

   // In onNetEvent(), ARDUINO_EVENT_ETH_CONNECTED handler (inside #ifdef ENABLE_WIRED_ETHERNET):
   ETH.enableIpV6();  // Note: ETH inherits from NetworkInterface in v2.x too
   ```

3. **`src/net_manager.h`** — Add IPv6 address storage:
   ```cpp
   String _ipv6address_linklocal;  // fe80:: address
   String _ipv6address_global;      // Global/ULA address
   ```

4. **`src/net_manager.cpp`** — Handle GOT_IP6 events using event payload (no ifkey lookup):
   ```cpp
   #include <esp_netif.h>
   // esp_netif_ip_addr.h (included transitively) defines esp_ip6_addr_type_t
   // including ESP_IP6_ADDR_IS_UNIQUE_LOCAL — a separate enum from IS_GLOBAL

   case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
   {
     // Event payload has the address directly — no fragile ifkey strings needed
     esp_ip6_addr_t addr = info.got_ip6.ip6_info.ip;
     esp_ip6_addr_type_t addr_type = esp_netif_ip6_get_addr_type(&addr);
     if (addr_type == ESP_IP6_ADDR_IS_LINK_LOCAL) {
       _ipv6address_linklocal = IPv6Address(addr.addr).toString();
       DBUGF("WiFi STA IPv6 link-local: %s", _ipv6address_linklocal.c_str());
     } else if (addr_type == ESP_IP6_ADDR_IS_GLOBAL || addr_type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL) {
       // IS_GLOBAL and IS_UNIQUE_LOCAL (fc00::/7) are SEPARATE enum values — both are routable
       _ipv6address_global = IPv6Address(addr.addr).toString();
       DBUGF("WiFi STA IPv6 global: %s", _ipv6address_global.c_str());
     }
     Mongoose.ipConfigChanged();
   } break;
   ```

   Also handle ETH GOT_IP6 (inside `#ifdef ENABLE_WIRED_ETHERNET`):
   ```cpp
   case ARDUINO_EVENT_ETH_GOT_IP6:
   {
     // ETH.enableIpV6() exists in v2.0.17 (capital V) — confirmed in ETH.h:93
     // Same event-payload approach as WiFi STA
     esp_ip6_addr_t addr = info.got_ip6.ip6_info.ip;
     esp_ip6_addr_type_t addr_type = esp_netif_ip6_get_addr_type(&addr);
     if (addr_type == ESP_IP6_ADDR_IS_LINK_LOCAL) {
       _ipv6address_linklocal = IPv6Address(addr.addr).toString();
       DBUGF("ETH IPv6 link-local: %s", _ipv6address_linklocal.c_str());
     } else if (addr_type == ESP_IP6_ADDR_IS_GLOBAL || addr_type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL) {
       // IS_GLOBAL and IS_UNIQUE_LOCAL (fc00::/7) are SEPARATE enum values — both are routable
       _ipv6address_global = IPv6Address(addr.addr).toString();
       DBUGF("ETH IPv6 global: %s", _ipv6address_global.c_str());
     }
     Mongoose.ipConfigChanged();
   } break;
   ```

   **Why not `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")`?** The ifkey strings are ESP-IDF internal details that could change across versions. The event payload is the authoritative source — `ip_event_got_ip6_t` carries both the `esp_netif_t*` handle and the IPv6 address. The Arduino core already classifies the event into distinct `ARDUINO_EVENT_WIFI_STA_GOT_IP6` / `ARDUINO_EVENT_ETH_GOT_IP6` enum values, so the firmware knows which interface got the address without any lookup.

5. **`src/net_manager.h`** — Add public accessors:
   ```cpp
   String getIpv6Global() { return _ipv6address_global; }
   String getIpv6LinkLocal() { return _ipv6address_linklocal; }
   ```

6. **`src/web_server.cpp`** — Add IPv6 addresses to status API (after `doc["ipaddress"]` around line 214):
   ```cpp
   doc["ipv6address_global"] = net.getIpv6Global();
   doc["ipv6address_linklocal"] = net.getIpv6LinkLocal();
   ```

7. **`src/mqtt.cpp` line ~164** — Fix IPv4 literal in MQTT announce URL:
   ```cpp
   // Current (IPv4 only):
   "http://" + net.getIp() + "/"
   // Should include IPv6 when available:
   String announceUrl = "http://" + net.getIp() + "/";
   // TODO: add IPv6 announce when we have a routable address
   ```

   Also clear IPv6 addresses on disconnect and re-enable on reconnect:
   ```cpp
   case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
     _ipv6address_global = "";
     _ipv6address_linklocal = "";
     // ... existing disconnect handling (line 252+) ...
   ```

   **⚠️ Reconnect behavior — MUST re-enable IPv6 after disconnect.** Source-code verification of ESP-IDF v4.4.7 `esp_netif_lwip.c` reveals:

   1. `WiFi.enableIpV6()` calls `esp_netif_create_ip6_linklocal()` — a **one-shot** function that creates the link-local address on the LwIP netif. There is no persistent `WANT_IP6_BIT` or idempotent flag in ESP-IDF v4.4.
   2. `WIFI_EVENT_STA_DISCONNECTED` triggers `esp_netif_action_disconnected()` → `esp_netif_down()` — which **explicitly clears ALL IPv6 addresses** from every address slot (`netif_ip6_addr_set(lwip_netif, i, IP6_ADDR_ANY6)` + `IP6_ADDR_INVALID` state). See `esp_netif_lwip.c` lines 1390-1400.
   3. On reconnect, `WIFI_EVENT_STA_CONNECTED` triggers `esp_netif_action_connected()` → `esp_netif_up()` + DHCP start. IPv6 is NOT re-enabled automatically. The Arduino core at `WiFiGeneric.cpp:1056` has a **commented-out** `esp_netif_create_ip6_linklocal()` call in the `STA_CONNECTED` handler — this was intentionally left disabled.

   **Fix:** Call `WiFi.enableIpV6()` in the `ARDUINO_EVENT_WIFI_STA_CONNECTED` handler (or after `WiFi.begin()` in the reconnect path). This ensures IPv6 is re-created on every connect. The call is safe even if IPv6 was already enabled — `esp_netif_create_ip6_linklocal()` returns `ESP_FAIL` if the netif is not up, and `ESP_OK` if it succeeds (idempotent for our purposes). Example:
   ```cpp
   case ARDUINO_EVENT_WIFI_STA_CONNECTED:
     WiFi.enableIpV6();  // Re-enable IPv6 after disconnect cleared it
     break;
   ```

   Also clear stored IPv6 addresses on disconnect:
   ```cpp
   case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
     _ipv6address_global = "";
     _ipv6address_linklocal = "";
     // ... existing disconnect handling (line 252+) ...
   ```

**Timing note:** `GOT_IP6` fires **multiple times** — first for link-local (immediately after `enableIpV6()`), then again for each global/ULA address (when Router Advertisements arrive, seconds later). At the first fire, `esp_netif_get_ip6_global()` returns `ESP_FAIL` — the handler re-queries on each event, so the global address populates later. **This is expected — an empty global address on first fire is not a bug.**

**ODR invariant:** `MG_ENABLE_IPV6=1` changes `sizeof(union socket_address)` (16→28 bytes) and therefore `struct mg_connection`. It **must** be defined identically for ArduinoMongoose and every OpenEVSE translation unit. Use PlatformIO global `build_flags` — never scope `MG_ENABLE_IPV6` to the library only, or you get silent ABI mismatch and heap corruption.

**Testing:** ESP32 should have IPv6 addresses visible in `/status` and `/config`. Can ping6 the device. mDNS should advertise IPv6. Web UI still IPv4-only (Mongoose not patched yet). Note: link-local addresses require zone ID for access (`ping6 fe80::...%eth0`); use global address for testing. Verify `WiFi.enableIpV6()` re-enables IPv6 on reconnect by cycling WiFi and checking `/status` still shows IPv6 addresses.

**Firmware size warning:** Default partition (`min_spiffs.csv`) has only **~110 KB headroom** (1.88 MB partition, 1.76 MB current firmware). `MG_ENABLE_IPV6=1` activates ~106 lines of IPv6 code in Mongoose's socket path, estimated at ~3-5 KB additional code size, plus LwIP IPv6 stack code that's already compiled but with more code paths exercised. Budget 10-20 KB increase. Monitor firmware size — if it exceeds partition, switch to `min_spiffs_debug.csv` (3.75 MB partition, single-app, no OTA) for development, or switch to 16MB flash partition (`openevse_16mb.csv`). The 16 MB TFT variant has 6.25 MB app partitions with ample room.

### Phase 2: Mongoose HTTP Server — Dual-stack listening (socket path)

**⚠️ TEMP PLATFORMIO HACK — MUST REVERT BEFORE ANY PR**

platformio.ini has two coordinated changes that must be reverted together:

1. `lib_deps` line 34: `jeremypoulter/ArduinoMongoose@0.0.22` is **commented out**
2. `[env:olimex_esp32-gateway-f]`: `lib_extra_dirs = /home/rnavarro/workspace/ArduinoMongoose` points at the local fork on `fix/ipv6-dual-stack` branch

**Revert steps before PR:**
1. Publish `rnavarro/ArduinoMongoose` fork as a PlatformIO-usable package (or get upstream merge)
2. Uncomment the `lib_deps` line, update version or change to fork URL
3. Remove `lib_extra_dirs` from the Olimex env
4. Verify ALL build envs (WiFi, Olimex, etc.) pull the IPv6-patched library

**Current impact:** Only `olimex_esp32-gateway-f` has Phase 2 patches. All other envs (`openevse_wifi_v1`, etc.) still use stock 0.0.22 and will **not** listen on IPv6.

**Goal:** Web UI and REST API accessible over both IPv4 and IPv6.

**Changes in `rnavarro/ArduinoMongoose` (fork):**

1. **`src/mongoose.c` — Fix `mg_parse_address()` bare port + IPv6 literal port handling**

   When bare port is specified, use AF_INET6 on LwIP so `socket(AF_INET6, ...)` creates a dual-stack PCB:

   ```c
   // Line ~2683, the bare port case:
   } else if (sscanf(str, ":%u%n", &port, &len) == 1 ||
              sscanf(str, "%u%n", &port, &len) == 1) {
   #if MG_ENABLE_IPV6 && MG_LWIP
     /* On LwIP socket path, AF_INET6 + V6ONLY=0 = dual-stack listener */
     sa->sin6.sin6_family = AF_INET6;
     sa->sin6.sin6_port = htons((uint16_t) port);
   #else
     sa->sin.sin_port = htons((uint16_t) port);
   #endif
   ```

   **Also fix existing bug at line ~2666** — IPv6 literal `[::1]:8080` sets port on wrong union member:
   ```c
   // Current (bug):
   sa->sin6.sin6_family = AF_INET6;
   sa->sin.sin_port = htons((uint16_t) port);  // wrong member
   // Fix:
   sa->sin6.sin6_family = AF_INET6;
   sa->sin6.sin6_port = htons((uint16_t) port);  // correct member
   ```

   **Cross-platform guard is critical.** On Linux/BSD, `IPV6_V6ONLY` defaults to 1 — toggling bare-port to AF_INET6 would create an IPv6-only listener, silently dropping IPv4. The `MG_LWIP` guard limits this to LwIP targets where V6ONLY defaults to 0.

2. **`src/mongoose.c` — Fix `mg_socket_if_connect_tcp()` sa_family + connect() addrlen**

   ```c
   // Change from:
   nc->sock = socket(AF_INET, SOCK_STREAM, proto);
   // To:
   nc->sock = socket(sa->sa.sa_family, SOCK_STREAM, proto);
   ```

   **Also fix `connect()` addrlen** (line ~3693) — hardcoded `sizeof(sa->sin)` (16 bytes) truncates IPv6 (28 bytes):
   ```c
   // Change from:
   rc = connect(nc->sock, &sa->sa, sizeof(sa->sin));
   // To:
   socklen_t sa_len = (sa->sa.sa_family == AF_INET6) ? sizeof(sa->sin6) : sizeof(sa->sin);
   rc = connect(nc->sock, &sa->sa, sa_len);
   ```

3. **`src/mongoose.c` — Fix `mg_socket_if_connect_udp()` to use `nc->sa.sa.sa_family`**

   Note: this function takes no `sa` parameter — must use `nc->sa`.
   ```c
   // Change from:
   nc->sock = socket(AF_INET, SOCK_DGRAM, 0);
   // To:
   nc->sock = socket(nc->sa.sa.sa_family, SOCK_DGRAM, 0);
   ```

4. **`src/mongoose.c` — Add explicit `setsockopt(IPV6_V6ONLY=0)` in `mg_open_listening_socket()` (MANDATORY, requires structural change)**

   The existing code at line 3817 is a single chained conditional — all existing setsockopt calls are inside `#if !MG_LWIP` which is **0 on ESP32**. The V6ONLY setsockopt must go **outside** this guard, requiring the chain to be broken into separate statements:

   ```c
   if ((sock = socket(sa->sa.sa_family, type, proto)) == INVALID_SOCKET) {
     return -1;
   }
   #if MG_ENABLE_IPV6
   if (sa->sa.sa_family == AF_INET6) {
     int v6only = 0;
     setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
   }
   #endif
   // Note: Guard with #if defined(IPV6_V6ONLY) for broader portability.
   // On ESP32/LwIP, IPV6_V6ONLY is always defined when LWIP_IPV6=1.
   // On platforms without IPv6 socket support, the setsockopt would fail at compile time.
   #if !MG_LWIP
   setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, ...);
   setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, ...);
   #endif
   if (bind(sock, &sa->sa, sa_len) != 0 ||
       (type == SOCK_STREAM && listen(sock, SOMAXCONN) != 0)) {
     closesocket(sock);
     return -1;
   }
   ```

   **Not optional.** The ESP-lwip auto-promotion to `IPADDR_TYPE_ANY` is ESP-specific, not portable POSIX. On Linux/BSD, `IPV6_V6ONLY` defaults to 1 — an AF_INET6 socket without this would be IPv6-only.

**Testing:** `curl -4 http://openevse-single/status` and `curl -6 http://[fe80::...]/status` both work. Web UI accessible from both IPv4 and IPv6 browsers.

### Phase 3: Mongoose DNS — AAAA Record Resolution

**Goal:** Outbound connections (MQTT, EmonCMS, OCPP) can resolve and connect to IPv6 hosts.

**⚠️ Agent research found 8 distinct IPv6 bugs in Mongoose's DNS/UDP stack. Phase 3 is dead on arrival without patching all of them.**

**Changes in `rnavarro/ArduinoMongoose` (fork):**

#### Complete bug inventory for Phase 3

| # | Location | Line | Issue |
|---|----------|------|-------|
| 1 | `mg_socket_if_connect_udp()` | 3879 | `socket(AF_INET, SOCK_DGRAM, 0)` — hardcoded IPv4 |
| 2 | `mg_socket_if_connect_tcp()` | 3863 | `socket(AF_INET, SOCK_STREAM, proto)` — hardcoded IPv4 |
| 3 | `mg_socket_if_udp_send()` | 3921 | `sizeof(nc->sa.sin)` — always IPv4 sockaddr size (16 bytes vs 28) |
| 4 | `mg_socket_if_connect_tcp()` | 3869 | `connect(..., sizeof(sa->sin))` — always IPv4 size |
| 5 | `mg_connect_opt()` | 3329 | `MG_DNS_A_RECORD` — only queries A, never AAAA |
| 6 | `resolve_cb()` | 3211-16 | Only processes A records; writes to `sin.sin_addr` with size 4 |
| 7 | DNS server URL | 12249 | Hardcoded `udp://8.8.8.8:53` — IPv4 only |
| 8 | `inet_ntop` macro | 645 | `AF_INET6` branch returns `NULL` |

The `TODO(lsm): handle IPv6 answers too` comment at line 3214 has been there since Mongoose 6.x — **never implemented**. The low-level parser `mg_dns_parse_record_data()` does handle AAAA when `MG_ENABLE_IPV6=1`, but the plumbing to request and use those records is missing.

All three outbound paths (MQTT, EmonCMS, OCPP) go through `mg_connect_opt()` → IPv4-only DNS resolver.

#### Alternative: Pre-resolve hostnames via Arduino WiFi stack

Since patching all 8 bugs is invasive, an alternative is to bypass Mongoose's DNS entirely:

1. Resolve the hostname using `WiFi.hostByName()` (uses LwIP's DNS, supports AAAA) or `getaddrinfo()`
2. Pass the IPv6 address literal to Mongoose (e.g., `[fe80::1]:1883`) — this bypasses `mg_parse_address()`'s DNS path since it handles bracketed IPv6 literals (line 2820-2824)
3. **But** this still hits `mg_socket_if_connect_tcp()` which creates `AF_INET` sockets — patches 1-2 (socket family) are still required

**Recommended approach for v0:** Fix patches 1-4 (socket creation + send size) in Phase 2. For Phase 3, ship A-only DNS queries with AAAA parsing added to `resolve_cb()` (zero regression). Defer AAAA query emission and pre-resolve fallback to a later iteration.

1. **`src/mongoose.c` — Add AAAA handling in `resolve_cb()`** (line ~3052)

   ```c
   for (i = 0; i < msg->num_answers; i++) {
     if (msg->answers[i].rtype == MG_DNS_A_RECORD) {
       mg_dns_parse_record_data(msg, &msg->answers[i],
                                &nc->sa.sin.sin_addr, 4);
       mg_do_connect(nc, nc->flags & MG_F_UDP ? SOCK_DGRAM : SOCK_STREAM,
                     &nc->sa);
       return;
     }
   #if MG_ENABLE_IPV6
     if (msg->answers[i].rtype == MG_DNS_AAAA_RECORD) {
       uint16_t port = nc->sa.sin.sin_port;  // Preserve port before family change
       nc->sa.sin6.sin6_family = AF_INET6;
       nc->sa.sin6.sin6_port = port;          // Explicit copy
       mg_dns_parse_record_data(msg, &msg->answers[i],
                                &nc->sa.sin6.sin6_addr, 16);
       mg_do_connect(nc, nc->flags & MG_F_UDP ? SOCK_DGRAM : SOCK_STREAM,
                     &nc->sa);
       return;
     }
   #endif
   }
   ```

   **Port preservation is critical.** Before the DNS callback, the port is in `nc->sa.sin.sin_port` (AF_INET from `mg_parse_address`). Changing to AF_INET6 without copying the port would lose it if `sin_port` and `sin6_port` don't overlap on some platform.

2. **`src/mongoose.c` — A-first DNS queries; AAAA with fallback later** (line ~3182)

   **Important regression risk:** AAAA-first without fallback silently breaks all IPv4-only targets (MQTT brokers, EmonCMS, OCPP). The `resolve_cb` failure path destroys `nc` with no retry. The safe approach is A-first:

   ```c
   // Phase 3a: Keep A-first (zero regression)
   int dns_query_type = MG_DNS_A_RECORD;
   if (mg_resolve_async_opt(nc->mgr, host, dns_query_type, resolve_cb, nc, o) != 0) {
   ```

   **Sequencing:**
   - Phase 3a: Add AAAA parsing to `resolve_cb` (code above). Ship with A-only queries. Zero regression.
   - Phase 3b: Try AAAA when device has global IPv6, fall back to A on failure. Requires stashing hostname on `nc` (e.g., `priv_1`) for re-resolution since `mg_resolve_async_request.name` is freed after callback.
   - Phase 3c: Parallel A+AAAA with happy eyeballs. Requires resolver architecture changes. Low priority.

   **Consider deferring Phase 3 entirely.** The actual goal — EVSE units accessible via IPv6 — is fully delivered by Phases 1+2 (inbound). Outbound AAAA is additive and carries the most regression risk.

**Revised recommendation:** Fix socket creation bugs (patches 1-4 from the inventory above) as part of Phase 2 since they're needed for any IPv6 outbound path. Pack the AAAA `resolve_cb()` parsing as a Phase 3a (zero regression — only parses AAAA responses if they arrive, still sends A-only queries). Defer AAAA query emission (patches 5-7) to Phase 3b.

**Testing:** Configure MQTT with a hostname that has AAAA records. Verify connection works over IPv6. Verify IPv4 fallback works when AAAA fails.

### Phase 4: Application Layer — Full Integration

**Goal:** All OpenEVSE networking features work over IPv6.

**Changes in `openevse_esp32_firmware`:**

1. **MQTT config** — Allow IPv6 broker addresses in `[bracket]:port` format
2. **EmonCMS config** — Allow IPv6 server URLs
3. **OCPP config** — Allow IPv6 server URLs
4. **OTA updates** — Already uses ESP32 Arduino HTTPClient (supports IPv6), but test
5. **Captive portal** — IPv4-only in v0 (see v2 section below)
6. **Web UI** — Display IPv6 addresses in the network status page

### Phase 5: Hardening and Testing

1. **Dual-stack stress test** — Run both units for 1+ week with IPv6 enabled
2. **IPv4-mapped addresses** — After Phase 2 Mongoose fork patches (inet_ntoa fixes + stripping), verify `::ffff:a.b.c.d` from IPv4 clients displays as `a.b.c.d` in debug logs
3. **IPv6-only network test** — Disable IPv4 on test network, verify EVSE still functions
3. **Address change resilience** — Verify firmware handles IPv6 address changes gracefully
4. **Memory impact** — Measure heap usage with IPv6 enabled
5. **Flash impact** — Verify firmware still fits in 16MB partition
6. **Regression test** — Verify all IPv4 functionality unchanged

---

## Complete Patch Inventory

### ArduinoMongoose (fork) — mongoose.c

| # | Location | Change | Phase |
|---|---|---|---|
| 1 | `mg_parse_address()` ~2683 | Bare port → AF_INET6 when MG_ENABLE_IPV6 | 2 |
| 2 | `mg_socket_if_connect_tcp()` ~3685 | `socket(sa->sa.sa_family, ...)` instead of `AF_INET` + fix `connect()` addrlen (patch 10c) | 2 |
| 3 | `mg_socket_if_connect_udp()` ~3700 | `socket(sa->sa.sa_family, ...)` instead of `AF_INET` | 2 |
| 4 | `mg_open_listening_socket()` | Add explicit `setsockopt(IPV6_V6ONLY=0)` (**mandatory**) | 2 |
| 5 | `resolve_cb()` ~3052 | Add `MG_DNS_AAAA_RECORD` handling | 3 |
| 6 | `mg_connect_opt()` ~3182 | Use variable DNS query type | 3 |
| 7 | `mg_lwip_handle_accept()` ~15228 | Set `sa.sa.sa_family` based on PCB type; fix `sizeof(sa.sin)` → `sizeof(sa)` | 2 |
| 8 | `mg_lwip_if_connect_tcp_tcpip()` ~15108 | Branch on `sa_family` for IPv4 vs IPv6 address extraction | 3 |
| 9 | `mg_lwip_if_listen_tcp_tcpip()` ~15292 | Branch on `sa_family` for bind address; use `IP_ANY_TYPE` or `IP_ADDR_ANY` | 3 |
| 10 | `mg_lwip_if_udp_send()` ~15760 | Check `sa_family` for IPv6 destination, set `.type` correctly | 3 |
| 10b | `mg_socket_if_udp_send()` ~3744 | Fix `sendto()` addrlen: `sizeof(nc->sa.sin)` → family-conditional | 2 |
| 10c | `mg_socket_if_connect_tcp()` ~3693 | Fix `connect()` addrlen: `sizeof(sa->sin)` → family-conditional (16 vs 28 bytes) | 2 |
| 10d | `mg_accept_conn()` ~3804 + 4 others (2758, 2911, 3003, 3018) | Replace `inet_ntoa()` debug calls with `mg_sock_addr_to_str()` — prints garbage for IPv6 | 2 |
| 11 | `mg_lwip_udp_recv_cb()` ~15506 | Use `SET_ADDR` macro instead of `ip_2_ip4()` for IPv6 source addresses | 2 |
| 12 | UDP macro block ~15270 | Add `UDP_NEW` macro mapping to `udp_new_ip6` when `MG_ENABLE_IPV6` | 2 |
| 13 | `mg_lwip_if_connect_udp_tcpip()` ~15545 | Replace `udp_new()` with `UDP_NEW` macro | 1 |
| 14 | `mg_lwip_if_listen_udp_tcpip()` ~15653 | Replace `udp_new()` with `UDP_NEW` macro; branch on `sa_family` for bind address | 3 |

### openevse_esp32_firmware

| # | Location | Change | Phase |
|---|---|---|---|
| 15 | `platformio.ini` | Add `-D MG_ENABLE_IPV6=1` build flag | 1 |

**Compilation note for `MG_ENABLE_IPV6=1`:** The raw LwIP code block (lines 14874-15762) in mongoose.c references `tcp_new_ip6` / `tcp_bind_ip6` / `udp_bind_ip6` which are LwIP 1.x APIs and don't exist in LwIP 2.x. However, this block is gated by `#if MG_ENABLE_NET_IF_LWIP_LOW_LEVEL` which evaluates to 0 on ESP32 (which uses `MG_NET_IF_SOCKET`). So the undefined symbols are never compiled — no link errors. The `MG_ENABLE_IPV6=1` flag only activates IPv6-aware code in `mg_parse_address()`, `mg_sock_addr_to_str()`, `mg_dns_parse_record_data()`, and the DNS resolver, all of which compile fine against LwIP 2.x.
| 16 | `src/net_manager.cpp` | `WiFi.enableIpV6()` / `ETH.enableIpV6()` (v2.x naming) | 1 |
| 17 | `src/net_manager.h` | IPv6 address storage + accessors | 1 |
| 18 | `src/net_manager.cpp` | `GOT_IP6` event handler | 1 |
| 19 | `src/web_server.cpp` | IPv6 addresses in status/config API | 1 |
| 20 | MQTT/EmonCMS/OCPP configs | IPv6 address format support | 4 |

---

## Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Dual-stack socket on LwIP | **Resolved** | `socket(AF_INET6)` + V6ONLY=0 creates `IPADDR_TYPE_ANY` PCB. Confirmed in esp-lwip source. |
| `tcp_new_ip6()` in raw LwIP path | **Confirmed broken** | Not relevant for ESP32 (socket path). Raw LwIP patches are separate and untested. |
| DNS AAAA queries break IPv4-only targets | High | A-first queries with AAAA added later. Do NOT ship AAAA-first without fallback. |
| Memory pressure from IPv6 | Low | ESP32 rev 3 with 16MB flash. Monitor free_heap. |
| Breaking IPv4 | Low | Dual-stack preserves IPv4. Test regression at each phase. |
| Upstream won't accept Mongoose patches | High | Patches live in our fork. Offer upstream later. |
| `mg_parse_address` change affects non-ESP32 platforms | Medium | Guard with `#if MG_ENABLE_IPV6` — only changes behavior when IPv6 is enabled |
| Captive portal breaks with IPv6 | Low | Captive portal stays IPv4-only in v0. v2 adds IPv6 support separately. See v0→v2 compatibility below. |
| **IPv6 lost after WiFi reconnect** | **High (now fixed)** | Must call `WiFi.enableIpV6()` in `STA_CONNECTED` handler. `esp_netif_down()` clears all IPv6 addresses on disconnect. No persistent WANT_IP6 bit exists in v4.4. |
| **Firmware size exceeds partition** | Medium | Default partition has only ~110 KB headroom. MG_ENABLE_IPV6=1 adds ~10-20 KB. Monitor size. Fallback: `min_spiffs_debug.csv` (3.75 MB) or 16 MB partition. |
| **NTP fails on IPv6-only networks** | Low (Phase 5) | SNTP uses Mongoose DNS (A-only until Phase 3b). Only affects IPv6-only networks. Dual-stack networks use IPv4 for NTP. |
| **IPv6-only networks** | **Solved by custom LwIP** | Prebuilt LwIP has `RDNSS_MAX_DNS_SERVERS=0` (RDNSS compiled out). Custom framework package with `RDNS_MAX_DNS_SERVERS=2` enables auto-learning of IPv6 DNS from Router Advertisements. Built via `esp32-arduino-lib-builder`, hosted as GitHub repo, referenced via `platform_packages`. Zero ABI impact, no OTA breakage. See Phase 0 section. |
| **3-address LwIP cap** | Low | `LWIP_IPV6_NUM_ADDRESSES=3` is prebuilt, un-tunable. Multi-prefix networks may churn addresses. Handler must tolerate address churn. Could increase via custom `liblwip.a` (same process as Phase 0 RDNSS) if needed. |
| **MQTT IPv6 literal ambiguity** | Medium | `mqtt_server + ":" + mqtt_port` creates ambiguous address for IPv6 literals. Fix: bracket-qualify in Phase 4. |
| **WiFi power save must stay disabled** | **By constraint** | `WIFI_PS_NONE` required for SLAAC RA reception. Do not enable WiFi power save — it silently breaks IPv6 address renewal. |

---

## Dependency Graph

```
Phase 0 (Custom LwIP RDNSS)      ← one-time build, no external deps
     │
Phase 1 (WiFi Layer + build flag)  ← requires Phase 0 for IPv6-only networks
     │
     ├── Phase 2 (HTTP dual-stack) ← requires ArduinoMongoose fork, patches 1-4
     │
     └── Phase 3 (DNS AAAA)       ← requires ArduinoMongoose fork, patches 5-6
            │
            Phase 4 (App integration) ← requires Phase 2 + 3
               │
               Phase 5 (Hardening) ← requires Phase 4 on both devices
```

Phases 2 and 3 can be done in parallel since they touch different functions in mongoose.c.

---

## Fork Setup

### ArduinoMongoose

```bash
cd ~/workspace
gh repo fork jeremypoulter/ArduinoMongoose --clone=false
git clone git@github.com:rnavarro/ArduinoMongoose.git
cd ArduinoMongoose
git remote add upstream git@github.com:jeremypoulter/ArduinoMongoose.git
git checkout -b integration
git push -u origin integration
git checkout -b fix/ipv6-dual-stack
```

### Custom Arduino ESP32 Framework (RDNSS-enabled)

```bash
cd ~/workspace
# Fork arduino-esp32, replace liblwip.a with RDNSS-enabled rebuild
git clone git@github.com:rnavarro/framework-arduinoespressif32-rdnss.git
cd framework-arduinoespressif32-rdnss
# The RDNSS-enabled liblwip.a is already committed on idf-release/v4.4 branch
```

Reference in `platformio.ini`:
```ini
platform_packages = framework-arduinoespressif32@https://github.com/rnavarro/framework-arduinoespressif32-rdnss.git#idf-release/v4.4
```

### OpenEVSE firmware

In `platformio.ini`, replace:
```
jeremypoulter/ArduinoMongoose@0.0.22
```
With:
```
../../workspace/ArduinoMongoose
```

---

## Additional Findings from Deep Research

### Arduino ESP32 Core v2.x API Differences

The firmware uses `espressif32@6.12.0` which ships Arduino ESP32 core **v2.0.17** (ESP-IDF v4.4.7). This is NOT v3.x — all API names differ:
- `WiFi.enableIpV6()` (not `enableIPv6()`)
- `WiFi.localIPv6()` returns link-local only (not global)
- No `hasGlobalIPv6()` or Arduino `globalIPv6()` wrapper — use event payload `info.got_ip6.ip6_info.ip` + `esp_netif_ip6_get_addr_type()` to classify addresses
- `IPv6Address` is a separate class from `IPAddress` (has `.toString()`)

### MQTT Announce URL — Add `http6` field (Phase 2, ~5 lines)

`src/mqtt.cpp` line ~164 constructs an IPv4-literal announce URL: `"http://" + net.getIp() + "/"`. Add an `http6` field when a global IPv6 address exists.

**Announce payload** (JSON on `openevse/announce/<shortId>`, retained):
```json
{"state":"connected","id":"...","name":"...","mqtt":"...","http":"http://192.168.1.50/","http6":"http://[2001:db8::1]/"}
```

**Minimal change in `onMqttConnect()`:**
```cpp
DynamicJsonDocument doc(JSON_OBJECT_SIZE(6) + 300);  // was (5) + 200
// ... existing fields ...
doc["http"] = "http://" + net.getIp() + "/";
String ipv6 = net.getIp6Global();
if (ipv6.length() > 0) {
  doc["http6"] = "http://[" + ipv6 + "]/";  // bracket notation
}
```

- Backward compatible — additive JSON field, existing subscribers ignore unknown keys
- Only announce global IPv6 (not link-local fe80::, not ULA fd00::/fc00::)
- Last-will message does NOT include `http` field, so no change needed there
- Nice-to-have: `notifyIpv6Changed()` to re-publish announce when global IPv6 changes
- No HA autodiscovery format — this is a custom announce, not `homeassistant/sensor/...`
- Prerequisite: Phase 1 must be done first (`getIp6Global()` accessor)

### Captive Portal — IPv4-only in v0 (v2 effort documented below)

Uses Arduino `DNSServer` library (not Mongoose). IPv4-only A records. AP should stay IPv4 in v0 — no changes needed. The redirect URL uses `net.getIp()` which returns `192.168.4.1` in AP mode.

**v2 Captive Portal IPv6 would require:**
1. Patch or replace DNSServer to add AAAA response (modern DNSServer correctly filters QType — responds to A/ANY only, sends SOA negative response for AAAA. Not a bug, but means IPv6 clients get no DNS redirect. Fix options: ship a custom `DNSServerV6` class, or patch framework source directly. Cannot subclass — no virtual methods, all handlers are private. `_dnsServer` is a member by composition, so the header type must change.)
2. Call `WiFi.softAPenableIpV6()` in `wifiStartAccessPoint()` to give the AP interface a link-local address
3. Implement Router Advertisement sending on the soft AP (LwIP does not provide a built-in RA sender for AP mode — significant work)
4. Update redirect URL construction in `handleNotFound()` for IPv6 bracket notation: `http://[fe80::...]/`
5. Handle link-local zone IDs (`%wlan0`) for Android/iOS browser compatibility

**Why it's v2 and not v0:** Without RAs, clients have no IPv6 default gateway, no SLAAC prefix, and no IPv6 DNS — they use IPv4 exclusively for captive detection. IPv4 works perfectly on 100% of devices. The only scenario where v2 matters is an IPv6-only phone without IPv4, which effectively doesn't exist.

**v0 decisions that must NOT block v2:** See [v0 → v2 Compatibility](#v0--v2-captive-portal-compatibility) section below.

### Already-Clean Paths (No Changes Needed)

| Component | Why Clean |
|---|---|
| `mg_open_listening_socket()` | Already uses `sa->sa.sa_family` and correct `sa_len` |
| `mg_socket_if_listen_tcp()` | Delegates to `mg_open_listening_socket()` |
| `mg_socket_if_get_conn_addr()` | Uses `getpeername()`/`getsockname()`, AF-agnostic |
| `mg_socket_if_tcp_send/recv()` | Plain `read()`/`write()`, AF-agnostic |
| `mg_parse_address()` `[IPv6]:port` | Already handles bracket notation when `MG_ENABLE_IPV6` |
| `mg_connect_http_base()` URL parsing | Preserves brackets through to `mg_parse_address()` |
| WebSocket frames | Pure byte stream, no AF awareness |
| SSL/TLS (mbedTLS) | Wraps existing socket, AF-agnostic |
| HTTP Host header | Brackets preserved from URL parsing, RFC-compliant |
| OCPP (MicroOcpp) | Uses Mongoose WebSocket, DNS handled internally |
| HTTP OTA | Uses Mongoose HTTP client, DNS handled internally |
| EmonCMS | Uses Mongoose HTTP client, DNS handled internally |
| `mg_sock_addr_to_str()` | Handles AF_INET6 with bracket notation when `MG_ENABLE_IPV6=1` |

### IPv4-Mapped Addresses — Fix debug + add stripping (Phase 2)

When IPv4 clients connect to a dual-stack listener, `accept()` returns `::ffff:a.b.c.d`.

**No user-facing peer IP display exists in the firmware.** The firmware never shows client IPs in the UI, API, or logs. No IP-based access control, allowlists, blocklists, or rate limiters exist. All display is the device's own IP.

**Two changes needed (both in Mongoose fork, not firmware):**

1. **Fix 5 `inet_ntoa()` debug calls** that print garbage for IPv6 connections (lines 2758, 2911, 3003, 3018, 3804). Replace with `mg_sock_addr_to_str()` which handles both AF_INET and AF_INET6. ~25 lines changed.

2. **Add IPv4-mapped stripping to `mg_sock_addr_to_str()`** after the `inet_ntop()` call. Detection via `IN6_IS_ADDR_V4MAPPED()` macro (available in ESP32 newlib). Rewrite `::ffff:192.168.1.5` as `192.168.1.5` for display. ~8 lines added. Always-on when `MG_ENABLE_IPV6=1` — the `::ffff:` prefix is a dual-stack implementation artifact, not meaningful to any consumer. (Go's `net.IP.String()` does this automatically; nginx/Apache do not.)

**No firmware-side changes needed.** If peer IP display is ever needed in the future, add `clientIP()` method to `MongooseHttpServerRequest` using `mg_conn_addr_to_str()` with `MG_SOCK_STRINGIFY_REMOTE`.

---

## v0 → v2 Captive Portal Compatibility

Verification confirms **no v0 blockers** — the current code takes no irreversible design decisions that would prevent a future v2 captive portal IPv6 effort. This section documents what v0 must preserve, and what v2 will need to touch.

### What v0 must NOT do

| Constraint | Why |
|---|---|
| **Do NOT call `WiFi.softAPenableIpV6()` in AP mode** | Gives the AP interface a link-local address that nothing uses. Wastes ~300-500 bytes heap. v2 adds it when needed. |
| **Do NOT store or advertise the AP's link-local address** | `getIp6Global()` returns empty in AP mode (no global IPv6). The MQTT announce code already only advertises when global exists. No change needed. |
| **Do NOT modify `handleNotFound()` redirect URL** | Stays `http://192.168.4.1/` in v0. v2 adds bracket-aware IPv6 URL construction. |
| **Do NOT touch DNSServer** | Stays IPv4-only in v0. v2 patches or replaces it for AAAA support. The current QType filtering (A/ANY only) is correct behavior — v2 adds AAAA, not fixes a bug. |
| **Do NOT change `_dnsServer` from composition to pointer** | v2 will need to replace the `DNSServer` member type anyway (see below). If v0 changes it to a pointer with heap allocation, v2 must change the destructor/copy semantics too. Leave the composition as-is — v2 does the refactor in one shot. |

### What v0 MUST ensure for v2 compatibility

| Requirement | Detail |
|---|---|
| **`MG_ENABLE_IPV6=1` must be a global build flag** | Already required by the ODR invariant. If scoped to the library only, the AP-mode Mongoose HTTP server has the wrong `sizeof(struct mg_connection)`. |
| **`mg_parse_address("80")` → AF_INET6 works in AP mode** | With the Phase 2 patch, bare port `"80"` sets `sa_family = AF_INET6`. In AP mode, `bind([::]:80)` succeeds and accepts IPv4 connections via `IPV6_V6ONLY=0` mapped addresses. This is correct behavior — no special AP-mode handling needed. **Must verify at runtime.** |
| **`IPV6_V6ONLY=0` setsockopt fires in AP mode** | The restructured `mg_open_listening_socket()` applies equally in AP and STA mode. The AP-mode Mongoose HTTP server uses `begin(80)` → `mg_bind("80")` → `mg_open_listening_socket()`. The setsockopt fires. On LwIP it defaults to 0 anyway, so the call is a safety belt — but it must not fail. |
| **DNSServer and Mongoose are independent** | DNSServer uses `AsyncUDP` (async callback on port 53). Mongoose uses POSIX `socket()/bind()/listen()` on port 80. They don't share sockets, don't interfere. Enabling IPv6 on the Mongoose side does not affect DNSServer. Note: `_dnsServer.processNextRequest()` at `net_manager.cpp:719-722` is a no-op stub — modern DNSServer is fully async. |
| **Redirect URL uses `net.getIp()`, not hardcoded IP** | Already the case — `web_server.cpp:1099` builds `"http://" + net.getIp()`. In v0, `getIp()` returns IPv4 in AP mode. In v2, a new method or modified getter could return an IPv6-compatible URL. |
| **GOT_IP6 events are silently ignored** | The event handler at `src/net_manager.cpp:376-446` has no case for `ARDUINO_EVENT_WIFI_STA_GOT_IP6` or `ARDUINO_EVENT_WIFI_AP_GOT_IP6` — both fall through to `default: break;`. This is safe: v0 never enables IPv6 so these events never fire. v2 adds handlers for the distinct STA/AP IP6 events. The important thing is v0 does NOT add a handler that accidentally stores the wrong address type. |
| **`_ipaddress` is a single field** | `net_manager.h:215` returns a single `_ipaddress` String. In AP mode it's set to `192.168.4.1` once and never overwritten (STA never connects). v2 adds a new `_ip6address` field and `getIp6()` getter — does not modify the existing field. v0 must not change the field to a combined type that would complicate v2. |
| **MQTT announce never fires in AP mode** | `mqtt.cpp:87` is guarded by `net.isConnected()` which returns `false` in AP mode (requires STA). So the `http://<ip>/` announce URL is always STA IPv4. v2 adds an `http6` field for STA IPv6 — no conflict with AP mode. |

### AP-mode dual-stack verification (runtime test for v0)

After Phase 2 is deployed, verify these AP-mode scenarios:

1. **WiFi config UI still accessible** at `http://192.168.4.1/` from phone connected to soft AP
2. **Captive portal redirect still works** — phone auto-detects and shows config page
3. **Mongoose debug log shows `::ffff:192.168.4.x`** for IPv4 clients (not garbage; after Phase 2 stripping, displays as `192.168.4.x`)
4. **DNSServer responds correctly** to DNS queries from connected clients
5. **Heap remaining** in AP mode is acceptable (>50KB free) with `MG_ENABLE_IPV6=1`

### v2 Captive Portal IPv6: Exact patches required

For reference, here is what a v2 captive portal IPv6 effort would entail, anchored to exact code locations:

#### Mongoose patches (in ArduinoMongoose fork)

1. **`mg_parse_address` — port-only strings return AF_INET6 when `MG_ENABLE_IPV6=1`** (`mongoose.c:2692-2694`). Currently `"80"` sets `sa->sin.sin_family = AF_INET`. v2 changes this to `AF_INET6` + `IN6ADDR_ANY` when IPv6 is enabled, so `bind([::]:80)` creates a dual-stack listener.

2. **`mg_open_listening_socket` — add `IPV6_V6ONLY=0` for LwIP** (`mongoose.c:3817-3841`). Currently ALL `setsockopt` calls are inside `#if !MG_LWIP` which is **0 on ESP32** — LwIP gets zero socket options. v2 must add `setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off))` **outside** the `#if !MG_LWIP` guard, requiring the chained conditional to be broken into separate statements. LwIP on ESP32 does support this `setsockopt` — it's defined in the LwIP socket API when `LWIP_IPV6=1`.

3. **`mg_sock_addr_to_str` — strip IPv4-mapped prefix** (as documented in Phase 2). Add `IN6_IS_ADDR_V4MAPPED()` check after `inet_ntop()`, rewrite `::ffff:192.168.4.x` as `192.168.4.x`.

#### Firmware patches (in openevse_esp32_firmware)

4. **Call `WiFi.softAPenableIpV6()` in `wifiStartAccessPoint()`** (`src/net_manager.cpp:86-87`) — gives the AP interface a link-local `fe80::` address via `esp_netif_create_ip6_linklocal()`. Memory cost: ~300-500 bytes heap, negligible.

5. **Add `ARDUINO_EVENT_WIFI_AP_GOT_IP6` handler** (`src/net_manager.cpp:376-446`) — store the AP's link-local address in a new `_apIp6Address` field. This is distinct from the STA GOT_IP6 event handler (separate enum values, no confusion).

6. **Update `handleNotFound()` redirect URL** (`src/web_server.cpp:1096-1111`) — when an IPv6 client connects, build `http://[fe80::...]/` with bracket notation. Link-local requires zone ID (`%wlan0`) which has inconsistent browser support. May need to fall back to IPv4 redirect for link-local.

7. **Patch or replace DNSServer for AAAA queries** — modern DNSServer (`libraries/DNSServer/src/DNSServer.cpp` in arduino-esp32) **does check QType**: only A (1) and ANY (255) get `replyWithIP()`, AAAA (28) gets `replyWithNoAnsw()` (SOA negative response with TTL=5). This is not a protocol violation — it's correct "no IPv6 here" behavior — but it means IPv6 clients never get redirected via DNS. Options:
   - **Wrap, don't subclass**: DNSServer has no virtual methods and all handlers are `private`. Subclassing won't work. Instead, v2 either (a) ships a custom `DNSServerV6` class alongside the firmware that extends `replyWithIP()` logic, or (b) patches the framework source directly (fragile, breaks on core updates).
   - **`processNextRequest()` is a no-op**: Modern DNSServer uses `AsyncUDP::onPacket()` for async callback handling. The firmware's `_dnsServer.processNextRequest()` call at `net_manager.cpp:719-722` is dead code. v2 doesn't need to touch this — the fix is in the async handler, not the polling loop.
   - **`_dnsServer` is by composition**: `net_manager.h` declares `DNSServer _dnsServer` as a direct member (not a pointer). Replacing it with a custom class requires changing the header type. This is a minor refactor (~5 lines) but must happen.
   - **`replyWithIP()` hardcodes DNS_TYPE_A**: The answer record always writes a 4-byte IPv4 address (`DNS_RDLENGTH_IPV4`). A v2 `replyWithIPv6()` method would need to write a 16-byte AAAA record with the AP's link-local address.

8. **Implement Router Advertisement sending** — LwIP does not provide a built-in RA sender for AP mode. Would need to craft and send ICMPv6 RA packets with prefix information options. This is significant work (essentially implementing IPv6 router functionality from scratch). Without RAs, clients have no IPv6 connectivity beyond link-local Neighbor Discovery.

#### Practical assessment

Items 1-3 are already part of v0 Phase 2 (the dual-stack listener is needed for STA mode too). Items 4-7 are ~3 days. Item 8 (RA sender) is ~3-5 days and may not be worth it: IPv4 captive portal works on 100% of devices, phones on the soft AP get DHCPv4 (192.168.4.x) and never attempt IPv6 for captive detection because there are no RAs. The only scenario where IPv6 captive portal matters is an IPv6-only phone without IPv4, which effectively doesn't exist.

**Effort estimate:** ~1 week for items 4-7 (firmware + DNSServer), ~2 weeks total if RA sender is included. The RA sender is the hardest part and delivers questionable value.

---

## Success Criteria

**Phase 0 (Custom LwIP):**
- [x] `esp32-arduino-lib-builder` builds custom library with `CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS=2`
- [x] Custom `liblwip.a` linked via `custom_libs/` + `scripts/override_lwip.py` (LIBPATH preemption)
- [x] Device auto-learns IPv6 DNS from Router Advertisements (confirmed: `fd00:0:3801:9::2` received via RDNSS)
- [x] `WiFi.hostByName()` resolves over IPv6 DNS (RDNSS-supplied) — not separately tested but NTP works on dual-stack

**Phase 1 (Enable IPv6 stack):**
- [x] Both EVSE units have IPv6 global addresses (visible in `/status`)
- [x] Ethernet IPv6 works on wired builds (GOT_IP6 fires, addresses reported)
- [x] IPv6 addresses stored from GOT_IP6 event payload (not fragile ifkey lookups)
- [x] IPv6 cleared on WiFi disconnect, re-enabled on reconnect

**Phase 2 (Mongoose HTTP dual-stack):**
- [x] Web UI accessible over IPv6 (`curl -6 http://[2603:8000:2d00:4605:b68a:0aff:fe75:c107]/status` returns 200)
- [x] Web UI still accessible over IPv4 (no regression)
- [x] mDNS advertises IPv6 global address (`avahi-resolve -6` returns `2603:8000:2d00:4605:b68a:0aff:fe75:c107`)
- [x] IPv4 functionality unchanged (regression testing)
- [x] AP-mode captive portal still works after Phase 2 (not separately tested on Olimex — WiFi-only feature)
- [x] Response parity: IPv4 and IPv6 `/status` JSON both have 78 keys, identical content

**Phase 3 (complete — AAAA-first DNS in Mongoose + MQTT pre-resolve hardening):**
- [x] MQTT connects to broker over IPv6 (pre-resolve workaround with getaddrinfo, not Mongoose DNS)
- [x] Mongoose DNS emits AAAA queries natively (Phase 3b — AAAA-first, A-fallback in resolve_cb)
- [x] IPv6 failure cooldown (2 consecutive failures → 10-min AAAA suppression, IPv4 fallback)
- [x] MQTT SNI preserved over IP literals via setTlsServerName()
- [x] DNS cache with 5-min TTL + IP literal fast-path
- [x] 30s connecting watchdog
- [x] net_manager decoupled from mqtt.h (MicroTasks::Event)
- [x] GOT_IP6 triple guard debounce (transition + re-check + family check)
- [x] WiFi STA mDNS AAAA slot swap parity with ETH
- [x] millis() unsigned long overflow-safe comparison

**Phase 4 (Application Layer — Full Integration):**
- [x] MQTT config accepts IPv6 broker addresses in `[bracket]:port` format (since Phase 3)
- [x] EmonCMS server URLs with IPv6 literals auto-bracketed via `ensureIpv6Brackets()`
- [x] OCPP server URLs with IPv6 literals auto-bracketed via `ensureIpv6Brackets()`
- [x] HTTP OTA — uses file upload, not URL-based (no change needed)
- [x] Web UI displays `ipv6address_global` and `ipv6address_linklocal` on Network page
- [x] Captive portal IPv4-only in v0 (confirmed no v0/v2 blockers)
- [x] *Hardware verification: IPv6 literals in config fields* — verified 2026-06-04:
  EmonCMS server field accepted `http://[2603:…:d490]:8888` via web UI without
  mangling; saved, persisted, and posted successfully over IPv6.
- [x] *Hardware verification: Network page shows IPv6 addresses* — verified
  2026-06-04 in Firefox: Network page displays both `IPv6:` (global) and
  `IPv6 (LL):` (link-local) fields.
- [x] *Hardware verification: confirm AP mode captive portal still works* — verified
  2026-06-04 on the c9078a1 integration build (dual-stack Mongoose listener +
  net_manager per-interface refactor). Bogus SSID → AP fallback (~23s of STA
  retries) → Android phone joined `OpenEVSE_c104` → captive portal **auto-popped**
  (DNS redirect intact) → setup page served from 192.168.4.1 → selected WLAN_2G →
  "Connection successful" dialog → device reconnected with full IPv4+IPv6+MQTT
  in ~6s. Note: post-setup redirect dialog points at the IPv4 literal
  (`172.16.5.162`) — documented v0 behavior, and the right choice for a client
  mid-network-hop. SSID config changes require a device restart to apply
  (`wifiRestartTime` in web_server.cpp is checked but never set — pre-existing
  upstream quirk, not an IPv6 regression).
- [x] *End-to-end IPv6-by-hostname from a desktop browser* — verified 2026-06-04:
  Firefox loads `http://openevse-c104.local` over IPv6. Client-side notes for
  Linux desktops (not firmware issues):
  - Ubuntu's default nsswitch uses `mdns4_minimal` (IPv4-only) — switch to
    `mdns_minimal` for AAAA from `.local` via libc.
  - Snap-packaged browsers (Firefox, Chromium) cannot use host nsswitch mDNS
    modules at all (long-standing snapd issue, forum.snapcraft.io/t/7603).
    Fix system-wide via systemd-resolved: `MulticastDNS=resolve` in
    /etc/systemd/resolved.conf.d/mdns.conf + `resolvectl mdns <link> resolve` —
    snaps query 127.0.0.53 and get mDNS AAAA+A from resolved.
  - Firefox caches negative DNS results ~60s — clear via about:networking#dns.

**GUI Fork & Submodule Setup (completed):**

- The firmware build uses the **submodule** at `openevse_esp32_firmware/gui-v2` (not the
  standalone clone at `~/workspace/openevse-gui-v2`). Confirmed via `scripts/extra_script.py`
  line 188: `gui_dir = join(PROJECT_DIR, "gui-v2")`.

- Forked `OpenEVSE/openevse-gui-v2` → `rnavarro/openevse-gui-v2` on GitHub.

- **Standalone clone** (`~/workspace/openevse-gui-v2`) reconfigured:
  - `origin` → `git@github.com:rnavarro/openevse-gui-v2.git`
  - `upstream` → `https://github.com/OpenEVSE/openevse-gui-v2.git`
  - Local `integration` branch (not pushed) merges both fix branches for convenience
  - `master` stays clean mirror of upstream

- **Submodule** (`openevse_esp32_firmware/gui-v2`) reconfigured:
  - `.gitmodules` URL updated to `git@github.com:rnavarro/openevse-gui-v2.git`
  - `origin` → `git@github.com:rnavarro/openevse-gui-v2.git`
  - `upstream` → `https://github.com/OpenEVSE/openevse-gui-v2.git`
  - Checked out on local `integration` branch (both patches merged)

- **Fix branches on fork** (both pushed):
  - `fix/eco-timer-scheduling` — Eco state option in timer scheduling (8 files)
  - `fix/ipv6-network-page` — Display IPv6 addresses on Network config page (1 file)

- **Non-breaking guarantee:** Both patches are purely additive. The IPv6 display uses
  `{#if}` conditionals — if the firmware doesn't send `ipv6address_global` or
  `ipv6address_linklocal` in the `/status` JSON, the blocks simply don't render.
  The eco timer change adds a new scheduler dropdown option with no effect on existing
  functionality. Either patch can be upstreamed independently without breaking upstream
  firmware.

- **Upstreaming flow:** GUI-v2 is a submodule dependency of the main repo. Changes must
  be pushed to the fork first, then the main repo updates its submodule reference. When
  upstreaming, each fix branch goes as a separate PR to `OpenEVSE/openevse-gui-v2`.
  `integration` is ephemeral and local-only — never pushed.

**Phase 5 (Hardening and Testing):**
- [x] IPv6 addresses restored after WiFi reconnect — **verified 2026-06-04 on Olimex**.
  ETH unplug → AP mode → WiFi STA → IPv4 MQTT → IPv6 upgrade all work correctly.
  WiFi uses `c104` MAC suffix (different from ETH `c107`), gets its own SLAAC global
  address `2603:8000:2d00:4605:b68a:0aff:fe75:c104`. MQTT broker log confirms clean
  upgrade pattern with no duplicate sessions or stuck connections. ETH→WiFi transition
  takes ~92s (AP mode timeout before STA attempt) — expected behavior, not a bug.
- [x] MQTT broker confirms clean IPv6 operation — verified against gokrazy mqtt-server
  log (`http://mqtt-server/log?path=%2fuser%2fmqtt-server&stream=stdout`). Every boot
  shows IPv4 connect → clean DISCONNECT + FIN → IPv6 connect. ETH unplug detected via
  keepalive i/o timeout (~85s, = 1.5× 60s keepalive). `"use of closed network
  connection"` errors in broker log are normal Go behavior for a clean remote TCP close
  during a blocking read — firmware sends proper MQTT DISCONNECT before FIN.
- [x] EmonCMS posts work over IPv6 — verified 2026-06-04 on Olimex (WiFi) against a
  mock EmonCMS server with four DNS configurations:
  | Host config | Result | Path |
  |---|---|---|
  | A+AAAA | success ~100ms | IPv6 |
  | A only | success ~100ms | IPv4 |
  | AAAA only | success ~100ms | IPv6 |
  | valid A + blackhole AAAA | 1st attempt 23s timeout, retry 98ms | IPv4 via cooldown |
  Testing surfaced two real bugs, both fixed in the ArduinoMongoose fork
  (commit 865d00d on fix/ipv6-dual-stack):
  1. **Nameserver corruption**: `WiFi.dnsIP(0)` read the first 4 bytes of an IPv6
     RDNSS server in LwIP DNS slot 0 as IPv4 — `fd00::...` became `253.0.0.0`,
     silently breaking ALL Mongoose DNS (EmonCMS, OCPP, SNTP, OHM) once a global
     IPv6 address arrived. Fixed: `esp_netif_get_dns_info()` with explicit
     address-type checking (prefer IPv4 DNS, fall back to IPv6 RDNSS).
  2. **No IPv6 connect-failure fallback**: AAAA-first DNS fell back to A only when
     no AAAA record existed. If the AAAA resolved but the IPv6 TCP connect failed
     (blackhole route), the connection just timed out with no IPv4 retry — and this
     affected every Mongoose TCP client. Fixed with a host-keyed cooldown cache in
     mongoose.c (6 slots, 5-min TTL, ~330B static): an IPv6 connect failure records
     the hostname; subsequent connects for that host skip AAAA and resolve A
     directly, so app-level retries (EmonCMS cycle, OCPP reconnect) succeed fast
     over IPv4. Per-host keyed — hosts with working IPv6 unaffected. IP literals
     never enter the cache. SNTP (UDP, connectionless) keeps its own timeout-retry.
     Design per Opus consult; a first attempt at an in-poll-loop re-resolve caused
     a use-after-free crash loop (Mongoose owns the nc lifecycle — never pause a
     failed connection for re-resolve; fresh connection per attempt only).
  Bonus fix: `mg_destroy_conn` never freed the hostname strdup'd for AAAA→A
  fallback — leaked on every successful AAAA resolve. Now freed.
- [x] HTTP OTA updates work over IPv6 — verified 2026-06-04 on Olimex over WiFi IPv6.
  1.8MB firmware upload to `http://[2603:…:c104]/update` returned HTTP 200. Device
  rebooted and came back with full IPv4+IPv6 in ~12.6s. Command:
  `curl -6 --max-time 120 -F "file=@firmware.bin" "http://[<ipv6>]/update"`
  Bonus: a truncated upload (60s timeout, 81% complete) triggered automatic OTA
  rollback — ESP32 bootloader detected `invalid segment length 0xffffffff` and
  reverted to previous firmware. OTA safety net confirmed working.
- [ ] No memory leaks or crashes over 1+ week runtime
- [x] IPv4-mapped addresses display as `a.b.c.d` not `::ffff:a.b.c.d` in debug logs — **non-issue confirmed**: Mongoose `inet_ntoa()` calls operate on `nc->sa.sin.sin_addr` (IPv4-only struct, can't produce `::ffff:` output). Application-layer IPv6 display uses LwIP's `ip6addr_ntoa()` which produces proper `2001:db8::1` notation. DNS-level IPv4-mapped filtering in `mqtt.cpp` two-step `getaddrinfo()` prevents `::ffff:` from ever reaching the connect path.
- [ ] IPv6-only network test: disable IPv4, verify EVSE still functions
- [ ] Address change resilience — **deferred, low priority**. EUI-64 SLAAC on a stable
  home prefix means mid-session address changes are rare. Existing TCP connections
  survive address deprecation (RFC 4862) and only break on flash renumbering
  (valid-lifetime=0). Already self-heals: dead socket → keepalive timeout → MQTT
  reconnects → fresh AAAA lookup picks new address. A proper fix requires two changes:
  (1) replace `had_global` guard in net_manager.cpp:632-637 with `new != current`
  change-detection, AND (2) add a separate "address changed while already on IPv6 →
  restart" path in mqtt.cpp:140-161 that bypasses the `!isConnectedViaIPv6()` gate
  (which currently makes the net_manager fix a no-op in the on-IPv6 case). No
  `LOST_IP6` event exists in ESP-IDF — address deprecation is silent in LwIP; only
  option is polling `ip6_addr_state[]` or relying on TCP self-heal. Self-heal is
  correct for home use. Revisit if upstreaming to users on dynamic ISPs. (Opus review
  2026-06-04)
- [x] Memory impact: RAM +136 bytes (+0.04%, 63,360→63,496 of 327,680). Well within heap budget.
- [x] Flash impact: Flash +11,120 bytes (+0.6%, 1,841,185→1,852,305 of 1,966,080). Fits 16MB partition at 94.2%.

**IPv6-only networks (precise scope):** Inbound HTTP over IPv6-only works after Phase 0-2 (SLAAC + RDNSS DNS + dual-stack listener). Outbound Mongoose connections (MQTT/EmonCMS/OCPP/SNTP/OHM) on IPv6-only networks work after Phase 3b — Mongoose DNS queries AAAA first with A-fallback, and MongooseCore configures IPv6 nameservers from RDNSS. LwIP-level DNS (`WiFi.hostByName()`) also works on IPv6-only after Phase 0. DHCPv6 remains out of scope (v1+).

## Consult-Driven Fixes (2026-06-04)

Three-model consult (GPT-5.5, Opus 4.8, Opus 4.6) reviewed the MQTT IPv6
connection code and identified 8 issues (4 must-fix, 4 should-fix). All 8
have been fixed and tested on Olimex hardware.

### Must-Fix (all resolved)

1. **IPv6 failure pins MQTT forever** — if IPv6 TCP connect fails but AAAA
   resolves, MQTT retried IPv6 indefinitely with no IPv4 fallback. Fixed with
   `_ipv6FailCount` / `_ipv6SuppressedUntil` tracking: after 2 consecutive
   IPv6 connect failures, AAAA queries are suppressed for 10 minutes.
   Resets on successful IPv6 connect.

2. **WiFi STA missing MQTT restart** — `WIFI_STA_GOT_IP6` handler stored IPv6
   but never triggered MQTT reconnect. Only ETH did. Fixed by adding
   `onGlobalIPv6Acquired()` call in both WiFi and ETH handlers.

3. **WiFi STA missing mDNS slot swap** — AAAA slot-swap workaround only ran
   in `ETH_GOT_IP6`. Fixed by parameterizing `onGlobalIPv6Acquired(ifkey)`
   with `"WIFI_STA_DEF"` / `"ETH_DEF"`.

4. **MQTTS SNI breaks with IP literals** — pre-resolving to `[2603:...]:8883`
   meant Mongoose never sent hostname for TLS SNI. Fixed by adding
   `_tls_server_name` field and `setTlsServerName()` method to
   `MongooseMqttClient`, which sets `opts.ssl_server_name` in
   `mg_connect_opt()`.

### Should-Fix (all resolved)

5. **GOT_IP6 fires repeatedly** — SLAAC renewal, RA re-advertisement re-trigger
   MQTT restart. Fixed with triple guard: (1) `!had_global` transition debounce
   in net_manager, (2) MQTT's EventListener re-checks `net.hasGlobalIPv6()`,
   (3) MQTT checks `!isConnectedViaIPv6()` so no restart when already on IPv6.

6. **millis() signed/unsigned overflow** — `_nextMqttReconnectAttempt` was `long`,
   `millis()` is `unsigned long`, comparison breaks at ~25 days. Fixed by
   changing to `unsigned long` and using overflow-safe `(long)(now - ...)`.

7. **Blocking getaddrinfo() in event loop** — two sequential synchronous DNS
   lookups can stall for up to 28 seconds. Fixed with: (a) IP literal fast-path
   that skips DNS when `mqtt_server` contains no alpha chars, (b) DNS cache with
   5-min TTL that reuses resolved address across reconnects, invalidated on
   IPv6 failure.

8. **Restart-while-connecting race** — `restartConnection()` zeroed
   `_connecting` and called `disconnect()` which is async. Fixed by removing
   the `_connecting = false` from restart handler; async disconnect cascades
   through `onClose` -> `onMqttDisconnect` which clears `_connecting`.
   Added `_pendingRestartForIPv6` flag for deferred upgrades.

### Architecture refactor: MicroTasks::Event decoupling

All three consultants identified a layering violation: `net_manager.cpp`
contained MQTT-specific policy (`mqtt.isConnected() &&
!mqtt.isConnectedViaIPv6()`). The fix follows the existing
`EvseMonitor::onStateChange` pattern in the codebase:

- `NetManagerTask` owns debounce + fires `MicroTasks::Event` on IPv6 transitions
- `onGlobalIPv6Acquired(ifkey)` centralizes mDNS slot swap, mDNS restart,
  event notification, and event emission
- `onGlobalIPv6Lost()` centralizes state clearing and event emission
- `onIPv6GlobalChanged(EventListener*)` public registration method
- `hasGlobalIPv6()` public query
- MQTT owns its own upgrade policy via `_ipv6GlobalListener` in its `loop()`
- `net_manager.cpp` no longer includes `mqtt.h`

Any future service (OCPP, EmonCMS) subscribes with one line:
`net.onIPv6GlobalChanged(&_myListener)`

## Test Results — Phase 0-2 (2026-06-04)

Hardware: Olimex ESP32-Gateway RevF (ESP32 rev 3, dual-core LX6, 16MB flash), wired Ethernet only
Firmware: `feat/ipv6-support` branch, commit `f02ace5`

### Before IPv6 changes (stock firmware)

```
OpenEVSE WiFI c104
Firmware: master
Git Hash: xxxxxxxx_modified
Build date: Jun  3 2026
IDF version: v4.4.7-dirty
Free: 270636
Server started
OpenEVSE not responding or not connected
Connected, IP: 172.16.5.158
```

Key observations:
- Only IPv4 address printed
- No IPv6 addresses
- No DNS server info
- mDNS AAAA returns nothing (no IPv6 to advertise)

### After IPv6 changes (feat/ipv6-support)

```
OpenEVSE WiFI c104
Firmware: local_feat/ipv6-support_4978b1be_modified
Git Hash: 4978b1be_modified
Build date: Jun  4 2026 00:06:51
IDF version: v4.4.7-dirty
Free: 270636
Server started
OpenEVSE not responding or not connected
Connected, ETH IPv6 link-local: fe80:0000:0000:0000:b68a:0aff:fe75:c107
Connected, IP: 172.16.5.158
Connected, IPv6 link-local: fe80:0000:0000:0000:b68a:0aff:fe75:c107
DNS0 (ETH_DEF IPv4): 172.16.9.2
DNS1 (ETH_DEF IPv4): 172.16.9.3
mDNS debug: ETH link-local: fe80:0000:0000:0000:b68a:0aff:fe75:c107
Connected, ETH IPv6 global: 2603:8000:2d00:4605:b68a:0aff:fe75:c107
DNS0 (IPv6 after GOT_IP6): fd00:0000:3801:0009:0000:0000:0000:0002
DNS1 (IPv6 after GOT_IP6): fd00:0000:3801:0009:0000:0000:0000:0003
mDNS: restarting to advertise global IPv6
IPv6 slot swap: swapped link-local (slot 0) with global (slot 1)
mDNS: restarted with global IPv6
```

Key observations:
- IPv4 address still printed first ✓
- IPv6 link-local arrives before IPv4 (GOT_IP6 fires before GOT_IP)
- IPv4 DNS servers from DHCP (172.16.9.2, .3)
- IPv6 DNS servers from RDNSS (fd00:0:3801:9::2, ::3) ✓
- Global IPv6 arrives seconds after link-local (Router Advertisement delay)
- mDNS restarted after slot swap to advertise global IPv6 ✓

### Functional test results

| Test | Command | Result |
|------|---------|--------|
| IPv4 HTTP | `curl -4 http://172.16.5.158/status` | 200 ✓ |
| IPv6 global HTTP | `curl -6 http://[2603:8000:2d00:4605:b68a:0aff:fe75:c107]/status` | 200 ✓ |
| IPv6 link-local HTTP | `curl -6 http://[fe80::b68a:aff:fe75:c107%bond0]/status` | 200 ✓ |
| mDNS A record | `avahi-resolve -4 -n openevse-c104.local` | `172.16.5.158` ✓ |
| mDNS AAAA record | `avahi-resolve -6 -n openevse-c104.local` | `2603:8000:2d00:4605:b68a:0aff:fe75:c107` ✓ |
| Response parity | IPv4 vs IPv6 /status JSON | 78 keys, identical ✓ |
| mDNS hostname HTTP | `curl http://openevse-c104.local/status` | 200 ✓ |
| RDNSS DNS servers | `esp_netif_get_dns_info()` after GOT_IP6 | `fd00:0:3801:9::2`, `::3` ✓ |

### OPNsense Router Advertisement details

The OPNsense router at `fe80::669d:99ff:fed0:a4c6` advertises:
- Prefix: `2603:8000:2d00:4605::/64` (SLAAC, valid 30d, preferred 7d)
- RDNSS: `fd00:0:3801:9::2` and `fd00:0:3801:9::3` (infinite lifetime)
- DNS search list: `co.crshman.info`, `fmt2.crshman.info`
- Router lifetime: 1800 seconds

The Comcast gateway at `fe80::7a9a:18ff:fe48:8feb` also advertises RDNSS but with lifetime=0 (effectively ignored).

### IPv6Address.toString() zero-expanded format note

The `fe80:0000:0000:0000:b68a:0aff:fe75:c107` format shown in serial output is the Arduino ESP32 v2.x `IPv6Address.toString()` output — always 39 characters, zero-padded, no `::` compression. This is cosmetic only. The actual addresses work correctly in all network operations.

### Serial debug \r\n note

We switched from `DEBUG.print()+DEBUG.println()` to `DEBUG.printf("...\r\n")` during testing. Early versions using `DEBUG.printf("...\n")` produced right-shifting output in minicom because `\n` (LF only) doesn't return the cursor to column 0 — minicom requires CR+LF. `DEBUG.println()` internally sends `\r\n` and works correctly. Since both paths ultimately call the same `uart_write_bytes()`, the difference was likely a minicom terminal session setting, but `DEBUG.printf()` is still cleaner code (single call vs two-call pattern).

---

### IPv6Address.toString() Produces Uncompressed Format

The Arduino ESP32 v2.x `IPv6Address::toString()` produces **16-group, zero-padded, always-39-char** output:
- `fe80:0000:0000:0000:0000:0000:0000:0001` (39 chars) instead of `fe80::1` (7 chars)
- No `::` compression is applied
- No zone ID is included (even though `esp_ip6_addr_t` has a `zone` field)

`IPv6Address::fromString()` **requires exactly 39 characters** — it rejects all compressed formats like `fe80::1` or `::1`.

**Impact on our implementation:**
1. **API/JSON output:** `/status` will show `fe80:0000:...` — ugly but functionally correct. No fix needed.
2. **MQTT announce:** `http6` field will be `http://[fe80:0000:0000:0000:0000:0000:0000:0001]/` — long but valid. Bracket notation still works.
3. **No zone ID:** `toString()` doesn't include the `%zone` suffix — fine for our use since we store addresses as strings, not for connection endpoints.
4. **Constructor works:** `IPv6Address(addr.addr)` where `addr` is `esp_ip6_addr_t` calls the `IPv6Address(const uint32_t*)` constructor which does `memcpy(_address.bytes, address, 16)` — correct byte layout.

**Optional future improvement:** Replace `IPv6Address::toString()` output with compressed format using a custom `ip6addr_compressed()` helper (~15 lines). Low priority — cosmetic, not functional.

## Additional Research Findings (2026-06-03, round 2)

### ODR Audit: No Conflicts Found

- **ArduinoMongoose `library.json`**: No `build_flags` defined — inherits all Mongoose macros from the firmware's PlatformIO `build_flags`. Adding `-DMG_ENABLE_IPV6=1` globally will apply uniformly.
- **MicroOcpp `library.json`**: No `build_flags`, no Mongoose headers bundled. Pure application logic.
- **MicroOcppMongoose `library.json`**: No `build_flags`. Includes `mongoose.h` from ArduinoMongoose (same include path via PlatformIO dependency resolution). No duplicate mongoose.h copy.
- **Only one copy of `mongoose.h`** exists per build environment (`.pio/libdeps/<env>/ArduinoMongoose/src/mongoose.h`). No header duplication risk.
- **PlatformIO `build_flags`** propagate to ALL translation units including library source code. PlatformIO compiles library source with the same `build_flags` as the main firmware. The `-D MG_ENABLE_IPV6=1` flag will reach every `.cpp` and `.c` file in the build.
- **Verdict: ODR safe.** A single global `-D MG_ENABLE_IPV6=1` in `platformio.ini` `build_flags` will produce identical struct layouts across all translation units.

### Firmware Size: Only ~110 KB Headroom

The default partition table (`min_spiffs.csv`) allocates **1.88 MB per app partition** (OTA A/B layout). Current firmware is **1.76 MB**, leaving only **~110 KB (5.7%) headroom**.

| Partition Table | App Size | Current FW | Headroom |
|---|---|---|---|
| `min_spiffs.csv` (default, 4 MB) | 1.88 MB | 1.76 MB | **~110 KB** |
| `min_spiffs_debug.csv` (debug, 4 MB) | 3.75 MB | 1.76 MB | ~1.97 MB |
| `openevse_16mb.csv` (16 MB flash) | 6.25 MB | 1.76 MB | ~4.47 MB |

`MG_ENABLE_IPV6=1` activates ~106 lines of IPv6 code in the Mongoose socket path. Estimated code size increase: **~10-20 KB** (including LwIP IPv6 code paths exercised more fully). This fits within 110 KB but leaves less room for future features.

**Contingency:** If firmware exceeds partition, switch to `min_spiffs_debug.csv` for development (single app, no OTA) or start using the 16 MB partition variant.

### NTP/SNTP: Goes Through Mongoose DNS (A-Only)

The firmware uses `MongooseSntpClient` (from ArduinoMongoose) which calls `mg_sntp_connect()` → `mg_connect()` → Mongoose DNS resolver. The SNTP connection resolves `pool.ntp.org` (user-configurable via `sntp_hostname`) using the Mongoose DNS path. Phase 3b (AAAA-first with A-fallback) is now complete, so SNTP works over IPv6.

**Impact:** NTP time sync on IPv6-only networks requires Phase 3b (AAAA query emission). Phase 3b is now complete — Mongoose DNS queries AAAA first with A-record fallback. SNTP still requires the UDP socket fix (already patched in Phase 2) and an IPv6 nameserver (provided by MongooseCore from RDNSS). On dual-stack networks, NTP works via IPv4 A-record resolution regardless.

**Note:** SNTP uses `mg_connect()` with `udp://` URL. UDP socket bug #3 (`connect_udp` hardcodes AF_INET) was fixed in Phase 2. sendto addrlen bug #10b was also fixed in Phase 2. SNTP IPv6 is fully functional.

### WiFi Power Save: Disabled

The firmware explicitly sets `WiFi.setSleep(WIFI_PS_NONE)` in `wifiClientConnect()` (net_manager.cpp:170). WiFi radio stays fully awake — no modem sleep, no light sleep. **Good for IPv6 stability:** no missed Router Advertisements, no NDP timer issues during sleep transitions. **Recommend adding a guard comment at net_manager.cpp:170:** `// Required for IPv6 SLAAC RA reception — do not enable WiFi power save` — so a future power-optimization PR doesn't silently break IPv6 address renewal.

### Static IP: Not Supported

The firmware uses **DHCP-only** — `WiFi.config()` (static IP overload) is never called anywhere in the codebase. `wifiClientConnect()` calls `WiFi.begin(ssid, pass)` without static IP arguments. No static IP / IPv6 conflict exists.

### HTTP Host Header: Safe for IPv6

`request->host()` returns the raw `Host` header value. The only code that uses it is `handleHttpsRedirect()` (web_server.cpp:1125) which constructs `"https://" + request->host().toString() + request->uri()`. When an IPv6 client sends `Host: [fe80::1]:80`, this produces the valid URL `https://[fe80::1]:80/path`. No colon-splitting logic exists that would break on IPv6 addresses.

### LwIP IPv6 Configuration: Mostly Defaults (One Override)

- **`CONFIG_LWIP_IPV6_NUM_ADDRESSES`**: Default 3 per interface. Sufficient for link-local + global + ULA. No project-level override.
- **`CONFIG_LWIP_IPV6_FORWARD`**: Disabled (default). ESP32 is an endpoint, not a router.
- **RA/NDP timers**: All at ESP-IDF defaults. Since WiFi power save is disabled, NDP reachability timers won't expire unexpectedly.
- **`CONFIG_LWIP_IPV6_AUTOCONFIG`** (SLAAC): Enabled by default when `CONFIG_LWIP_IPV6=y`. ESP32 will auto-configure global addresses from Router Advertisements.
- **`CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS`**: **OVERRIDE from 0 to 2** via custom `liblwip.a` (Phase 0). Default of 0 compiles out RDNSS handler. Custom build enables it so the device auto-learns IPv6 DNS from RAs.

### WiFi.enableIpV6() is One-Shot, No Persistent Bit

**CRITICAL CORRECTION:** Previous plan stated `enableIpV6()` sets a persistent `WANT_IP6_BIT` that survives disconnect. Source-code verification of ESP-IDF v4.4.7 proves this **WRONG**:

1. `WiFi.enableIpV6()` calls `esp_netif_create_ip6_linklocal()` — a one-shot function that creates the link-local address if the netif is up. No persistent flag is set.
2. `WIFI_EVENT_STA_DISCONNECTED` triggers `esp_netif_down()` which **explicitly clears ALL IPv6 address slots** to `IP6_ADDR_ANY6` / `IP6_ADDR_INVALID` state (esp_netif_lwip.c:1390-1400).
3. The Arduino WiFiGeneric.cpp has a **commented-out** `esp_netif_create_ip6_linklocal()` in the `STA_CONNECTED` handler — confirming this was intentionally left to user code.

**Fix:** Must call `WiFi.enableIpV6()` in the `ARDUINO_EVENT_WIFI_STA_CONNECTED` handler to re-enable IPv6 after every disconnect/reconnect cycle. See Phase 1 code above for the updated handler.

### Captive Portal Startup: No IPv6 Interference

On factory-fresh devices, the firmware starts in AP mode with no stored WiFi credentials. `WiFi.enableIpV6()` is only called in the STA path (`wifiClientConnect()` and `STA_CONNECTED` handler). AP mode startup (`wifiStartAccessPoint()`) does not call `enableIpV6()`. The `WIFI_PS_NONE` sleep setting is not applied to AP mode. IPv6 changes to STA mode have zero impact on AP/provisioning path.

### IPv6 DNS: No Auto-Learning from RAs (RDNSS Compiled Out)

The prebuilt LwIP sdkconfig (`framework-arduinoespressif32/tools/sdk/esp32/sdkconfig`) has these IPv6 settings:

```
CONFIG_LWIP_IPV6=y
CONFIG_LWIP_IPV6_AUTOCONFIG=y              # SLAAC enabled
CONFIG_LWIP_IPV6_NUM_ADDRESSES=3             # 3 addresses per interface
CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS=0     # RDNSS DISABLED (in prebuilt; custom rebuild sets to 2 — see Phase 0)
# CONFIG_LWIP_IPV6_DHCP6 is not set          # No DHCPv6
CONFIG_LWIP_IPV6_MEMP_NUM_ND6_QUEUE=3        # 3-packet ND queue
CONFIG_LWIP_IPV6_ND6_NUM_NEIGHBORS=5         # 5-entry neighbor cache
# CONFIG_LWIP_IPV6_FORWARD is not set        # Not a router
```

**`RDNSS_MAX_DNS_SERVERS=0`** means the prebuilt `liblwip.a` **cannot auto-learn an IPv6 DNS resolver from Router Advertisements**. RDNSS (RFC 8106, DNS-server-in-RA) option parsing is compiled out — the `case ND6_OPTION_TYPE_RDNSS:` handler in `nd6.c` is guarded by `#if LWIP_ND6_RDNSS_MAX_DNS_SERVERS` and is simply absent from the binary. DHCPv6 is also disabled.

**This is solved by the custom LwIP build (Phase 0).** Rebuilding `liblwip.a` with `RDNSS_MAX_DNS_SERVERS=2` enables the RDNSS handler. The handler is simple and self-contained: it reads 16-byte addresses from RA option type 25 via `pbuf_copy_partial()` and calls `dns_setserver()` — the same `dns_servers[]` array DHCPv4 uses. Zero ABI impact (only affects file-scope statics and stack locals in `nd6.c`). No OTA breakage (statically linked into firmware binary). See Phase 0 for details.

**After Phase 0, the remaining IPv6 DNS constraint is Mongoose-specific:**

1. **LwIP's DNS client supports IPv6 transport.** `dns_init()` creates the UDP PCB with `udp_new_ip_type(IPADDR_TYPE_ANY)` — a dual-stack socket. `dns_send()` uses `udp_sendto()` which routes based on the destination address family. If the DNS server is IPv6, queries go over IPv6.

2. **`esp_netif_set_dns_info()` accepts IPv6 addresses.** `esp_netif_dns_info_t` holds `esp_ip_addr_t`, which is a union of `esp_ip4_addr_t` + `esp_ip6_addr_t` + `uint8_t type`. Setting an IPv6 DNS server works:
   ```cpp
   esp_netif_dns_info_t dns;
   dns.ip.type = ESP_IPADDR_TYPE_V6;
   esp_netif_set_dns_info(esp_netif, ESP_NETIF_DNS_MAIN, &dns);
   ```

3. **The RDNSS constraint is solved by Phase 0.** With the custom `liblwip.a` rebuild, the device auto-learns IPv6 DNS servers from Router Advertisements via the RDNSS option (RFC 8106). On dual-stack networks, the DHCPv4-supplied IPv4 DNS suffices for LwIP. On IPv6-only networks, RDNSS populates `dns_servers[]` automatically. The remaining constraint is Mongoose-specific (hardcoded `8.8.8.8`).

**Two independent DNS resolvers on the ESP32:**

| Resolver | Transport | Used By | IPv6 DNS Support |
|---|---|---|---|
| LwIP DNS (`dns_setserver` / `esp_netif_set_dns_info`) | Dual-stack PCB | `WiFi.hostByName()`, `getaddrinfo()`, `netconn_gethostbyname()` | ✅ Yes — routes based on server address family |
| Mongoose DNS (`MG_DEFAULT_NAMESERVER` = `8.8.8.8`) | IPv4-only UDP | MQTT, EmonCMS, OCPP, SNTP, OHM Connect | ❌ Hardcoded IPv4 nameserver |

**Impact on v0 by phase:**

- **Phase 1-2 (listener + dual-stack HTTP):** No DNS impact. The device listens for inbound connections — no outbound DNS needed. IPv4 DNS from DHCPv4 works for any host lookup.
- **Phase 3 (outbound connections over IPv6):** Mongoose DNS hardcoded to `8.8.8.8` (IPv4) means all Mongoose-based outbound connections (MQTT, EmonCMS, OCPP, SNTP, OHM Connect) always resolve hostnames over IPv4 and connect over IPv4, *even if the host has AAAA records*. IPv6 outbound connectivity requires fixing Mongoose DNS — either configuring `mg_resolve_async_opt()` with an IPv6 nameserver, or routing through LwIP's DNS subsystem via `WiFi.hostByName()` pre-resolve.
- **IPv6-only networks:** Custom LwIP build (Phase 0) enables RDNSS, so LwIP-level DNS is auto-learned from RAs. Mongoose DNS remains hardcoded to `8.8.8.8` (IPv4) — this affects all Mongoose-based outbound connections on IPv6-only networks until Phase 3b. **Optional fallback: manually set well-known IPv6 DNS as a safety net** (~20 lines in the `GOT_IP6` handler, in addition to RDNSS):
  ```cpp
  // Safety net: if RDNSS doesn't work (no RDNSS option in RAs), set a well-known IPv6 DNS server
  // This is NOT needed if the network's RAs include RDNSS — most IPv6 routers do
  esp_netif_dns_info_t dns;
  dns.ip.type = ESP_IPADDR_TYPE_V6;
  ip6addr_aton("2001:4860:4860::8888", &dns.ip.u_addr.ip6);  // Google DNS
  esp_netif_set_dns_info(esp_netif, ESP_NETIF_DNS_MAIN, &dns);
  ```
  This fixes LwIP DNS (`WiFi.hostByName()`) but NOT Mongoose DNS — Mongoose needs its own fix (Phase 3b scope).

**Full workaround inventory for IPv6 DNS:**

| Approach | Effort | Scope | Status |
|---|---|---|---|
| **Custom LwIP build (RDNSS>0)** | **Low (~15 min)** | **v0 Phase 0** | **Primary approach — enables automatic IPv6 DNS learning from RAs** |
| **Manual well-known IPv6 DNS** | Low (~20 lines) | v0 Phase 1 (optional) | Safety net only — RDNSS handles this automatically. Fragile if servers blocked. |
| **Fix Mongoose DNS nameserver** | ~~Medium~~ Done | v0 Phase 3b | Complete: AAAA-first with A-fallback in resolve_cb(), IPv6 nameserver from RDNSS in MongooseCore. |
| **Enable DHCPv6** | Medium | v1 | `CONFIG_LWIP_IPV6_DHCP6=y`; experimental in ESP-IDF v4.4. Only needed for DHCPv6-only networks (no SLAAC). |

**LwIP address limit:** `LWIP_IPV6_NUM_ADDRESSES=3` is also prebuilt and un-tunable. Link-local + GUA + ULA = exactly 3. Networks advertising two global prefixes or any RFC 4941 privacy/temporary addresses overflow and LwIP silently drops the extras. Address-classification handler must treat global-address churn as **normal, not an error**.

**ND6 neighbor cache:** 5-entry limit. On a large/busy LAN this thrashes (LRU eviction → extra NS/NA traffic). Self-healing. For a home EVSE with ~3-5 peers (router, MQTT broker, phone), 5 is adequate.

### Outbound Mongoose Service Inventory

Five firmware features use Mongoose's outbound path through `mg_connect_opt()`. Phase 3b (AAAA-first with A-fallback) is now complete:

| Service | Source | Protocol | Phase 3 Impact |
|---|---|---|---|
| MQTT | `mqtt.cpp:142` | TCP | AAAA-first DNS (also has pre-resolve workaround) |
| EmonCMS | `emoncms.cpp:70` | HTTPS | AAAA-first DNS |
| OCPP | `MicroOcppMongooseClient.cpp` | WSS | AAAA-first DNS |
| SNTP | `MongooseSntpClient.cpp` \u2192 `mg_sntp_connect()` | UDP | AAAA-first DNS (UDP socket fixed in Phase 2) |
| OHM Connect | `ohm.cpp:38` | HTTPS | AAAA-first DNS |

SNTP is especially impacted: it hits **both** the DNS bug (A-only) AND the UDP socket bug (`socket(AF_INET, SOCK_DGRAM)` at `mg_socket_if_connect_udp()`). Time sync won't work on IPv6-only networks until Phase 3b (Mongoose DNS AAAA queries) — even with custom LwIP enabling RDNSS, Mongoose's own DNS resolver bypasses LwIP's DNS and hardcodes `8.8.8.8` (IPv4).

### MQTT IPv6 Literal Config Creates Ambiguous Address

`mqtt.cpp:115` builds the broker address via simple concatenation:
```cpp
String mqtt_host = mqtt_server + ":" + String(mqtt_port);
```

If a user enters `2001:db8::1` as `mqtt_server`, this produces `2001:db8::1:1883` — Mongoose's `mg_parse_address()` cannot distinguish the port colon from IPv6 colons. The fix belongs in Phase 4 — bracket-qualify IPv6 literals:
```cpp
String mqtt_host;
if (mqtt_server.indexOf(':') >= 0) {  // IPv6 literal
  mqtt_host = "[" + mqtt_server + "]:" + String(mqtt_port);
} else {
  mqtt_host = mqtt_server + ":" + String(mqtt_port);
}
```

The same pattern may affect EmonCMS and OCPP config paths — audit all `host + ":" + port` concatenations.

### OTA: Two Paths, Different IPv6 Impact

- **HTTP OTA** (`http_update.cpp`): Uses `MongooseHttpClient` — goes through Mongoose DNS. Same A-only DNS path as other outbound services. Will work over IPv6 (dual-stack) once Phase 2 dual-stack HTTP is active. Redirect URLs with IPv6 literals (e.g., CDN redirect to `http://[2001:db8::1]/fw.bin`) are handled correctly by `mg_parse_address()` bracket parsing.
- **ArduinoOTA** (`ota.cpp`): Uses `WiFiUdp` + `ArduinoOTA` — completely separate from Mongoose. `ArduinoOTA.begin()` binds a UDP listener via `WiFiUdp` which calls `socket(AF_INET, ...)` in v2.x — hardcoded IPv4. **Arduino OTA stays IPv4-only.** This is not a bug to fix; document in success criteria that "OTA over IPv6" refers to HTTP OTA only.

### Firmware Rollback Safety — Confirmed Safe

No IPv6 data is persisted to EEPROM/NVS. The proposed `_ipv6address_linklocal` and `_ipv6address_global` are `String` member variables in `NetManagerTask` — runtime only. The `app_config` EEPROM storage uses string key/value pairs, not binary structs. An old pre-IPv6 firmware booting after rollback simply doesn't have the `-D MG_ENABLE_IPV6=1` flag, sees its own smaller `mg_connection` layout, and works normally. **Rollback is safe — no persistent storage format changes.**

### TLS/SNI with IPv6 Literals — Out of Scope

All outbound TLS targets (MQTT-TLS, EmonCMS HTTPS, OCPP WSS) connect to **hostnames**, not IP literals. `mongoose.c:5028` sets SNI via `mbedtls_ssl_set_hostname(server_name)` using the hostname string. SNI and CN/SAN verification use the hostname — **unaffected by IPv6**.

The only gap is TLS to a *bracketed IPv6 literal* (e.g., `mqtts://[2001:db8::1]/`), where brackets must be stripped before `set_hostname` and the cert must carry an iPAddress-SAN. The firmware never dials IP-literal TLS in normal operation. **Mark explicitly out of scope for v0.**

### DAD (Duplicate Address Detection) Failure Path

LwIP performs DAD with `DUP_DETECT_ATTEMPTS=1`. On duplicate-address collision (rare — same EUI-64), the address is rejected and `GOT_IP6` never fires. The firmware handles this gracefully by default (no IPv6 = IPv4 fallback). But there's no logging. Consider adding a timeout warning: if no `GOT_IP6` arrives within ~10 seconds after `enableIpV6()`, log a `DBUGF` note. Not a Phase 1 requirement — can be added in Phase 5.

### Future Path: Mongoose 7.x Migration

Mongoose 7.21 (current upstream) natively solves every IPv6 issue we're patching in 6.18: dual-stack sockets, AAAA DNS resolution, proper `struct mg_addr` with IPv6 support, `mg_listen("[::]:80")` just working, no hardcoded `AF_INET`, no `union socket_address` truncation bugs, no A-only DNS resolver. All 8+ Mongoose patches in our Phase 2-3 inventory are unnecessary in 7.x.

Additionally, 7.x natively addresses every one of Jeremy's local patches on 6.18: `MG_ENABLE_CALLBACK_USERDATA` is eliminated (fn_data removed from handler signature in 7.13), certificate/key parsing from memory buffers is the default (POSIX FS is opt-in since 7.15), mbedTLS error logging is built-in (7.19), and the HTTP/multipart/SNTP user_data propagation bugs don't exist in the redesigned API.

**Why we're not doing it now:** Mongoose 7.x is a complete API rewrite — different event handler signatures, different connection struct, different HTTP/MQTT/SNTP APIs, different address handling. Every C++ wrapper in ArduinoMongoose (`MongooseCore`, `MongooseHttpServer`, `MongooseHttpClient`, `MongooseMqttClient`, `MongooseSntpClient`, `MongooseWebSocketClient`) would need a ground-up rewrite. The OpenEVSE firmware has thousands of lines using the 6.18 API. Migration is weeks of work with high regression risk.

**Strategic position:** Our ArduinoMongoose fork with 6.18 IPv6 patches is the pragmatically correct approach for now. If a Mongoose 7.x migration ever becomes warranted, it would replace our entire fork — all 6.18-specific patches (both Jeremy's and ours) become obsolete. The firmware-level changes (Phase 1: `enableIpV6()`, GOT_IP6 handlers, API reporting) are Mongoose-version-independent and would carry forward unchanged. **Do not invest in 6.18 patches that would complicate a future 7.x migration** — keep patches minimal and well-documented.

---

### Boot Timing Analysis — Serial + Packet Correlation

**Timestamped serial capture** (relative to boot ROM = T+0):
```
T+ 0.000s  Boot ROM (reset)
T+ 0.762s  OpenEVSE WiFi / Firmware banner
T+ 0.840s  VFS error: /littlefs/schedule.json (harmless — first boot)
T+ 2.826s  Server started
T+ 3.332s  OpenEVSE not responding (first RAPI poll)
T+ 4.201s  Connected, ETH IPv6 link-local  ← link-local DAD complete
T+ 7.274s  Connected, IP: 172.16.5.158    ← DHCP complete (IPv4 up)
T+ 7.283s  Connected, IPv6 link-local (duplicate — from haveNetworkConnection)
T+ 7.334s  MQTT resolved → IPv4 172.16.9.106
T+ 7.340s  MQTT Connecting (IPv4)
T+12.202s  Connected, ETH IPv6 global     ← SLAAC DAD complete (GOT_IP6)
T+12.237s  MQTT: upgrading connection to IPv6
T+12.393s  MQTT resolved → IPv6 2603:…
T+12.408s  MQTT connected over IPv6       ← fully operational on IPv6
```

**IPv4 MQTT ready: T+7.3s | IPv6 MQTT ready: T+12.4s | Gap: ~5.1s**

The 5s gap is almost entirely the RS #1 dropped response (4s RFC retransmit timeout).
If RS #1 were answered, IPv6 global would be ~T+8s and MQTT on IPv6 by ~T+8.5s.

**Known anomalies (harmless):**
- `Connected, IPv6 link-local` appears twice: once from `ARDUINO_EVENT_ETH_GOT_IP6`
  at T+4.2s (correct), and once from `haveNetworkConnection()` at T+7.3s (dumps
  whatever IPv6 state is already known when IPv4 arrives).
- `Connected, ETH IPv6 global` fires a second time ~60s post-boot: ESP-IDF re-fires
  `GOT_IP6` when the router RA refreshes the prefix lifetime. The `had_global` guard
  in net_manager.cpp prevents a redundant `onGlobalIPv6Acquired()` call.

**How to capture timestamped serial output (relative T+0 from first byte):**
```bash
sudo picocom -b 115200 /dev/ttyUSB2 --quiet | ts -s '[%H:%M:%.S]'
# requires: sudo apt install moreutils
# T=0 = first byte from serial (boot ROM or running firmware, whichever comes first)
# Reset the device after starting picocom to get clean T=0 from boot ROM
```

**Why RS #1 gets no RA response (open question):**
Packet analysis shows RS #1 goes out at T+3.5s (boot), RS #2 at T+7.5s. Only RS #2
triggers an RA. Likely cause: router RA rate-limiting (min ~3s between solicited RAs
per RFC 4861 §6.2.6), and RS #1 landed within the rate-limit window of a recent
unsolicited RA. No firmware fix needed — this is router policy behavior.

---

### Serial Debug Output: Use `\r\n` Not `\n` in `DEBUG.printf()`

During testing, we observed right-shifting output in minicom when using `DEBUG.printf("...\n")`. We initially assumed `printf()` only sends LF while `println()` sends CR+LF. **However, source code analysis of the Arduino ESP32 core shows both paths call the same `write()` → `uart_write_bytes()` function.** The real `CONFIG_NEWLIB_STDOUT_LINE_ENDING_CRLF=1` translation only applies to newlib `stdout` (C library's `printf()`/`fprintf()`), not to Arduino's `Serial.printf()` which bypasses newlib entirely.

Both `DEBUG.println()` and `DEBUG.printf("...\n")` send identical bytes via the same UART path. The observed drift was likely a minicom terminal session configuration issue, not a firmware bug. Nevertheless, `DEBUG.printf()` is still cleaner code (single call vs two-call `print()+println()` pattern) and using `\r\n` is defensive against terminals that don't auto-append CR.

**Rule:** Always use `\r\n` in `DEBUG.printf()` calls:
```cpp
// WRONG — causes rightward drift in minicom:
DEBUG.printf("Connected, IPv6 global: %s\n", addr.c_str());

// CORRECT — stays left-aligned:
DEBUG.printf("Connected, IPv6 global: %s\r\n", addr.c_str());
```

Alternative: use `DBUGF()` macro instead of `DEBUG.printf()` — it handles line endings correctly and is the project's standard debug macro.

### `esp32-ipv6-assessment.md` Contains Stale API Names

The assessment doc (`docs/esp32-ipv6-assessment.md`) uses v3.x API names (`enableIPv6()`, `linkLocalIPv6()`, `globalIPv6()`, `hasGlobalIPv6()`). The implementation plan corrected these, but the assessment doc is still wrong. If a developer reads it during implementation, they'll get compile errors. **Fix before starting Phase 1.** Also, `docs/mongoose-ipv6-assessment.md` line 18 incorrectly states `MG_ENABLE_IPV6` is "hardcoded to 1 for ESP32 builds" — it is NOT (that's the NRF51 section at line 1155; ESP32 defaults to 0 at line 3245). This error could cause someone to skip the `-D MG_ENABLE_IPV6=1` build flag entirely.

---

## Appendix: ESP-IDF v4.4 mDNS AAAA Bug and Workarounds

**Problem:** The precompiled `libmdns.a` in ESP-IDF v4.4 only calls `esp_netif_get_ip6_linklocal()` when building AAAA responses. It never calls `esp_netif_get_ip6_global()`. This means mDNS only advertises the link-local `fe80::` address, not the global IPv6 address.

**Root cause:** In `mdns.c:1100-1124`, the AAAA branch for `_mdns_self_host` calls only `esp_netif_get_ip6_linklocal()`, which reads `ip6_addr[0]` (the link-local slot). `esp_netif_get_all_ip6()` or `esp_netif_get_ip6_global()` are never called. A `TODO(lsm): handle IPv6 answers too` comment marks where global AAAA should be added.

**Verification:** `nm -C libmdns.a` shows only `esp_netif_get_ip6_linklocal` as an undefined symbol — no reference to `esp_netif_get_ip6_global` or `esp_netif_get_all_ip6`.

### Workarounds evaluated (consultant analysis)

Three external consultants (GPT-5.5, Opus 4.8, Opus 4.6) evaluated five workaround approaches:

#### 1. LwIP ip6 slot swap (IMPLEMENTED in v0)

Use `esp_netif_get_netif_impl()` to get the raw LwIP `struct netif*` pointer, then swap `ip6_addr[0]` (link-local) with `ip6_addr[1]` (global) along with their state and lifetime arrays. Since `esp_netif_get_ip6_linklocal()` reads slot 0, it now returns the global address, and mDNS advertises it. Then restart mDNS (`mdns_free()` + `mdns_init()` + re-register services).

**Status:** Implemented and verified working. `avahi-resolve -6 -n openevse-c104.local` returns the global address.

**Risk:** After the swap, `esp_netif_get_ip6_linklocal()` returns the global address for ALL callers on this interface. The firmware captures addresses from the GOT_IP6 event payload (not these functions), so this is safe. LwIP's internal source address selection (`ip6_select_source_address()`) iterates by type, not slot index.

**Code location:** `src/net_manager.cpp`, ETH GOT_IP6 handler, between global address arrival and mDNS restart.

#### 2. Rebuild libmdns.a from ESP-IDF v4.4 sources (RECOMMENDED for v1)

The `esp32-arduino-lib-builder` already emits `build/esp-idf/mdns/libmdns.a` as a standalone archive. The same `custom_libs/` + `scripts/override_lwip.py` LIBPATH preemption mechanism that works for `liblwip.a` works identically for `libmdns.a` — drop the patched archive into `custom_libs/` and the linker resolves `-lmdns` from the first archive it finds.

**The patch:** Replace the single `esp_netif_get_ip6_linklocal()` call in the AAAA response builder with `esp_netif_get_all_ip6()` to iterate all IPv6 addresses (link-local + global + ULA):

```c
if (answer->host == &_mdns_self_host) {
    // ... existing PCB check ...
    esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    int count = esp_netif_get_all_ip6(_mdns_get_esp_netif(tcpip_if), addrs);
    uint8_t num = 0;
    for (int i = 0; i < count; i++) {
        if (_ipv6_address_is_zero(addrs[i])) continue;
        if (_mdns_append_aaaa_record(packet, index, _mdns_server->hostname,
                                     (uint8_t *)addrs[i].addr, answer->flush, answer->bye) > 0) {
            num++;
        }
    }
    return num;
}
```

This mirrors Espressif's own fix in later ESP-IDF versions. Advertises all IPv6 addresses under the real hostname (no alias needed). Re-reads on every response so SLAAC address changes are picked up automatically.

**Effort:** ~4 hours (find patch location, apply, rebuild, test).

#### 3. Delegated hostname (mdns_delegate_hostname_add)

**NOT VIABLE for advertising under the real hostname.** The API exists in v4.4 and the delegated-host code path correctly iterates its `address_list` (including global IPv6). However, `mdns.c:182` / `2601-2603` rejects any delegated name where `_hostname_is_ours()` is true. You can only advertise the global address under a *different* name like `openevse-c104-6.local`. Clients resolving the real hostname still get link-local only. Also requires hand-maintaining the address list across SLAAC changes.

**Verdict:** Rejected. Half a fix with ongoing babysitting.

#### 4. Raw mDNS packet injection

Bind a second socket to UDP 5353, parse incoming queries, and inject supplementary AAAA response packets. Extremely fragile: `libmdns.a` already binds that port (potential `EADDRINUSE`), you'd race with the library's own responses, and mDNS conflict resolution causes unpredictable behavior.

**Verdict:** Rejected. Too fragile for production firmware.

#### 5. Swap netif ip6 slots without restart

Same as approach #1 but without the mDNS restart. The restart is needed because mDNS caches interface state at init time; a simple swap without restart does not force re-enumeration.

**Verdict:** Rejected. Restart is required (already proven empirically).

### Recommendation

- **v0:** Slot swap + mDNS restart (already implemented and working)
- **v1:** Rebuild `libmdns.a` with `esp_netif_get_all_ip6()` patch (same approach as `liblwip.a` RDNSS rebuild in Phase 0). This eliminates the slot swap hack entirely.
- **Upstream PR:** Document the v4.4 limitation. Espressif's own fix landed in ESP-IDF v5.x. The project is unlikely to backport.
