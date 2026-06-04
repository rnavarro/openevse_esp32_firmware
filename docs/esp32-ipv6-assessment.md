# ESP32 Arduino IPv6 Stack Assessment

**Project:** OpenEVSE ESP32 Firmware  
**Platform:** `espressif32@6.12.0` (PlatformIO), Arduino framework  
**Arduino ESP32 Core:** ~v3.1.x (shipped with espressif32@6.12.0)  
**Date:** 2026-06-03

---

## 1. Available IPv6 APIs in ESP32 Arduino

The IPv6 API is gated behind `#if CONFIG_LWIP_IPV6` compile-time flag. The ESP32 Arduino core v3.x ships with `CONFIG_LWIP_IPV6=y` enabled by default.

### 1.1 NetworkInterface (base class for WiFi STA, ETH, AP)

All network interfaces inherit from `NetworkInterface` (defined in `libraries/Network/src/NetworkInterface.h`). The IPv6 methods are:

```cpp
#if CONFIG_LWIP_IPV6
bool enableIPv6(bool en = true);       // Request IPv6 on this interface
bool hasLinkLocalIPv6() const;         // Check if fe80:: address assigned
bool hasGlobalIPv6() const;            // Check if global scope address assigned
IPAddress linkLocalIPv6() const;       // Get fe80:: link-local address
IPAddress globalIPv6() const;          // Get global scope address
#endif
```

**How `enableIPv6()` works internally:**
1. Sets the `ESP_NETIF_WANT_IP6_BIT` status bit
2. If already connected, calls `esp_netif_create_ip6_linklocal()` to trigger link-local address creation
3. The link-local address (`fe80::`) is created automatically via the EUI-64 method from the MAC address
4. A global address is obtained via SLAAC from Router Advertisements (if the network supports it)

### 1.2 WiFi STA (`WiFiSTAClass` / `STAClass`)

```cpp
// From WiFiSTA.h (compatibility wrapper)
#if CONFIG_LWIP_IPV6
bool enableIPv6(bool en = true);       // WiFi.enableIPv6() - delegates to NetworkInterface
IPAddress linkLocalIPv6();             // WiFi.linkLocalIPv6()
IPAddress globalIPv6();                // WiFi.globalIPv6()
#endif
```

Note: The older API `WiFi.localIPv6()` does **not** exist in the current codebase. The available accessors are:
- `WiFi.linkLocalIPv6()` - returns the `fe80::` address
- `WiFi.globalIPv6()` - returns the global/ULA scope address
- `WiFi.localIP()` - still returns IPv4 only

### 1.3 WiFi AP (`WiFiAPClass`)

```cpp
#if CONFIG_LWIP_IPV6
bool softAPenableIPv6(bool enable = true);
IPAddress softAPlinkLocalIPv6();
#endif
```

### 1.4 Ethernet (`ETHClass`)

`ETHClass` inherits from `NetworkInterface`, so all the base methods work:
```cpp
ETH.enableIPv6();          // Enable IPv6 on Ethernet
ETH.linkLocalIPv6();       // Get fe80:: address
ETH.globalIPv6();          // Get global address
```

There is no separate `ETH.enableIPv6()` wrapper in `ETH.h` - it uses the inherited `NetworkInterface::enableIPv6()`.

### 1.5 Events

The following IPv6-related events are defined in `NetworkEvents.h`:

| Event | Description |
|---|---|
| `ARDUINO_EVENT_WIFI_STA_GOT_IP6` | STA received an IPv6 address |
| `ARDUINO_EVENT_WIFI_AP_GOT_IP6` | AP received an IPv6 address |
| `ARDUINO_EVENT_ETH_GOT_IP6` | Ethernet received an IPv6 address |
| `ARDUINO_EVENT_PPP_GOT_IP6` | PPP interface received an IPv6 address |

The `GOT_IP6` event fires when **any** IPv6 address is assigned (link-local or global). To distinguish, check `hasLinkLocalIPv6()` vs `hasGlobalIPv6()` in the event handler.

### 1.6 API Naming Note: `enableIpV6()` vs `enableIPv6()`

The canonical name is `enableIPv6()` (lowercase 'p', uppercase 'V'). The older v2.x core used different casing. In v3.x, the naming is consistent. The `WiFiIPv6.ino` example uses `WiFi.enableIPv6()`.

### 1.7 `localIPv6()` - Does Not Exist

