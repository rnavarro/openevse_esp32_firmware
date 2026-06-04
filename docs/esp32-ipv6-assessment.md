# ESP32 Arduino IPv6 Stack Assessment

**Project:** OpenEVSE ESP32 Firmware  
**Platform:** `espressif32@6.12.0` (PlatformIO), Arduino framework  
**Arduino ESP32 Core:** v2.0.17 (shipped with espressif32@6.12.0, based on ESP-IDF v4.4.7)  
**Date:** 2026-06-03 (updated 2026-06-04 with v2.x corrections)

> **⚠️ v2.x vs v3.x:** This assessment targets the Arduino ESP32 core **v2.0.17** actually shipped with
> our PlatformIO package. Earlier drafts incorrectly referenced v3.x API names. The v2.x and v3.x APIs
> differ significantly in IPv6 method naming and behavior. All API references below are verified against
> the installed v2.0.17 headers.

---

## 1. Available IPv6 APIs in ESP32 Arduino (v2.0.17)

The IPv6 API is gated behind `CONFIG_LWIP_IPV6` (enabled by default). The v2.0.17 API names differ
from v3.x — notably `enableIpV6()` (capital V) instead of `enableIPv6()`, and `localIPv6()` (link-local
only) instead of `linkLocalIPv6()` + `globalIPv6()`.

### 1.1 WiFi STA (`WiFiSTAClass`)

```cpp
// From WiFiSTA.h (v2.0.17)
#if CONFIG_LWIP_IPV6
bool enableIpV6();                // Request IPv6 on STA interface (note: capital V)
IPv6Address localIPv6();          // Returns LINK-LOCAL only (fe80::). No global IPv6 getter.
#endif
```

**Key differences from v3.x:**
- Method name is `enableIpV6()` (lowercase `p`, uppercase `V`) — NOT `enableIPv6()`
- `localIPv6()` exists and returns **link-local only** — there is NO `globalIPv6()` or `linkLocalIPv6()`
- `IPv6Address` is a separate class from `IPAddress` (not the same type)
- `IPv6Address.toString()` produces zero-expanded format: `fe80:0000:0000:0000:b68a:0aff:fe75:c107`

**How `enableIpV6()` works internally (v2.0.17):**
1. Calls `esp_netif_create_ip6_linklocal()` — a **one-shot** function, not a persistent flag
2. Creates the link-local address via EUI-64 from the MAC address
3. No persistent `WANT_IP6_BIT` is set — must re-call after every disconnect/reconnect
4. Global address is obtained via SLAAC from Router Advertisements (if the network supports it)

### 1.2 WiFi AP (`WiFiAPClass`)

```cpp
// From WiFiAP.h (v2.0.17)
#if CONFIG_LWIP_IPV6
bool softAPenableIpV6();          // Enable IPv6 on soft AP (capital V)
IPv6Address softAPIPv6();         // Returns AP link-local IPv6
#endif
```

### 1.3 Ethernet (`ETHClass`)

```cpp
// From ETH.h (v2.0.17) — confirmed at line 93
bool enableIpV6();                // ETH.enableIpV6() — capital V, exists in v2.0.17
// Implementation uses deprecated tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_ETH)
// while WiFi STA uses the newer esp_netif API
```

**No `linkLocalIPv6()` or `globalIPv6()` wrappers exist on ETH in v2.x.** To get IPv6 addresses
on Ethernet, use the GOT_IP6 event payload directly (see §1.5).

### 1.4 Getting Global IPv6 Addresses — Event Payload Approach

Since v2.x has no Arduino wrapper for global IPv6, use the GOT_IP6 event data:

```cpp
case ARDUINO_EVENT_ETH_GOT_IP6:
{
  esp_ip6_addr_t addr = info.got_ip6.ip6_info.ip;
  esp_ip6_addr_type_t addr_type = esp_netif_ip6_get_addr_type(&addr);
  if (addr_type == ESP_IP6_ADDR_IS_LINK_LOCAL) {
    // link-local fe80:: address
  } else if (addr_type == ESP_IP6_ADDR_IS_GLOBAL) {
    // globally routable address (includes ULA fc00::/7)
  }
}
```

The `info.got_ip6.ip6_info.ip` field contains the actual IPv6 address directly — no need to
call `esp_netif_get_ip6_global()` or `esp_netif_get_handle_from_ifkey()`.

