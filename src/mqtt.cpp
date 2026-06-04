#if defined(ENABLE_DEBUG) && !defined(ENABLE_DEBUG_MQTT)
#undef ENABLE_DEBUG
#endif

#include "mqtt.h"
#include "app_config.h"
#include "openevse.h"
#include "divert.h"
#include "input.h"
#include "espal.h"
#include "net_manager.h"
#include "web_server.h"
#include "manual.h"
#include "scheduler.h"
#include "current_shaper.h"

#if MG_ENABLE_IPV6
#include <lwip/netdb.h>
#include <sys/socket.h>
#include <netdb.h>
#endif

Mqtt mqtt(evse); // global instance

Mqtt::Mqtt(EvseManager &evseManager) :
  MicroTasks::Task(),
  _evse(&evseManager)
{
}

Mqtt::~Mqtt() {
  if (_mqttclient.connected()) {
    _mqttclient.disconnect();
  }
}

void Mqtt::begin() {
  MicroTask.startTask(this);
}

void Mqtt::setup() {
  DBUGLN("Mqtt::setup called");
  // Initialization that needs to run once when the task is set up.

#if MG_ENABLE_IPV6
  // Listen for IPv6 global address changes. MQTT decides its own
  // upgrade policy: reconnect over IPv6 only if currently on IPv4.
  net.onIPv6GlobalChanged(&_ipv6GlobalListener);
#endif

  _configVersion = INITIAL_CONFIG_VERSION -1; // Force initial publish
  // Initialize versions to force publish on first connect
  _claimsVersion = evse.getClaimsVersion() == 0 ? 1 : evse.getClaimsVersion() -1; // ensure different
  _overrideVersion = manual.getVersion() == 0 ? 1 : manual.getVersion() -1;
  _scheduleVersion = scheduler.getVersion() == 0 ? 1 : scheduler.getVersion() -1;
  _limitVersion = limit.getVersion() == 0 ? 1 : limit.getVersion() -1;

  // Setup MQTT client callbacks
  _mqttclient.onMessage([this](MongooseString topic, MongooseString payload) {
    this->handleMqttMessage(topic, payload);
  });

  _mqttclient.onError([this](int err) {
    DBUGF("MQTT error %d", err);
    this->onMqttDisconnect(err,
      MG_EV_MQTT_CONNACK_UNACCEPTABLE_VERSION == err ? "CONNACK_UNACCEPTABLE_VERSION" :
      MG_EV_MQTT_CONNACK_IDENTIFIER_REJECTED == err ? "CONNACK_IDENTIFIER_REJECTED" :
      MG_EV_MQTT_CONNACK_SERVER_UNAVAILABLE == err ? "CONNACK_SERVER_UNAVAILABLE" :
      MG_EV_MQTT_CONNACK_BAD_AUTH == err ? "CONNACK_BAD_AUTH" :
      MG_EV_MQTT_CONNACK_NOT_AUTHORIZED == err ? "CONNACK_NOT_AUTHORIZED" :
      strerror(err)
    );
    _error_time = millis();
  });

  _mqttclient.onClose([this]() {
    DBUGLN("MQTT connection closed");
    if (_error_time + 100 < millis()) { // Avoid double event if error just occurred
        this->onMqttDisconnect(-1, "CLOSED");
    }
  });
}

