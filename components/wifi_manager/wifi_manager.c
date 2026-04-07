#include <string.h>
#include <stdio.h>
#include "wifi_manager.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE   "wifi_mgr"
#define MAX_STA_RETRIES 5

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* ---------- state ---------- */
static wifi_mgr_state_t s_state = WIFI_MGR_STATE_INIT;
static wifi_mgr_state_cb_t s_state_cb = NULL;
static void *s_state_cb_arg = NULL;
static char s_ap_ssid[33] = {0};
static char s_sta_ip[16] = {0};
static int s_retry_count = 0;
static EventGroupHandle_t s_wifi_event_group = NULL;

/* HTTP & DNS handles */
static httpd_handle_t s_httpd = NULL;
static dns_server_handle_t s_dns = NULL;

/* Embedded portal HTML */
#include "portal_html.h"

/* ---------- helpers ---------- */

static void set_state(wifi_mgr_state_t new_state)
{
    s_state = new_state;
    ESP_LOGI(TAG, "State -> %d", new_state);
    if (s_state_cb) {
        s_state_cb(new_state, s_state_cb_arg);
    }
}

static esp_err_t nvs_read_str(const char *key, char *out, size_t max_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = max_len;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool has_stored_credentials(void)
{
    char ssid[33];
    return nvs_read_str("ssid", ssid, sizeof(ssid)) == ESP_OK && strlen(ssid) > 0;
}

/* ---------- URL-decode helper ---------- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 1; si++) {
        if (src[si] == '%' && src[si+1] && src[si+2]) {
            int h = hex_val(src[si+1]);
            int l = hex_val(src[si+2]);
            if (h >= 0 && l >= 0) {
                dst[di++] = (char)((h << 4) | l);
                si += 2;
                continue;
            }
        }
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

/* ---------- form parsing ---------- */

static esp_err_t parse_form_field(const char *body, const char *key, char *out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *start = strstr(body, search);
    if (!start) {
        out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    start += strlen(search);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    /* Temporary buffer for encoded value */
    char encoded[256];
    if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
    memcpy(encoded, start, len);
    encoded[len] = '\0';

    url_decode(out, encoded, out_size);
    return ESP_OK;
}

/* ---------- HTTP handlers ---------- */

static esp_err_t http_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, portal_html, strlen(portal_html));
    return ESP_OK;
}

static esp_err_t http_get_scan(httpd_req_t *req)
{
    /* Trigger a WiFi scan */
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    /* Build JSON response */
    char *json = malloc(2048);
    if (!json) {
        free(ap_list);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int pos = 0;
    pos += snprintf(json + pos, 2048 - pos, "[");
    for (int i = 0; i < ap_count; i++) {
        if (i > 0) pos += snprintf(json + pos, 2048 - pos, ",");
        /* Escape SSID for JSON */
        char escaped_ssid[65];
        int ei = 0;
        for (int si = 0; ap_list[i].ssid[si] && ei < 62; si++) {
            char ch = (char)ap_list[i].ssid[si];
            if (ch == '"' || ch == '\\') {
                escaped_ssid[ei++] = '\\';
            }
            escaped_ssid[ei++] = ch;
        }
        escaped_ssid[ei] = '\0';
        pos += snprintf(json + pos, 2048 - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d}",
                        escaped_ssid, ap_list[i].rssi);
    }
    pos += snprintf(json + pos, 2048 - pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);

    free(json);
    free(ap_list);
    return ESP_OK;
}

static void reboot_timer_cb(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Rebooting after provisioning...");
    esp_restart();
}

