/* nv_hasp_mqtt.cpp — openHASP's MQTT client interface (src/mqtt/hasp_mqtt.h) over the NucleoOS
 * system MQTT connection (ABI 12: nv_mqtt_sub / nv_mqtt_pub / nv_mqtt_recv).
 *
 * It follows upstream's PC client (src/mqtt/hasp_mqtt_paho_async.cpp) topic for topic:
 *   subscribe  hasp/<node>/command/#   hasp/<node>/config/#
 *              hasp/<group>/command/#  hasp/<group>/config/#   hasp/broadcast/command/#
 *   publish    hasp/<node>/state/<subtopic>, hasp/discovery/<hwid>, hasp/<node>/LWT
 * Incoming messages go through dispatch_defer_command() exactly like upstream's, and the main
 * loop runs them (dispatch_process_deferred() in main.cpp's loop()).
 *
 * Differences, all forced by the platform:
 *  - no broker, user or password here: the OS connection is configured in Settings > Home, so the
 *    "host/port/user/pass" config keys are kept (for config.json round-trips) but unused;
 *  - no MQTT will: the connection belongs to the OS, which has its own LWT. The app publishes
 *    LWT "online" (retained) when it starts or the connection comes back, and "offline" when it
 *    exits (nv_hasp_main.cpp);
 *  - "connected" means the OS accepted our last publish (nv_mqtt_pub returns -2 while offline).
 */
#include "hasplib.h"
#include "hasp_debug.h"
#include "mqtt/hasp_mqtt.h"
#include "hasp/hasp_dispatch.h"

#include "nucleo_sdk.h"

#include <string>
#include <string.h>

#if !HASP_USE_CONFIG
const char FP_CONFIG_HOST[] PROGMEM  = "host";
const char FP_CONFIG_PORT[] PROGMEM  = "port";
const char FP_CONFIG_NAME[] PROGMEM  = "name";
const char FP_CONFIG_USER[] PROGMEM  = "user";
const char FP_CONFIG_PASS[] PROGMEM  = "pass";
const char FP_CONFIG_GROUP[] PROGMEM = "group";
#endif

// hasp_dispatch.cpp (not in its header)
void dispatch_queue_discovery(const char*, const char*, uint8_t source);

/* The OS queues up to 8 KB per message (NV_MQTT_APP_MSG_MAX). */
#define NV_MQTT_PAYLOAD_MAX 8192

std::string mqttNodeTopic;
std::string mqttGroupTopic;
std::string mqttLwtTopic;
bool mqttEnabled        = false;
bool mqttHAautodiscover = false;
uint32_t mqttPublishCount;
uint32_t mqttReceiveCount;
uint32_t mqttFailedCount;

std::string mqttServer    = "NucleoOS";
std::string mqttUsername  = "";
std::string mqttPassword  = "";
std::string mqttGroupName = MQTT_GROUPNAME;
uint16_t mqttPort         = MQTT_PORT;

static bool mqttSubscribed = false; // filters registered with the OS
static bool mqttConnected  = false; // last publish went through
static bool mqttWasOnline  = false; // "online" + scripts already done for this connection
static int32_t mqttLastErr = 0;

static void mqtt_subscribe(const std::string& topic)
{
    int32_t rc = nv_mqtt_sub(topic.c_str());
    mqttLastErr = rc;
    if(rc != 0) {
        LOG_WARNING(TAG_MQTT, D_BULLET D_MQTT_NOT_SUBSCRIBED " (%d)", topic.c_str(), (int)rc);
    } else {
        LOG_VERBOSE(TAG_MQTT, D_BULLET D_MQTT_SUBSCRIBED, topic.c_str());
    }
}

// Same routing as upstream's mqtt_message_cb (hasp_mqtt_paho_async.cpp)
static void mqtt_message_cb(char* topic, char* payload, size_t length)
{
    mqttReceiveCount++;
    payload[length] = '\0';
    LOG_TRACE(TAG_MQTT_RCV, F("%s = %s"), topic, payload);

    if(topic == strstr(topic, mqttNodeTopic.c_str())) { // node topic
        topic += mqttNodeTopic.length();

    } else if(topic == strstr(topic, mqttGroupTopic.c_str())) { // group topic
        topic += mqttGroupTopic.length();
        dispatch_defer_command(topic, payload);
        return;

#ifdef HASP_USE_BROADCAST
    } else if(topic == strstr_P(topic, PSTR(MQTT_PREFIX "/" MQTT_TOPIC_BROADCAST "/"))) {
        topic += strlen(MQTT_PREFIX "/" MQTT_TOPIC_BROADCAST "/");
        dispatch_defer_command(topic, payload);
        return;
#endif

    } else {
        LOG_ERROR(TAG_MQTT, F(D_MQTT_INVALID_TOPIC));
        return;
    }

    // catch a dangling LWT from a previous connection if it appears
    if(!strcmp_P(topic, PSTR(MQTT_TOPIC_LWT))) {
        if(!strcasecmp_P(payload, PSTR("offline"))) mqttPublish(mqttLwtTopic.c_str(), "online", 6, true);
    } else {
        dispatch_defer_command(topic, payload);
    }
}

