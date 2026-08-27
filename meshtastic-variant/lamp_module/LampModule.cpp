#include "LampModule.h"
#include "MeshService.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "main.h"
#include <Preferences.h>
#include <time.h>

LampModule *lampModule;

// PRIVATE_APP is the portnum range reserved for third-party use, so lamp traffic
// never collides with Meshtastic's own apps and other nodes simply ignore it.
LampModule::LampModule()
    : SinglePortModule("lamp", meshtastic_PortNum_PRIVATE_APP), OSThread("Lamp"),
      strip_(NUM_LEDS, PIN_LED_DATA, NEO_GRBW + NEO_KHZ800)
{
}

// Same 19-byte layout as the standalone firmware: magic, type, counter, payload,
// node id, visual code, flags.
static const uint8_t LAMP_MAGIC = 0xC1;
static const uint8_t LAMP_SCENE = 1;
static const uint8_t LAMP_POWER = 2;
static const uint8_t LAMP_STATE = 3;
static const uint8_t LAMP_COLOUR = 4;
// Not part of colour sync - a per-device touch-threshold set, unicast (never
// broadcast) so tuning one lamp's oversensitivity never touches the other's.
// payload carries the new threshold as raw float bits (4 bytes). Persisted to
// NVS so it survives a reboot; see LampModule::runOnce() and handleReceived().
static const uint8_t LAMP_THRESHOLD = 5;
// Also per-device, also unicast-only, also NVS-persisted: sets a sunrise
// alarm. payload bit-packs enabled(1) | hour(5) | minute(6) | durationMin(20)
// - see LampModule::handleReceived and web/index.html's setAlarm().
static const uint8_t LAMP_ALARM = 6;
// Broadcast, syncs like LAMP_COLOUR/LAMP_SCENE - explicit per-group hues from
// a human dragging handles on a hue bar, not a generated scene. Bit-packed:
// (count-1)(2 bits) | pos0(10 bits) | pos1(10 bits) | pos2(10 bits), each
// position quantized 0..1023 over 0.0..1.0 - see ColourEngine::setGroupHues/
// packedGroups() and web/index.html's setGroupHues().
static const uint8_t LAMP_GROUPS = 7;
static const int LAMP_LEN = 19;

static const char *PREFS_NAMESPACE = "lamp";
static const char *PREFS_THRESHOLD_KEY = "threshold";
static const char *PREFS_ALARM_ON_KEY = "alarmOn";
static const char *PREFS_ALARM_HOUR_KEY = "alarmHour";
static const char *PREFS_ALARM_MIN_KEY = "alarmMin";
static const char *PREFS_ALARM_DUR_KEY = "alarmDur";

// Sunrise ramp endpoints: starts as a dim ember, ends at the same warm white
// the rest of the firmware treats as "home" (see BASE_WARM_WHITE). Both the
// hue AND the brightness move over the ramp, not just one or the other.
static const Rgbw ALARM_START{40, 4, 0, 0};
static const Rgbw ALARM_END{BASE_WARM_WHITE[0], BASE_WARM_WHITE[1], BASE_WARM_WHITE[2], BASE_WARM_WHITE[3]};

// Meshtastic delays ordinary app traffic by seconds. A colour change is a direct
// response to someone touching the lamp, so ask for the highest priority the
// stack offers - it will not reach the standalone firmware's 211 ms, but it is
// the difference between "slow" and "did that even work".
static const meshtastic_MeshPacket_Priority LAMP_PRIORITY = meshtastic_MeshPacket_Priority_HIGH;

// How often to re-announce. A tap is sent once with no retry, so without this a
// single lost packet would leave the lamps disagreeing indefinitely.
// Announcements are cheap but not free, and every one competes with whatever
// the phone is doing. A minute is ample: it is a safety net against a lost
// packet, not the mechanism by which taps propagate.
static const uint32_t STATE_INTERVAL_MS = 60000;

void LampModule::sendState(uint8_t type)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->priority = LAMP_PRIORITY;
    p->want_ack = false;

    uint32_t payload = engine_.isSolid()     ? engine_.packedSolid()
                        : engine_.isGroupHues() ? engine_.packedGroups()
                                                 : engine_.seed();
    uint32_t me = nodeDB->getNodeNum();
    code_ = engine_.visualCode();

    uint8_t *b = p->decoded.payload.bytes;
    b[0] = LAMP_MAGIC;
    b[1] = type;
    memcpy(b + 2, &counter_, 4);
    memcpy(b + 6, &payload, 4);
    memcpy(b + 10, &me, 4);
    memcpy(b + 14, &code_, 4);
    b[18] = engine_.poweredOn() ? 1 : 0;
    p->decoded.payload.size = LAMP_LEN;

    service->sendToMesh(p);
    lastState_ = millis();
    LOG_INFO("Lamp sent type=%d counter=%u code=%08x", type, counter_, code_);
}