static esp_err_t http_post_connect(httpd_req_t *req)
{
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    char refresh_tk[256] = {0};
    char timezone[64] = {0};
    char fetch_int[8] = {0};

    parse_form_field(buf, "ssid", ssid, sizeof(ssid));
    parse_form_field(buf, "password", password, sizeof(password));
    parse_form_field(buf, "refresh_tk", refresh_tk, sizeof(refresh_tk));
    parse_form_field(buf, "timezone", timezone, sizeof(timezone));
    parse_form_field(buf, "fetch_int", fetch_int, sizeof(fetch_int));

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saving credentials: SSID='%s'", ssid);
    nvs_write_str("ssid", ssid);
    nvs_write_str("password", password);
    if (strlen(refresh_tk) > 0) nvs_write_str("refresh_tk", refresh_tk);
    if (strlen(timezone) > 0) nvs_write_str("timezone", timezone);
    if (strlen(fetch_int) > 0) nvs_write_str("fetch_int", fetch_int);

    /* Send success response */
    const char *resp = "<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:sans-serif;text-align:center;padding:40px;}</style>"
        "</head><body><h2>WiFi Configured!</h2>"
        "<p>Device will reboot and connect to your network.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));

    /* Schedule reboot in 2 seconds */
    TimerHandle_t reboot_timer = xTimerCreate("reboot", pdMS_TO_TICKS(2000),
                                               pdFALSE, NULL, reboot_timer_cb);
    xTimerStart(reboot_timer, 0);

    return ESP_OK;
}

/* Captive portal: redirect all unknown URLs to / */
static esp_err_t http_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---------- Station mode: config page ---------- */