unsigned long Mqtt::loop(MicroTasks::WakeReason reason) {
  Profile_Start(Mqtt_loop);

  // Handle MQTT restart requests
  if (_mqttRestartTime > 0 && (long)(millis() - _mqttRestartTime) > 0) {
    _mqttRestartTime = 0;
    if (_mqttclient.connected()) {
      DBUGLN("Disconnecting MQTT for restart");
      _mqttclient.disconnect(); // Async: will fire onClose -> onMqttDisconnect
    }
    // Don't clear _connecting or zero the backoff here. If we're mid-connect,
    // the disconnect() above will cascade through onClose -> onMqttDisconnect,
    // which clears _connecting. If we weren't connected, we weren't connecting
    // either, and the normal reconnect path handles it.
    _nextMqttReconnectAttempt = 0; // Allow immediate reconnect after teardown
  }

  // Watchdog: if _connecting has been true for too long, neither onConnect
  // nor onError/onClose fired. Force-reset so the reconnect path can retry.
  // This handles Mongoose bugs, network black holes, and half-closed states.
  if (_connecting && (long)(millis() - _connectingSince) > (long)CONNECTING_TIMEOUT_MS) {
    DEBUG.printf("MQTT: connecting watchdog expired after %lus, forcing reset\r\n",
                (unsigned long)(CONNECTING_TIMEOUT_MS / 1000));
    _connecting = false;
    // Treat same as a disconnect — normal reconnect path will retry
  }

#if MG_ENABLE_IPV6
  // Handle deferred IPv6 upgrade: if IPv6 arrived while mid-connect, the
  // upgrade was deferred until the in-flight connection completed. Now
  // that we're idle, proceed with the upgrade.
  if (_pendingRestartForIPv6 && !_connecting) {
    _pendingRestartForIPv6 = false;
    // Cancel if IPv6 was lost between the event and the deferred restart.
    // A missing global address means the network changed and the upgrade
    // would needlessly tear down a working IPv4 connection.
    if (!net.hasGlobalIPv6()) {
      DBUGLN("MQTT: deferred IPv6 upgrade cancelled — IPv6 no longer available");
    } else if (_mqttclient.connected() && !isConnectedViaIPv6()) {
      DBUGLN("MQTT: deferred IPv6 upgrade proceeding");
      restartConnection();
    } else if (!_mqttclient.connected()) {
      DBUGLN("MQTT: deferred IPv6 upgrade — not connected, allowing immediate reconnect");
      _nextMqttReconnectAttempt = 0;
    }
  }

  // IPv6 global address transition: upgrade to IPv6 if currently on IPv4.
  // This is MQTT's own policy — net_manager fires the event, each service
  // decides what to do. Re-check hasGlobalIPv6() because the state may
  // have changed between trigger and this loop iteration.
  //
  // Handles three states:
  //  - Connected over IPv4: teardown and reconnect with fresh AAAA-first resolve
  //  - Mid-connect over IPv4: defer upgrade until current connect completes
  //  - Disconnected: next reconnect attempt will do fresh resolve
  if (_ipv6GlobalListener.IsTriggered() && net.hasGlobalIPv6()
      && !isConnectedViaIPv6()) {
    // Invalidate DNS cache so attemptConnection() does a fresh AAAA-first
    // resolve instead of reusing the stale IPv4 cached result.
    // Also reset IPv6 suppression — a fresh global address is a strong
    // signal that the network changed and IPv6 deserves another chance.
    _resolvedHost = "";
    _resolvedAt = 0;
    _resolvedIsIPv6 = false;
    _ipv6FailCount = 0;
    _ipv6SuppressedUntil = 0;
    if (isConnected()) {
      DEBUG.printf("MQTT: upgrading connection to IPv6\r\n");
      restartConnection();
    } else if (_connecting) {
      DEBUG.printf("MQTT: deferring IPv6 upgrade until current connect completes\r\n");
      _pendingRestartForIPv6 = true;
    } else {
      // Not connected and not connecting — next attempt will resolve fresh
      _nextMqttReconnectAttempt = 0;
    }
  }
#endif

  // Manage connection state
  if (net.isConnected() && config_mqtt_enabled() && !_mqttclient.connected() && !_connecting) {
    unsigned long now = millis();
    if ((long)(now - _nextMqttReconnectAttempt) > 0) {
      _nextMqttReconnectAttempt = now + MQTT_CONNECT_TIMEOUT;
      attemptConnection();
    }
  }

  // If connected, perform periodic checks
  if (_mqttclient.connected()) {
    if (millis() - _loop_timer > MQTT_LOOP_INTERVAL) {
      _loop_timer = millis();
      checkAndPublishUpdates();
    }
  }

  Profile_End(Mqtt_loop, 5);
  //return config_mqtt_enabled() ? MQTT_LOOP_INTERVAL : MicroTask.Infinate;
  return MQTT_LOOP_INTERVAL;
}

