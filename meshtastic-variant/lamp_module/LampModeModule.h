#pragma once
// ============================================================
//  LampModeModule - switch back to the fast firmware
// ============================================================
// Both lamp firmwares live in flash at once: the Meshtastic build in app0 and
// the direct-radio build in app1, which is possible only because they share an
// identical 8 MB partition table. Switching between them is just repointing the
// bootloader - nothing is erased, nothing is downloaded.
//
// This module listens on TEXT_MESSAGE_APP rather than inventing a control
// channel, because a text message is the one thing the Meshtastic phone app can
// already send. Type "lamp fast" into the app and the lamps reboot into the
// low-latency firmware.
//
// It always returns CONTINUE, so ordinary text messages still reach the normal
// text module and nothing else changes.

#include "SinglePortModule.h"

class LampModeModule : public SinglePortModule
{
  public:
    LampModeModule() : SinglePortModule("lampmode", meshtastic_PortNum_TEXT_MESSAGE_APP) {}

  protected:
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
};