**Alternative for explicit retrieval outside events:**
```cpp
esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");  // or "WIFI_STA_DEF"
esp_ip6_addr_t ip6;
if (esp_netif_get_ip6_global(netif, &ip6) == ESP_OK) {
  // got global IPv6
}
if (esp_netif_get_ip6_linklocal(netif, &ip6) == ESP_OK) {
  // got link-local IPv6
}
```

### 1.5 Events

The following IPv6-related events are defined in `NetworkEvents.h`:

| Event | Description |
|---|---|
| `ARDUINO_EVENT_WIFI_STA_GOT_IP6` | STA received an IPv6 address |
| `ARDUINO_EVENT_WIFI_AP_GOT_IP6` | AP received an IPv6 address |
| `ARDUINO_EVENT_ETH_GOT_IP6` | Ethernet received an IPv6 address |

The `GOT_IP6` event fires **multiple times** — first for link-local (immediately after
`enableIpV6()`), then again for global/ULA addresses (when Router Advertisements arrive, seconds
later). The event payload `info.got_ip6.ip6_info.ip` contains the actual address and
`info.got_ip6.esp_netif` contains the esp_netif handle.

**To distinguish address types**, use `esp_netif_ip6_get_addr_type()` on the address from the
event payload:
- `ESP_IP6_ADDR_IS_LINK_LOCAL` — fe80:: address
- `ESP_IP6_ADDR_IS_GLOBAL` — globally routable (includes ULA fc00::/7)
- `ESP_IP6_ADDR_IS_UNIQUE_LOCAL` — fc00::/7 (also matched by `IS_GLOBAL`)

### 1.6 API Naming: `enableIpV6()` vs `enableIPv6()`

**On v2.0.17:** The correct name is `enableIpV6()` (lowercase `p`, uppercase `V`).
The v3.x naming `enableIPv6()` does NOT compile on v2.x.

### 1.7 `WiFi.localIPv6()` — Link-Local Only

On v2.0.17, `WiFi.localIPv6()` exists and returns the **link-local fe80:: address only**.
There is no Arduino wrapper for global IPv6 — use the GOT_IP6 event payload or ESP-IDF
`esp_netif_get_ip6_global()` directly.

### 1.8 `IPv6Address` Class

`IPv6Address` is a **separate class** from `IPAddress` in v2.x. Key differences:
- Constructor: `IPv6Address(const uint8_t *addr)` or `IPv6Address(const uint32_t *addr)`
- `toString()` produces **zero-expanded** 39-char format: `fe80:0000:0000:0000:b68a:0aff:fe75:c107`
- No `::` compression is applied (cosmetic, not functional)
- `fromString()` requires exactly 39 characters — rejects compressed formats like `fe80::1`
- Can construct from `esp_ip6_addr_t.addr`: `IPv6Address(addr.addr)` (uint32_t[4] array)

---

## 2. LwIP IPv6 Stack Behavior

### 2.1 SLAAC (Stateless Address Autoconfiguration)

**Supported.** The ESP32 LwIP stack implements SLAAC via Router Advertisements. When IPv6 is enabled and the interface is connected:

1. **Link-local address** (`fe80::`) is created immediately using EUI-64 from the MAC address
2. When Router Advertisements (RA) are received, global addresses are autoconfigured from the advertised prefixes
3. Multiple global addresses may be created if the network advertises multiple prefixes (e.g., a global prefix + ULA prefix)

### 2.2 DHCPv6

**Not supported for address assignment (stateful DHCPv6).** The LwIP stack used by ESP32 does not implement stateful DHCPv6 (RFC 3315) for obtaining addresses. Stateless DHCPv6 (for DNS server information only, RFC 3736) may work via the RDNSS option in Router Advertisements.

**RDNSS requires custom liblwip.a:** The prebuilt `liblwip.a` has `CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS=0`, which compiles out the RDNSS handler entirely. Our custom build (Phase 0) sets this to 2, enabling automatic IPv6 DNS server learning from RAs.

**Practical impact:** The ESP32 can only obtain IPv6 addresses via SLAAC. If your network only provides addresses via DHCPv6 (rare for home networks, more common in enterprise), the ESP32 will only get a link-local address.

### 2.3 Router Advertisements

**Supported.** The LwIP NDP (Neighbor Discovery Protocol) implementation processes incoming Router Advertisements:
- RA messages advertise on-link prefixes
- The ESP32 autoconfigures addresses from these prefixes using EUI-64
- RA messages may include RDNSS (DNS server) information — but only with custom `liblwip.a`

