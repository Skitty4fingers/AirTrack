#include "board_internal.h"

#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define BOARD_SPI_HOST SPI2_HOST
#define BOARD_PIN_SD_CS 4
#define BOARD_SD_CLOCK_KHZ 10000
#define BOARD_SD_MAX_FILES 6

static const char *TAG = "board_sd";

esp_err_t board_internal_sd_mount(void)
{
    if (!g_board_state.spi_ready || g_board_state.sd_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_board_state.spi_gate, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.max_freq_khz = BOARD_SD_CLOCK_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BOARD_PIN_SD_CS;
    slot_config.host_id = BOARD_SPI_HOST;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = BOARD_SD_MAX_FILES,
        .allocation_unit_size = 16U * 1024U,
    };

    const esp_err_t err = esp_vfs_fat_sdspi_mount(
        BOARD_SD_MOUNT_POINT, &host, &slot_config, &mount_config,
        &g_board_state.sd_card);
    if (err == ESP_OK) {
        g_board_state.sd_mounted = true;
        g_board_state.sd_capacity_bytes =
            (uint64_t)g_board_state.sd_card->csd.capacity *
            g_board_state.sd_card->csd.sector_size;
        ESP_LOGI(TAG, "Mounted FAT SD at %s (%llu MiB, 10 MHz)",
                 BOARD_SD_MOUNT_POINT,
                 (unsigned long long)(g_board_state.sd_capacity_bytes /
                                      (1024ULL * 1024ULL)));
    } else {
        g_board_state.sd_card = NULL;
        g_board_state.sd_mounted = false;
        g_board_state.sd_capacity_bytes = 0;
    }

    xSemaphoreGive(g_board_state.spi_gate);
    return err;
}

void board_internal_sd_unmount(void)
{
    if (!g_board_state.sd_mounted || g_board_state.sd_card == NULL) {
        return;
    }

    if (g_board_state.spi_gate != NULL &&
        xSemaphoreTake(g_board_state.spi_gate, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT_POINT,
                                                     g_board_state.sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD unmount failed: %s", esp_err_to_name(err));
    }
    g_board_state.sd_card = NULL;
    g_board_state.sd_mounted = false;
    g_board_state.sd_capacity_bytes = 0;
    if (g_board_state.spi_gate != NULL) {
        xSemaphoreGive(g_board_state.spi_gate);
    }
}
