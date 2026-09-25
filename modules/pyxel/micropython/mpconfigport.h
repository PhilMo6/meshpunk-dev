// MicroPython configuration for the Pyxel module.
// genhdr/ was generated from this feature set: enabling or disabling a module,
// builtin or feature here requires regenerating genhdr (see ../VENDOR.md).

#include <stdint.h>
#include <alloca.h>

typedef long mp_off_t;
#define MP_SSIZE_MAX (0x7fffffff)

#define MICROPY_MPHALPORT_H                     "mphalport.h"

#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#define MICROPY_ENABLE_COMPILER                 (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_PY_GC                           (1)
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_LONGINT_IMPL                    (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_ENABLE_EXTERNAL_IMPORT          (1)
#define MICROPY_PY_SYS                          (1)
#define MICROPY_PY_SYS_PATH                     (1)
#define MICROPY_PY_IO                           (1)
#define MICROPY_PY_JSON                         (1)
#define MICROPY_PY_TIME                         (0)
#define MICROPY_PY_OS                           (0)
#define MICROPY_ENABLE_FINALISER                (0)
#define MICROPY_PY_BUILTINS_HELP                (0)
#define MICROPY_HELPER_REPL                     (0)
#define MICROPY_PY_THREAD                       (0)
#define MICROPY_VFS                             (0)
#define MICROPY_PY_MICROPYTHON_MEM_INFO         (1)
#define MICROPY_PY_SYS_STDFILES                 (0)
#define MICROPY_USE_INTERNAL_ERRNO              (1)
#define MICROPY_PY_ERRNO                        (0)
#define MICROPY_PY_BUILTINS_INPUT               (0)
#define MICROPY_PY_SYS_PLATFORM                 "meshpunk"
#define MICROPY_KBD_EXCEPTION                   (0)
#define MICROPY_PY_UCTYPES                      (0)

// The ESP32-S3 uses the windowed register ABI: nlrxtensa.c is call0-only, so
// NLR and the GC register capture both go through setjmp.
#define MICROPY_NLR_SETJMP                      (1)
#define MICROPY_GCREGS_SETJMP                   (1)

// Source files are read by src/vfs.c (mp_lexer_new_from_file), not POSIX read().
#define MICROPY_READER_POSIX                    (0)

// Bytecode dispatch through a label table (in .rodata) instead of a switch.
#define MICROPY_OPT_COMPUTED_GOTO               (1)

// The interpreter's hottest functions run from internal SRAM: the ELF loader
// copies the .iram.text section there. Files holding them are compiled with
// -mtext-section-literals -fno-jump-tables (build.ps1).
#if defined(__xtensa__)
#define PX_IRAM __attribute__((section(".iram.text")))
#define MICROPY_WRAP_MP_EXECUTE_BYTECODE(f)     PX_IRAM f
#define MICROPY_WRAP_MP_MAP_LOOKUP(f)           PX_IRAM f
#define MICROPY_WRAP_MP_LOAD_GLOBAL(f)          PX_IRAM f
#define MICROPY_WRAP_MP_LOAD_NAME(f)            PX_IRAM f
#define MICROPY_WRAP_MP_BINARY_OP(f)            PX_IRAM f
#define MICROPY_WRAP_MP_OBJ_GET_TYPE(f)         PX_IRAM f
#endif

// Every 64 branches/returns the VM calls px_audio_poll() (src/audio.c), which
// keeps the audio ring fed while Python code runs.
void px_audio_poll(void);
#define MICROPY_VM_HOOK_COUNT                   (64)
#define MICROPY_VM_HOOK_INIT                    unsigned int vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL                    if (--vm_hook_divisor == 0) { \
        vm_hook_divisor = MICROPY_VM_HOOK_COUNT; \
        px_audio_poll(); \
}
#define MICROPY_VM_HOOK_LOOP                    MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN                  MICROPY_VM_HOOK_POLL
