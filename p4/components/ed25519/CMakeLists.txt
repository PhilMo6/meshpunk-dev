# components/ed25519 — lib/ed25519, the firmware's identity crypto
# (src/elf_host.cpp exports ed25519_key_exchange, ed25519_create_keypair,
# ed25519_derive_pub and ed25519_sign to protocol modules).
set(ED25519_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/ed25519")
file(GLOB ED25519_SOURCES "${ED25519_DIR}/*.c")
idf_component_register(
    SRCS ${ED25519_SOURCES}
    INCLUDE_DIRS "${ED25519_DIR}")
