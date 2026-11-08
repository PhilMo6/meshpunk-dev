// meshcore_Utils.cpp — lib/MeshCore/src/Utils.cpp compiled through a
// translation unit that lives inside the P4 project.
//
// Why: pioarduino's builder names each object after the source's path
// relative to the IDF tree, the build directory or the project directory,
// and falls back to the bare file name for every other source
// (builder/frameworks/espidf.py, compile_source_files). All lib/ and src/
// sources take that fallback, so MeshCore's Utils.cpp and RadioLib's
// src/utils/Utils.cpp would both become Utils.cpp.o and SCons stops with
// "Multiple ways to build the same target". This file keeps its own path
// under p4/ and includes the source.
#include "../../../lib/MeshCore/src/Utils.cpp"
