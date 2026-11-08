# components/meshcore — the two lib/MeshCore files the firmware links:
# Utils.cpp (mesh::Utils::toHex, src/main.cpp) and helpers/ESP32Board.cpp
# (the ESP32Board instance, src/main.cpp). The rest of MeshCore's src/ is
# the mesh protocol, which runs as an ELF module; on the S3 boards PlatformIO
# compiles MeshCore's whole library filter and the linker drops the
# unreferenced objects. Headers src/ includes from here: Mesh.h, MeshCore.h,
# helpers/ArduinoHelpers.h, helpers/ESP32Board.h and
# helpers/radiolib/CustomSX1262.h (RadioLib); Utils.cpp needs Crypto's AES
# and SHA256.
#
# The two sources are compiled through wrapper translation units in this
# directory (meshcore_Utils.cpp, meshcore_ESP32Board.cpp): pioarduino's
# builder names objects outside p4/ and the IDF tree by bare file name, and
# RadioLib has a Utils.cpp too (see meshcore_Utils.cpp).
set(MESHCORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/MeshCore/src")
idf_component_register(
    SRCS "meshcore_Utils.cpp" "meshcore_ESP32Board.cpp"
    INCLUDE_DIRS "${MESHCORE_DIR}"
    REQUIRES arduino-esp32 crypto radiolib ed25519)
