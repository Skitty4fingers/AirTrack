#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_VERSION_MAX_BYTES 31U
#define OTA_NOTES_MAX_BYTES 159U
#define OTA_URL_MAX_BYTES 255U
#define OTA_ERROR_MAX_BYTES 63U

typedef enum {
    OTA_STATE_IDLE = 0,       /* nothing checked yet */
    OTA_STATE_CHECKING,       /* fetching the manifest */
    OTA_STATE_UP_TO_DATE,     /* manifest read; running version is current */
    OTA_STATE_AVAILABLE,      /* newer version offered by the manifest */
    OTA_STATE_DOWNLOADING,    /* streaming into the inactive slot */
    OTA_STATE_VERIFYING,      /* image written; checking hash and header */
    OTA_STATE_READY,          /* boot partition switched; restart imminent */
    OTA_STATE_FAILED,         /* see error; running image untouched */
} ota_state_t;

typedef struct {
    ota_state_t state;
    char current_version[OTA_VERSION_MAX_BYTES + 1U];
    char available_version[OTA_VERSION_MAX_BYTES + 1U];
    char notes[OTA_NOTES_MAX_BYTES + 1U];
    uint32_t size;
    uint32_t downloaded;
    uint8_t percent;
    char error[OTA_ERROR_MAX_BYTES + 1U];
    int64_t checked_monotonic_ms;   /* 0 until the first successful check */
    bool pending_verify;            /* this boot's image awaits self-test */
    char running_partition[17];     /* "ota_0" / "ota_1" */
} ota_status_t;

/** Called in the OTA task right before the download starts (pause polling). */
typedef void (*ota_prepare_cb_t)(void *user_context);

/**
 * Initialise with the manifest URL (HTTPS, verified with the certificate
 * bundle) and read the running partition / rollback state.
 */
esp_err_t ota_init(const char *manifest_url, ota_prepare_cb_t prepare,
                   void *user_context);

/** Fetch the manifest in a worker task; poll ota_get_status() for the result. */
esp_err_t ota_check_async(void);

/**
 * Download and install the version offered by the last check.  The version
 * must equal available_version so a stale page cannot install something the
 * user did not see.  On success the device restarts by itself.
 */
esp_err_t ota_start(const char *version);

esp_err_t ota_get_status(ota_status_t *status);

/** True while a check or download task is running. */
bool ota_busy(void);

/** Rollback bookkeeping for a freshly installed image. */
bool ota_pending_verify(void);
esp_err_t ota_confirm_running_image(void);
esp_err_t ota_reject_running_image_and_reboot(void);

/** Compare "a.b.c" strings: <0, 0, >0. Non-numeric parts compare as 0. */
int ota_compare_versions(const char *left, const char *right);

#ifdef __cplusplus
}
#endif