void LampModule::localTap()
{
    counter_++;
    owner_ = nodeDB->getNodeNum();
    engine_.applyScene((uint32_t)millis() ^ (owner_ << 7) ^ (uint32_t)random(0x7FFFFFFF));
    sendState(LAMP_SCENE);
}

ProcessMessage LampModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    auto &d = mp.decoded;
    if (d.payload.size != LAMP_LEN || d.payload.bytes[0] != LAMP_MAGIC)
        return ProcessMessage::CONTINUE;   // not ours; let other modules see it

    const uint8_t *b = d.payload.bytes;
    uint8_t type = b[1];
    uint32_t counter, payload, node, code;
    memcpy(&counter, b + 2, 4);
    memcpy(&payload, b + 6, 4);
    memcpy(&node, b + 10, 4);
    memcpy(&code, b + 14, 4);
    bool on = b[18] & 1;

    if (type == LAMP_THRESHOLD) {
        // Deliberately outside the counter/code sync logic below - this is a
        // per-device hardware setting, not shared strip state, and must only
        // ever apply to the device it was actually addressed to.
        if (mp.to != nodeDB->getNodeNum()) return ProcessMessage::STOP;
        float t;
        memcpy(&t, &payload, 4);
        touch_.setThreshold(t);
        Preferences prefs;
        prefs.begin(PREFS_NAMESPACE, false);
        prefs.putFloat(PREFS_THRESHOLD_KEY, touch_.threshold());
        prefs.end();
        LOG_INFO("Touch threshold set to %.2f (from 0x%x, saved)", touch_.threshold(), node);
        return ProcessMessage::STOP;
    }

    if (type == LAMP_ALARM) {
        // Same reasoning as LAMP_THRESHOLD: a personal per-device setting,
        // not shared strip state - a sunrise alarm is deliberately NOT
        // something tapping one lamp should schedule on the other.
        if (mp.to != nodeDB->getNodeNum()) return ProcessMessage::STOP;
        alarmEnabled_ = payload & 1;
        alarmHour_ = (payload >> 1) & 0x1F;
        alarmMinute_ = (payload >> 6) & 0x3F;
        alarmDurationMin_ = (payload >> 12) & 0xFFFFF;
        if (alarmDurationMin_ == 0) alarmDurationMin_ = 1;  // guard against div-by-zero in the ramp
        alarmLastFiredKey_ = -1;  // a freshly (re)armed alarm should be able to fire today
        Preferences prefs;
        prefs.begin(PREFS_NAMESPACE, false);
        prefs.putBool(PREFS_ALARM_ON_KEY, alarmEnabled_);
        prefs.putUChar(PREFS_ALARM_HOUR_KEY, alarmHour_);
        prefs.putUChar(PREFS_ALARM_MIN_KEY, alarmMinute_);
        prefs.putUShort(PREFS_ALARM_DUR_KEY, alarmDurationMin_);
        prefs.end();
        LOG_INFO("Alarm %s %02u:%02u ramp=%umin (from 0x%x, saved)", alarmEnabled_ ? "set" : "cleared", alarmHour_,
                 alarmMinute_, alarmDurationMin_, node);
        return ProcessMessage::STOP;
    }

    if (node == nodeDB->getNodeNum()) return ProcessMessage::STOP;   // our own echo

    // Identical fingerprints mean identical strips, whatever the counters say.
    if (code == code_ && counter <= counter_) return ProcessMessage::STOP;

    // Equal counters with different codes is a genuine disagreement that
    // counters cannot resolve, so fall back to the node id.
    bool wins = counter > counter_ || (counter == counter_ && code != code_ && node > owner_);
    if (!wins) {
        if (code != code_) sendState(engine_.isSolid() ? LAMP_COLOUR : (engine_.isGroupHues() ? LAMP_GROUPS : LAMP_STATE));
        return ProcessMessage::STOP;
    }

    counter_ = counter;
    owner_ = node;

    if (type == LAMP_COLOUR) {
        Rgbw c{(uint8_t)(payload & 0xFF), (uint8_t)((payload >> 8) & 0xFF),
               (uint8_t)((payload >> 16) & 0xFF), (uint8_t)((payload >> 24) & 0xFF)};
        engine_.setSolid(c);
        LOG_INFO("Lamp colour rgbw(%u,%u,%u,%u) from 0x%x", c.r, c.g, c.b, c.w, node);
    } else if (type == LAMP_POWER) {
        if (engine_.poweredOn() != on) {
            // togglePower() alone is enough - ColourEngine already owns a
            // smooth powerLevel_ fade (see LampColour.h) that render() picks
            // up automatically. startupChain() is reserved for an actual
            // device boot now, not a power-on request.
            engine_.togglePower();
        }
    } else if (type == LAMP_GROUPS) {
        int count = (int)(payload & 0x3) + 1;
        float pos[3];
        pos[0] = (float)((payload >> 2) & 0x3FF) / 1023.0f;
        pos[1] = (float)((payload >> 12) & 0x3FF) / 1023.0f;
        pos[2] = (float)((payload >> 22) & 0x3FF) / 1023.0f;
        engine_.setGroupHues(count, pos);
        LOG_INFO("Lamp groups count=%d pos=[%.2f,%.2f,%.2f] from 0x%x", count, pos[0], pos[1], pos[2], node);
    } else {
        engine_.applyScene(payload);
        LOG_INFO("Lamp scene seed=%u from 0x%x", payload, node);
    }
    code_ = engine_.visualCode();
    return ProcessMessage::STOP;
}

