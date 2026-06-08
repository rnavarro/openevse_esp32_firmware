#ifndef _EMONESP_WIFI_H
#define _EMONESP_WIFI_H

#include <functional>
#include <list>

#include <Arduino.h>
#include <MicroTasks.h>
#include <MicroTasksMessage.h>
#include <MicroTasksEvent.h>

#ifdef ESP32
#include <WiFi.h>
#include "wifi_esp32.h"
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#error Platform not supported
#endif

#include "lcd.h"
#include "LedManagerTask.h"
#include "time_man.h"
#include <DNSServer.h>


class NetManagerTask;

class NetManagerTask : public MicroTasks::Task
{
  typedef std::function<void(int networksFound)> WiFiScanCompleteCallback;

  private:
    class NetState
    {
      public:
        enum Value : uint8_t {
          Starting,
          WiredConnecting,
          AccessPointConnecting,
          StationClientConnecting,
          StationClientReconnecting,
          Connected
        };

      NetState() = default;
      constexpr NetState(Value value) : _value(value) { }

      operator Value() const { return _value; }
      explicit operator bool() = delete;        // Prevent usage: if(state)
      NetState operator= (const Value val) {
        _value = val;
        return *this;
      }

      private:
        Value _value;
    };

    class NetMessage : public MicroTasks::Message
    {
      public:
        enum Value : uint32_t {
          WiFiStart,
          WiFiStop,
          WiFiRestart,
          WiFiAccessPointEnable,
          WiFiAccessPointDisable,
          NetworkEvent
        };

      NetMessage(Value value) : MicroTasks::Message(static_cast<uint32_t>(value)) { }

      operator Value() { return static_cast<Value>(id()); }
      explicit operator bool() = delete;        // Prevent usage: if(state)
    };

    class NetworkEventMessage : public NetMessage
    {
      private:
        WiFiEvent_t _event;
        arduino_event_info_t _info;
      public:
        NetworkEventMessage(WiFiEvent_t event, arduino_event_info_t info) :
          _event(event),
          _info(info),
          NetMessage(NetMessage::NetworkEvent)
        { }
        WiFiEvent_t event() { return _event; };
        arduino_event_info_t &info() { return _info; };
    };

    // Last discovered WiFi access points
    String _st;
    String _rssi;

    // Network state
    NetState _state;
    String _ipaddress;
    String _macaddress;
    String _ipv6address_linklocal_wifi;  // WiFi STA link-local, written in Arduino event task
    String _ipv6address_global_wifi;      // WiFi STA global, written in Arduino event task
    bool _wifiIpv6Enabled = false;        // enableIpV6 succeeded; GOT_IP retries if false (cold-boot race)
    int _slaacGraceCount = 0;             // retry-timer grace periods granted while associated awaiting SLAAC
#ifdef ENABLE_WIRED_ETHERNET
    String _ipv6address_linklocal_eth;    // ETH link-local, written in Arduino event task
    String _ipv6address_global_eth;       // ETH global, written in Arduino event task
#endif

    DNSServer _dnsServer;                  // Create class DNS server, captive portal re-direct
    bool _dnsServerStarted;
    const byte _dnsPort;

    // Access point IP address. SSID will be softAP_ssid + chipID to make SSID unique
    const char *_softAP_ssid;
    const char *_softAP_password;
    IPAddress _apIP;
    IPAddress _apNetMask;
    int _apClients;
    uint32_t _apAutoApStopTime;

    // Wifi Network Strings
    int _clientDisconnects;
    bool _clientRetry;
    unsigned long _clientRetryTime;

    int _wifiButtonState;
    unsigned long _wifiButtonTimeOut;
    bool _apMessage;

    #ifdef ENABLE_WIRED_ETHERNET
    bool _ethConnected;
    uint32_t _wiredTimeout;
    #endif

    LcdTask &_lcd;
    LedManagerTask &_led;
    TimeManager &_time;

    std::list<WiFiScanCompleteCallback> _scanCompleteCallbacks;

    static class NetManagerTask *_instance;

  private:
    void wifiStartInternal();
    void wifiStopInternal();

    void wifiStartAccessPoint();
    void wifiStopAccessPoint();

    void wifiStartClient();
    void wifiStopClient();
    void wifiClientConnect();

    #ifdef ENABLE_WIRED_ETHERNET
    void wiredStart();
    void wiredStop();
    #endif

    void displayState();
    void haveNetworkConnection(IPAddress myAddress);
    void onGlobalIPv6Acquired(const char *ifkey);
    void onGlobalIPv6Lost(const char *ifkey);
    void handleGotIPv6(esp_ip6_addr_t &addr, const char *ifkey, const char *label);

    // Public-fireable event for IPv6 global address transitions.
    // MicroTasks::Event::Trigger() is protected, so we derive and expose it.
    class IPv6GlobalEvent : public MicroTasks::Event {
      public:
        void Fire() { Trigger(); }
    };
    IPv6GlobalEvent _ipv6GlobalChanged;

