# ArduinoMongoose IPv6 Readiness Assessment

**Date:** 2026-06-03
**Library:** ArduinoMongoose 0.0.22 (jeremypoulter)
**Embedded Mongoose:** Cesanta Mongoose 6.18
**Latest ArduinoMongoose:** 0.0.24 (same Mongoose 6.18, no IPv6 changes)

---

## 1. Mongoose Version

ArduinoMongoose 0.0.22 embeds **Cesanta Mongoose 6.18** (`#define MG_VERSION "6.18"` in `mongoose.h:26`).

This is significant because Mongoose 6.x is the **legacy** branch. Cesanta moved to Mongoose 7.x (then 7.x Embedded) which has a completely rewritten networking stack with proper dual-stack support. The 6.18 branch is no longer actively developed upstream.

## 2. What MG_ENABLE_IPV6 Actually Does

`MG_ENABLE_IPV6` is **hardcoded to 1** for ESP32 builds (`mongoose.h:1155`), so it is always enabled on the OpenEVSE firmware targets. There is no compile-time toggle; IPv6 code paths are always compiled in.

### Conditional blocks summary

There are **14 total references** to `MG_ENABLE_IPV6` across the two files:
- `mongoose.h`: 8 references
- `mongoose.c`: 6 references

The IPv6 code blocks are **mature, not stubbed**. They cover:

| Area | File:Line | What it does |
|------|-----------|--------------|
| `union socket_address` | `mongoose.h:3549` | Adds `struct sockaddr_in6 sin6` to the address union |
| Address parsing | `mongoose.c:2661-2666` | Parses `[IPv6]:port` format via `sscanf` + `inet_pton(AF_INET6, ...)` |
| Address stringification | `mongoose.c:10435-10442` | Detects `AF_INET6` family, formats with `[brackets]` for port display |
| DNS AAAA records | `mongoose.c:11437-11443` | Copies AAAA record data (16 bytes) into `struct in6_addr` |
| LwIP TCP/UDP functions | `mongoose.c:14908-14915` | Remaps `tcp_new` -> `tcp_new_ip6`, `tcp_bind` -> `tcp_bind_ip6`, etc. |
| Address copy macro | `mongoose.c:14913-14915` | `SET_ADDR` copies 16-byte IPv6 addresses |

### The critical remapping (mongoose.c:14908-14930)

```c
#if MG_ENABLE_IPV6
#define TCP_NEW tcp_new_ip6
#define TCP_BIND tcp_bind_ip6
#define UDP_BIND udp_bind_ip6
#define IPADDR_NTOA(x) ip6addr_ntoa((const ip6_addr_t *)(x))
#define SET_ADDR(dst, src) \
  memcpy((dst)->sin6.sin6_addr.s6_addr, (src)->ip6.addr, \
         sizeof((dst)->sin6.sin6_addr.s6_addr))
#else
#define TCP_NEW tcp_new
#define TCP_BIND tcp_bind
...
```

**This is the crux of the problem.** When `MG_ENABLE_IPV6=1`, ALL TCP/UDP operations are routed through LwIP's `_ip6` variants exclusively. There is no dual-stack mode.

## 3. How mg_bind() Behaves with MG_ENABLE_IPV6

### The flow

1. `MongooseHttpServer::begin(port)` converts the port number to a string (e.g., `"80"`) and calls `mg_bind(mgr, "80", ...)`.
2. `mg_bind()` calls `mg_parse_address("80", &sa, ...)` which matches the `:%u` pattern, setting `sa.sin.sin_port = htons(80)` with `AF_INET` family and all-zeroes address (INADDR_ANY).
3. The listen path calls `mg_lwip_if_listen_tcp_tcpip()` which:
   - Creates a PCB via `TCP_NEW()` -> **`tcp_new_ip6()`**
   - Binds via `TCP_BIND(tpcb, ip, port)` -> **`tcp_bind_ip6()`**

### The problem: IPv6-only binding

When `MG_ENABLE_IPV6=1`:
- `TCP_NEW` = `tcp_new_ip6()` creates an **IPv6-only** PCB
- `TCP_BIND` = `tcp_bind_ip6()` binds to an **IPv6-only** address

**There is no dual-stack listen.** The server will only accept IPv6 connections. IPv4 clients will be unable to connect.

On LwIP, dual-stack requires either:
- Creating an IPv6 PCB with `tcp_new_ip6()` and binding to `ip6_addr_any`, then relying on LwIP's `IPV6_V6ONLY=0` behavior, OR
- Creating separate IPv4 and IPv6 listening PCBs