void Mqtt::attemptConnection() {
  if (!config_mqtt_enabled() || _connecting || _mqttclient.connected()) {
    return;
  }
  _connecting = true;
  _connectingSince = millis();
  DBUGF("MQTT attempting connection... (%s)\n", net.isConnected() ? "connected" : "not connected");

  String mqtt_host = mqtt_server + ":" + String(mqtt_port);
  bool resolved_ipv6 = false;

#if MG_ENABLE_IPV6
  // Pre-resolve hostname via LwIP's getaddrinfo() instead of using
  // Mongoose's built-in DNS resolver (which only queries A records).
  // This is an IPv6-preferred Mongoose DNS workaround, not full RFC 8305.
  //
  // LwIP's getaddrinfo(AF_INET6) may return IPv4-mapped addresses
  // (::ffff:x.x.x.x) for dual-stack hosts instead of real AAAA records.
  // Two-step resolution with IPv4-mapped filtering handles this.
  //
  // If IPv6 TCP connect fails repeatedly, we suppress AAAA queries for a
  // cooldown period to avoid pinning MQTT to a broken IPv6 path.
  //
  // Optimization: skip DNS entirely if mqtt_server is already an IP literal.
  // Also cache DNS results for up to 5 minutes to avoid blocking the
  // event loop with repeated getaddrinfo() calls on every reconnect.

  // Fast path: if mqtt_server is already an IP literal, skip DNS entirely.
  // IPv4 literal: "192.168.1.1"  IPv6 literal: "2001:db8::1" or "::1"
  bool server_is_ip_literal = false;
  {
    // Simple heuristic: contains only hex digits, dots, colons, and no alpha
    // (hostname must have at least one letter to be a DNS name)
    const char *p = mqtt_server.c_str();
    server_is_ip_literal = true;
    while (*p) {
      if (isalpha(*p)) { server_is_ip_literal = false; break; }
      p++;
    }
  }

  if (server_is_ip_literal) {
    // Server is already an IP literal — no DNS needed. Only need to check
    // if it's IPv6 so we can wrap in brackets for Mongoose.
    mqtt_host = mqtt_server + ":" + String(mqtt_port);
    if (mqtt_server.indexOf(':') >= 0) {
      // IPv6 literal — wrap in brackets for Mongoose URL format
      mqtt_host = "[" + mqtt_server + "]:" + String(mqtt_port);
      resolved_ipv6 = true;
      DEBUG.printf("MQTT: server is IPv6 literal, skipping DNS\r\n");
    } else {
      DEBUG.printf("MQTT: server is IPv4 literal, skipping DNS\r\n");
    }
  } else {
    // Server is a hostname — resolve via DNS.
    // Use cached result if fresh (within TTL) and address family unchanged.
    unsigned long now = millis();
    bool cache_valid = (_resolvedHost.length() > 0) &&
                       ((long)(now - _resolvedAt) >= 0) &&
                       ((long)(now - _resolvedAt) < (long)DNS_CACHE_TTL_MS);

    if (cache_valid) {
      // Use cached resolution
      mqtt_host = _resolvedHost + ":" + String(mqtt_port);
      if (_resolvedIsIPv6) {
        mqtt_host = "[" + _resolvedHost + "]:" + String(mqtt_port);
      }
      resolved_ipv6 = _resolvedIsIPv6;
      DEBUG.printf("MQTT: using cached %s -> %s\r\n", mqtt_server.c_str(), _resolvedHost.c_str());
    } else {
      // Cache expired or empty — resolve fresh.
      // NOTE: getaddrinfo() is synchronous and blocks the event loop.
      // LwIP default DNS timeout is ~14s with retries, worst case ~28s
      // for two sequential lookups. The cache mitigates this on reconnects.
      bool ipv6_suppressed = (now < _ipv6SuppressedUntil);
      // Snapshot global IPv6 — local copy avoids cross-task String race
      // (net_manager writes on Arduino event task, we read on MicroTasks loop,
      //  both on core 1, so the window is tiny but we copy to be safe)
      String ipv6_global = net.getIpv6Global();

      if (ipv6_global.length() > 0 && !ipv6_suppressed) {
        // Step 1: Try IPv6 (AAAA record)
        struct addrinfo hints, *result = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        int rc = getaddrinfo(mqtt_server.c_str(), NULL, &hints, &result);
        if (rc == 0) {
          // Iterate linked list for first real (non-IPv4-mapped) AAAA address.
          // LwIP may return IPv4-mapped addresses for dual-stack hosts.
          for (struct addrinfo *rp = result; rp != NULL && !resolved_ipv6; rp = rp->ai_next) {
            if (rp->ai_family != AF_INET6 || rp->ai_addr == NULL) continue;
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)rp->ai_addr;
            if (ip6_addr_isipv4mappedipv6((ip6_addr_t *)&s6->sin6_addr)) continue;
            char addrstr[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &s6->sin6_addr, addrstr, sizeof(addrstr));
            mqtt_host = "[" + String(addrstr) + "]:" + String(mqtt_port);
            DEBUG.printf("MQTT resolved %s -> IPv6 %s\r\n", mqtt_server.c_str(), addrstr);
            resolved_ipv6 = true;
            _resolvedHost = String(addrstr);
            _resolvedIsIPv6 = true;
            _resolvedAt = now;
          }
          if (!resolved_ipv6) {
            DEBUG.printf("MQTT: getaddrinfo(AF_INET6) returned only IPv4-mapped for %s\r\n", mqtt_server.c_str());
          }
          freeaddrinfo(result);
        }
      } else if (ipv6_suppressed) {
        DEBUG.printf("MQTT: IPv6 suppressed for %lus (fail count: %u)\r\n",
                    (unsigned long)((_ipv6SuppressedUntil - now) / 1000), _ipv6FailCount);
      }

      if (!resolved_ipv6) {
        // Step 2: Fall back to IPv4
        struct addrinfo hints, *result = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int rc = getaddrinfo(mqtt_server.c_str(), NULL, &hints, &result);
        if (rc == 0 && result != NULL) {
          struct sockaddr_in *s4 = (struct sockaddr_in *)result->ai_addr;
          char addrstr[INET6_ADDRSTRLEN];
          inet_ntop(AF_INET, &s4->sin_addr, addrstr, sizeof(addrstr));
          mqtt_host = String(addrstr) + ":" + String(mqtt_port);
          DEBUG.printf("MQTT resolved %s -> IPv4 %s\r\n", mqtt_server.c_str(), addrstr);
          // Cache the result
          _resolvedHost = String(addrstr);
          _resolvedIsIPv6 = false;
          _resolvedAt = now;
          freeaddrinfo(result);
        } else {
          DEBUG.printf("MQTT getaddrinfo(%s) IPv4 failed: %d\r\n", mqtt_server.c_str(), rc);
          // Invalidate cache on failure
          _resolvedHost = "";
          _resolvedAt = 0;
        }
      }
    }
  }

  _lastAttemptWasIPv6 = resolved_ipv6;