There is no `localIPv6()` method in the v3.x Arduino ESP32 core. The discussion in GitHub #9144 and #9009 raised concerns about this naming confusion:
- `localIP()` returns IPv4 (always has)
- `linkLocalIPv6()` returns the `fe80::` address
- `globalIPv6()` returns the global/ULA scope address

A `localIPv6()` method was considered but rejected in favor of the explicit `linkLocalIPv6()` / `globalIPv6()` split.

---

## 2. LwIP IPv6 Stack Behavior

### 2.1 SLAAC (Stateless Address Autoconfiguration)

**Supported.** The ESP32 LwIP stack implements SLAAC via Router Advertisements. When IPv6 is enabled and the interface is connected:

1. **Link-local address** (`fe80::`) is created immediately using EUI-64 from the MAC address
2. When Router Advertisements (RA) are received, global addresses are autoconfigured from the advertised prefixes
3. Multiple global addresses may be created if the network advertises multiple prefixes (e.g., a global prefix + ULA prefix)

### 2.2 DHCPv6

**Not supported for address assignment (stateful DHCPv6).** The LwIP stack used by ESP32 does not implement stateful DHCPv6 (RFC 3315) for obtaining addresses. From the LwIP documentation:

> "Stateful DHCPv6 (for addresses) is not implemented yet."

Stateless DHCPv6 (for DNS server information only, RFC 3736) may work via the RDNSS option in Router Advertisements.

**Practical impact:** The ESP32 can only obtain IPv6 addresses via SLAAC. If your network only provides addresses via DHCPv6 (rare for home networks, more common in enterprise), the ESP32 will only get a link-local address.

### 2.3 Router Advertisements

**Supported.** The LwIP NDP (Neighbor Discovery Protocol) implementation processes incoming Router Advertisements. This is how SLAAC works:
- RA messages advertise on-link prefixes
- The ESP32 autoconfigures addresses from these prefixes using EUI-64
- RA messages may also include RDNSS (DNS server) information

### 2.4 Address Lifecycle

A typical dual-stack connection sequence:

```
1. WiFi.begin(ssid, pass)
2. ARDUINO_EVENT_WIFI_STA_CONNECTED
3. WiFi.enableIPv6()  -> triggers esp_netif_create_ip6_linklocal()
4. ARDUINO_EVENT_WIFI_STA_GOT_IP6  (link-local fe80:: assigned)
5. ARDUINO_EVENT_WIFI_STA_GOT_IP   (IPv4 via DHCP)
6. (seconds later) RA received -> SLAAC
7. ARDUINO_EVENT_WIFI_STA_GOT_IP6  (global address assigned)
```

Note: The `GOT_IP6` event fires **twice** - once for link-local, once for global. This is normal behavior.

### 2.5 Privacy Extensions (RFC 4941)

**Not supported.** The ESP32 LwIP stack does not implement RFC 4941 privacy extensions. All SLAAC addresses use EUI-64 based on the MAC address, making them predictable and stable. This is typical for IoT devices but means the IPv6 address can be used to track the device.

### 2.6 Multiple IPv6 Addresses

The ESP32 can hold multiple IPv6 addresses simultaneously (`CONFIG_LWIP_IPV6_NUM_ADDRESSES` defaults to 3). However, the Arduino API only exposes:
- One link-local address via `linkLocalIPv6()`
- One global address via `globalIPv6()`

If the network has multiple prefixes, `globalIPv6()` returns only the first global address found.

---

## 3. DNS Resolution with IPv6

### 3.1 `getaddrinfo()` / AAAA Records

The ESP-IDF LwIP stack supports AAAA record queries. The underlying `lwip_getaddrinfo()` function can return IPv6 addresses when `CONFIG_LWIP_IPV6` is enabled.

### 3.2 Arduino `hostByName()`