Mongoose 6.18 does neither. It creates a single IPv6 PCB and that is it.

### What `MongooseHttpServer::begin()` would need

Currently (`MongooseHttpServer.cpp:24-27`):
```cpp
bool MongooseHttpServer::begin(uint16_t port) {
  char s_http_port[6];
  utoa(port, s_http_port, 10);
  nc = mg_bind(Mongoose.getMgr(), s_http_port, defaultEventHandler, this);
```

Passing just `"80"` results in `mg_parse_address` setting `AF_INET` family, but then the LwIP layer overrides to IPv6 via `tcp_new_ip6()`. Changing to `"[::]:80"` would not help because:
1. `mg_parse_address` would parse it into `AF_INET6` family (correct)
2. But the listen path still calls `tcp_bind_ip6()` which only listens on IPv6

**The real fix requires changes at the LwIP integration layer, not just the address string.**

## 4. DNS AAAA Record Handling

### Sync resolver (`mg_resolve2`)

The sync resolver (`mongoose.c:2539`) uses `getaddrinfo()` with `hints.ai_family = AF_INET` -- **IPv4 only**. It never requests AAAA records.

### Async resolver (`mg_resolve_async`)

The async resolver (`mongoose.c:12067`) takes a `query` parameter. When `mg_connect_opt()` triggers DNS resolution for a hostname (mongoose.c:3182):

```c
if (mg_resolve_async_opt(nc->mgr, host, MG_DNS_A_RECORD, resolve_cb, nc, o) != 0) {
```

**It hardcodes `MG_DNS_A_RECORD` (0x01), never `MG_DNS_AAAA_RECORD` (0x1c).**

Even though Mongoose can parse AAAA record responses (mongoose.c:11438), the async resolver never queries for them. The callback (`resolve_cb` at mongoose.c:3047) also only looks for `MG_DNS_A_RECORD` answers:

```c
for (i = 0; i < msg->num_answers; i++) {
  if (msg->answers[i].rtype == MG_DNS_A_RECORD) {
    /* Async resolver guarantees that there is at least one answer.
     * TODO(lsm): handle IPv6 answers too */
    mg_dns_parse_record_data(msg, &msg->answers[i], &nc->sa.sin.sin_addr, 4);
```

The comment `TODO(lsm): handle IPv6 answers too` at line 3055 confirms this is **known unfinished work**.

### Summary: DNS never resolves to IPv6

- AAAA record parsing exists but is never invoked
- DNS queries always use A record type
- Even if AAAA records were returned, the callback ignores them
- The resolved address is written to `nc->sa.sin.sin_addr` (IPv4 field only)

## 5. MongooseHttpServer::begin() Detail

As analyzed above, `begin()` passes a bare port number string (`"80"`) to `mg_bind()`. This is the `:%u` parse path in `mg_parse_address()`, which sets `AF_INET` + `INADDR_ANY`.

The address format does not matter for IPv6 binding because the LwIP layer unconditionally uses `tcp_new_ip6()`/`tcp_bind_ip6()` when `MG_ENABLE_IPV6=1`. The address family from parsing is effectively ignored at the LwIP binding level.

## 6. MQTT Client (MongooseMqttClient)

`MongooseMqttClient::connect()` calls `mg_connect_opt()` with the server address string:

```cpp
_nc = mg_connect_opt(Mongoose.getMgr(), server, eventHandler, this, opts);
```

For `mg_connect_opt()`:
- If `server` is a literal IPv6 address like `[::1]:1883`, `mg_parse_address()` will parse it into `AF_INET6` family (when `MG_ENABLE_IPV6=1`)
- If `server` is a hostname like `mqtt.example.com:1883`, the async resolver queries only A records (see above)
- The connect path calls `mg_lwip_if_connect_tcp_tcpip()` which uses `TCP_NEW()` -> `tcp_new_ip6()` and `tcp_connect(tpcb, ip, port, ...)` where `ip` is cast from `sa->sin.sin_addr.s_addr` (4 bytes)

### IPv6 literal addresses: partially works

If you pass `[::1]:1883`:
- `mg_parse_address` correctly parses the IPv6 address into `sa.sin6`
- But the connect path at mongoose.c:15106 does: `ip_addr_t *ip = (ip_addr_t *) &sa->sin.sin_addr.s_addr;` -- this takes the **IPv4 offset** of the union, not the IPv6 part
- With a `sockaddr_in6` in the union, `sin_addr.s_addr` overlaps with the first 4 bytes of the IPv6 flow info field, not the address

