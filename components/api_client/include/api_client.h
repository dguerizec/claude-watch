#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float utilization;
    char resets_at[32];  /* "HH:MM" or "Apr 10 12:00" */
} api_usage_tier_t;

typedef struct {
    api_usage_tier_t five_hour;
    api_usage_tier_t seven_day;
    bool valid;
    char error[64];
} api_usage_t;

/**
 * Fetch usage data from Anthropic API.
 * On HTTP 401, automatically refreshes the access token using the refresh token,
 * saves updated tokens to NVS, and retries.
 *
 * @param access_token   Bearer access token
 * @param refresh_token  OAuth refresh token (for auto-refresh on 401)
 * @param out            Parsed usage result
 * @return ESP_OK on success, ESP_FAIL on error (check out->error)
 */
esp_err_t api_client_get_usage(const char *access_token, const char *refresh_token,
                               api_usage_t *out);

#ifdef __cplusplus
}
#endif
