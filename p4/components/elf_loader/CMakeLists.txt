# components/elf_loader — lib/elf_loader, the firmware's ELF module loader.
# On the P4 it syncs the loaded code with esp_cache_msync (esp_mm) over the
# cache lines hal/cache_hal.h reports for the address (hal).
set(ELF_LOADER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/elf_loader")
idf_component_register(
    SRCS "${ELF_LOADER_DIR}/elf_loader.c"
    INCLUDE_DIRS "${ELF_LOADER_DIR}"
    REQUIRES heap esp_mm hal)
