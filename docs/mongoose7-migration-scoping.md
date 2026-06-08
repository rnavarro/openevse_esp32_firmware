# Mongoose 7 Migration — Scoping

Status: scoping (2026-06-08). **Separate initiative from the IPv6 work** (`ipv6-implementation-plan.md` = v1, `ipv6-v2-planning.md` = core-3 rebase). M7 is tracked apart because it is not technically an IPv6 task — though it *interacts* with IPv6 (M7 is natively dual-stack and would retire our IPv6 patches; see "Relationship to the IPv6 work").

This doc scopes what a Mongoose 6.x → 7.x migration of the ESP32 firmware would actually touch, so the "does it make sense / what does it cost" decision and any proposal to the maintainers is grounded rather than hand-wavy. **Nothing here is started.** No code, no proposal sent.

## Where this sits in the device (safety boundary)

OpenEVSE is two MCUs:
- **The OpenEVSE controller** (`OpenEVSE/open_evse`, C++): the J1772 state machine, GFCI, ground/diode check, relay control, thermal limits — the UL-listed safety logic. It enforces its safety envelope **independently** of whatever it is told.
- **This repo** (the ESP32 WiFi module): WiFi, web UI, MQTT, OCPP, OTA, cloud. It speaks to the controller over **serial via RAPI** (confirmed: `RapiSender`/`OpenEVSE.` usage in `evse_man`, `evse_monitor`, `main.cpp`, etc.). That serial link is the boundary.

**Consequence for this migration:** a Mongoose rewrite lives entirely on the comms/UI side. It cannot compromise charging safety — the controller won't honor anything outside its envelope, and we never touch its firmware. So the risk profile of M7 is **connectivity / OCPP / OTA reliability regression** (a software-quality concern), not a safety-certification concern. The maintainer bar is "convince them it won't regress remote control/monitoring," not "pass a safety review."

## Why M7 at all