### 2.4 Address Lifecycle

A typical dual-stack connection sequence (Ethernet):

```
1. ETH.begin() / WiFi.begin(ssid, pass)
2. ARDUINO_EVENT_ETH_CONNECTED / ARDUINO_EVENT_WIFI_STA_CONNECTED
3. ETH.enableIpV6() / WiFi.enableIpV6()  -> triggers esp_netif_create_ip6_linklocal()
4. ARDUINO_EVENT_ETH_GOT_IP6  (link-local fe80:: assigned)
5. ARDUINO_EVENT_ETH_GOT_IP   (IPv4 via DHCP)
6. (seconds later) RA received -> SLAAC
7. ARDUINO_EVENT_ETH_GOT_IP6  (global address assigned)
```

Note: The `GOT_IP6` event fires **multiple times** — once for link-local, once for each
global/ULA address. This is normal behavior.

**Critical: `enableIpV6()` is one-shot on v2.x.** Unlike v3.x (which sets a persistent
`WANT_IP6_BIT`), v2.0.17's `enableIpV6()` calls `esp_netif_create_ip6_linklocal()` directly
with no persistent flag. On disconnect, `esp_netif_down()` clears ALL IPv6 address slots.
You **must** re-call `enableIpV6()` in the `STA_CONNECTED` / `ETH_CONNECTED` handler after
every reconnect.

### 2.5 Privacy Extensions (RFC 4941)

**Not supported.** The ESP32 LwIP stack does not implement RFC 4941 privacy extensions. All SLAAC addresses use EUI-64 based on the MAC address, making them predictable and stable. This is typical for IoT devices but means the IPv6 address can be used to track the device.

### 2.6 Multiple IPv6 Addresses

The ESP32 can hold multiple IPv6 addresses simultaneously (`CONFIG_LWIP_IPV6_NUM_ADDRESSES` defaults to 3). Typical layout:

| Slot | Address Type | Example |
|---|---|---|
| 0 | Link-local (always) | `fe80::b68a:aff:fe75:c107` |
| 1 | Global (SLAAC) | `2603:8000:2d00:4605:b68a:aff:fe75:c107` |
| 2 | ULA or second prefix | `fd00::b68a:aff:fe75:c107` |

`esp_netif_get_ip6_linklocal()` reads slot 0. `esp_netif_get_ip6_global()` reads slot 1. This slot
ordering is significant for the mDNS AAAA workaround (see §5.3).

---

## 3. DNS Resolution with IPv6

### 3.1 LwIP DNS Resolver

The LwIP DNS resolver supports AAAA record queries when `CONFIG_LWIP_IPV6=y`. The resolver creates
its UDP PCB with `udp_new_ip_type(IPADDR_TYPE_ANY)` — a dual-stack socket. If the DNS server is
IPv6, queries go over IPv6.

**RDNSS (RFC 8106):** Router Advertisements can include DNS server addresses. With the custom
`liblwip.a` (Phase 0), RDNSS-populated IPv6 DNS servers are stored in the same `dns_servers[]`
array that DHCPv4 uses. `WiFi.hostByName()` uses this resolver and can resolve over IPv6 DNS.

### 3.2 Arduino `hostByName()`

`WiFi.hostByName()` uses `lwip_getaddrinfo()` internally. It can return IPv6 addresses when
IPv6 is enabled and the DNS server is reachable.

**Known issues:**
- In dual-stack networks, `hostByName()` may prefer IPv4 over IPv6 even when both are available
- In IPv6-only networks, DNS may fail entirely if the DNS server address is only learned via IPv4 DHCP
- RDNSS (custom liblwip.a) fixes this by auto-learning IPv6 DNS from RAs

### 3.3 Mongoose DNS — Separate from LwIP

**Critical:** Mongoose has its own DNS resolver that **bypasses LwIP's DNS entirely**. It hardcodes
`MG_DEFAULT_NAMESERVER` to `8.8.8.8` (IPv4) and queries A records only. All Mongoose-based outbound
connections (MQTT, EmonCMS, OCPP, SNTP) use this resolver. Even with RDNSS providing IPv6 DNS to
LwIP, Mongoose's outbound connections remain IPv4-only until its DNS is patched (Phase 3).