static esp_err_t http_get_config(httpd_req_t *req)
{
    wifi_mgr_credentials_t *creds = malloc(sizeof(wifi_mgr_credentials_t));
    if (!creds) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    wifi_mgr_get_credentials(creds);

    /* Mask token for display: show first 20 chars + "..." */
    char rt_display[28] = {0};
    if (strlen(creds->refresh_token) > 20) {
        snprintf(rt_display, sizeof(rt_display), "%.20s...", creds->refresh_token);
    } else if (strlen(creds->refresh_token) > 0) {
        strncpy(rt_display, creds->refresh_token, sizeof(rt_display) - 1);
    }

    char *page = malloc(4096);
    if (!page) {
        free(creds);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int len = snprintf(page, 4096,
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Claude Monitor</title>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{font-family:-apple-system,sans-serif;max-width:400px;margin:0 auto;padding:20px;background:#f5f5f5;color:#333}"
        "h2{margin:0 0 20px;text-align:center}"
        "label{display:block;margin:12px 0 4px;font-weight:600;font-size:14px}"
        "input,textarea,select,button{width:100%%;padding:10px;margin:0 0 4px;border:1px solid #ccc;border-radius:6px;font-size:14px}"
        "textarea{height:60px;resize:vertical;font-family:monospace}"
        "button{background:#2563eb;color:#fff;border:none;font-weight:600;cursor:pointer;margin-top:16px}"
        ".info{background:#e0e7ff;padding:12px;border-radius:6px;margin-bottom:16px;font-size:14px}"
        ".ok{background:#d1fae5;color:#065f46;padding:12px;border-radius:6px;text-align:center;display:none}"
        ".hint{font-size:12px;color:#888;margin:2px 0 0}"
        ".cmd{background:#1e293b;color:#e2e8f0;padding:10px;border-radius:6px;font-family:monospace;font-size:12px;word-break:break-all;margin:8px 0;cursor:pointer}"
        ".cmd:active{background:#334155}"
        "</style></head><body>"
        "<h2>Claude Monitor</h2>"
        "<div class=\"info\">WiFi: <b>%s</b><br>IP: %s</div>"
        "<div class=\"ok\" id=\"ok\">Saved!</div>"
        "<form id=\"f\">"
        "<label>Timezone</label>"
        "<select name=\"timezone\" id=\"tz\">"
        "<option value=\"CET-1CEST,M3.5.0,M10.5.0/3\">Europe/Paris</option>"
        "<option value=\"GMT0BST,M3.5.0/1,M10.5.0\">Europe/London</option>"
        "<option value=\"EET-2EEST,M3.5.0/3,M10.5.0/4\">Europe/Helsinki</option>"
        "<option value=\"EST5EDT,M3.2.0,M11.1.0\">US/Eastern</option>"
        "<option value=\"CST6CDT,M3.2.0,M11.1.0\">US/Central</option>"
        "<option value=\"MST7MDT,M3.2.0,M11.1.0\">US/Mountain</option>"
        "<option value=\"PST8PDT,M3.2.0,M11.1.0\">US/Pacific</option>"
        "<option value=\"JST-9\">Asia/Tokyo</option>"
        "<option value=\"CST-8\">Asia/Shanghai</option>"
        "<option value=\"IST-5:30\">Asia/Kolkata</option>"
        "<option value=\"AEST-10AEDT,M10.1.0,M4.1.0/3\">Australia/Sydney</option>"
        "<option value=\"UTC0\">UTC</option>"
        "</select>"
        "<label>Fetch Interval</label>"
        "<select name=\"fetch_int\" id=\"fi\">"
        "<option value=\"1\">1 min</option>"
        "<option value=\"2\">2 min</option>"
        "<option value=\"5\">5 min</option>"
        "<option value=\"10\">10 min</option>"
        "<option value=\"15\">15 min</option>"
        "<option value=\"30\">30 min</option>"
        "</select>"
        "<label>Refresh Token</label>"
        "<p class=\"hint\">Run this in a terminal, then paste the result:</p>"
        "<div class=\"cmd\" onclick=\"navigator.clipboard.writeText(this.textContent)\">jq -r '.claudeAiOauth.refreshToken' ~/.claude/.credentials.json</div>"
        "<textarea name=\"refresh_tk\" id=\"rt\" placeholder=\"Paste refresh token here\"></textarea>"
        "<p class=\"hint\">Current: %s</p>"
        "<button type=\"submit\">Save</button>"
        "</form>"
        "<script>"
        "document.getElementById('tz').value='%s'||'CET-1CEST,M3.5.0,M10.5.0/3';"
        "document.getElementById('fi').value='%s'||'10';"
        "document.getElementById('f').onsubmit=function(e){"
        "e.preventDefault();"
        "var b='timezone='+encodeURIComponent(document.getElementById('tz').value)"
        "+'&fetch_int='+encodeURIComponent(document.getElementById('fi').value)"
        "+'&refresh_tk='+encodeURIComponent(document.getElementById('rt').value);"
        "fetch('/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
        ".then(function(){var o=document.getElementById('ok');o.style.display='block';setTimeout(function(){o.style.display='none'},2000)})"
        "};"
        "</script></body></html>",
        creds->ssid, s_sta_ip, rt_display, creds->timezone, creds->fetch_interval);

    free(creds);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, len);
    free(page);
    return ESP_OK;
}

static esp_err_t http_post_config(httpd_req_t *req)
{
    char *buf = malloc(1024);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, buf, 1023);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char refresh_tk[256] = {0};
    char timezone[64] = {0};
    char fetch_int[8] = {0};
    parse_form_field(buf, "refresh_tk", refresh_tk, sizeof(refresh_tk));
    parse_form_field(buf, "timezone", timezone, sizeof(timezone));
    parse_form_field(buf, "fetch_int", fetch_int, sizeof(fetch_int));
    free(buf);

    /* Only update non-empty fields */
    if (strlen(refresh_tk) > 0) nvs_write_str("refresh_tk", refresh_tk);
    if (strlen(timezone) > 0) nvs_write_str("timezone", timezone);
    if (strlen(fetch_int) > 0) nvs_write_str("fetch_int", fetch_int);
    ESP_LOGI(TAG, "Config updated");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ---------- HTTP server ---------- */

static void start_http_server_provisioning(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = http_get_root };
    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = http_get_scan };
    httpd_uri_t connect = { .uri = "/connect", .method = HTTP_POST, .handler = http_post_connect };
    httpd_uri_t redirect = { .uri = "/*", .method = HTTP_GET, .handler = http_redirect_handler };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &connect);
    httpd_register_uri_handler(s_httpd, &redirect);

    ESP_LOGI(TAG, "HTTP server started (provisioning)");
}

static void start_http_server_station(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = http_get_config };
    httpd_uri_t cfg = { .uri = "/config", .method = HTTP_POST, .handler = http_post_config };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &cfg);

    ESP_LOGI(TAG, "HTTP server started (station)");
}

/* ---------- WiFi event handler ---------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_state == WIFI_MGR_STATE_CONNECTING) {
                if (s_retry_count < MAX_STA_RETRIES) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retry %d/%d...", s_retry_count, MAX_STA_RETRIES);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "Connection failed after %d retries", MAX_STA_RETRIES);
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
            }
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_sta_ip);
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ---------- SoftAP provisioning mode ---------- */