#endif

  DEBUG.printf("MQTT Connecting to %s://%s\r\n", MQTT_MQTT == config_mqtt_protocol() ? "mqtt" : "mqtts", mqtt_host.c_str());

  DynamicJsonDocument willDoc(JSON_OBJECT_SIZE(3) + 60);
  willDoc["state"] = "disconnected";
  willDoc["id"] = ESPAL.getLongId();
  willDoc["name"] = esp_hostname;
  _lastWill = "";
  serializeJson(willDoc, _lastWill);

  if (!config_mqtt_reject_unauthorized()) {
    DEBUG.println("WARNING: Certificate verification disabled");
  }

  _mqttclient.setCredentials(mqtt_user, mqtt_pass);
  _mqttclient.setLastWillAndTestimment(mqtt_announce_topic, _lastWill, true);
  _mqttclient.setRejectUnauthorized(config_mqtt_reject_unauthorized());

#if MG_ENABLE_IPV6
  // When connecting via a pre-resolved IP literal, Mongoose cannot extract
  // the hostname for TLS SNI and certificate verification. Pass the original
  // hostname explicitly so MQTTS works correctly with virtual-hosted brokers.
  _mqttclient.setTlsServerName(mqtt_server.c_str());
#endif

  if (mqtt_certificate_id != "") {
    uint64_t cert_id = std::stoull(mqtt_certificate_id.c_str(), nullptr, 16);
    const char *cert = certs.getCertificate(cert_id);
    const char *key = certs.getKey(cert_id);
    if (NULL != cert && NULL != key) {
      _mqttclient.setCertificate(cert, key);
    }
  }

  _connecting = _mqttclient.connect((MongooseMqttProtocol)config_mqtt_protocol(), mqtt_host, esp_hostname, [this]() {
    this->onMqttConnect();
  });

  if(!_connecting) { // if connect call itself failed immediately
      DBUGLN("MQTT immediate connection attempt failed.");
      // onError or onClose should handle the detailed reason if it gets to that stage.
      // If connect() returns false, it means it couldn't even start the attempt.
      onMqttDisconnect(-100, "Initial connection failed"); // Custom error
  }
}