| Resolver | Transport | Used By | IPv6 DNS Support |
|---|---|---|---|
| LwIP DNS | Dual-stack UDP | `WiFi.hostByName()`, `getaddrinfo()` | ✅ (with custom liblwip.a + RDNSS) |
| Mongoose DNS | IPv4-only UDP | MQTT, EmonCMS, OCPP, SNTP | ❌ (hardcoded `8.8.8.8`, A-only queries) |

---

## 4. Web Server IPv6 Support

### 4.1 Mongoose (used by OpenEVSE)

The OpenEVSE project uses **Mongoose 6.18** (via `ArduinoMongoose@0.0.22`), not AsyncWebServer.

**Current status in OpenEVSE (before IPv6 patches):**
- `MG_ENABLE_IPV6` defaults to **0** for ESP32 (mongoose.h line 3246) — NOT enabled
- The NRF51/NRF52 sections (lines 1155, 1198) set it to 1, but ESP32 does not
- The build flag `-D MG_ENABLE_IPV6=1` in platformio.ini is required

**After Phase 2 patches (our ArduinoMongoose fork, `fix/ipv6-dual-stack` branch):**
- `mg_parse_address()` — bare port `"80"` returns AF_INET6 on LwIP, creating dual-stack listener
- `mg_socket_if_connect_tcp()` / `mg_socket_if_connect_udp()` — use `sa.sa.sa_family` instead of hardcoded AF_INET
- `mg_open_listening_socket()` — sets `IPV6_V6ONLY=0` explicitly for dual-stack
- `mg_socket_if_connect_tcp()` — correct `connect()` addrlen for IPv6

**Testing confirms:** Both `curl -4` and `curl -6` to the device return HTTP 200 with identical
JSON responses.

---

## 5. mDNS and IPv6

### 5.1 ESP-IDF mDNS Service

The ESP-IDF `mdns` component supports IPv6 address advertising. When `CONFIG_LWIP_IPV6` is enabled
and the interface has IPv6 addresses, mDNS should advertise both A and AAAA records.

### 5.2 ESPmDNS Library (v2.0.17)

The v2.x Arduino ESPmDNS library wraps the ESP-IDF mDNS component. `MDNS.begin(hostname)` and
`MDNS.addService()` register services with both IPv4 and IPv6 addresses.

### 5.3 mDNS AAAA Bug in ESP-IDF v4.4

**The precompiled `libmdns.a` only advertises link-local IPv6, not global.** Root cause: the AAAA
response builder calls only `esp_netif_get_ip6_linklocal()` (reads ip6_addr slot 0), never
`esp_netif_get_ip6_global()` or `esp_netif_get_all_ip6()`.

**Workaround (implemented):** Swap LwIP's `ip6_addr[0]` (link-local) with `ip6_addr[1]` (global)
via `esp_netif_get_netif_impl()` → raw `struct netif*` access, then restart mDNS. Since the
library reads slot 0, it now returns the global address.

**Verification:** `avahi-resolve -6 -n openevse-c104.local` returns `2603:8000:2d00:4605:b68a:aff:fe75:c107`
(global) instead of the link-local `fe80::` address.

**Proper fix (v1):** Rebuild `libmdns.a` from ESP-IDF v4.4 sources with a patch to call
`esp_netif_get_all_ip6()` in the AAAA response builder. Same `custom_libs/` + LIBPATH preemption
approach used for the custom `liblwip.a`. This eliminates the slot swap hack.

### 5.4 mDNS AAAA is Automatic

No code changes are needed for mDNS AAAA beyond enabling IPv6 and the slot swap workaround.
ESPmDNS automatically advertises AAAA records by querying `esp_netif_get_all_ip6()` dynamically
at response time — no stored address list, no re-registration needed when addresses change.

---

## 6. Current State in OpenEVSE Firmware

### 6.1 What Was Already There (before our changes)

The `net_manager.cpp` already had IPv6 event names in the debug string table:
```cpp
ARDUINO_EVENT_WIFI_STA_GOT_IP6 == event ? "ARDUINO_EVENT_WIFI_STA_GOT_IP6" :
ARDUINO_EVENT_WIFI_AP_GOT_IP6 == event ? "ARDUINO_EVENT_WIFI_AP_GOT_IP6" :
ARDUINO_EVENT_ETH_GOT_IP6 == event ? "ARDUINO_EVENT_ETH_GOT_IP6" :
```

However, no code actually **acted** on these events — they fell through to `default: break;`
in the switch statement.

### 6.2 What's Now Implemented (Phase 0-2)

