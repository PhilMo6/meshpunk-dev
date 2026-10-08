// Rung 7c: C6 ROM download over SDIO — adapter-free flashing feasibility.
// No flash writes: the C6's own firmware is untouched.
//
// The C6 enters its ROM download mode when its GPIO9 (the C6 "B" button)
// is low at reset (esptool boot-mode docs; the C6 ROM's download mode
// lists SDIO alongside UART0/UART1). The P4 resets the C6 through expander
// IO14 (the C6's EN line — LilyGo's coprocessor_download_mode source) while
// the button is held by hand. Espressif's esp-serial-flasher then connects
// over the C6 SDIO link: it identifies the chip from the SDIO CIS (vendor
// 0x0092, device 0x100D = ESP32-C6), loads its flasher stub into the C6's
// RAM and waits for the stub's "OHAI" packet. Afterwards the probe only
// reads: the C6's MAC and flash size through the stub. A normal reset
// (button released) returns the C6 to its own firmware, which rung 7 then
// checks over AT.
//
// Port: the library's stock ESP32 SDIO port drives reset/BOOT from plain
// GPIOs; this board has an expander line and a physical button, so this
// file supplies the port ops (esp_loader_port_ops_t, esp-serial-flasher
// v2.0.0). SDIO host + slot 1 come from p4_c6_sdio_init() (rung7_c6.c).

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_loader.h"
#include "esp_loader_io.h"
#include "p4_shared.h"

// The library hands the ops a pointer to `base`; it is the first member,
// so that pointer is also a pointer to the whole struct.
typedef struct {
    esp_loader_port_t base;
    sdmmc_host_t host;
    sdmmc_card_t card;
    int64_t time_end;
} board_port_t;

#define HOLD_COUNTDOWN_S 8
#define RELEASE_COUNTDOWN_S 5

static esp_loader_error_t map_err(esp_err_t err)
{
    if (err == ESP_OK) return ESP_LOADER_SUCCESS;
    if (err == ESP_ERR_TIMEOUT) return ESP_LOADER_ERROR_TIMEOUT;
    return ESP_LOADER_ERROR_FAIL;
}

static const char *loader_err_name(esp_loader_error_t e)
{
    switch (e) {
    case ESP_LOADER_SUCCESS:                return "SUCCESS";
    case ESP_LOADER_ERROR_FAIL:             return "FAIL";
    case ESP_LOADER_ERROR_TIMEOUT:          return "TIMEOUT";
    case ESP_LOADER_ERROR_IMAGE_SIZE:       return "IMAGE_SIZE";
    case ESP_LOADER_ERROR_INVALID_MD5:      return "INVALID_MD5";
    case ESP_LOADER_ERROR_INVALID_PARAM:    return "INVALID_PARAM";
    case ESP_LOADER_ERROR_INVALID_TARGET:   return "INVALID_TARGET";
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP: return "UNSUPPORTED_CHIP";
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC: return "UNSUPPORTED_FUNC";
    case ESP_LOADER_ERROR_INVALID_RESPONSE: return "INVALID_RESPONSE";
    }
    return "?";
}

static esp_loader_error_t board_init(esp_loader_port_t *port)
{
    board_port_t *p = (board_port_t *)port;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    // Required on this chip for byte-mode CMD53 from cached buffers (see
    // rung7_c6.c); the stock esp-serial-flasher SDIO port sets it too.
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    p->host = host;
    return ESP_LOADER_SUCCESS;
}

// sdmmc_card_init allocates a 512-byte bounce buffer per call and the
// driver never frees it; the previous one is released before each init
// and at deinit.
static void board_deinit(esp_loader_port_t *port)
{
    board_port_t *p = (board_port_t *)port;
    heap_caps_free(p->card.host.dma_aligned_buffer);
    p->card.host.dma_aligned_buffer = NULL;
}

