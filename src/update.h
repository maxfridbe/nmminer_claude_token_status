// Firmware updates over the air from the project's GitHub releases.
#pragma once
#include <Arduino.h>

const char *fwVersion();     // "v26.09.01", or "dev" for an unstamped build
void screenFirmwareUpdate(); // modal: check, confirm, download, restart
void updateSelfTest();       // OTA_SELFTEST builds only
