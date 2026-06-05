#ifndef _EMONESP_MQTT_H
#define _EMONESP_MQTT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <MongooseMqttClient.h>
#include <MicroTasks.h>

#include "emonesp.h"
#include "app_config.h"
#include "evse_man.h" // For EvseProperties, EvseClient_OpenEVSE_MQTT etc.
#include "limit.h"    // For LimitProperties
#include "event.h"
#include "scheduler.h" // For scheduler interaction
#include "manual.h"    // For manual override interaction
#include "divert.h"    // For divert interaction
#include "input.h"     // For solar, grid_ie
#include "espal.h"
#include "net_manager.h"
#include "certificates.h"
#include "current_shaper.h" // For shaper interaction, if any direct calls were made

// Forward declarations

#define MQTT_LOOP_INTERVAL 50 // ms, replaces MQTT_LOOP define
#define MQTT_CONNECT_TIMEOUT (5 * 1000) // ms

class Mqtt : public MicroTasks::Task {
  private:
    MongooseMqttClient _mqttclient;
    EvseManager *_evse; // Pointer to EvseManager instance

    // MQTT connection state and timing
    unsigned long _nextMqttReconnectAttempt = 0;
    unsigned long _mqttRestartTime = 0;
    bool _connecting = false;
    unsigned long _connectingSince = 0;  // millis() when _connecting became true
    static constexpr unsigned long CONNECTING_TIMEOUT_MS = 30 * 1000; // 30s watchdog
    unsigned long _error_time = 0; // To handle disconnect events properly

#if MG_ENABLE_IPV6
    // IPv6 failure tracking: if IPv6 TCP connect fails repeatedly,
    // suppress IPv6 resolution for a cooldown period to avoid
    // getting pinned to a broken IPv6 path with no IPv4 fallback.
    uint8_t _ipv6FailCount = 0;                  // Consecutive IPv6 connect failures
    unsigned long _ipv6SuppressedUntil = 0;        // millis() timestamp: skip AAAA until this time
    bool _lastAttemptWasIPv6 = false;              // Did the last attempt resolve to IPv6?
    bool _connectedViaIPv6 = false;               // Is the current connection over IPv6?
    static constexpr uint8_t IPV6_FAIL_THRESHOLD = 2;   // Failures before suppression
    static constexpr unsigned long IPV6_SUPPRESS_MS = 10 * 60 * 1000; // 10 min cooldown

    // DNS resolution cache: avoid blocking getaddrinfo() on every reconnect.
    // Cached result is used if fresh (within TTL); re-resolved otherwise.
    String _resolvedHost;                         // Resolved address string (IP literal)
    String _resolvedFor;                          // Hostname this cache entry resolved (invalidates on mqtt_server change)
    bool _resolvedIsIPv6 = false;                 // Was the resolved address IPv6?
    unsigned long _resolvedAt = 0;                 // millis() when resolved
    static constexpr unsigned long DNS_CACHE_TTL_MS = 5 * 60 * 1000; // 5 min TTL
    bool _pendingRestartForIPv6 = false;          // Delayed restart waiting for disconnect

    // EventListener for IPv6 global address changes from net_manager.
    // MQTT decides its own upgrade policy: only reconnect if currently on IPv4.
    MicroTasks::EventListener _ipv6GlobalListener{this};
#endif

    // Version tracking for publishing updates
    uint8_t _claimsVersion = 0;
    uint8_t _overrideVersion = 0;
    uint8_t _scheduleVersion = 0;
    uint8_t _limitVersion = 0;
    uint32_t _configVersion = 0;

    String _lastWill = "";
    unsigned long _loop_timer = 0; // Timer for periodic publishing tasks

    // Properties for claims, overrides, limits
    EvseProperties _claim_props;
    EvseProperties _override_props;
    LimitProperties _limit_props;

    // Internal helper methods
    void attemptConnection();
    void onMqttConnect();
    void onMqttDisconnect(int err, const char *reason);
    void subscribeTopics();
    void publishInitialState();
    void checkAndPublishUpdates();

    // Callback for incoming MQTT messages
    // Made static because MongooseMqttClient might need a C-style function pointer
    // or we pass 'this' and use a wrapper. For simplicity, we can make it a member
    // and ensure the lambda captures 'this' correctly.
    // MongooseMqttClient::onMessage takes std::function, so member function is fine.
    void handleMqttMessage(MongooseString topic, MongooseString payload);

  protected:
    void setup() override;
    unsigned long loop(MicroTasks::WakeReason reason) override;

  public:
    Mqtt(EvseManager &evse);
    ~Mqtt();

    void begin(); // To start the task

    // Public interface (existing functions from mqtt.h, adapted)
    bool isConnected();
    void restartConnection();
#if MG_ENABLE_IPV6
    bool isConnectedViaIPv6() { return _connectedViaIPv6; }
#endif

    // Publishing methods - these can be called from other modules
    void publishData(JsonDocument &data); // Generic data publish
    void publishConfig();
    void publishClaim();
    void setClaim(bool override, EvseProperties &props);
    void publishOverride();
    void publishSchedule();
    void setSchedule(String schedule);
    void clearSchedule(uint32_t event);
    void publishLimit();
    void setLimit(LimitProperties &limitProps);

    // Method to be called by other services when their state changes
    void notifyEvseClaimChanged();
    void notifyManualOverrideChanged();
    void notifyScheduleChanged();
    void notifyLimitChanged();
    void notifyConfigChanged();
};

extern Mqtt mqtt; // Global instance of the MQTT task

#endif // _EMONESP_MQTT_H