void LampModule::startupChain()
{
    const int SPARK = 4;
    for (int head = 0; head <= NUM_LEDS + SPARK; head++) {
        for (int i = 0; i < NUM_LEDS; i++) {
            if (i > head) { strip_.setPixelColor(i, 0); continue; }
            Rgbw c = engine_.ledColour(i);
            int behind = head - i;
            if (behind < SPARK) {
                float t = 1.0f - (float)behind / SPARK;
                strip_.setPixelColor(i, strip_.Color(
                    (uint8_t)min(255.0f, c.r + (255 - c.r) * t * 0.5f),
                    (uint8_t)min(255.0f, c.g + (255 - c.g) * t * 0.5f),
                    (uint8_t)min(255.0f, c.b + (255 - c.b) * t * 0.5f),
                    (uint8_t)min(255.0f, c.w + (255 - c.w) * t)));
            } else {
                strip_.setPixelColor(i, strip_.Color(c.r, c.g, c.b, c.w));
            }
        }
        strip_.show();
        delay(22);
    }
}

void LampModule::render()
{
    for (int i = 0; i < NUM_LEDS; i++) {
        Rgbw c = engine_.ledColour(i);
        strip_.setPixelColor(i, strip_.Color(c.r, c.g, c.b, c.w));
    }
    strip_.show();
}

// Write-only: prints the raw pin state and smoothed level on every touched/
// idle transition, plus a slow heartbeat (every 2 s) while held, so wiring
// and threshold tuning can be watched live in the normal log stream - no
// input needed, so nothing here competes with Meshtastic's own serial API.
void LampModule::debugTouch()
{
    uint32_t now = millis();
    bool touched = touch_.level() >= touch_.threshold();
    bool edge = touched != touchWasTouched_;
    bool heartbeat = touched && (now - lastTouchLog_ >= 2000);
    if (edge || heartbeat) {
        LOG_INFO("Touch GPIO%d: raw=%d level=%.2f threshold=%.2f -> %s", PIN_TOUCH, touch_.raw() ? 1 : 0,
                 touch_.level(), touch_.threshold(), touched ? "TOUCHED" : "idle");
        lastTouchLog_ = now;
    }
    touchWasTouched_ = touched;
}

// Fires at most once per calendar day. Needs a valid NTP/GPS-derived clock -
// silently does nothing until one exists (Wi-Fi + internet, set up from the
// app's Wi-Fi card), same as any wall-clock alarm without a clock to read.
void LampModule::checkAlarm()
{
    if (!alarmEnabled_ || alarmActive_) return;
    uint32_t nowSecs = getValidTime(RTCQualityNTP, true);  // local time; 0 if not yet synced
    if (nowSecs == 0) return;
    time_t t = (time_t)nowSecs;
    struct tm tmNow;
    gmtime_r(&t, &tmNow);  // getValidTime(..., true) already applied the local offset
    if (tmNow.tm_hour != alarmHour_ || tmNow.tm_min != alarmMinute_) return;

    int32_t todayKey = ((int32_t)tmNow.tm_year << 9) | tmNow.tm_yday;
    if (todayKey == alarmLastFiredKey_) return;  // already fired this minute/today

    alarmLastFiredKey_ = todayKey;
    alarmActive_ = true;
    alarmStartMs_ = millis();
    lastAlarmFrame_ = 0;
    LOG_INFO("Alarm firing: %02u:%02u ramp=%umin", alarmHour_, alarmMinute_, alarmDurationMin_);
}