**Literal IPv6 addresses in connect will not work correctly.** The address extraction uses the wrong union member.

## 7. Known Issues and TODOs Blocking IPv6

There are **4 explicit IPv6 TODOs** in mongoose.c:

| Line | TODO | Impact |
|------|------|--------|
| 3055 | `TODO(lsm): handle IPv6 answers too` | Async DNS resolver ignores AAAA answers. Hostname-based IPv6 connections impossible. |
| 4282 | `TODO(lsm): process IPv6 too` | SOCKS proxy only sends IPv4 addresses. IPv6 SOCKS proxying broken. |
| 11963 | `TODO(mkm): handle ipv6` | `/etc/hosts` file resolver skips IPv6 entries entirely. |
| 15965 | `TODO(alashkin): do something with _potential_ IPv6` | PIC32 accept callback zeroes out IPv6 peer addresses. (ESP32 only, so not directly impactful.) |

### Additional implicit issues found:

1. **No dual-stack listen**: `tcp_new_ip6()` creates IPv6-only PCBs. IPv4 clients cannot connect to the HTTP server.
2. **Hardcoded A-record DNS**: `mg_connect_opt()` always queries `MG_DNS_A_RECORD`, never AAAA.
3. **Wrong address extraction in connect**: `mg_lwip_if_connect_tcp_tcpip()` extracts `ip` from `sa->sin.sin_addr` (IPv4 offset) regardless of address family. IPv6 addresses get corrupted.
4. **UDP send uses IPv4 address**: `mg_lwip_if_udp_send()` at mongoose.c:15427 constructs `ip_addr_t` from `nc->sa.sin.sin_addr.s_addr` even when the destination is IPv6.
5. **No IPv6 link-local address configuration**: Mongoose never calls `netif_add_ip6()` or creates link-local addresses. LwIP IPv6 requires explicit address setup.
6. **No SLAAC or DHCPv6 integration**: No code to perform IPv6 address autoconfiguration on the ESP32's LwIP stack.

## 8. Newer ArduinoMongoose Versions

**ArduinoMongoose 0.0.24** (latest) embeds the **same Mongoose 6.18**. The v0.0.22 -> v0.0.24 diff adds:
- WebSocket client wrapper
- Build fixes (VLA replacement, extern "C" linkage)
- CI/dependency bumps

**No IPv6-related changes whatsoever.**

Cesanta's Mongoose 7.x has a completely rewritten networking stack with proper IPv6 support, but ArduinoMongoose has not migrated to it. The API is incompatible (7.x uses event handlers and `mg_http_listen()` instead of `mg_bind()`), making migration a significant rewrite.

---

## Conclusion: IPv6 Readiness Rating

**Mongoose 6.18 in ArduinoMongoose is NOT IPv6-ready.** Despite `MG_ENABLE_IPV6=1` being hardcoded, the implementation has fundamental gaps:

1. **HTTP Server**: Binds IPv6-only due to `tcp_new_ip6()`. IPv4 clients cannot connect. No dual-stack support.
2. **DNS Resolution**: Only queries A records. AAAA records are never requested or processed. Hostname-based IPv6 connections are impossible.
3. **Outbound Connections**: Address extraction uses IPv4 union offset even when IPv6 was parsed. Literal IPv6 addresses are silently corrupted.
4. **Address Configuration**: No code to configure LwIP IPv6 addresses (SLAAC, DHCPv6, or manual).

### What would be needed for IPv6 support

1. **Migrate to Mongoose 7.x** (or later) which has native dual-stack support. This is a major rewrite of ArduinoMongoose.
2. **Or patch Mongoose 6.18** to:
   - Implement dual-stack listen (separate IPv4/IPv6 PCBs or LwIP V6ONLY=0)
   - Add AAAA DNS queries alongside A queries (happy eyeballs)
   - Fix address extraction in connect/listen paths to check `sa.sa.sa_family`
   - Add LwIP IPv6 address configuration (SLAAC/DHCPv6)
   - Fix UDP send to use correct address family
3. **In the OpenEVSE firmware**: Add ESP-IDF LwIP IPv6 initialization (`esp_netif_create_ip6_linklocal()`, enable SLAAC via `esp_netif_set_action_for_lost_ip6()`).

The path of least resistance is likely migrating to Mongoose 7.x rather than patching the legacy 6.18 codebase.
