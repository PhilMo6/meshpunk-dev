// meshpunk_p4.cpp — the P4 project's hook into Arduino's boot. Arduino
// 3.3.12 is an IDF component here; its cores/esp32/main.cpp provides
// app_main() (CONFIG_AUTOSTART_ARDUINO) and runs the firmware's setup() and
// loop() from components/meshpunk. That file declares the function below
// weak.
//
// shouldPrintChipDebugReport(): when it returns true, Arduino prints its
// chip report (chip-debug-report.cpp: chip model, revision, cores and
// features, flash size and mode, PSRAM, the partition table, the IDF and
// Arduino versions, the board) before setup() and the heap report after it.
// On the P4 this is the boot banner the bring-up rungs printed by hand.
//
// Linking: this object lives in libmain.a while Arduino's main.cpp carries
// a weak definition of the same function, so the linker would resolve the
// weak one and never open this archive member. main/CMakeLists.txt passes
// -u with the mangled name, the technique Arduino's own CMakeLists uses to
// pull setup() and loop().
#include <Arduino.h>

bool shouldPrintChipDebugReport(void) { return true; }