    void wifiOnStationModeConnected(const WiFiEventStationModeConnected &event);
    void wifiOnStationModeGotIP(const WiFiEventStationModeGotIP &event);
    void wifiOnStationModeDisconnected(const WiFiEventStationModeDisconnected &event);
    void wifiOnAPModeStationConnected(const WiFiEventSoftAPModeStationConnected &event);
    void wifiOnAPModeStationDisconnected(const WiFiEventSoftAPModeStationDisconnected &event);
#ifdef ESP32
    static void onNetEventStatic(WiFiEvent_t event, arduino_event_info_t info);
    void onNetEvent(WiFiEvent_t event, arduino_event_info_t &info);
#endif

    unsigned long handleMessage();
    unsigned long serviceButton();
    unsigned long manageState();

  protected:
    void setup();
    unsigned long loop(MicroTasks::WakeReason reason);

  public:
    NetManagerTask(LcdTask &lcd, LedManagerTask &led, TimeManager &time);

    void begin();

    void wifiScan();

    void wifiStart();
    void wifiStop();
    void wifiRestart();

    void wifiTurnOffAp();
    void wifiTurnOnAp();

    void wifiScanNetworks(WiFiScanCompleteCallback callback);

    bool isConnected();
    bool isWifiClientConnected();
    bool isWiredConnected();

    bool isWifiClientConfigured() {
      return (WiFi.SSID() != "");
    }

    bool isWifiModeSta() {
      return (WIFI_STA == (WiFi.getMode() & WIFI_STA));
    }

    bool isWifiModeStaOnly() {
      return (WIFI_STA == WiFi.getMode());
    }

    bool isWifiModeAp() {
      return (WIFI_AP == (WiFi.getMode() & WIFI_AP));
    }

    // Performing a scan enables STA so we end up in AP+STA mode so treat AP+STA with no
    // ssid set as AP only
    bool isWifiModeApOnly() {
      return ((WIFI_AP == WiFi.getMode()) ||
              (WIFI_AP_STA == WiFi.getMode() && !isWifiClientConfigured()));
    }

    String getIp() {
      return _ipaddress;
    }
    // Preferred routable IPv6 (GUA > ULA) for one interface, derived from
    // LIVE LwIP state at call time — never stale, unlike the event-written
    // Strings (invalidation is silent; a later ULA event would otherwise
    // clobber the GUA). Defined in net_manager.cpp.
    String livePreferredGlobal(const char *ifkey);

    // Full per-slot dump of the LwIP IPv6 address table (state + lifetimes)
    // for one interface. Backs both the GOT_IP6 serial log and /debug/ipv6.
    String dumpIpv6Table(const char *ifkey);

    // Best routable IPv6 address: prefer WiFi STA, then ETH (in STA+AP
    // fallback the WiFi address is the reachable one). Live LwIP state.
    // Defined in net_manager.cpp.
    String getIpv6Global();
    String getIpv6LinkLocal() {
      if (_ipv6address_linklocal_wifi.length() > 0) return _ipv6address_linklocal_wifi;
#ifdef ENABLE_WIRED_ETHERNET
      return _ipv6address_linklocal_eth;
#else
      return "";
#endif
    }
    String getMac() {
      return _macaddress;
    }
    bool hasGlobalIPv6() {
      return getIpv6Global().length() > 0;
    }
    void onIPv6GlobalChanged(MicroTasks::EventListener *listener) {
      _ipv6GlobalChanged.Register(listener);
    }
};

extern NetManagerTask net;

#if MG_ENABLE_IPV6
/*
 * Ensure IPv6 address literals in URLs are bracket-enclosed per RFC 3986.
 * Handles two forms:
 *   - Bare IPv6: "2001:db8::1" → "[2001:db8::1]"
 *   - URL with unbracketed IPv6: "http://2001:db8::1/path" → "http://[2001:db8::1]/path"
 * URLs that already have brackets or don't contain IPv6 literals are returned unchanged.
 */
static inline String ensureIpv6Brackets(const String &url) {
  if (url.length() == 0) return url;

  int schemeEnd = url.indexOf("://");
  if (schemeEnd >= 0) {
    // URL has a scheme (e.g. http://, wss://)
    int hostStart = schemeEnd + 3;
    if (hostStart < (int)url.length() && url.charAt(hostStart) == '[') {
      return url;  // Already bracketed
    }
    // Find end of host (first '/' or ':' after host start)
    int hostEnd = hostStart;
    while (hostEnd < (int)url.length()) {
      char c = url.charAt(hostEnd);
      if (c == '/' || c == ':' || c == '?' || c == '#') break;
      hostEnd++;
    }
    String host = url.substring(hostStart, hostEnd);
    if (host.indexOf(':') >= 0) {
      // Host contains colons = IPv6 literal. Wrap in brackets.
      return url.substring(0, hostStart) + "[" + host + "]" + url.substring(hostEnd);
    }
  } else {
    // No scheme — bare host or host:port
    if (url.charAt(0) == '[') {
      return url;  // Already bracketed
    }
    if (url.indexOf(':') >= 0) {
      // IPv6 literal (possibly with :port). Check if it's host:port vs IPv6
      // IPv6 has multiple colons; host:port has exactly one
      int firstColon = url.indexOf(':');
      int lastColon = url.lastIndexOf(':');
      if (firstColon != lastColon) {
        // Multiple colons = IPv6 literal without port
        return "[" + url + "]";
      }
      // Single colon: could be host:port (not IPv6)
    }
  }
  return url;
}
#endif // MG_ENABLE_IPV6

#endif // _EMONESP_WIFI_H
