# Flashing the Olimex ESP32-Gateway (Ethernet)

## Hardware

- **Board:** Olimex ESP32-Gateway RevF/RevG
- **Connection:** USB serial via on-board CH341 UART bridge
- **Build target:** `olimex_esp32-gateway-f`
- **No OpenEVSE controller needed** for ESP32-side testing (the controller set command fails harmlessly)

## Serial Port

Plug in via USB. The CH341 bridge appears as `/dev/ttyUSBx`:

```
dmesg | tail -5
# Jun 03 23:35:10 rnavarro-ryzen kernel: usb 5-4: ch341-uart converter now attached to ttyUSB1
```

If multiple USB serial devices are present, check `dmesg` for the most recent one. We had `/dev/ttyUSB0` (persistent, from another device) and `/dev/ttyUSB1` (the Olimex, just plugged in).

## Build and Flash

```bash
cd ~/workspace/openevse_esp32_firmware

# Build
pio run -e olimex_esp32-gateway-f

# Flash
pio run -e olimex_esp32-gateway-f -t upload --upload-port /dev/ttyUSB1
```

### What we hit on first attempt

The first `pio run -t upload` failed with:

```
A fatal error occurred: The chip stopped responding.
*** [upload] Error 2
```

esptool could not put the chip into download mode via DTR/RTS auto-toggle. We had to manually enter boot mode:

1. Hold the **BOOT** button on the Olimex board
2. Press and release **RST**
3. Release **BOOT**
4. Re-run the `pio run -t upload` command

The second attempt worked immediately at 460800 baud — wrote 1,841,248 bytes in ~19 seconds.

**Note:** We tried running esptool.py directly to get more control, but it failed with `ModuleNotFoundError: No module named 'serial'` — the system Python3 doesn't have pyserial installed, and PlatformIO's bundled esptool uses its own venv. Always use `pio run -t upload` instead of invoking esptool directly.

### Manual boot mode (if auto-reset fails)

On the Olimex ESP32-Gateway, you can force download mode:

1. Hold **BOOT**
2. Press and release **RST**
3. Release **BOOT**
4. Run the flash command immediately

Some Olimex boards have a "BAT/MEM" jumper that can also affect boot mode — leave it in the default position.

## Serial Console (115200 baud)

```bash
# Quick boot log capture
timeout 15 cat /dev/ttyUSB1 | head -80

# Interactive monitor
pio device monitor --port /dev/ttyUSB1 --baud 115200 -e olimex_esp32-gateway-f
```

### Boot output we observed

```
onnected
Connected, IP: 172.16.5.158
E (43044) wifi_init_default: esp_wifi_get_mac failed with 12289
OpenEVSE not responding or not connected
```

- The first line is truncated ("onnected") — we caught it mid-message
- `wifi_init_default` error is expected — the Olimex Ethernet build doesn't init WiFi
- `OpenEVSE not responding` is expected — no controller is attached for bench testing

## Post-Flash Verification

These are the actual commands and results from our Phase 0+1 test session (June 3, 2026):

```bash
# Find the device on the network
avahi-browse -r _http._tcp -t | grep -B1 -A3 "openevse"
# Result: hostname = [openevse-c104.local], address = [172.16.5.158]

# IPv4 API test
curl -s http://172.16.5.158/status | python3 -m json.tool
# Result: works, full JSON response

# IPv6 address check (Phase 0+1)
curl -s http://172.16.5.158/status | python3 -c "import json,sys; d=json.load(sys.stdin); print('ipv6_global:', d.get('ipv6address_global','MISSING')); print('ipv6_linklocal:', d.get('ipv6address_linklocal','MISSING'))"
# Result: ipv6_global: 2603:8000:2d00:4605:b68a:0aff:fe75:c107
#         ipv6_linklocal: fe80:0000:0000:0000:b68a:0aff:fe75:c107

# ping6 (global address)
ping6 -c 3 2603:8000:2d00:4605:b68a:0aff:fe75:c107
# Result: 3 packets, 0% loss, ~0.5ms RTT

# mDNS AAAA record
avahi-resolve -6 -n openevse-c104.local
# Result: openevse-c104.local  fe80::b68a:aff:fe75:c107

# HTTP over IPv6 (Phase 2+ only — fails before that)
curl -6 --connect-timeout 5 http://[2603:8000:2d00:4605:b68a:0aff:fe75:c107]/status
# Result: connection refused / timeout — expected failure until Phase 2 lands
```

## Full Phase 0+1 Test Results

| # | Test | Result |
|---|---|---|
| 1 | IPv4 regression (API still works) | ✅ 172.16.5.158 |
| 2 | IPv6 global (SLAAC) | ✅ 2603:8000:2d00:4605:b68a:0aff:fe75:c107 |
| 3 | IPv6 link-local | ✅ fe80::b68a:0aff:fe75:c107 |
| 4 | API reports IPv6 | ✅ Both fields in /status |
| 5 | ping6 (global) | ✅ 0% loss, ~0.5ms |
| 6 | mDNS AAAA | ✅ openevse-c104.local → fe80::b68a:aff:fe75:c107 |
| 7 | HTTP over IPv6 | ❌ Connection failed — expected, Phase 2 will fix |

## Known Issues

- The Ethernet PHY takes 3-5 seconds to link after boot. The serial log may show `Connected, IP: 0.0.0.0` before the real address appears.
- If the device was previously running stock firmware, the first boot after flashing may take longer (NVS partition migration).
- Do not try to invoke `esptool.py` directly — it depends on pyserial which is only in PlatformIO's internal venv. Use `pio run -t upload` instead.