void Mqtt::onMqttConnect() {
  DBUGLN("MQTT connected");
  _connecting = false;
  _nextMqttReconnectAttempt = 0; // Reset reconnect timer

#if MG_ENABLE_IPV6
  // Successful connect resets IPv6 failure tracking
  if (_lastAttemptWasIPv6) {
    DEBUG.println("MQTT connected over IPv6");
    _ipv6FailCount = 0;
    _ipv6SuppressedUntil = 0;
  }
  _connectedViaIPv6 = _lastAttemptWasIPv6;
#endif

  DynamicJsonDocument doc(JSON_OBJECT_SIZE(5) + 200);
  doc["state"] = "connected";
  doc["id"] = ESPAL.getLongId();
  doc["name"] = esp_hostname;
  doc["mqtt"] = mqtt_topic;
  doc["http"] = "http://" + net.getIp() + "/";

  String announce = "";
  serializeJson(doc, announce);
  _mqttclient.publish(mqtt_announce_topic, announce, true);

  doc.clear();
  doc["mqtt_connected"] = 1;
  event_send(doc);

  subscribeTopics();
  publishInitialState(); // Publish current states like config, claims, etc.
}

void Mqtt::onMqttDisconnect(int err, const char *reason) {
  DBUGLN("MQTT disconnected");
  _connecting = false;
  // _nextMqttReconnectAttempt is handled by the main loop to retry.

#if MG_ENABLE_IPV6
  // Track IPv6 connection failures. If IPv6 fails repeatedly,
  // suppress AAAA queries for a cooldown to fall back to IPv4.
  if (_lastAttemptWasIPv6) {
    _ipv6FailCount++;
    DEBUG.printf("MQTT: IPv6 connect failure #%u\r\n", _ipv6FailCount);
    if (_ipv6FailCount >= IPV6_FAIL_THRESHOLD) {
      _ipv6SuppressedUntil = millis() + IPV6_SUPPRESS_MS;
      DEBUG.printf("MQTT: IPv6 suppressed for %lus after %u failures\r\n",
                    (unsigned long)(IPV6_SUPPRESS_MS / 1000), _ipv6FailCount);
    }
    // Invalidate DNS cache on IPv6 failure — address may be stale/broken
    _resolvedHost = "";
    _resolvedAt = 0;
  }
  _connectedViaIPv6 = false;
#endif

  DynamicJsonDocument doc(JSON_OBJECT_SIZE(3) + 70);
  doc["mqtt_connected"] = 0;
  doc["mqtt_close_code"] = err;
  doc["mqtt_close_reason"] = reason;
  event_send(doc);
}

void Mqtt::subscribeTopics() {
  String mqtt_sub_topic;

  // RAPI commands
  mqtt_sub_topic = mqtt_topic + "/rapi/in/#";
  _mqttclient.subscribe(mqtt_sub_topic);
  yield();

  // Divert mode related
  if (config_divert_enabled()) {
    if (divert_type == DIVERT_TYPE_SOLAR && mqtt_solar != "") {
      _mqttclient.subscribe(mqtt_solar); yield();
    }
    if (divert_type == DIVERT_TYPE_GRID && mqtt_grid_ie != "") {
      _mqttclient.subscribe(mqtt_grid_ie); yield();
    }
  }

  // Current shaper related
  if (config_current_shaper_enabled()) {
    if (mqtt_live_pwr != "" && mqtt_live_pwr != mqtt_grid_ie) {
      _mqttclient.subscribe(mqtt_live_pwr); yield();
    }
  }

  // Vehicle data
  if (mqtt_vehicle_soc != "") { _mqttclient.subscribe(mqtt_vehicle_soc); yield(); }
  if (mqtt_vehicle_range != "") { _mqttclient.subscribe(mqtt_vehicle_range); yield(); }
  if (mqtt_vehicle_eta != "") { _mqttclient.subscribe(mqtt_vehicle_eta); yield(); }
  if (mqtt_vrms != "") { _mqttclient.subscribe(mqtt_vrms); yield(); }

  // Settable topics
  _mqttclient.subscribe(mqtt_topic + "/divertmode/set"); yield();
  _mqttclient.subscribe(mqtt_topic + "/shaper/set"); yield();
  _mqttclient.subscribe(mqtt_topic + "/override/set"); yield();
  _mqttclient.subscribe(mqtt_topic + "/claim/set"); yield();
  _mqttclient.subscribe(mqtt_topic + "/schedule/set"); yield();
  _mqttclient.subscribe(mqtt_topic + "/schedule/clear"); yield();
  _mqttclient.subscribe(mqtt_topic + "/limit/set"); yield();
  _mqttclient.subscribe(mqtt_topic + "/config/set"); yield();
  _mqttclient.subscribe(mqtt_topic + "/restart"); yield();

  DBUGLN("MQTT Subscriptions complete");
}