static void c6_reset_pulse(void)
{
    p4_xl_set(IO_C6_EN, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    p4_xl_set(IO_C6_EN, true);
}

// Called once by esp_loader_connect() before the SDIO handshake.
static void board_enter_bootloader(esp_loader_port_t *port)
{
    (void)port;
    printf("[c6rom] >>> PRESS AND HOLD the C6 'B' button now <<<\n");
    for (int s = HOLD_COUNTDOWN_S; s > 0; s--) {
        printf("[c6rom]     keep holding B — C6 reset in %d...\n", s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    c6_reset_pulse();
    vTaskDelay(pdMS_TO_TICKS(300));
    printf("[c6rom] >>> RELEASE the B button <<<\n");
}

static void board_reset_target(esp_loader_port_t *port)
{
    (void)port;
    c6_reset_pulse();
}

static void board_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    board_port_t *p = (board_port_t *)port;
    p->time_end = esp_timer_get_time() + (int64_t)ms * 1000;
}

static uint32_t board_remaining_time(esp_loader_port_t *port)
{
    board_port_t *p = (board_port_t *)port;
    int64_t remaining = (p->time_end - esp_timer_get_time()) / 1000;
    return (remaining > 0) ? (uint32_t)remaining : 0;
}

static void board_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    if (ms == 0) return;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static void board_log(esp_loader_port_t *port, esp_loader_log_level_t level,
                      const char *fmt, va_list args)
{
    (void)port;
    (void)level;
    printf("[c6rom:lib] ");
    vprintf(fmt, args);
    printf("\n");
}

static void board_log_hex(esp_loader_port_t *port,
                          esp_loader_log_level_t level, const char *label,
                          const uint8_t *data, size_t size)
{
    (void)port;
    (void)level;
    printf("[c6rom:lib] %s:", label ? label : "");
    for (size_t i = 0; i < size && i < 64; i++) printf(" %02X", data[i]);
    printf("%s\n", size > 64 ? " ..." : "");
}

static esp_loader_error_t board_sdio_write(esp_loader_port_t *port,
                                           uint32_t function, uint32_t addr,
                                           const uint8_t *data, uint16_t size,
                                           uint32_t timeout)
{
    board_port_t *p = (board_port_t *)port;
    (void)timeout;
    if (data == NULL) return ESP_LOADER_ERROR_INVALID_PARAM;
    return map_err(sdmmc_io_write_bytes(&p->card, function, addr, data,
                                        (size_t)size));
}

static esp_loader_error_t board_sdio_read(esp_loader_port_t *port,
                                          uint32_t function, uint32_t addr,
                                          uint8_t *data, uint16_t size,
                                          uint32_t timeout)
{
    board_port_t *p = (board_port_t *)port;
    (void)timeout;
    if (data == NULL) return ESP_LOADER_ERROR_INVALID_PARAM;
    return map_err(sdmmc_io_read_bytes(&p->card, function, addr, data,
                                       (size_t)size));
}

static esp_loader_error_t board_sdio_card_init(esp_loader_port_t *port)
{
    board_port_t *p = (board_port_t *)port;
    heap_caps_free(p->card.host.dma_aligned_buffer);
    p->card.host.dma_aligned_buffer = NULL;
    return map_err(sdmmc_card_init(&p->host, &p->card));
}

static const esp_loader_port_ops_t k_board_ops = {
    .init             = board_init,
    .deinit           = board_deinit,
    .enter_bootloader = board_enter_bootloader,
    .reset_target     = board_reset_target,
    .start_timer      = board_start_timer,
    .remaining_time   = board_remaining_time,
    .delay_ms         = board_delay_ms,
    .log              = board_log,
    .log_hex          = board_log_hex,
    .sdio_write       = board_sdio_write,
    .sdio_read        = board_sdio_read,
    .sdio_card_init   = board_sdio_card_init,
};

void rung7c_c6_rom(void)
{
    printf("[c6rom] rung 7c: C6 ROM download over SDIO (no flash writes)\n");
    if (!g_p4_i2c0) {
        printf("[c6rom] SKIP: no expander (rung 2 stopped early)\n");
        return;
    }
    if (!p4_c6_sdio_init()) return;

    static board_port_t port = { .base.ops = &k_board_ops };
    static esp_loader_t loader;
    esp_loader_error_t e = esp_loader_init_sdio(&loader, &port.base);
    if (e != ESP_LOADER_SUCCESS) {
        printf("[c6rom] FAIL: esp_loader_init_sdio: %s\n", loader_err_name(e));
        return;
    }

    printf("[c6rom] stage: connect (B held → ROM download mode → stub)\n");
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    int64_t t0 = esp_timer_get_time();
    e = esp_loader_connect(&loader, &args);
    printf("[c6rom] connect: %s (%lld ms after the countdown)\n",
           loader_err_name(e),
           (long long)((esp_timer_get_time() - t0) / 1000) -
               HOLD_COUNTDOWN_S * 1000);

    if (e == ESP_LOADER_SUCCESS) {
        target_chip_t chip = esp_loader_get_target(&loader);
        printf("[c6rom] target chip id %d%s\n", (int)chip,
               chip == ESP32C6_CHIP ? " (ESP32-C6)" : "");

        uint8_t mac[6] = { 0 };
        e = esp_loader_read_mac(&loader, mac);
        if (e == ESP_LOADER_SUCCESS) {
            printf("[c6rom] C6 MAC %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0],
                   mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            printf("[c6rom] read MAC: %s\n", loader_err_name(e));
        }

        uint32_t flash_size = 0;
        e = esp_loader_flash_detect_size(&loader, &flash_size);
        if (e == ESP_LOADER_SUCCESS) {
            printf("[c6rom] C6 flash size %lu bytes (%lu MB)\n",
                   (unsigned long)flash_size,
                   (unsigned long)(flash_size / (1024 * 1024)));
        } else {
            printf("[c6rom] flash size: %s\n", loader_err_name(e));
        }
        printf("[c6rom] verdict: ROM download over SDIO WORKS — the P4 can "
               "flash the C6 with no adapter (B button held)\n");
    } else {
        printf("[c6rom] verdict: no ROM connection — if B was held through "
               "the reset, the SDIO ROM path failed at the stage the "
               "library logged above; if not, reboot and hold B through "
               "the countdown\n");
    }

    // The C6 samples B (GPIO9) at reset: if B is still held, the reset below
    // re-enters ROM download mode instead of starting the C6's own firmware.
    printf("[c6rom] >>> RELEASE the B button now (if still held) <<<\n");
    for (int s = RELEASE_COUNTDOWN_S; s > 0; s--) {
        printf("[c6rom]     B released? C6 normal reset in %d...\n", s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("[c6rom] stage: normal reset (B released) → C6 own firmware\n");
    esp_loader_reset_target(&loader);
    esp_loader_deinit(&loader);
}