1. ✅ **Custom `liblwip.a` with RDNSS** — auto-learns IPv6 DNS from Router Advertisements
2. ✅ **`WiFi.enableIpV6()` / `ETH.enableIpV6()`** — called in connect/connected handlers
3. ✅ **GOT_IP6 event handlers** — store IPv6 addresses, classify as link-local/global/ULA
4. ✅ **`/status` API fields** — `ipv6address_global` and `ipv6address_linklocal`
5. ✅ **MG_ENABLE_IPV6=1** — build flag in platformio.ini
6. ✅ **Mongoose dual-stack HTTP** — 4 patches in ArduinoMongoose fork
7. ✅ **mDNS AAAA global address** — slot swap + restart workaround
8. ✅ **IPv6 cleared on disconnect** — re-enabled on reconnect

### 6.3 What's Not Yet Done

| Task | Phase | Status |
|---|---|---|
| Mongoose DNS AAAA queries (outbound) | Phase 3 | ❌ 8 bugs in DNS/UDP stack |
| MQTT over IPv6 | Phase 4 | ❌ Blocked on Phase 3 |
| EmonCMS over IPv6 | Phase 4 | ❌ Blocked on Phase 3 |
| OCPP over IPv6 | Phase 4 | ❌ Blocked on Phase 3 |
| Captive portal IPv6 | v2 | ❌ Not in v0 scope |
| Proper `libmdns.a` rebuild | v1 | ❌ Slot swap works for now |

---

## 7. Readiness Assessment

### What Works (ESP32 Core Level)

| Feature | Status | Notes |
|---|---|---|
| IPv6 address assignment (SLAAC) | ✅ Working | Link-local + global via RA |
| IPv6 API (`enableIpV6`, `localIPv6`) | ✅ Working | v2.x naming (capital V) |
| IPv6 events (`GOT_IP6`) | ✅ Working | Fires for link-local and global |
| `IPv6Address` class | ✅ Working | Separate from `IPAddress`, zero-expanded `toString()` |
| mDNS IPv6 advertisements | ✅ Working | With slot swap workaround for global AAAA |
| Ethernet IPv6 | ✅ Working | `ETH.enableIpV6()` exists in v2.0.17 |
| RDNSS DNS from RA | ✅ Working | Custom `liblwip.a` required |
| Dual-stack HTTP (Mongoose) | ✅ Working | Phase 2 patches applied |

### What Doesn't Work Well

| Issue | Severity | Notes |
|---|---|---|
| DHCPv6 address assignment | Medium | LwIP limitation; SLAAC-only |
| Mongoose DNS (outbound) | High | Hardcoded `8.8.8.8`, A-only queries |
| mDNS AAAA global address | Low | Fixed with slot swap; proper rebuild deferred to v1 |
| IPv4 preference in dual-stack DNS | Medium | `hostByName()` may return IPv4 even when IPv6 available |
| RFC 4941 privacy extensions | Low | Addresses are predictable (EUI-64) |
| `IPv6Address.toString()` no compression | Low | Cosmetic — `fe80:0000:...` instead of `fe80::...` |
| RDNSS in prebuilt liblwip.a | High | Compiled out by default; custom build required |
| `enableIpV6()` one-shot (no persistent flag) | Medium | Must re-call after every reconnect |

---

## 8. References

- **Official IPv6 Example:** [libraries/WiFi/examples/WiFiIPv6/WiFiIPv6.ino](https://github.com/espressif/arduino-esp32/blob/master/libraries/WiFi/examples/WiFiIPv6/WiFiIPv6.ino)
- **IPv6 Discussion (comprehensive):** [GitHub Discussion #9009](https://github.com/espressif/arduino-esp32/discussions/9009)
- **DNS IPv6 Fix PR:** [GitHub PR #9439](https://github.com/espressif/arduino-esp32/pull/9439)
- **IPv6-only network issues:** [GitHub Issue #9143](https://github.com/espressif/arduino-esp32/issues/9143)
- **localIPv6 accessor inconsistency:** [GitHub Issue #9144](https://github.com/espressif/arduino-esp32/issues/9144)
- **ESP-LWIP DNS sorting fix:** [esp-lwip PR #66](https://github.com/espressif/esp-lwip/pull/66)
- **Lib builder IPv6 DNS config:** [esp32-arduino-lib-builder PR #166](https://github.com/espressif/esp32-arduino-lib-builder/pull/166)
- **Mongoose IPv6 docs:** Built into `mongoose.h` — search for `MG_ENABLE_IPV6`