void Mqtt::publishInitialState() {
    DBUGLN("MQTT Publishing initial state");
    // Force publish everything on new connection
    _configVersion = INITIAL_CONFIG_VERSION -1;
    _claimsVersion = evse.getClaimsVersion() == 0 ? 1 : evse.getClaimsVersion() -1;
    _overrideVersion = manual.getVersion() == 0 ? 1 : manual.getVersion() -1;
    _scheduleVersion = scheduler.getVersion() == 0 ? 1 : scheduler.getVersion() -1;
    _limitVersion = limit.getVersion() == 0 ? 1 : limit.getVersion() -1;

    checkAndPublishUpdates(); // This will now publish everything
}


void Mqtt::checkAndPublishUpdates() {
  if (!isConnected()) return;

  if (_claimsVersion != _evse->getClaimsVersion()) {
    publishClaim();
    DBUGLN("Claims has changed, publishing to MQTT");
    _claimsVersion = _evse->getClaimsVersion();
  }

  if (_overrideVersion != manual.getVersion()) {
    publishOverride();
    DBUGLN("Override has changed, publishing to MQTT");
    _overrideVersion = manual.getVersion();
  }

  if (_scheduleVersion != scheduler.getVersion()) {
    publishSchedule();
    DBUGLN("Schedule has changed, publishing to MQTT");
    _scheduleVersion = scheduler.getVersion();
  }
  if (_limitVersion != limit.getVersion()) {
    publishLimit();
    DBUGLN("Limit has changed, publishing to MQTT");
    _limitVersion = limit.getVersion();
  }

  if (_configVersion != config_version()) {
    publishConfig(); // publishConfig now returns void
    DBUGLN("Config has changed, publishing to MQTT");
    _configVersion = config_version();
  }
}

