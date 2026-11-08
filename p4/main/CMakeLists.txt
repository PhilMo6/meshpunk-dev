# The firmware. Arduino's cores/esp32/main.cpp is app_main()
# (CONFIG_AUTOSTART_ARDUINO) and runs setup()/loop() from components/meshpunk
# (src/main.cpp); this component carries the P4 project's Arduino hook
# (meshpunk_p4.cpp) and nothing else. No REQUIRES on purpose: a main
# component without requirements is linked against every other component in
# the build (IDF project.cmake), which is what brings the firmware, the
# libraries and Arduino into the image.
#
# The bring-up rungs (main.c, rung*.c, rung6_lora_rx.cpp) stay in this
# directory as the hardware-proven reference for Phases 1-5 of
# docs/P4_PORT_PLAN_2026-10-12.md. They are not registered, so neither CMake
# nor pioarduino compiles them (the builder compiles the sources the CMake
# code model lists).
idf_component_register(SRCS "meshpunk_p4.cpp")

# Keep the strong shouldPrintChipDebugReport() in meshpunk_p4.cpp over the
# weak one in Arduino's main.cpp: without -u the linker never opens this
# archive member (see meshpunk_p4.cpp).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u _Z26shouldPrintChipDebugReportv")