- The bundled Mongoose is **6.18** — the last 6.x release (~Feb 2021), **EOL**. Same dead-dependency pattern as IDF 4.4, but on the networking library.
- Mongoose 7.x gives **native dual-stack IPv6**, modern TLS (in-memory certs via `mg_tls_opts`), and a simpler event/URL-based connect model.
- It is well-timed with the core-3/IDF5 modernization wave (#1091 etc.).

## What actually pins the project to 6.x

Not OCPP. **`matth-x/MicroOcppMongoose` already supports Mongoose 7** — its README lists v6.14 / v7.8 / v7.13–v7.17, selected via the build flag `MO_MG_USE_VERSION` (e.g. `MO_MG_V717`), and its source carries both 6.x and 7.x `MG_EV_*` code paths. The `MO_MG_VERSION_614` flag in our `platformio.ini` is a *selectable* version, not a lock.

The real pin is **`jeremypoulter/ArduinoMongoose`** — an original Arduino wrapper (not a Cesanta fork; `isFork: false`, created 2019) that vendors the 6.18 amalgamation and whose wrapper classes are built on 6.x types. The firmware consumes those classes.

## Scoping findings

### 1. The firmware consumes the wrapper, not raw Mongoose

Symbol frequency across `src/` (top):

| Symbol | count | layer |
|--------|-------|-------|
| `MongooseHttpServerRequest` | 78 | HTTP server (web API) |
| `MongooseHttpServerResponseStream` | 56 | HTTP server responses |
| `MongooseString` | 19 | string wrapper |
| `MongooseHttpClient` / `…Response` / `…Request` | 11 / 9 / 5 | HTTP client (OTA, ohm, etc.) |
| `MongooseHttpServer` | 5 | server object |
| `MongooseMqttClient` / `MongooseClient` | 4 / 3 | MQTT |
| `MongooseHttpWebSocketConnection` | 4 | web UI live socket |
| `MongooseCore` | 4 | mgr lifecycle |
| `MongooseSntpClient` | 2 | NTP |
| raw `mg_*` | ~6 total | `mg_str`, `mg_mk_str`, `mg_strfree`, `mg_parse_uri`, `mg_url_encode` |
| raw `MG_EV_*` | ~8 total | `MG_EV_MQTT_CONNACK_*` (mqtt error map), `MG_EV_HTTP_PART_BEGIN/END` (upload) |

**The migration is therefore largely contained inside the wrapper.** Direct firmware exposure to raw Mongoose is small (string/URI helpers + a few event enums). If the wrapper's public *firmware-facing* method signatures are preserved, most of the 78+56+… call sites don't move.

### 2. But the wrapper internals are a thorough rewrite — it binds 6.x types pervasively

The wrapper's public/inline methods reach directly into 6.x amalgamation types, which **do not exist in M7**:

- **HTTP (server + client) — the bulk.** `MongooseHttpServerRequest`/`MongooseHttpClientResponse` hold a raw `http_message *_msg` and expose `_msg->body`, `_msg->uri`, `_msg->method`, `_msg->proto`, `_msg->query_string`, `_msg->resp_status_msg`, `_msg->header_names[i]`/`header_values[i]`, and call `mg_get_http_header`. In M7 `http_message` becomes `struct mg_http_message` (fields `.body`, `.uri`, `.method`, `.query`, `.proto`, `.headers[]` as `mg_http_header`), the listen/serve model changes (`mg_bind` + `MG_EV_HTTP_REQUEST` + `mg_serve_http` → `mg_http_listen` + `MG_EV_HTTP_MSG` + `mg_http_reply`/`mg_http_serve_dir`), and `mg_get_http_header` → `mg_http_get_header`. **Most rewrite effort lives here.** Good news: the firmware mostly calls `.uri()`/`.body()`/`.method()`/`.headers(name)` which return `MongooseString` — those signatures can stay stable; only the wrapper's internal field access is rewritten.
- **MQTT.** `MongooseMqttClient` uses `MG_MQTT_QOS`, `mg_mk_str`, `mg_str` payloads, a raw `mg_connection *_nc`, and the 6.x `MG_EV_MQTT_*` events. M7 MQTT is `mg_mqtt_connect`/`mg_mqtt_pub`/`mg_mqtt_sub` with `mg_mqtt_opts`, events `MG_EV_MQTT_OPEN`/`MG_EV_MQTT_MSG`. Full internal rewrite; public `connect/publish/subscribe/onMessage` signatures can be preserved.
- **WebSocket.** `MongooseWebSocketClient` (already hand-rolls reconnect/ping/stale) maps a 6.x event handler. M7 is `mg_ws_connect`/`mg_ws_send`, `MG_EV_WS_OPEN`/`MG_EV_WS_MSG`. Event-mapping rewrite.
- **SNTP.** `MongooseSntpClient` on the 6.x SNTP path → M7 `mg_sntp_connect`/`mg_sntp_request`.
- **Core / TLS.** `MongooseCore` exposes `struct mg_connect_opts` and `getDefaultOpts(mg_connect_opts*, secure)`; TLS in 6.x is `mg_set_ssl(...)` with file paths / root-CA callback. M7 uses URL-based `mg_connect`/`mg_http_connect` (`tcp://`,`tls://`,`ws://`,`mqtt://`, native IPv6 literals) + `mg_tls_init(c, &mg_tls_opts)` with in-memory `mg_str` certs. The opts/TLS model is the second-biggest change after HTTP.
- **String.** `MongooseString` wraps `mg_str` with `mg_strcmp`, `operator mg_str`. **`mg_str` survives into M7**, so this class is mostly portable; `mg_mk_str` → `mg_str`/`mg_str_n` constructors. Low effort.

### 3. Firmware-side direct touches (the only spots outside the wrapper)

- `mg_str` (survives), `mg_mk_str` → `mg_str_n`, `mg_strfree`, `mg_parse_uri`/`mg_url_encode` → M7 URL helpers differ. ~handful of call sites.
- `MG_EV_MQTT_CONNACK_*` constants in the MQTT error mapping, and `MG_EV_HTTP_PART_BEGIN/END` in multipart upload handling — these 6.x enums/flows changed in M7 (MQTT connack reason codes; multipart via `mg_http_next_multipart`). These specific firmware spots need direct rework.

> Exact M7 symbol names above are stated from the documented 6→7 redesign and should be re-confirmed against the Mongoose 7.17 headers during implementation — treat them as the shape of the delta, not a final API list.

## Effort & risk estimate

- **Bounded but real.** One library (the wrapper, ~8 classes) rewritten internally onto M7, with the public C++ API preserved, plus ~10–15 firmware spots (raw `mg_*` helpers + `MG_EV_*` constants + any inline-method type leaks). Not a firmware-wide rewrite; not a weekend either. Order of weeks.
- **Heaviest piece:** the HTTP server/request/response layer (134 of the call sites flow through it).
- **OCPP is free:** flip `MO_MG_USE_VERSION` to a 7.x value (matth-x already did the work).
- **IPv6 falls out for free:** M7 is dual-stack natively, so our v1 IPv6 patches retire (the ArduinoMongoose IPv6 fork + the lwIP/mDNS hacks become unnecessary on this axis).
- **Risk is regression, not safety:** the test surface is HTTP API, web UI WebSocket, MQTT (incl. TLS), OCPP, NTP, OTA. The native test harness (`pio test -e native`, #1091) + FakeEVSE would help exercise it without hardware.

## Relationship to the IPv6 work

M7 would **obsolete** our IPv6 patches (it does dual-stack itself). That is a reason to **sequence M7 after** landing IPv6 on core-3, not instead of it: IPv6-on-core-3 is near, validated, and useful now; M7 is a larger, speculative rewrite needing maintainer buy-in. If M7 lands later, our IPv6 work ages out gracefully (it bridged the interim and taught us the stack). Do **not** block a near-term validated feature on a speculative rewrite.

## Recommended approach (if pursued)

1. **Don't surprise-PR it.** It's Jez's wrapper and a tiny team's review budget. Float a design issue with Jez (owns `ArduinoMongoose`) + Chris (release lead), using this scoping as the basis, to gauge appetite before writing code.
2. **Rewrite strategy:** keep the wrapper's public C++ API stable; rewrite internals onto M7. Consider whether the wrapper can carry *both* 6.x and 7.x internally behind a version flag (as MicroOcppMongoose does) for a staged cutover — though a full wrapper is more complex to dual-path than MicroOcpp's adapter.
3. **Sequence after IPv6-on-core-3.**
4. **Validate** via `pio test -e native` + FakeEVSE + on-metal (16MB core-3 board), covering HTTP/WS/MQTT-TLS/OCPP/NTP/OTA.

## Open questions / verification needed

1. Confirm exact M7 (7.17) API names for the symbols above against the real headers.
2. Inventory any inline wrapper methods that leak 6.x types into firmware signatures (e.g. `getDefaultOpts(mg_connect_opts*)`, `publish(..., MG_MQTT_QOS(0))`) — do any firmware call sites pass those types directly? (Quick grep before committing to "public API stays stable.")
3. Does the firmware's multipart upload (`MG_EV_HTTP_PART_*`) have a clean M7 equivalent path, or does it need restructuring?
4. Maintainer appetite (Jez/Chris) — the gating non-technical question.