void Mqtt::handleMqttMessage(MongooseString topic, MongooseString payload) {
  String topic_string = topic.toString();
  String payload_str = payload.toString();

  DBUGLN("Mqtt received:");
  DBUGLN("Topic: " + topic_string);
  DBUGLN("Payload: " + payload_str);

  // Logic from old mqttmsg_callback
  if (topic_string == mqtt_solar){
    solar = payload_str.toInt();
    DBUGF("solar:%dW", solar);
    divert.update_state();
    if (shaper.getState()) {
      shaper.shapeCurrent();
    }
  }
  else if (topic_string == mqtt_grid_ie) {
    grid_ie = payload_str.toInt();
    DBUGF("grid:%dW", grid_ie);
    divert.update_state();
    if (mqtt_live_pwr == mqtt_grid_ie) {
      shaper.setLivePwr(grid_ie);
    }
  }
  else if (topic_string == mqtt_live_pwr) {
      shaper.setLivePwr(payload_str.toInt());
      DBUGF("shaper: Live Pwr:%dW", shaper.getLivePwr());
  }
  else if (topic_string == mqtt_vrms) {
    double volts = payload_str.toFloat();
    DBUGF("voltage:%.1f", volts);
    _evse->setVoltage(volts);
  }
  else if (topic_string == mqtt_vehicle_soc && vehicle_data_src == VEHICLE_DATA_SRC_MQTT) {
    int vehicle_soc = payload_str.toInt();
    _evse->setVehicleStateOfCharge(vehicle_soc);
    StaticJsonDocument<128> event; event["battery_level"] = vehicle_soc; event_send(event);
  }
  else if (topic_string == mqtt_vehicle_range && vehicle_data_src == VEHICLE_DATA_SRC_MQTT) {
    int vehicle_range = payload_str.toInt();
    _evse->setVehicleRange(vehicle_range);
    StaticJsonDocument<128> event; event["battery_range"] = vehicle_range; event_send(event);
  }
  else if (topic_string == mqtt_vehicle_eta && vehicle_data_src == VEHICLE_DATA_SRC_MQTT) {
    int vehicle_eta = payload_str.toInt();
    _evse->setVehicleEta(vehicle_eta);
    StaticJsonDocument<128> event; event["time_to_full_charge"] = vehicle_eta; event_send(event);
  }
  else if (topic_string == mqtt_topic + "/divertmode/set") {
    byte newdivert = payload_str.toInt();
    if ((newdivert==1) || (newdivert==2)) {
      divert.setMode((DivertMode)newdivert);
    }
  }
  else if (topic_string == mqtt_topic + "/shaper/set") {
    byte newshaper = payload_str.toInt();
    if (newshaper==0) shaper.setState(false); else if (newshaper==1) shaper.setState(true);
  }
  else if (topic_string == mqtt_topic + "/override/set") {
    if (payload_str.equals("clear")) {
      if (manual.release()) {
        _override_props.clear();
        publishOverride();
      }
    } else if (payload_str.equals("toggle")) {
      if (manual.toggle()) publishOverride();
    } else if (_override_props.deserialize(payload_str)) {
      setClaim(true, _override_props);
    }
  }
  else if (topic_string == mqtt_topic + "/claim/set") {
    if (payload_str.equals("release")) {
      if (_evse->release(EvseClient_OpenEVSE_MQTT)) {
        _claim_props.clear();
        publishClaim();
      }
    } else if (_claim_props.deserialize(payload_str)) {
      setClaim(false, _claim_props);
    }
  }
  else if (topic_string == mqtt_topic + "/schedule/set") {
    setSchedule(payload_str); // Calls scheduler.deserialize and publishSchedule
  }
  else if (topic_string == mqtt_topic + "/schedule/clear") {
    clearSchedule(payload_str.toInt()); // Calls scheduler.removeEvent and publishSchedule
  }
  else if (topic_string == mqtt_topic + "/limit/set") {
    if (payload_str.equals("clear")) {
      limit.clear();
      publishLimit(); // Need to ensure limit publishes its state.
    } else if (_limit_props.deserialize(payload_str)) {
      setLimit(_limit_props);
    }
  }
  else if (topic_string == mqtt_topic + "/config/set") {
    DynamicJsonDocument doc(4096); // Sufficiently large buffer
    DeserializationError error = deserializeJson(doc, payload_str);
    if(!error) {
      if(config_deserialize(doc)) {
        config_commit(false);
        DBUGLN("Config updated via MQTT");
        // publishConfig() will be called by checkAndPublishUpdates due to configVersion change
      }
    }
  }
  else if (topic_string == mqtt_topic + "/restart") {
    // This logic can reuse the existing mqtt_restart_device logic by making it a static helper or part of this class
    const size_t capacity = JSON_OBJECT_SIZE(1) + 16;
    DynamicJsonDocument doc(capacity);
    DeserializationError error = deserializeJson(doc, payload_str);
    if(!error && doc.containsKey("device")){
        if (strcmp(doc["device"], "gateway") == 0 ) restart_system();
        else if (strcmp(doc["device"], "evse") == 0) _evse->restartEvse();
    }
  }
  else { // RAPI commands
    int rapi_character_index = topic_string.indexOf('$');
    if (rapi_character_index > 1) {
      String cmd = topic_string.substring(rapi_character_index);
      if (payload.length() > 0) cmd += " " + payload_str;

      if(!_evse->isRapiCommandBlocked(cmd)) { // Use EvseManager instance
        rapiSender.sendCmd(cmd, [this](int ret) { // Capture 'this' only
          if (RAPI_RESPONSE_OK == ret || RAPI_RESPONSE_NK == ret) {
            String rapiString = rapiSender.getResponse();
            String mqtt_sub_topic = mqtt_topic + "/rapi/out";
            _mqttclient.publish(mqtt_sub_topic, rapiString);
          }
        });
      }
    }
  }
}

// --- Public Interface Methods ---
bool Mqtt::isConnected() {
  return _mqttclient.connected();
}

void Mqtt::restartConnection() {
  DBUGLN("MQTT restart requested");
  _mqttRestartTime = millis() + 50; // Schedule restart in the near future in loop
  MicroTask.wakeTask(this); // Ensure loop runs to handle the restart
}

void Mqtt::publishData(JsonDocument &data) {
  if (!config_mqtt_enabled() || !_mqttclient.connected()) {
    return;
  }
  JsonObject root = data.as<JsonObject>();
  for (JsonPair kv : root) {
    String topic = mqtt_topic + "/" + kv.key().c_str();
    String val = kv.value().as<String>(); // Consider non-string values too if needed
    _mqttclient.publish(topic, val, config_mqtt_retained());
  }
}

// Specific publish methods
void Mqtt::publishConfig() {
  if (!isConnected() || _evse->getEvseState() == OPENEVSE_STATE_STARTING) {
    return;
  }
  const size_t capacity = JSON_OBJECT_SIZE(128) + 1024;
  DynamicJsonDocument doc(capacity);
  config_serialize(doc, true, false, true);

  String fulltopic = mqtt_topic + "/config";
  String payload;
  serializeJson(doc, payload);
  _mqttclient.publish(fulltopic, payload, true); // Config usually retained

  if(config_version() == INITIAL_CONFIG_VERSION) {
      String versionTopic = mqtt_topic + "/config_version";
      String versionPayload = String(config_version());
      _mqttclient.publish(versionTopic, versionPayload, true);
  }
}

