#include <string.h>
#include <stdio.h>
#include "api_client.h"
#include "wifi_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "api_client";

#define API_URL "https://api.anthropic.com/api/oauth/usage"
#define TOKEN_URL "https://console.anthropic.com/v1/oauth/token"
#define CLIENT_ID "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define MAX_RESPONSE_SIZE 2048

/* Parse "2026-04-07T04:00:00.623562+00:00" into "04:00 UTC" or "Apr 10 12:00" */
static void parse_reset_time(const char *iso, char *out, size_t out_size)
{
    int year, month, day, hour, minute;
    if (sscanf(iso, "%d-%d-%dT%d:%d:", &year, &month, &day, &hour, &minute) == 5) {
        static const char *months[] = {
            "", "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };
        if (month >= 1 && month <= 12) {
            snprintf(out, out_size, "%s %d %02d:%02d",
                     months[month], day, hour, minute);
        } else {
            snprintf(out, out_size, "%02d:%02d", hour, minute);
        }
    } else {
        strncpy(out, iso, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static void parse_tier(cJSON *obj, api_usage_tier_t *tier)
{
    tier->utilization = 0;
    tier->resets_at[0] = '\0';

    if (!obj || !cJSON_IsObject(obj)) return;

    cJSON *util = cJSON_GetObjectItem(obj, "utilization");
    if (util && cJSON_IsNumber(util)) {
        tier->utilization = (float)cJSON_GetNumberValue(util);
    }

    cJSON *resets = cJSON_GetObjectItem(obj, "resets_at");
    if (resets && cJSON_IsString(resets)) {
        parse_reset_time(cJSON_GetStringValue(resets), tier->resets_at, sizeof(tier->resets_at));
    }
}

/**
 * Perform an HTTP request and read the response into a buffer.
 * Returns the HTTP status code, or -1 on connection error.
 */
static int http_get_with_auth(const char *url, const char *token,
                              char *resp_buf, size_t resp_size)
{
    char auth_header[300];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return -1;

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "anthropic-beta", "oauth-2025-04-20");
    esp_http_client_set_header(client, "User-Agent", "claude-monitor/1.0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    int total_read = 0;
    int read_len;
    while (total_read < (int)resp_size - 1) {
        read_len = esp_http_client_read(client, resp_buf + total_read,
                                         resp_size - 1 - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    resp_buf[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

/**
 * Refresh the OAuth access token using the refresh token.
 * On success, saves new tokens to NVS and copies the new access token to new_access_out.
 */
static esp_err_t refresh_access_token(const char *refresh_token,
                                      char *new_access_out, size_t out_size)
{
    ESP_LOGI(TAG, "Refreshing access token...");

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "grant_type", "refresh_token");
    cJSON_AddStringToObject(body, "refresh_token", refresh_token);
    cJSON_AddStringToObject(body, "client_id", CLIENT_ID);
    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!post_data) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = TOKEN_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    int post_len = strlen(post_data);
    esp_err_t err = esp_http_client_open(client, post_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Token refresh: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(post_data);
        return ESP_FAIL;
    }

    esp_http_client_write(client, post_data, post_len);
    free(post_data);

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char *resp = malloc(MAX_RESPONSE_SIZE);
    if (!resp) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int read_len;
    while (total_read < MAX_RESPONSE_SIZE - 1) {
        read_len = esp_http_client_read(client, resp + total_read,
                                         MAX_RESPONSE_SIZE - 1 - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    resp[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "Token refresh failed: HTTP %d: %s", status, resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        ESP_LOGE(TAG, "Token refresh: JSON parse error");
        return ESP_FAIL;
    }

    cJSON *at = cJSON_GetObjectItem(root, "access_token");
    cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");

    if (!at || !cJSON_IsString(at)) {
        ESP_LOGE(TAG, "Token refresh: no access_token in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *new_access = cJSON_GetStringValue(at);
    const char *new_refresh = rt && cJSON_IsString(rt) ? cJSON_GetStringValue(rt) : NULL;

    /* Save to NVS */
    wifi_mgr_update_tokens(new_access, new_refresh);

    /* Return new access token to caller */
    strncpy(new_access_out, new_access, out_size - 1);
    new_access_out[out_size - 1] = '\0';

    ESP_LOGI(TAG, "Token refreshed successfully");
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t api_client_get_usage(const char *access_token, const char *refresh_token,
                               api_usage_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!refresh_token || strlen(refresh_token) == 0) {
        snprintf(out->error, sizeof(out->error), "No token configured");
        return ESP_FAIL;
    }

    char *response_buf = malloc(MAX_RESPONSE_SIZE);
    if (!response_buf) {
        snprintf(out->error, sizeof(out->error), "Out of memory");
        return ESP_FAIL;
    }

    /* If no access token, refresh first */
    const char *token = access_token;
    char new_token[256] = {0};
    if (!token || strlen(token) == 0) {
        ESP_LOGI(TAG, "No access token — refreshing");
        if (refresh_access_token(refresh_token, new_token, sizeof(new_token)) == ESP_OK) {
            token = new_token;
        } else {
            snprintf(out->error, sizeof(out->error), "Token refresh failed");
            free(response_buf);
            return ESP_FAIL;
        }
    }

    int status = http_get_with_auth(API_URL, token, response_buf, MAX_RESPONSE_SIZE);

    /* On 401, try to refresh the token and retry */
    if (status == 401) {
        ESP_LOGW(TAG, "HTTP 401 — attempting token refresh");
        if (refresh_access_token(refresh_token, new_token, sizeof(new_token)) == ESP_OK) {
            status = http_get_with_auth(API_URL, new_token, response_buf, MAX_RESPONSE_SIZE);
        } else {
            snprintf(out->error, sizeof(out->error), "Token refresh failed");
            free(response_buf);
            return ESP_FAIL;
        }
    }

    if (status < 0) {
        snprintf(out->error, sizeof(out->error), "Connection failed");
        free(response_buf);
        return ESP_FAIL;
    }

    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, response_buf);
        snprintf(out->error, sizeof(out->error), "HTTP %d", status);
        free(response_buf);
        return ESP_FAIL;
    }

    /* Parse JSON */
    cJSON *root = cJSON_Parse(response_buf);
    free(response_buf);

    if (!root) {
        snprintf(out->error, sizeof(out->error), "JSON parse error");
        return ESP_FAIL;
    }

    parse_tier(cJSON_GetObjectItem(root, "five_hour"), &out->five_hour);
    parse_tier(cJSON_GetObjectItem(root, "seven_day"), &out->seven_day);
    out->valid = true;

    ESP_LOGI(TAG, "Usage: 5h=%.0f%% 7d=%.0f%%",
             out->five_hour.utilization, out->seven_day.utilization);

    cJSON_Delete(root);
    return ESP_OK;
}
