#include "LampModule.h"
#include "MeshService.h"
#include "configuration.h"
#include "main.h"

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
static const int LAMP_LEN = 19;

// Meshtastic delays ordinary app traffic by seconds. A colour change is a direct
// response to someone touching the lamp, so ask for the highest priority the
// stack offers - it will not reach the standalone firmware's 211 ms, but it is
// the difference between "slow" and "did that even work".
static const meshtastic_MeshPacket_Priority LAMP_PRIORITY = meshtastic_MeshPacket_Priority_HIGH;

// How often to re-announce. A tap is sent once with no retry, so without this a
// single lost packet would leave the lamps disagreeing indefinitely.
static const uint32_t STATE_INTERVAL_MS = 30000;

void LampModule::sendState(uint8_t type)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->priority = LAMP_PRIORITY;
    p->want_ack = false;

    uint32_t payload = engine_.isSolid() ? engine_.packedSolid() : engine_.seed();
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

    if (node == nodeDB->getNodeNum()) return ProcessMessage::STOP;   // our own echo

    // Identical fingerprints mean identical strips, whatever the counters say.
    if (code == code_ && counter <= counter_) return ProcessMessage::STOP;

    // Equal counters with different codes is a genuine disagreement that
    // counters cannot resolve, so fall back to the node id.
    bool wins = counter > counter_ || (counter == counter_ && code != code_ && node > owner_);
    if (!wins) {
        if (code != code_) sendState(engine_.isSolid() ? LAMP_COLOUR : LAMP_STATE);
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
            engine_.togglePower();
            if (engine_.poweredOn()) startupChain();
        }
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

int32_t LampModule::runOnce()
{
    if (!booted_) {
        booted_ = true;
        strip_.begin();
        strip_.clear();
        strip_.show();
        touch_.begin(PIN_TOUCH);
        engine_.begin((uint32_t)millis() ^ nodeDB->getNodeNum());
        code_ = engine_.visualCode();
        startupChain();
        sendState(LAMP_STATE);      // ask the mesh where we should be
        LOG_INFO("Lamp module up: %d LEDs on GPIO%d, touch on GPIO%d",
                 NUM_LEDS, PIN_LED_DATA, PIN_TOUCH);
    }

    TouchSensor::Event ev = touch_.update();
    if (ev == TouchSensor::TAP) {
        localTap();
    } else if (ev == TouchSensor::HOLD) {
        counter_++;
        owner_ = nodeDB->getNodeNum();
        engine_.togglePower();
        if (engine_.poweredOn()) startupChain();
        sendState(LAMP_POWER);
    }

    uint32_t now = millis();
    if (now - lastFrame_ >= 16) { lastFrame_ = now; engine_.tick(now); render(); }
    if (now - lastState_ > STATE_INTERVAL_MS + (nodeDB->getNodeNum() % 5000))
        sendState(engine_.isSolid() ? LAMP_COLOUR : LAMP_STATE);

    // 16 ms keeps the fades at ~60 fps without starving the mesh stack.
    return 16;
}