/* ===== Local HASP MQTT functions ===== */

int mqttPublish(const char* topic, const char* payload, size_t len, bool retain)
{
    if(!mqttEnabled) return MQTT_ERR_DISABLED;
    if(len > NV_MQTT_PAYLOAD_MAX) {
        mqttFailedCount++;
        LOG_ERROR(TAG_MQTT_PUB, F(D_MQTT_FAILED " '%s' (%u bytes)"), topic, (unsigned)len);
        return MQTT_ERR_PUB_FAIL;
    }
    int32_t rc = nv_mqtt_pub(topic, payload, (uint32_t)len, retain ? 1 : 0);
    if(rc == NV_NET_E_NOTCONF) { // MQTT off in Settings > Home, or the OS is not connected now
        mqttConnected = false;
        mqttFailedCount++;
        return MQTT_ERR_NO_CONN;
    }
    if(rc != 0) {
        mqttFailedCount++;
        LOG_ERROR(TAG_MQTT_PUB, F(D_MQTT_FAILED " '%s' => %s (%d)"), topic, payload, (int)rc);
        return MQTT_ERR_PUB_FAIL;
    }
    mqttConnected = true;
    mqttPublishCount++;
    return MQTT_ERR_OK;
}

/* ===== Public HASP MQTT functions ===== */

bool mqttIsConnected()
{
    return mqttConnected;
}

int mqtt_send_state(const __FlashStringHelper* subtopic, const char* payload, bool retain)
{
    char tmp_topic[mqttNodeTopic.length() + 64];
    snprintf_P(tmp_topic, sizeof(tmp_topic), ("%s" MQTT_TOPIC_STATE "/%s"), mqttNodeTopic.c_str(), subtopic);
    return mqttPublish(tmp_topic, payload, strlen(payload), retain);
}

int mqtt_send_discovery(const char* payload, size_t len)
{
    char tmp_topic[128];
    snprintf_P(tmp_topic, sizeof(tmp_topic), PSTR(MQTT_PREFIX "/" MQTT_TOPIC_DISCOVERY "/%s"),
               haspDevice.get_hardware_id());
    return mqttPublish(tmp_topic, payload, len, false);
}

/* Upstream's onConnect: LWT online, then the PC-only online.cmd script. Also queues a discovery
 * message so the HA integration finds the plate without waiting for its own broadcast. */
static void mqtt_on_online()
{
    LOG_INFO(TAG_MQTT, D_MQTT_CONNECTED, mqttServer.c_str(), haspDevice.get_hostname());
    dispatch_run_script(NULL, "L:/online.cmd", TAG_HASP);
    dispatch_queue_discovery(NULL, NULL, TAG_MQTT);
    mqttWasOnline = true;
}

void mqttStart()
{
    if(!mqttSubscribed) {
        std::string topic;
        mqttLastErr = 0;
        topic = mqttGroupTopic + MQTT_TOPIC_COMMAND "/#";
        mqtt_subscribe(topic);
        if(mqttLastErr != 0) {
            // NOTCONF: MQTT is off in Settings > Home; PERM/ARG: permission or filter refused.
            // Nothing is subscribed yet, so just try again in 5 s.
            if(mqttLastErr == NV_NET_E_NOTCONF) LOG_WARNING(TAG_MQTT, "MQTT is off in Settings > Home");
            mqttEnabled = false;
            return;
        }
        topic = mqttNodeTopic + MQTT_TOPIC_COMMAND "/#";
        mqtt_subscribe(topic);
        topic = mqttGroupTopic + "config/#";
        mqtt_subscribe(topic);
        topic = mqttNodeTopic + "config/#";
        mqtt_subscribe(topic);
#if defined(HASP_USE_CUSTOM) && HASP_USE_CUSTOM > 0
        topic = mqttGroupTopic + MQTT_TOPIC_CUSTOM "/#";
        mqtt_subscribe(topic);
        topic = mqttNodeTopic + MQTT_TOPIC_CUSTOM "/#";
        mqtt_subscribe(topic);
#endif
#ifdef HASP_USE_BROADCAST
        topic = MQTT_PREFIX "/" MQTT_TOPIC_BROADCAST "/" MQTT_TOPIC_COMMAND "/#";
        mqtt_subscribe(topic);
#endif
        mqttSubscribed = true;
        mqttEnabled    = true;
    }

    if(mqttPublish(mqttLwtTopic.c_str(), "online", 6, true) == MQTT_ERR_OK) {
        mqtt_on_online();
    } else {
        LOG_WARNING(TAG_MQTT, F(D_MQTT_NOT_CONNECTED));
    }
}