void Mqtt::setClaim(bool override, EvseProperties &props) {
  if (override) {
    if (manual.claim(props)) {
      publishOverride();
    }
  } else {
    if (_evse->claim(EvseClient_OpenEVSE_MQTT, EvseManager_Priority_MQTT, props)) {
      publishClaim();
    }
  }
}

void Mqtt::publishClaim() {
  if (!isConnected()) return;
  const size_t capacity = JSON_OBJECT_SIZE(7) + 1024;
  DynamicJsonDocument claimdata(capacity);
  if (_evse->clientHasClaim(EvseClient_OpenEVSE_MQTT)) {
    _evse->serializeClaim(claimdata, EvseClient_OpenEVSE_MQTT);
  } else {
    claimdata["state"] = "null";
  }
  String fulltopic = mqtt_topic + "/claim";
  String payload;
  serializeJson(claimdata, payload);
  _mqttclient.publish(fulltopic, payload, true); // Claims usually retained
}

void Mqtt::publishOverride() {
  if (!isConnected()) return;
  const size_t capacity = JSON_OBJECT_SIZE(7) + 1024;
  DynamicJsonDocument override_data(capacity);
  if (_evse->clientHasClaim(EvseClient_OpenEVSE_Manual) || manual.isActive()) {
    EvseProperties props = _evse->getClaimProperties(EvseClient_OpenEVSE_Manual);
    props.serialize(override_data);
  } else {
    override_data["state"] = "null";
  }
  String fulltopic = mqtt_topic + "/override";
  String payload;
  serializeJson(override_data, payload);
  _mqttclient.publish(fulltopic, payload, true); // Overrides usually retained
}

void Mqtt::setSchedule(String schedulePayload) {
  scheduler.deserialize(schedulePayload);
  publishSchedule();
}

void Mqtt::clearSchedule(uint32_t eventId) {
  scheduler.removeEvent(eventId);
  publishSchedule();
}

void Mqtt::publishSchedule() {
  if (!isConnected()) return;
  const size_t capacity = JSON_OBJECT_SIZE(40) + 2048;
  DynamicJsonDocument schedule_data(capacity);
  if (scheduler.serialize(schedule_data)) {
    String fulltopic = mqtt_topic + "/schedule";
    String payload;
    serializeJson(schedule_data, payload);
    _mqttclient.publish(fulltopic, payload, true); // Schedule usually retained
  }
}

void Mqtt::setLimit(LimitProperties &limitProps) {
  limit.set(limitProps);
  publishLimit(); // This will also update the version
}

void Mqtt::publishLimit() {
  if (!isConnected()) return;
  LimitProperties currentLimitProps = limit.get();
  const size_t capacity = JSON_OBJECT_SIZE(3) + 512;
  DynamicJsonDocument limit_data(capacity);
  if (currentLimitProps.serialize(limit_data)) {
    String fulltopic = mqtt_topic + "/limit";
    String payload;
    serializeJson(limit_data, payload);
    _mqttclient.publish(fulltopic, payload, true); // Limits usually retained
  }
}

// --- Notification methods ---
void Mqtt::notifyEvseClaimChanged() {
    if (_claimsVersion != _evse->getClaimsVersion()) {
        _claimsVersion = _evse->getClaimsVersion(); // Update internal version
        if (isConnected()) publishClaim();
    }
}

void Mqtt::notifyManualOverrideChanged() {
     if (_overrideVersion != manual.getVersion()) {
        _overrideVersion = manual.getVersion();
        if (isConnected()) publishOverride();
    }
}

void Mqtt::notifyScheduleChanged() {
    if (_scheduleVersion != scheduler.getVersion()) {
        _scheduleVersion = scheduler.getVersion();
        if (isConnected()) publishSchedule();
    }
}

void Mqtt::notifyLimitChanged() {
    if (_limitVersion != limit.getVersion()) {
        _limitVersion = limit.getVersion();
        if (isConnected()) publishLimit();
    }
}

void Mqtt::notifyConfigChanged() {
    if (_configVersion != config_version()) {
        _configVersion = config_version();
        if (isConnected()) publishConfig();
    }
}