`WiFi.hostByName()` (in `WiFiGenericClass`) was updated in v3.x to use `getaddrinfo()` internally (via PR #9439). This means:
- It can return IPv6 addresses
- It queries for both A and AAAA records
- Address selection follows a "best address" heuristic

**Known issues (from GitHub discussions #9009, #9487):**
- In dual-stack networks, `hostByName()` may prefer IPv4 over IPv6 even when both are available
- In IPv6-only networks, DNS may fail entirely if the DNS server address is only learned via IPv4 DHCP
- A fix for RFC 6724-compliant address sorting was proposed in esp-lwip#66

### 3.3 WiFiClient / HTTPClient

`NetworkClient` (the base for `WiFiClient` in v3.x) supports connecting to IPv6 addresses:
- `connect(hostname, port)` resolves the hostname via `getaddrinfo()` and connects to whichever address family is returned
- `connect(IPAddress, port)` works with both IPv4 and IPv6 `IPAddress` objects
- The `IPAddress` class in v3.x can hold either IPv4 (4 bytes) or IPv6 (16 bytes)

### 3.4 Hostname with Only AAAA Records

This **should** work in v3.x if:
1. IPv6 is enabled via `enableIPv6()`
2. The ESP32 has obtained a global IPv6 address (not just link-local)
3. The DNS server is reachable via IPv6 or dual-stack

In practice, this may be unreliable due to DNS server configuration issues in IPv6-only networks (see section 2.2 of discussion #9009).

---

## 4. Web Server IPv6 Support

### 4.1 Arduino WebServer / AsyncWebServer

The standard Arduino `WebServer` and `AsyncWebServer` bind to `0.0.0.0` (IPv4 only) by default. To listen on IPv6, you would need to bind to `::` (IPv6 any address). The Arduino WiFiServer class in v3.x can accept IPv6 connections if the underlying socket is created with `AF_INET6`.

**However**, this requires application-level changes and is not well-documented or tested.

### 4.2 Mongoose (used by OpenEVSE)

The OpenEVSE project uses **Mongoose** (via `ArduinoMongoose@0.0.22`), not AsyncWebServer.

**Current status in OpenEVSE:**
- `MG_ENABLE_IPV6` is **NOT enabled** for ESP32 builds
- The `mongoose.h` header defaults `MG_ENABLE_IPV6` to `0` (line 3246)
- `MG_ENABLE_IPV6=1` is only defined for NRF51/NRF52 platform sections
- No build flag in `platformio.ini` sets `-DMG_ENABLE_IPV6=1`

**To enable IPv6 in Mongoose**, add to the build flags:
```
build_flags = -DMG_ENABLE_IPV6=1
```

Mongoose itself has full IPv6 support when compiled with `MG_ENABLE_IPV6=1`:
- `mg_http_listen()` can bind to `[::]` for IPv6
- `mg_connect()` can connect to IPv6 addresses
- DNS AAAA record support is built in (`MG_DNS_AAAA_RECORD = 0x1c`)
- Literal IPv6 addresses in URLs are supported with bracket notation: `http://[::1]:80/`

### 4.3 Mongoose Version

The bundled Mongoose is **v6.x** (old; current upstream is v7.x). The `ArduinoMongoose@0.0.22` library bundles its own `mongoose.c`/`mongoose.h`. IPv6 support in this version requires:
1. Compile with `-DMG_ENABLE_IPV6=1`
2. Ensure the LwIP socket layer supports `AF_INET6`

---

## 5. mDNS and IPv6

### 5.1 ESP-IDF mDNS Service

The ESP-IDF `mdns` component supports IPv6 address advertising. When `CONFIG_LWIP_IPV6` is enabled:

- mDNS will advertise both A and AAAA records for the host
- `MDNS.addService()` registers the service with both IPv4 and IPv6 addresses
- `MDNS.queryHost()` returns an `IPAddress` that may be IPv4 or IPv6
- `MDNS.addressV6(idx)` explicitly returns IPv6 addresses from service queries

### 5.2 ESP32 Arduino ESPmDNS Library

```cpp
class MDNSResponder {
  // ...
  IPAddress queryHost(char *host, uint32_t timeout = 2000);
  IPAddress address(int idx);         // May return IPv4 or IPv6
  IPAddress addressV6(int idx);       // Explicit IPv6 query result
  // ...
};
```

The `addressV6()` method provides access to IPv6 addresses from mDNS service queries. The `queryHost()` method may return either IPv4 or IPv6 depending on what's available.

### 5.3 OpenEVSE mDNS Usage

The OpenEVSE firmware uses `MDNS.begin(hostname)` and `MDNS.addService()` for service discovery. No changes are needed for IPv6 - the ESP-IDF mDNS component automatically includes IPv6 addresses in advertisements when IPv6 is enabled and addresses are assigned.

---

## 6. Current State in OpenEVSE Firmware

### 6.1 What's Already There

The `net_manager.cpp` already handles IPv6 events in the event debug logging:
```cpp
ARDUINO_EVENT_WIFI_STA_GOT_IP6 == event ? "ARDUINO_EVENT_WIFI_STA_GOT_IP6" :
ARDUINO_EVENT_WIFI_AP_GOT_IP6 == event ? "ARDUINO_EVENT_WIFI_AP_GOT_IP6" :
ARDUINO_EVENT_ETH_GOT_IP6 == event ? "ARDUINO_EVENT_ETH_GOT_IP6" :
```

However, no code actually **acts** on these events or enables IPv6.

### 6.2 What's Missing

1. **No `WiFi.enableIPv6()` / `ETH.enableIPv6()` calls** anywhere in the codebase
2. **No IPv6 address reporting** - the `_ipaddress` field only stores IPv4
3. **Mongoose IPv6 disabled** - `MG_ENABLE_IPV6` defaults to 0
4. **No IPv6 connection handling** - no dual-stack listener configuration
5. **No IPv6 DNS consideration** - DNS servers only configured via IPv4

---

## 7. Readiness Assessment

### What Works (ESP32 Core Level)

| Feature | Status | Notes |
|---|---|---|
| IPv6 address assignment (SLAAC) | Working | Link-local + global via RA |
| IPv6 API (`enableIPv6`, `linkLocalIPv6`, `globalIPv6`) | Working | Available when `CONFIG_LWIP_IPV6=y` |
| IPv6 events (`GOT_IP6`) | Working | Fires for link-local and global |
| `IPAddress` class for IPv6 | Working | 16-byte IPv6 addresses supported |
| `WiFiClient` to IPv6 destinations | Partially working | DNS issues in some network configs |
| mDNS IPv6 advertisements | Working | Automatic when IPv6 enabled |
| Ethernet IPv6 | Working | Uses same `NetworkInterface` base |

### What Doesn't Work Well

| Issue | Severity | Notes |
|---|---|---|
| DHCPv6 address assignment | Medium | LwIP limitation; SLAAC-only |
| DNS in IPv6-only networks | High | DNS server may only be known via IPv4 DHCP |
| IPv4 preference in dual-stack | Medium | `hostByName()` may return IPv4 even when IPv6 available |
| RFC 4941 privacy extensions | Low | Addresses are predictable (EUI-64) |
| Multiple address exposure | Low | API only returns one link-local + one global |

### What Needs Work in OpenEVSE

| Task | Effort | Priority |
|---|---|---|
| Call `WiFi.enableIPv6()` / `ETH.enableIPv6()` after connect | Low | Required for any IPv6 |
| Add IPv6 address to status reporting | Low | Useful for diagnostics |
| Enable `MG_ENABLE_IPV6=1` in build flags | Low | Required for Mongoose IPv6 |
| Configure Mongoose to listen on `[::]` (dual-stack) | Medium | Needed for incoming IPv6 connections |
| Test Mongoose outgoing connections over IPv6 | Medium | MQTT, HTTP, WebSocket |
| Handle `GOT_IP6` events meaningfully | Low | Update status display |
| IPv6 in AP mode | Low | `WiFi.softAPenableIPv6()` |

---

## 8. References

- **Official IPv6 Example:** [libraries/WiFi/examples/WiFiIPv6/WiFiIPv6.ino](https://github.com/espressif/arduino-esp32/blob/master/libraries/WiFi/examples/WiFiIPv6/WiFiIPv6.ino)
- **IPv6 Discussion (comprehensive):** [GitHub Discussion #9009](https://github.com/espressif/arduino-esp32/discussions/9009)
- **DNS IPv6 Fix PR:** [GitHub PR #9439](https://github.com/espressif/arduino-esp32/pull/9439)
- **IPv6-only network issues:** [GitHub Issue #9143](https://github.com/espressif/arduino-esp32/issues/9143)
- **localIPv6 accessor inconsistency:** [GitHub Issue #9144](https://github.com/espressif/arduino-esp32/issues/9144)
- **ESP-LWIP DNS sorting fix:** [esp-lwip PR #66](https://github.com/espressif/esp-lwip/pull/66)
- **Lib builder IPv6 DNS config:** [esp32-arduino-lib-builder PR #166](https://github.com/espressif/esp32-arduino-lib-builder/pull/166)
- **Mongoose IPv6 docs:** Built into `mongoose.h` - search for `MG_ENABLE_IPV6`