void mqttStop()
{
    if(mqttEnabled && mqttConnected) {
        nv_mqtt_pub(mqttLwtTopic.c_str(), "offline", 7, 1);
#if HASP_TARGET_PC
        dispatch_run_script(NULL, "L:/offline.cmd", TAG_HASP);
#endif
    }
    mqttConnected = false;
    mqttWasOnline = false;
}

void mqttSetup()
{
    mqttNodeTopic = MQTT_PREFIX;
    mqttNodeTopic += "/";
    mqttNodeTopic += haspDevice.get_hostname();
    mqttNodeTopic += "/";

    mqttGroupTopic = MQTT_PREFIX;
    mqttGroupTopic += "/";
    mqttGroupTopic += mqttGroupName;
    mqttGroupTopic += "/";

    mqttLwtTopic = mqttNodeTopic;
    mqttLwtTopic += MQTT_TOPIC_LWT;

    mqttStart();
}

/* Called every main-loop pass: drain the OS queue (32 KB in PSRAM, newest dropped when full) so a
 * page push from Home Assistant never overflows it; the commands themselves run later from
 * dispatch_process_deferred(). */
IRAM_ATTR void mqttLoop()
{
    if(!mqttEnabled) return;
    static char topic[256];
    static char payload[NV_MQTT_PAYLOAD_MAX + 1];
    for(int i = 0; i < 256; i++) {
        int32_t n = nv_mqtt_recv(topic, sizeof(topic), payload, NV_MQTT_PAYLOAD_MAX);
        if(n < 0) break;
        if(n > NV_MQTT_PAYLOAD_MAX) {
            mqttFailedCount++;
            LOG_ERROR(TAG_MQTT_RCV, F(D_MQTT_PAYLOAD_TOO_LONG), (uint32_t)n);
            continue;
        }
        mqtt_message_cb(topic, payload, (size_t)n);
    }
}

void mqttEverySecond()
{}

/* Upstream reconnects here; the OS reconnects for us, so this only notices it: retry the
 * subscriptions if MQTT was off, and re-announce "online" once the connection is back. */
void mqttEvery5Seconds(bool wifiIsConnected)
{
    (void)wifiIsConnected;
    if(!mqttEnabled) {
        mqttStart();
        return;
    }
    if(!mqttConnected || !mqttWasOnline) {
        if(mqttPublish(mqttLwtTopic.c_str(), "online", 6, true) == MQTT_ERR_OK && !mqttWasOnline) mqtt_on_online();
    }
    if(!mqttConnected) mqttWasOnline = false;
}

void mqtt_get_info(JsonDocument& doc)
{
    JsonObject info           = doc.createNestedObject(F("MQTT"));
    info[F(D_INFO_SERVER)]    = mqttServer;
    info[F(D_INFO_USERNAME)]  = mqttUsername;
    info[F(D_INFO_CLIENTID)]  = haspDevice.get_hostname();
    info[F(D_INFO_STATUS)]    = mqttIsConnected() ? F(D_SERVICE_CONNECTED) : F(D_SERVICE_DISCONNECTED);
    info[F(D_INFO_RECEIVED)]  = mqttReceiveCount;
    info[F(D_INFO_PUBLISHED)] = mqttPublishCount;
    info[F(D_INFO_FAILED)]    = mqttFailedCount;
}

bool mqttGetConfig(const JsonObject& settings)
{
    bool changed = false;

    if(strcmp(haspDevice.get_hostname(), settings[FPSTR(FP_CONFIG_NAME)].as<String>().c_str()) != 0) changed = true;
    settings[FPSTR(FP_CONFIG_NAME)] = haspDevice.get_hostname();

    if(mqttGroupName != settings[FPSTR(FP_CONFIG_GROUP)].as<String>()) changed = true;
    settings[FPSTR(FP_CONFIG_GROUP)] = mqttGroupName;

    if(changed) configOutput(settings, TAG_MQTT);
    return changed;
}

/* Only "name" (the node: hasp/<name>/...) and "group" matter here; the broker is the OS's. */
bool mqttSetConfig(const JsonObject& settings)
{
    bool changed = false;

    if(!settings[FPSTR(FP_CONFIG_NAME)].isNull()) {
        const char* name = settings[FPSTR(FP_CONFIG_NAME)].as<const char*>();
        if(name && *name) {
            changed |= strcmp(haspDevice.get_hostname(), name) != 0;
            haspDevice.set_hostname(name);
        }
    }

    if(!settings[FPSTR(FP_CONFIG_GROUP)].isNull()) {
        changed |= mqttGroupName != settings[FPSTR(FP_CONFIG_GROUP)].as<std::string>();
        mqttGroupName = settings[FPSTR(FP_CONFIG_GROUP)].as<std::string>();
    }
    if(mqttGroupName.length() == 0) {
        mqttGroupName = MQTT_GROUPNAME;
        changed       = true;
    }
    return changed;
}