void LampModule::renderAlarmRamp(float progress)
{
    progress = constrain(progress, 0.0f, 1.0f);
    // Brightness ramps in on top of the hue ramp (progress twice), so it
    // starts genuinely dark rather than snapping to ALARM_START at t=0.
    float b = progress * LED_BRIGHTNESS;
    Rgbw c{(uint8_t)(ALARM_START.r + (ALARM_END.r - ALARM_START.r) * progress),
           (uint8_t)(ALARM_START.g + (ALARM_END.g - ALARM_START.g) * progress),
           (uint8_t)(ALARM_START.b + (ALARM_END.b - ALARM_START.b) * progress),
           (uint8_t)(ALARM_START.w + (ALARM_END.w - ALARM_START.w) * progress)};
    for (int i = 0; i < NUM_LEDS; i++)
        strip_.setPixelColor(i, strip_.Color((uint8_t)(c.r * b), (uint8_t)(c.g * b), (uint8_t)(c.b * b), (uint8_t)(c.w * b)));
    strip_.show();
}

int32_t LampModule::runOnce()
{
    if (!booted_) {
        booted_ = true;
        strip_.begin();
        strip_.clear();
        strip_.show();
        touch_.begin(PIN_TOUCH);
        {
            Preferences prefs;
            prefs.begin(PREFS_NAMESPACE, true);
            float saved = prefs.getFloat(PREFS_THRESHOLD_KEY, TOUCH_THRESHOLD);
            alarmEnabled_ = prefs.getBool(PREFS_ALARM_ON_KEY, false);
            alarmHour_ = prefs.getUChar(PREFS_ALARM_HOUR_KEY, alarmHour_);
            alarmMinute_ = prefs.getUChar(PREFS_ALARM_MIN_KEY, alarmMinute_);
            alarmDurationMin_ = prefs.getUShort(PREFS_ALARM_DUR_KEY, alarmDurationMin_);
            prefs.end();
            touch_.setThreshold(saved);
        }
        engine_.begin((uint32_t)millis() ^ nodeDB->getNodeNum());
        code_ = engine_.visualCode();
        startupChain();
        sendState(LAMP_STATE);      // ask the mesh where we should be
        LOG_INFO("Lamp module up: %d LEDs on GPIO%d, touch on GPIO%d, threshold=%.2f",
                 NUM_LEDS, PIN_LED_DATA, PIN_TOUCH, touch_.threshold());
    }

    debugTouch();
    checkAlarm();

    if (alarmActive_) {
        // Entirely separate render path from the one below - deliberately
        // bypasses engine_/counter_/sendState so this stays a local-only
        // effect (see the class comment on why). A touch during the ramp
        // still works normally afterward; it just doesn't interrupt it -
        // fine for an alarm, which should finish what it started.
        uint32_t now = millis();
        if (now - lastAlarmFrame_ >= 1000) {
            lastAlarmFrame_ = now;
            float progress = (float)(now - alarmStartMs_) / (alarmDurationMin_ * 60000.0f);
            if (progress >= 1.0f) {
                alarmActive_ = false;
                engine_.setSolid(ALARM_END);  // hand off to normal rendering below, no broadcast
                code_ = engine_.visualCode();
                LOG_INFO("Alarm ramp complete");
            } else {
                renderAlarmRamp(progress);
            }
        }
        return 40;
    }

    TouchSensor::Event ev = touch_.update();
    if (ev == TouchSensor::TAP) {
        localTap();
    } else if (ev == TouchSensor::HOLD) {
        counter_++;
        owner_ = nodeDB->getNodeNum();
        engine_.togglePower();  // dims up/down via ColourEngine's own powerLevel_ fade
        sendState(LAMP_POWER);
    }

    // Refresh hard only while something is moving. A settled strip still
    // breathes, but slowly, and 20 fps is indistinguishable for that - while
    // costing a third of the interrupt pressure that 60 fps does. Inside
    // Meshtastic that pressure is not free: it lands on the same core as the
    // BLE and LoRa stacks, and showed up as an unstable phone connection.
    uint32_t now = millis();
    uint32_t period = engine_.isFading() ? 16 : 50;
    if (now - lastFrame_ >= period) { lastFrame_ = now; engine_.tick(now); render(); }
    if (now - lastState_ > STATE_INTERVAL_MS + (nodeDB->getNodeNum() % 5000))
        sendState(engine_.isSolid() ? LAMP_COLOUR : (engine_.isGroupHues() ? LAMP_GROUPS : LAMP_STATE));

    // Yield for longer when nothing is animating, so the mesh and BLE stacks get
    // the core back.
    return engine_.isFading() ? 16 : 40;
}
