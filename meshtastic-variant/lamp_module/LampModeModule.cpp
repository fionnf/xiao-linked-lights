#include "LampModeModule.h"
#include "configuration.h"
#include "main.h"
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <string.h>

ProcessMessage LampModeModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    auto &p = mp.decoded;
    char text[64] = {0};
    size_t n = p.payload.size < sizeof(text) - 1 ? p.payload.size : sizeof(text) - 1;
    memcpy(text, p.payload.bytes, n);

    // Deliberately narrow: only an exact "lamp fast" switches firmware. A looser
    // match would turn any chat message containing the word into a reboot.
    if (strcasecmp(text, "lamp fast") == 0) {
        const esp_partition_t *other = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
        if (other && esp_ota_set_boot_partition(other) == ESP_OK) {
            LOG_INFO("Lamp: switching to the direct-radio firmware");
            delay(300);
            esp_restart();
        } else {
            LOG_WARN("Lamp: app1 missing - direct-radio firmware not flashed");
        }
    }
    return ProcessMessage::CONTINUE;   // never swallow ordinary messages
}