static void start_provisioning(void)
{
    /* Create AP netif */
    esp_netif_create_default_wifi_ap();

    /* Configure SoftAP */
    wifi_config_t wifi_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 1,
        },
    };
    strncpy((char *)wifi_cfg.ap.ssid, s_ap_ssid, sizeof(wifi_cfg.ap.ssid));
    wifi_cfg.ap.ssid_len = strlen(s_ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: %s", s_ap_ssid);

    /* Start DNS server — redirect all queries to our IP */
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);

    /* Start HTTP captive portal */
    start_http_server_provisioning();

    /* Update display AFTER WiFi is fully started (avoids SPI/NVS flash conflict) */
    set_state(WIFI_MGR_STATE_PROVISIONING);
}

/* ---------- Station mode ---------- */

static void sta_connect_task(void *arg)
{
    wifi_mgr_credentials_t creds;
    wifi_mgr_get_credentials(&creds);

    /* Create STA netif */
    esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, creds.ssid, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, creds.password, sizeof(wifi_cfg.sta.password));
    /* Enable WPA2/WPA3 and fallback */
    wifi_cfg.sta.threshold.authmode = strlen(creds.password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Update display AFTER WiFi is started */
    set_state(WIFI_MGR_STATE_CONNECTING);

    ESP_LOGI(TAG, "Connecting to '%s'...", creds.ssid);

    /* Wait for connection result */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdTRUE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        start_http_server_station();
        set_state(WIFI_MGR_STATE_CONNECTED);
    } else {
        set_state(WIFI_MGR_STATE_FAILED);
    }

    vTaskDelete(NULL);
}

/* ---------- Public API ---------- */

esp_err_t wifi_mgr_init(wifi_mgr_state_cb_t state_cb, void *cb_arg)
{
    s_state_cb = state_cb;
    s_state_cb_arg = cb_arg;

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupted, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize network stack and event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialize WiFi with default config */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, NULL));

    /* Create event group */
    s_wifi_event_group = xEventGroupCreate();

    /* Generate AP SSID from MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ClaudeMonitor-%02X%02X", mac[4], mac[5]);

    ESP_LOGI(TAG, "WiFi manager initialized (AP SSID: %s)", s_ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_mgr_start(void)
{
    if (has_stored_credentials()) {
        ESP_LOGI(TAG, "Credentials found, connecting as station...");
        xTaskCreate(sta_connect_task, "sta_connect", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGI(TAG, "No credentials, starting provisioning...");
        start_provisioning();
    }
    return ESP_OK;
}

wifi_mgr_state_t wifi_mgr_get_state(void)
{
    return s_state;
}

esp_err_t wifi_mgr_get_credentials(wifi_mgr_credentials_t *creds)
{
    memset(creds, 0, sizeof(*creds));
    esp_err_t err = nvs_read_str("ssid", creds->ssid, sizeof(creds->ssid));
    if (err != ESP_OK) return err;
    nvs_read_str("password", creds->password, sizeof(creds->password));
    nvs_read_str("access_tk", creds->access_token, sizeof(creds->access_token));
    nvs_read_str("refresh_tk", creds->refresh_token, sizeof(creds->refresh_token));
    nvs_read_str("timezone", creds->timezone, sizeof(creds->timezone));
    nvs_read_str("fetch_int", creds->fetch_interval, sizeof(creds->fetch_interval));
    return ESP_OK;
}

esp_err_t wifi_mgr_erase_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Credentials erased");
    return ESP_OK;
}

const char *wifi_mgr_get_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *wifi_mgr_get_sta_ip(void)
{
    return s_sta_ip;
}

esp_err_t wifi_mgr_update_tokens(const char *access_token, const char *refresh_token)
{
    esp_err_t err = ESP_OK;
    if (access_token) {
        err = nvs_write_str("access_tk", access_token);
    }
    if (refresh_token && err == ESP_OK) {
        err = nvs_write_str("refresh_tk", refresh_token);
    }
    ESP_LOGI(TAG, "Tokens updated in NVS");
    return err;
}
