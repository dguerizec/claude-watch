#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_STATE_INIT,
    WIFI_MGR_STATE_PROVISIONING,    /* SoftAP + captive portal active */
    WIFI_MGR_STATE_CONNECTING,      /* Attempting STA connection */
    WIFI_MGR_STATE_CONNECTED,       /* Connected to WiFi as station */
    WIFI_MGR_STATE_FAILED,          /* Connection failed after retries */
} wifi_mgr_state_t;

typedef struct {
    char ssid[33];
    char password[65];
    char access_token[256];
    char refresh_token[256];
    char timezone[64];
    char fetch_interval[8];   /* minutes between auto-fetches, as string */
} wifi_mgr_credentials_t;

/**
 * State change callback. Called from the WiFi event task context.
 */
typedef void (*wifi_mgr_state_cb_t)(wifi_mgr_state_t state, void *arg);

/**
 * Initialize WiFi manager: NVS, netif, event loop, WiFi driver.
 * Must be called once before wifi_mgr_start().
 */
esp_err_t wifi_mgr_init(wifi_mgr_state_cb_t state_cb, void *cb_arg);

/**
 * Start WiFi: checks NVS for stored credentials.
 * - If found: connects as station
 * - If not found: starts SoftAP + captive portal
 */
esp_err_t wifi_mgr_start(void);

/**
 * Get current WiFi manager state.
 */
wifi_mgr_state_t wifi_mgr_get_state(void);

/**
 * Read stored credentials from NVS.
 * Returns ESP_ERR_NVS_NOT_FOUND if no credentials are stored.
 */
esp_err_t wifi_mgr_get_credentials(wifi_mgr_credentials_t *creds);

/**
 * Erase all stored credentials from NVS.
 */
esp_err_t wifi_mgr_erase_credentials(void);

/**
 * Get the SoftAP SSID (available after wifi_mgr_init).
 */
const char *wifi_mgr_get_ap_ssid(void);

/**
 * Get the station IP address as string (valid only in CONNECTED state).
 */
const char *wifi_mgr_get_sta_ip(void);

/**
 * Update tokens in NVS (called after OAuth refresh).
 */
esp_err_t wifi_mgr_update_tokens(const char *access_token, const char *refresh_token);

/**
 * Read display config from NVS.
 * Format: "id:enabled,id:enabled,..." e.g. "0:1,1:1,2:1,3:1"
 * @param buf     Output buffer
 * @param buf_len Buffer size
 */
void wifi_mgr_get_display_config(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
