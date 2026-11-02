/*
 * SSHTerminal Implementation
 * Core SSH terminal functionality including WiFi connectivity, libssh2 session management,
 * command execution, terminal display rendering with LVGL, and command history management.
 */

#include "ssh_terminal.hpp"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "bsp/esp-bsp.h"
#include <cstring>
#include <cctype>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "SSH_TERMINAL";

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;
#define WIFI_MAXIMUM_RETRY 5

static esp_event_handler_instance_t s_instance_any_id = NULL;
static esp_event_handler_instance_t s_instance_got_ip = NULL;
static bool s_wifi_driver_started = false;
static esp_netif_t* s_sta_netif = NULL;
static uint8_t s_last_disconnect_reason = 0;
static SSHTerminal* s_terminal_instance = NULL;

SSHTerminal::SSHTerminal() 
    : terminal_screen(NULL), 
      terminal_output(NULL), 
      input_label(NULL),
      input_container(NULL),
      status_bar(NULL),
      byte_counter_label(NULL),
      side_panel(NULL),
      scroll_indicator_label(NULL),
      ctrl_c_btn(NULL),
      esc_btn(NULL),
      alt_btn(NULL),
      alt_btn_lbl(NULL),
      keys_btn(NULL),
      activity_label(NULL),
      alt_hud_panel(NULL),
      alt_mode(false),
      is_user_scrolled_back(false),
      ssh_cmd_buffer(""),
      ssh_at_line_start(true),
      is_local_mode_during_ssh(false),
      cursor_pos(0),
      bytes_received(0),
      history_index(-1),
      cursor_blink_timer(NULL),
      cursor_visible(true),
      battery_update_timer(NULL),
      history_needs_save(false),
      history_save_timer(NULL),
      last_display_update(0),
      wifi_connected(false),
      ssh_connected(false),
      battery_initialized(false),
      ssh_socket(-1),
      session(NULL),
      channel(NULL),
      ssh_mutex(NULL),
      hostname(NULL),
      port_number(22),
      ssh_rx_paused(false),
      ssh_rx_is_paused(false),
      m_is_virtual_target(false),
      getkey_dialog(NULL),
      getkey_path_ta(NULL)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "Initializing battery measurement...");
    esp_err_t battery_ret = battery.init();
    if (battery_ret == ESP_OK) {
        battery_initialized = true;
        ESP_LOGI(TAG, "Battery measurement initialized successfully");
        float test_voltage = battery.readBatteryVoltage();
        ESP_LOGI(TAG, "Test battery read: %.2fV", test_voltage);
    } else {
        battery_initialized = false;
        ESP_LOGE(TAG, "Battery measurement initialization FAILED: %s", esp_err_to_name(battery_ret));
    }
    
    s_terminal_instance = this;
    connected_ssid = "";
    ssh_mutex = xSemaphoreCreateRecursiveMutex();
    load_history_from_nvs();
    load_keys_from_nvs();
}

SSHTerminal::~SSHTerminal() 
{
    disconnect();
    if (ssh_mutex) {
        vSemaphoreDelete(ssh_mutex);
        ssh_mutex = NULL;
    }
    if (hostname) {
        free(hostname);
    }
    if (cursor_blink_timer) {
        lv_timer_del(cursor_blink_timer);
    }
    if (battery_update_timer) {
        lv_timer_del(battery_update_timer);
    }
    if (history_save_timer) {
        lv_timer_del(history_save_timer);
        if (history_needs_save) {
            save_history_to_nvs();
        }
    }
}

const char* SSHTerminal::wifi_reason_to_string(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED: return "Unspecified error";
        case WIFI_REASON_AUTH_EXPIRE: return "Auth expired";
        case WIFI_REASON_AUTH_LEAVE: return "Auth leave";
        case WIFI_REASON_ASSOC_EXPIRE: return "Assoc expired";
        case WIFI_REASON_ASSOC_TOOMANY: return "Too many associations";
        case WIFI_REASON_NOT_AUTHED: return "Not authenticated";
        case WIFI_REASON_NOT_ASSOCED: return "Not associated";
        case WIFI_REASON_ASSOC_LEAVE: return "Assoc leave";
        case WIFI_REASON_ASSOC_NOT_AUTHED: return "Assoc not authenticated";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "Disassoc bad power cap";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "Disassoc bad channel";
        case WIFI_REASON_IE_INVALID: return "Invalid IE";
        case WIFI_REASON_MIC_FAILURE: return "MIC failure";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout (wrong password or encryption mismatch)";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "Group key timeout";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE in 4-way differs";
        case WIFI_REASON_GROUP_CIPHER_INVALID: return "Group cipher invalid";
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "Pairwise cipher invalid";
        case WIFI_REASON_AKMP_INVALID: return "AKMP invalid (unsupported security/cipher)";
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "Unsupported RSN IE version";
        case WIFI_REASON_INVALID_RSN_IE_CAP: return "Invalid RSN IE cap";
        case WIFI_REASON_802_1X_AUTH_FAILED: return "802.1X auth failed";
        case WIFI_REASON_CIPHER_SUITE_REJECTED: return "Cipher suite rejected";
        case WIFI_REASON_BEACON_TIMEOUT: return "AP beacon timeout (signal too weak or out of range)";
        case WIFI_REASON_NO_AP_FOUND: return "AP not found (verify SSID; note ESP32 only supports 2.4 GHz)";
        case WIFI_REASON_AUTH_FAIL: return "Authentication failed (wrong password)";
        case WIFI_REASON_ASSOC_FAIL: return "Association failed";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "Handshake timeout (wrong password or encryption mismatch)";
        case WIFI_REASON_CONNECTION_FAIL: return "Connection failed";
        case WIFI_REASON_AP_TSF_RESET: return "AP TSF reset";
        case WIFI_REASON_ROAMING: return "Station roaming";
        default: return "Unknown WiFi error";
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    SSHTerminal* terminal = s_terminal_instance ? s_terminal_instance : (SSHTerminal*)arg;
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* dis = (wifi_event_sta_disconnected_t*) event_data;
        s_last_disconnect_reason = dis ? dis->reason : 0;
        ESP_LOGW(TAG, "WiFi disconnected, reason: %d (%s), retry: %d/%d",
                 s_last_disconnect_reason, SSHTerminal::wifi_reason_to_string(s_last_disconnect_reason),
                 s_retry_num, WIFI_MAXIMUM_RETRY);
                 
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            s_retry_num++;
            esp_wifi_connect();
        } else {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        s_retry_num = 0;
        s_last_disconnect_reason = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        
        if (terminal) {
            char ip_str[64];
            snprintf(ip_str, sizeof(ip_str), "\nWiFi Connected! IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
            terminal->append_text(ip_str);
        }
    }
}

esp_err_t SSHTerminal::ensure_wifi_driver_started(SSHTerminal* terminal)
{
    if (terminal) {
        s_terminal_instance = terminal;
    }
    if (terminal && terminal->is_virtual_target()) {
        return ESP_OK;
    }
    if (s_wifi_driver_started) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing WiFi driver and netif stack...");
    
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
    }
    
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }
    
    if (!s_instance_any_id) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            terminal,
                                                            &s_instance_any_id));
    }
    if (!s_instance_got_ip) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            terminal,
                                                            &s_instance_got_ip));
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Set country to worldwide to enable channels 1-13
    wifi_country_t country = {
        .cc = "01",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_AUTO
    };
    esp_wifi_set_country(&country);
    
    // Disable modem power save to ensure low latency and prevent packet/connection loss
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    s_wifi_driver_started = true;
    ESP_LOGI(TAG, "WiFi driver started successfully");
    return ESP_OK;
}

esp_err_t SSHTerminal::init_wifi(const char* ssid, const char* password)
{
    if (!ssid || strlen(ssid) == 0) {
        append_text("ERROR: SSID cannot be empty\n");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (m_is_virtual_target) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Connecting to '%s' (Virtual Target)...\n", ssid);
        append_text(buf);
        vTaskDelay(pdMS_TO_TICKS(300));
        wifi_connected = true;
        connected_ssid = ssid;
        append_text("WiFi Connected! IP Address: 192.168.1.100\n");
        update_status_bar();
        return ESP_OK;
    }
    
    ensure_wifi_driver_started(this);
    
    s_retry_num = 0;
    s_last_disconnect_reason = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    // Disconnect previous connection cleanly
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password && strlen(password) > 0) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    
    // Accept all security modes (OPEN, WEP, WPA, WPA2, WPA3)
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    
    // PMF configuration (capable=true allows both WPA2 non-PMF and WPA3 PMF)
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    // WPA3 Personal (SAE) with Hash-to-Element (H2E) and Hunting-and-Pecking
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.sta.sae_h2e_identifier[0] = '\0';
    
    // Scan all channels to pick strongest AP (vital for mesh & dual-band setups)
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.rssi = -127;
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
    
    const int max_wait_ms = 15000;
    const int check_interval_ms = 500;
    int elapsed_ms = 0;
    
    while (elapsed_ms < max_wait_ms) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(check_interval_ms));
        
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to AP SSID:%s", ssid);
            wifi_connected = true;
            connected_ssid = ssid;
            update_status_bar();
            save_wifi_config(ssid, password ? password : "");
            return ESP_OK;
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGI(TAG, "Failed to connect to SSID:%s", ssid);
            wifi_connected = false;
            update_status_bar();
            return ESP_FAIL;
        }
        
        append_text(".");
        elapsed_ms += check_interval_ms;
    }
    
    ESP_LOGE(TAG, "Connection timeout to SSID:%s", ssid);
    wifi_connected = false;
    s_retry_num = 0;
    update_status_bar();
    return ESP_ERR_TIMEOUT;
}

bool SSHTerminal::is_wifi_connected()
{
    return wifi_connected;
}

esp_err_t SSHTerminal::scan_wifi()
{
    if (m_is_virtual_target) {
        append_text("Scanning 2.4 GHz WiFi networks (Virtual Target)...\n");
        vTaskDelay(pdMS_TO_TICKS(300));
        cached_scan_results.clear();
        wifi_ap_record_t sim_ap = {};
        strncpy((char*)sim_ap.ssid, "Wokwi-GUEST", sizeof(sim_ap.ssid) - 1);
        sim_ap.rssi = -45;
        sim_ap.authmode = WIFI_AUTH_OPEN;
        sim_ap.primary = 6;
        cached_scan_results.push_back(sim_ap);
        
        append_text("Found 1 network (Virtual Target):\n #  SSID                 RSSI   CH  AUTH\n");
        append_text(" 1  Wokwi-GUEST          -45    6   Open\n\n");
        append_text("Type 'connect 1' or 'connect Wokwi-GUEST' to connect.\n");
        return ESP_OK;
    }
    
    ensure_wifi_driver_started(this);
    
    append_text("Scanning 2.4 GHz WiFi networks...\n");
    
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 120;
    scan_config.scan_time.active.max = 250;
    
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        char err_buf[80];
        snprintf(err_buf, sizeof(err_buf), "Scan failed (%s). Try 'disconnect' first.\n", esp_err_to_name(err));
        append_text(err_buf);
        return err;
    }
    
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        append_text("No 2.4 GHz WiFi networks found.\n(Ensure 2.4 GHz band is enabled on your router)\n\n");
        return ESP_OK;
    }
    
    std::vector<wifi_ap_record_t> ap_records(ap_count);
    esp_wifi_scan_get_ap_records(&ap_count, ap_records.data());
    
    // Sort all APs by RSSI descending
    std::sort(ap_records.begin(), ap_records.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });
    
    // Deduplicate SSIDs, keeping strongest
    std::vector<wifi_ap_record_t> unique_aps;
    std::set<std::string> seen;
    for (const auto& ap : ap_records) {
        std::string s((char*)ap.ssid);
        if (s.empty()) continue;
        if (seen.find(s) == seen.end()) {
            seen.insert(s);
            unique_aps.push_back(ap);
        }
    }
    cached_scan_results = unique_aps;
    
    std::string scan_result;
    char header[96];
    snprintf(header, sizeof(header), "Found %d networks:\n #  SSID                 RSSI   CH  AUTH\n", (int)unique_aps.size());
    scan_result += header;
    
    for (size_t i = 0; i < unique_aps.size() && i < 15; i++) {
        const auto& ap = unique_aps[i];
        const char* auth_str = "Open";
        switch (ap.authmode) {
            case WIFI_AUTH_OPEN: auth_str = "Open"; break;
            case WIFI_AUTH_WEP: auth_str = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: auth_str = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth_str = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE: auth_str = "WPA2-Ent"; break;
            case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2/WPA3"; break;
            case WIFI_AUTH_WAPI_PSK: auth_str = "WAPI"; break;
            case WIFI_AUTH_OWE: auth_str = "OWE"; break;
            default: auth_str = "Other"; break;
        }
        
        char line[128];
        std::string ssid_disp = (char*)ap.ssid;
        if (ssid_disp.length() > 18) {
            ssid_disp = ssid_disp.substr(0, 15) + "...";
        }
        snprintf(line, sizeof(line), "[%2d] %-18s %3ddBm %2d  %-9s\n",
                 (int)(i + 1), ssid_disp.c_str(), (int)ap.rssi, (int)ap.primary, auth_str);
        scan_result += line;
    }
    
    scan_result += "\nConnect: connect <#> [password]\n\n";
    append_text(scan_result.c_str());
    return ESP_OK;
}

void SSHTerminal::show_wifi_status()
{
    if (m_is_virtual_target) {
        char status_buf[256];
        snprintf(status_buf, sizeof(status_buf),
                 "\n=== WiFi Status ===\n"
                 " State:    %s\n"
                 " SSID:     %s\n"
                 " IP:       %s\n"
                 " Mode:     Virtual ESP32-S3 Target (Emulated WiFi)\n"
                 "===================\n\n",
                 wifi_connected ? "Connected" : "Disconnected",
                 connected_ssid.empty() ? "(none)" : connected_ssid.c_str(),
                 wifi_connected ? "192.168.1.100" : "(none)");
        append_text(status_buf);
        return;
    }
    
    ensure_wifi_driver_started(this);
    
    append_text("\n=== WiFi Status ===\n");
    if (wifi_connected) {
        char buf[128];
        snprintf(buf, sizeof(buf), " State:    Connected\n SSID:     %s\n", connected_ssid.c_str());
        append_text(buf);
        
        esp_netif_ip_info_t ip_info;
        if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            snprintf(buf, sizeof(buf), " IP:       " IPSTR "\n Netmask:  " IPSTR "\n Gateway:  " IPSTR "\n",
                     IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
            append_text(buf);
        }
        
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            snprintf(buf, sizeof(buf), " Signal:   %d dBm (CH %d)\n", ap_info.rssi, ap_info.primary);
            append_text(buf);
        }
    } else {
        append_text(" State:    Disconnected\n");
        if (s_last_disconnect_reason != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), " Last err: [%d] %s\n",
                     s_last_disconnect_reason, SSHTerminal::wifi_reason_to_string(s_last_disconnect_reason));
            append_text(buf);
        }
    }
    
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), " MAC:      %02X:%02X:%02X:%02X:%02X:%02X\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        append_text(buf);
    }
    
    std::string saved_ssid, saved_pass;
    if (load_wifi_config(saved_ssid, saved_pass)) {
        char buf[128];
        snprintf(buf, sizeof(buf), " Saved:    \"%s\" (Auto-connect: %s)\n",
                 saved_ssid.c_str(), get_wifi_autoconnect_enabled() ? "ON" : "OFF");
        append_text(buf);
    } else {
        append_text(" Saved:    None\n");
    }
    append_text("===================\n\n");
}

static bool save_wifi_to_sd(const char* ssid, const char* password, bool autoconnect)
{
    if (!is_sd_card_mounted()) {
        ESP_LOGW("WIFI", "Cannot save WiFi to SD: SD card not mounted");
        return false;
    }

    // 1. INI format (/sdcard/wifi.cfg)
    FILE* f = fopen("/sdcard/wifi.cfg", "w");
    if (f) {
        fprintf(f, "ssid=%s\n", ssid ? ssid : "");
        fprintf(f, "password=%s\n", password ? password : "");
        fprintf(f, "autoconnect=%d\n", autoconnect ? 1 : 0);
        fclose(f);
        ESP_LOGI("WIFI", "WiFi saved to /sdcard/wifi.cfg");
    } else {
        ESP_LOGE("WIFI", "Failed to open /sdcard/wifi.cfg for writing");
    }

    // 2. JSON format (/sdcard/wificonfig.json)
    FILE* jf = fopen("/sdcard/wificonfig.json", "w");
    if (jf) {
        fprintf(jf, "{\n  \"ssid\": \"%s\",\n  \"password\": \"%s\",\n  \"autoconnect\": %s\n}\n",
                ssid ? ssid : "", password ? password : "", autoconnect ? "true" : "false");
        fclose(jf);
        ESP_LOGI("WIFI", "WiFi saved to /sdcard/wificonfig.json");
    }

    return (f != NULL || jf != NULL);
}

static bool load_wifi_from_sd(std::string& ssid, std::string& password)
{
    if (!is_sd_card_mounted()) {
        return false;
    }

    // Try /sdcard/wifi.cfg
    FILE* f = fopen("/sdcard/wifi.cfg", "r");
    if (f) {
        char line[256];
        std::string s, p;
        while (fgets(line, sizeof(line), f)) {
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n')) {
                line[l - 1] = '\0';
                l--;
            }
            if (strncmp(line, "ssid=", 5) == 0) {
                s = line + 5;
            } else if (strncmp(line, "password=", 9) == 0) {
                p = line + 9;
            } else if (strncmp(line, "pass=", 5) == 0) {
                p = line + 5;
            }
        }
        fclose(f);
        if (!s.empty()) {
            ssid = s;
            password = p;
            ESP_LOGI("WIFI", "Loaded WiFi from /sdcard/wifi.cfg: SSID='%s'", ssid.c_str());
            return true;
        }
    }

    // Try /sdcard/wificonfig.json
    FILE* jf = fopen("/sdcard/wificonfig.json", "r");
    if (jf) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, jf);
        fclose(jf);
        buf[n] = '\0';
        std::string json(buf);
        
        size_t s_pos = json.find("\"ssid\"");
        if (s_pos != std::string::npos) {
            size_t colon = json.find(":", s_pos);
            size_t q1 = json.find("\"", colon + 1);
            size_t q2 = (q1 != std::string::npos) ? json.find("\"", q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
                ssid = json.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        size_t p_pos = json.find("\"password\"");
        if (p_pos == std::string::npos) p_pos = json.find("\"pass\"");
        if (p_pos != std::string::npos) {
            size_t colon = json.find(":", p_pos);
            size_t q1 = json.find("\"", colon + 1);
            size_t q2 = (q1 != std::string::npos) ? json.find("\"", q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
                password = json.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        if (!ssid.empty()) {
            ESP_LOGI("WIFI", "Loaded WiFi from /sdcard/wificonfig.json: SSID='%s'", ssid.c_str());
            return true;
        }
    }

    return false;
}

void SSHTerminal::save_wifi_config(const char* ssid, const char* password)
{
    if (!ssid || strlen(ssid) == 0) return;
    save_wifi_to_sd(ssid, password, get_wifi_autoconnect_enabled());
    save_wifi_to_nvs(ssid, password);
}

bool SSHTerminal::load_wifi_config(std::string& ssid, std::string& password)
{
    if (load_wifi_from_sd(ssid, password)) {
        return true;
    }
    return load_wifi_from_nvs(ssid, password);
}

void SSHTerminal::clear_wifi_config()
{
    if (is_sd_card_mounted()) {
        unlink("/sdcard/wifi.cfg");
        unlink("/sdcard/wificonfig.json");
        ESP_LOGI("WIFI", "Erased WiFi configs from SD card");
    }
    clear_wifi_nvs();
}

void SSHTerminal::save_wifi_to_nvs(const char* ssid, const char* password)
{
    if (!ssid || strlen(ssid) == 0) return;
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for saving wifi: %s", esp_err_to_name(err));
        return;
    }
    
    nvs_set_str(nvs_handle, "ssid", ssid);
    nvs_set_str(nvs_handle, "pass", password ? password : "");
    uint8_t autoconn = 1;
    nvs_get_u8(nvs_handle, "autoconn", &autoconn);
    nvs_set_u8(nvs_handle, "autoconn", autoconn);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "WiFi saved to NVS: %s", ssid);
}

bool SSHTerminal::load_wifi_from_nvs(std::string& ssid, std::string& password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return false;
    
    size_t ssid_len = 0;
    err = nvs_get_str(nvs_handle, "ssid", NULL, &ssid_len);
    if (err != ESP_OK || ssid_len == 0) {
        nvs_close(nvs_handle);
        return false;
    }
    
    std::vector<char> ssid_buf(ssid_len);
    nvs_get_str(nvs_handle, "ssid", ssid_buf.data(), &ssid_len);
    ssid = ssid_buf.data();
    
    size_t pass_len = 0;
    err = nvs_get_str(nvs_handle, "pass", NULL, &pass_len);
    if (err == ESP_OK && pass_len > 0) {
        std::vector<char> pass_buf(pass_len);
        nvs_get_str(nvs_handle, "pass", pass_buf.data(), &pass_len);
        password = pass_buf.data();
    } else {
        password = "";
    }
    
    nvs_close(nvs_handle);
    return !ssid.empty();
}

void SSHTerminal::clear_wifi_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_erase_key(nvs_handle, "ssid");
        nvs_erase_key(nvs_handle, "pass");
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Saved WiFi credentials erased from NVS");
    }
}

bool SSHTerminal::get_wifi_autoconnect_enabled()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return true;
    uint8_t autoconn = 1;
    nvs_get_u8(nvs_handle, "autoconn", &autoconn);
    nvs_close(nvs_handle);
    return autoconn != 0;
}

void SSHTerminal::set_wifi_autoconnect_enabled(bool enabled)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_u8(nvs_handle, "autoconn", enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

void SSHTerminal::try_autoconnect()
{
    if (m_is_virtual_target) {
        return;
    }
    ensure_wifi_driver_started(this);
    
    std::string saved_ssid, saved_pass;
    if (load_wifi_config(saved_ssid, saved_pass)) {
        if (get_wifi_autoconnect_enabled()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "\nSaved WiFi: \"%s\" (Auto-connecting...)\n", saved_ssid.c_str());
            append_text(buf);
            xTaskCreate(autoconnect_task, "wifi_autoconn", 4096, this, 3, NULL);
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "\nSaved WiFi: \"%s\" (Auto-connect OFF, type 'connect' to use)\n", saved_ssid.c_str());
            append_text(buf);
        }
    }
}

void SSHTerminal::autoconnect_task(void* param)
{
    SSHTerminal* terminal = (SSHTerminal*)param;
    if (!terminal) {
        vTaskDelete(NULL);
        return;
    }
    
    // Allow UI and splash screen to settle
    vTaskDelay(pdMS_TO_TICKS(1500));
    
    std::string saved_ssid, saved_pass;
    if (terminal->load_wifi_from_nvs(saved_ssid, saved_pass)) {
        esp_err_t ret = terminal->init_wifi(saved_ssid.c_str(), saved_pass.c_str());
        if (ret == ESP_OK) {
            terminal->append_text("Auto-connect WiFi successful!\n");
        } else {
            char err_buf[128];
            snprintf(err_buf, sizeof(err_buf), "\nAuto-connect failed [%d]: %s\nType 'scan' or 'connect' to retry.\n\n",
                     s_last_disconnect_reason, SSHTerminal::wifi_reason_to_string(s_last_disconnect_reason));
            terminal->append_text(err_buf);
        }
    }
    
    vTaskDelete(NULL);
}

ssize_t SSHTerminal::ssh_write_all(const char* data, size_t len)
{
    if (!channel || !data || len == 0) return -1;
    if (ssh_mutex) xSemaphoreTakeRecursive(ssh_mutex, portMAX_DELAY);
    if (!channel) {
        if (ssh_mutex) xSemaphoreGiveRecursive(ssh_mutex);
        return -1;
    }
    size_t total = 0;
    int retries = 0;
    while (total < len && retries < 25) {
        ssize_t n = libssh2_channel_write(channel, data + total, len - total);
        if (n > 0) {
            total += n;
        } else if (n == LIBSSH2_ERROR_EAGAIN) {
            retries++;
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            break;
        }
    }
    if (ssh_mutex) xSemaphoreGiveRecursive(ssh_mutex);
    return total;
}

void SSHTerminal::send_interrupt()
{
    if (ssh_connected && channel) {
        ssh_write_all("\x03", 1);
        ESP_LOGI(TAG, "Sent SIGINT (Ctrl+C)");
        set_activity_status("[^C SENT]", 0xFF4040);
        append_text("^C\n");
    } else {
        current_input.clear();
        cursor_pos = 0;
        append_text("^C\n");
        update_input_display();
    }
}

void SSHTerminal::set_activity_status(const char* text, uint32_t color_hex)
{
    if (!activity_label) return;
    if (bsp_display_lock(500)) {
        lv_label_set_text(activity_label, text ? text : "");
        lv_obj_set_style_text_color(activity_label, lv_color_hex(color_hex), 0);
        bsp_display_unlock();
    }
}

void SSHTerminal::ctrl_c_btn_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    if (terminal) {
        terminal->send_interrupt();
    }
}

void SSHTerminal::alt_btn_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    if (terminal) {
        terminal->toggle_alt_mode();
    }
}

void SSHTerminal::esc_btn_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    if (terminal) {
        if (terminal->alt_mode) {
            terminal->set_alt_mode(false);
        }
        if (terminal->ssh_connected && terminal->channel) {
            terminal->ssh_write_all("\x1B", 1);
            terminal->set_activity_status("[ESC]", 0x50FFFF);
        } else {
            terminal->current_input.clear();
            terminal->cursor_pos = 0;
            terminal->append_text(" [esc]\n");
            terminal->update_input_display();
        }
    }
}

void SSHTerminal::keys_btn_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    if (terminal) {
        terminal->toggle_side_panel();
    }
}

lv_obj_t* SSHTerminal::create_terminal_screen()
{
    terminal_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(terminal_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(terminal_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(terminal_screen, LV_OBJ_FLAG_SCROLLABLE);

    status_bar = lv_label_create(terminal_screen);
    lv_label_set_text(status_bar, "Status: Disconnected");
    lv_obj_set_style_text_color(status_bar, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(status_bar, &lv_font_montserrat_12, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_LEFT, 5, 5);
    
    // Live Activity / Wait Indicator (Green / Yellow / Cyan)
    activity_label = lv_label_create(terminal_screen);
    lv_label_set_text(activity_label, "");
    lv_obj_set_style_text_color(activity_label, lv_color_hex(0x50FF50), 0);
    lv_obj_set_style_text_font(activity_label, &lv_font_montserrat_10, 0);
    lv_obj_align(activity_label, LV_ALIGN_TOP_LEFT, 68, 6);

    // Dedicated [ALT] Mode toggle touch button (top right)
    alt_btn = lv_btn_create(terminal_screen);
    lv_obj_set_size(alt_btn, 30, 18);
    lv_obj_align(alt_btn, LV_ALIGN_TOP_RIGHT, -142, 3);
    lv_obj_set_style_bg_color(alt_btn, lv_color_hex(0x181824), 0);
    lv_obj_set_style_bg_color(alt_btn, lv_color_hex(0x403000), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(alt_btn, lv_color_hex(0x5070AA), 0);
    lv_obj_set_style_border_width(alt_btn, 1, 0);
    lv_obj_set_style_radius(alt_btn, 3, 0);
    lv_obj_set_style_pad_all(alt_btn, 0, 0);
    alt_btn_lbl = lv_label_create(alt_btn);
    lv_label_set_text(alt_btn_lbl, "ALT");
    lv_obj_set_style_text_color(alt_btn_lbl, lv_color_hex(0x80B0FF), 0);
    lv_obj_set_style_text_font(alt_btn_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(alt_btn_lbl);
    lv_obj_add_event_cb(alt_btn, alt_btn_event_cb, LV_EVENT_CLICKED, this);

    // Dedicated [ESC] touch button (top right)
    esc_btn = lv_btn_create(terminal_screen);
    lv_obj_set_size(esc_btn, 30, 18);
    lv_obj_align(esc_btn, LV_ALIGN_TOP_RIGHT, -108, 3);
    lv_obj_set_style_bg_color(esc_btn, lv_color_hex(0x152030), 0);
    lv_obj_set_style_bg_color(esc_btn, lv_color_hex(0x3060FF), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(esc_btn, lv_color_hex(0x4080FF), 0);
    lv_obj_set_style_border_width(esc_btn, 1, 0);
    lv_obj_set_style_radius(esc_btn, 3, 0);
    lv_obj_set_style_pad_all(esc_btn, 0, 0);
    lv_obj_t* esc_lbl = lv_label_create(esc_btn);
    lv_label_set_text(esc_lbl, "ESC");
    lv_obj_set_style_text_color(esc_lbl, lv_color_hex(0x70B0FF), 0);
    lv_obj_set_style_text_font(esc_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(esc_lbl);
    lv_obj_add_event_cb(esc_btn, esc_btn_event_cb, LV_EVENT_CLICKED, this);

    // Dedicated Interrupt [^C] touch button (top right)
    ctrl_c_btn = lv_btn_create(terminal_screen);
    lv_obj_set_size(ctrl_c_btn, 30, 18);
    lv_obj_align(ctrl_c_btn, LV_ALIGN_TOP_RIGHT, -74, 3);
    lv_obj_set_style_bg_color(ctrl_c_btn, lv_color_hex(0x281010), 0);
    lv_obj_set_style_bg_color(ctrl_c_btn, lv_color_hex(0xFF0000), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(ctrl_c_btn, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_border_width(ctrl_c_btn, 1, 0);
    lv_obj_set_style_radius(ctrl_c_btn, 3, 0);
    lv_obj_set_style_pad_all(ctrl_c_btn, 0, 0);
    lv_obj_t* ctrl_c_lbl = lv_label_create(ctrl_c_btn);
    lv_label_set_text(ctrl_c_lbl, "^C");
    lv_obj_set_style_text_color(ctrl_c_lbl, lv_color_hex(0xFF6060), 0);
    lv_obj_set_style_text_font(ctrl_c_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(ctrl_c_lbl);
    lv_obj_add_event_cb(ctrl_c_btn, ctrl_c_btn_event_cb, LV_EVENT_CLICKED, this);

    // Dedicated Keys Panel [KEY] touch button (top right)
    keys_btn = lv_btn_create(terminal_screen);
    lv_obj_set_size(keys_btn, 34, 18);
    lv_obj_align(keys_btn, LV_ALIGN_TOP_RIGHT, -36, 3);
    lv_obj_set_style_bg_color(keys_btn, lv_color_hex(0x102810), 0);
    lv_obj_set_style_bg_color(keys_btn, lv_color_hex(0x00CC00), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(keys_btn, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(keys_btn, 1, 0);
    lv_obj_set_style_radius(keys_btn, 3, 0);
    lv_obj_set_style_pad_all(keys_btn, 0, 0);
    lv_obj_t* keys_btn_lbl = lv_label_create(keys_btn);
    lv_label_set_text(keys_btn_lbl, "KEY");
    lv_obj_set_style_text_color(keys_btn_lbl, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(keys_btn_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(keys_btn_lbl);
    lv_obj_add_event_cb(keys_btn, keys_btn_event_cb, LV_EVENT_CLICKED, this);

    byte_counter_label = lv_label_create(terminal_screen);
    lv_label_set_text(byte_counter_label, "0 B");
    lv_obj_set_style_text_color(byte_counter_label, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_text_font(byte_counter_label, &lv_font_montserrat_10, 0);
    lv_obj_align(byte_counter_label, LV_ALIGN_TOP_RIGHT, -4, 6);

    terminal_output = lv_textarea_create(terminal_screen);
    lv_obj_set_size(terminal_output, lv_pct(100), lv_pct(75));
    lv_obj_align(terminal_output, LV_ALIGN_TOP_MID, 0, 25);
    lv_obj_set_style_bg_color(terminal_output, lv_color_black(), 0);
    lv_obj_set_style_text_color(terminal_output, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(terminal_output, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(terminal_output, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(terminal_output, 2, 0);
    lv_obj_set_style_pad_left(terminal_output, 3, 0);
    lv_obj_set_style_pad_right(terminal_output, 3, 0);
    lv_obj_set_style_pad_top(terminal_output, 2, 0);
    lv_obj_set_style_pad_bottom(terminal_output, 2, 0);
    lv_textarea_set_cursor_click_pos(terminal_output, false);
    lv_textarea_set_one_line(terminal_output, false);
    
    // Enable inline rich-text recoloring for full ANSI 256-color support
    lv_obj_t* ta_label = lv_textarea_get_label(terminal_output);
    if (ta_label) {
        lv_label_set_recolor(ta_label, true);
    }
    
    // Auto scrollbar with visible green thumb
    lv_obj_set_scrollbar_mode(terminal_output, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(terminal_output, lv_color_hex(0x00CC00), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(terminal_output, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(terminal_output, 5, LV_PART_SCROLLBAR);
    
    // Enable vertical touch drag scrolling with momentum
    lv_obj_set_scroll_dir(terminal_output, LV_DIR_VER);
    lv_obj_add_flag(terminal_output, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(terminal_output, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(terminal_output, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_snap_x(terminal_output, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(terminal_output, LV_SCROLL_SNAP_NONE);
    
    // Visible blinking green cursor for terminal output
    lv_obj_add_flag(terminal_output, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_opa(terminal_output, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(terminal_output, lv_color_hex(0x00FF00), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(terminal_output, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_width(terminal_output, 2, LV_PART_CURSOR);
    lv_obj_set_style_anim_time(terminal_output, 500, LV_PART_CURSOR);
    
    // Event callback for touch scroll tracking
    lv_obj_add_event_cb(terminal_output, terminal_scroll_event_cb, LV_EVENT_SCROLL, this);
    
    // Floating Scroll indicator badge inside terminal window (y=30), completely clear of top bar
    scroll_indicator_label = lv_label_create(terminal_screen);
    lv_label_set_text(scroll_indicator_label, "[^ SCROLL]");
    lv_obj_set_style_text_color(scroll_indicator_label, lv_color_hex(0xFFFF00), 0); // Yellow
    lv_obj_set_style_text_font(scroll_indicator_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_bg_color(scroll_indicator_label, lv_color_hex(0x181800), 0);
    lv_obj_set_style_bg_opa(scroll_indicator_label, LV_OPA_90, 0);
    lv_obj_set_style_border_color(scroll_indicator_label, lv_color_hex(0xAAAA00), 0);
    lv_obj_set_style_border_width(scroll_indicator_label, 1, 0);
    lv_obj_set_style_radius(scroll_indicator_label, 3, 0);
    lv_obj_set_style_pad_hor(scroll_indicator_label, 5, 0);
    lv_obj_set_style_pad_ver(scroll_indicator_label, 2, 0);
    lv_obj_align(scroll_indicator_label, LV_ALIGN_TOP_RIGHT, -12, 30);
    lv_obj_add_flag(scroll_indicator_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scroll_indicator_label, [](lv_event_t* e) {
        SSHTerminal* term = (SSHTerminal*)lv_event_get_user_data(e);
        if (term) term->scroll_to_bottom();
    }, LV_EVENT_CLICKED, this);

    // Floating HUD hint panel for Alt mode
    alt_hud_panel = lv_obj_create(terminal_screen);
    lv_obj_set_size(alt_hud_panel, 312, 54);
    lv_obj_align(alt_hud_panel, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(alt_hud_panel, lv_color_hex(0x061406), 0);
    lv_obj_set_style_bg_opa(alt_hud_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(alt_hud_panel, lv_color_hex(0xDD9900), 0);
    lv_obj_set_style_border_width(alt_hud_panel, 1, 0);
    lv_obj_set_style_radius(alt_hud_panel, 4, 0);
    lv_obj_set_style_pad_all(alt_hud_panel, 4, 0);
    lv_obj_clear_flag(alt_hud_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(alt_hud_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(alt_hud_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(alt_hud_panel, [](lv_event_t* e) {
        SSHTerminal* term = (SSHTerminal*)lv_event_get_user_data(e);
        if (term) term->set_alt_mode(false);
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* hud_l1 = lv_label_create(alt_hud_panel);
    lv_label_set_text(hud_l1, "[ALT ON] Enter:Local Shell | Space:Tab | Trackball:Arrows");
    lv_obj_set_style_text_color(hud_l1, lv_color_hex(0xFFCC00), 0);
    lv_obj_set_style_text_font(hud_l1, &lv_font_montserrat_10, 0);
    lv_obj_align(hud_l1, LV_ALIGN_TOP_LEFT, 2, 0);

    lv_obj_t* hud_l2 = lv_label_create(alt_hud_panel);
    lv_label_set_text(hud_l2, "Symbols: t:~  p:|  b:\\  e:=  u:[  i:]  o:{  j:}  v:%");
    lv_obj_set_style_text_color(hud_l2, lv_color_hex(0x50FF50), 0);
    lv_obj_set_style_text_font(hud_l2, &lv_font_montserrat_10, 0);
    lv_obj_align(hud_l2, LV_ALIGN_TOP_LEFT, 2, 16);

    lv_obj_t* hud_l3 = lv_label_create(alt_hud_panel);
    lv_label_set_text(hud_l3, "More:    k:<  m:>  q:`  s:$  g:^  n:&  c:^C  z:^Z  d:^D");
    lv_obj_set_style_text_color(hud_l3, lv_color_hex(0x70D0FF), 0);
    lv_obj_set_style_text_font(hud_l3, &lv_font_montserrat_10, 0);
    lv_obj_align(hud_l3, LV_ALIGN_TOP_LEFT, 2, 32);
    lv_obj_add_flag(scroll_indicator_label, LV_OBJ_FLAG_HIDDEN);

    input_container = lv_obj_create(terminal_screen);
    lv_obj_set_size(input_container, lv_pct(100) - 10, 25);
    lv_obj_set_style_bg_opa(input_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_container, 0, 0);
    lv_obj_set_style_pad_all(input_container, 0, 0);
    lv_obj_set_scrollbar_mode(input_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(input_container, LV_DIR_HOR);
    lv_obj_align(input_container, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    
    input_label = lv_label_create(input_container);
    lv_label_set_text(input_label, "> ");
    lv_obj_set_style_text_color(input_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_text_font(input_label, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(input_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(input_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Enable touch events on input label
    lv_obj_add_flag(input_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(input_label, input_touch_event_cb, LV_EVENT_CLICKED, this);

    create_side_panel();
    
    lv_obj_add_event_cb(terminal_screen, gesture_event_cb, LV_EVENT_GESTURE, this);
    lv_obj_clear_flag(terminal_screen, LV_OBJ_FLAG_GESTURE_BUBBLE);
    
    cursor_blink_timer = lv_timer_create(cursor_blink_cb, 500, this);
    
    battery_update_timer = lv_timer_create(battery_update_cb, 60000, this);
    
    history_save_timer = lv_timer_create(history_save_cb, 5000, this);

#ifdef POCKETSSH_VERSION
    const char* logo = 
        "\n"
        "  ================================================\n"
        "         POCKET SSH TERM v" POCKETSSH_VERSION " - ESP32-S3\n"
        "  ================================================\n"
        "\n"
        "  WiFi Commands:\n"
        "   scan                       - Scan 2.4GHz networks\n"
        "   connect [SSID|#] [PASS]    - Connect WiFi (or saved)\n"
        "   save <SSID|#> [PASS]       - Save network to NVS\n"
        "   saved / forget             - View / clear saved WiFi\n"
        "   autoconnect <on|off>       - Auto-connect on boot\n"
        "   status / disconnect        - WiFi status / disconnect\n"
        "  SSH Commands:\n"
        "   ssh <HOST> [PORT] <U> <P>  - SSH password\n"
        "   sshkey <H> [PORT] <U> <K>  - SSH keyfile\n"
        "   exit                       - SSH off\n"
        "   clear                      - Clear screen\n"
        "   version                    - Show version info\n"
        "   help                       - Show all commands\n"
        "\n"
        "  Scroll: Trackball Up/Dn to scroll | Click to bottom\n"
        "  Ready. Type 'scan' or 'connect' to start...\n\n";
#else
    const char* logo = 
        "\n"
        "  ================================================\n"
        "         POCKET SSH TERM v1.3.0 - ESP32-S3\n"
        "  ================================================\n"
        "\n"
        "  WiFi Commands:\n"
        "   scan                       - Scan 2.4GHz networks\n"
        "   connect [SSID|#] [PASS]    - Connect WiFi (or saved)\n"
        "   save <SSID|#> [PASS]       - Save network to NVS\n"
        "   saved / forget             - View / clear saved WiFi\n"
        "   autoconnect <on|off>       - Auto-connect on boot\n"
        "   status / disconnect        - WiFi status / disconnect\n"
        "  SSH Commands:\n"
        "   ssh <HOST> [PORT] <U> <P>  - SSH password\n"
        "   sshkey <H> [PORT] <U> <K>  - SSH keyfile\n"
        "   exit                       - SSH off\n"
        "   clear                      - Clear screen\n"
        "   version                    - Show version info\n"
        "   help                       - Show all commands\n"
        "\n"
        "  Scroll: Trackball Up/Dn to scroll | Click to bottom\n"
        "  Ready. Type 'scan' or 'connect' to start...\n\n";
#endif
    
    lv_textarea_set_text(terminal_output, logo);

    return terminal_screen;
}

void SSHTerminal::append_text(const char* text)
{
    if (!text) {
        return;
    }
    esp_rom_printf("%s", text);
    if (!terminal_output) {
        return;
    }
    
    if (!bsp_display_lock(1500)) {
        ESP_LOGW(TAG, "append_text could not acquire display lock");
        return;
    }
    
    int64_t start_time = esp_timer_get_time() / 1000;
    
    // Check if the user is currently viewing backlog (scrolled back)
    int32_t scroll_bottom_before = lv_obj_get_scroll_bottom(terminal_output);
    int32_t scroll_y_before = lv_obj_get_scroll_y(terminal_output);
    bool was_scrolled_back = (scroll_bottom_before > 20);
    
    const char* current_text = lv_textarea_get_text(terminal_output);
    size_t current_len = current_text ? strlen(current_text) : 0;
    size_t new_len = strlen(text);
    
    // Scrollback capacity: 4 KB (hundreds of lines of terminal backlog)
    const size_t MAX_BUFFER_SIZE = 4096;
    
    if (current_len + new_len >= MAX_BUFFER_SIZE) {
        // Trim oldest ~2KB from the top, retaining recent lines
        size_t trim_target = (current_len > 2048) ? 2048 : (current_len / 2);
        const char* cut = strchr(current_text + trim_target, '\n');
        if (cut && *(cut + 1) != '\0') {
            lv_textarea_set_text(terminal_output, cut + 1);
        } else {
            lv_textarea_set_text(terminal_output, "");
        }
    }
    
    lv_textarea_add_text(terminal_output, text);
    
    if (was_scrolled_back) {
        // Preserve user's scrollback reading position
        lv_obj_scroll_to_y(terminal_output, scroll_y_before, LV_ANIM_OFF);
        is_user_scrolled_back = true;
    } else {
        // Auto-scroll to show latest text
        lv_obj_scroll_to_y(terminal_output, LV_COORD_MAX, LV_ANIM_OFF);
        is_user_scrolled_back = false;
    }
    
    update_scroll_indicator();
    
    int64_t total_time = (esp_timer_get_time() / 1000) - start_time;
    if (total_time > 1000) {
        ESP_LOGW(TAG, "append_text took %lld ms", total_time);
    }
    
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(1));
}

void SSHTerminal::clear_terminal()
{
    if (terminal_output) {
        if (bsp_display_lock(1000)) {
            lv_textarea_set_text(terminal_output, "");
            is_user_scrolled_back = false;
            update_scroll_indicator();
            bsp_display_unlock();
        }
    }
}

void SSHTerminal::handle_key_input(char key, bool from_serial)
{
    // Physical Keyboard Alt+C (T-Deck coprocessor sends 0x0C) or standard Ctrl+C (0x03)
    if ((uint8_t)key == 0x0C || (uint8_t)key == 0x03) {
        send_interrupt();
        return;
    }

    // Only process T-Deck physical keyboard modifier codes and symbol substitution
    // when input originates from the physical keyboard, NEVER from a PC serial terminal!
    if (!from_serial) {
        // Physical Alt/Sym key codes from T-Deck keyboard coprocessor
        if ((uint8_t)key == 0x80 || (uint8_t)key == 0x81 || (uint8_t)key == 0x82 || 
            (uint8_t)key == 0x83 || (uint8_t)key == 0x84 || (uint8_t)key == 0x11 || 
            (uint8_t)key == 0x12 || (uint8_t)key == 0x13 || (uint8_t)key == 0x14 ||
            (uint8_t)key == 0xA4 || (uint8_t)key == 0xA5) {
            toggle_alt_mode();
            return;
        }

        // ========================================================
        // ALT MODE ACTIVE KEY DISPATCH
        // ========================================================
        if (alt_mode) {
            // Alt + Enter: Toggle between Local Shell Mode and Full Interactive SSH Mode
            if (key == '\n' || key == '\r') {
                set_alt_mode(false);
                if (ssh_connected) {
                    toggle_local_shell_mode();
                } else {
                    append_text("\n[Local Shell Active. Type 'help' for commands]\n> ");
                    scroll_to_bottom();
                }
                return;
            }

            // Backspace in Alt Mode
            if (key == 8 || key == 127) {
                if (ssh_connected && channel && !is_local_mode_during_ssh) {
                    if (!ssh_cmd_buffer.empty()) {
                        ssh_cmd_buffer.pop_back();
                        if (ssh_cmd_buffer.empty()) {
                            ssh_at_line_start = true;
                            set_activity_status("[ONLINE]", 0x50FF50);
                        } else {
                            std::string hud_text = "[" + ssh_cmd_buffer + "]";
                            if (hud_text.length() > 10) hud_text = hud_text.substr(0, 9) + "]";
                            set_activity_status(hud_text.c_str(), 0xFFFF50);
                        }
                        return;
                    }
                    ssh_write_all("\x7F", 1);
                } else {
                    if (cursor_pos > 0 && !current_input.empty()) {
                        current_input.erase(cursor_pos - 1, 1);
                        cursor_pos--;
                        update_input_display();
                    }
                }
                return;
            }

            // Space -> Tab
            if (key == ' ') {
                if (ssh_connected && channel) {
                    ssh_write_all("\t", 1);
                } else {
                    handle_tab_completion();
                }
                return;
            }

            // Missing Symbols
            if (key == 't' || key == 'T') { send_char_or_insert('~'); return; }
            if (key == 'p' || key == 'P') { send_char_or_insert('|'); return; }
            if (key == 'b' || key == 'B') { send_char_or_insert('\\'); return; }
            if (key == 'e' || key == 'E') { send_char_or_insert('='); return; }
            if (key == 'u' || key == 'U') { send_char_or_insert('['); return; }
            if (key == 'i' || key == 'I') { send_char_or_insert(']'); return; }
            if (key == 'o' || key == 'O') { send_char_or_insert('{'); return; }
            if (key == 'j' || key == 'J') { send_char_or_insert('}'); return; }
            if (key == 'k' || key == 'K' || key == ',') { send_char_or_insert('<'); return; }
            if (key == 'm' || key == 'M' || key == '.') { send_char_or_insert('>'); return; }
            if (key == 'v' || key == 'V' || key == '5') { send_char_or_insert('%'); return; }
            if (key == 'q' || key == 'Q') { send_char_or_insert('`'); return; }
            if (key == 's' || key == 'S' || key == '4') { send_char_or_insert('$'); return; }
            if (key == 'g' || key == 'G' || key == '6') { send_char_or_insert('^'); return; }
            if (key == 'n' || key == 'N' || key == '7') { send_char_or_insert('&'); return; }
            if (key == '0') { send_char_or_insert('0'); return; }

            // Control Keys (for SSH)
            if (key == 'c' || key == 'C') { send_interrupt(); return; }
            if (key == 'z' || key == 'Z') {
                if (ssh_connected && channel) {
                    ssh_write_all("\x1A", 1);
                    set_activity_status("[^Z SENT]", 0xFFFF50);
                }
                return;
            }
            if (key == 'l' || key == 'L') {
                if (ssh_connected && channel) {
                    ssh_write_all("\x0C", 1);
                } else {
                    clear_terminal();
                }
                return;
            }
            if (key == 'a' || key == 'A') {
                if (ssh_connected && channel) {
                    ssh_write_all("\x01", 1);
                }
                return;
            }
            if (key == 'r' || key == 'R') {
                if (ssh_connected && channel) {
                    ssh_write_all("\x12", 1);
                }
                return;
            }
            if (key == 'x' || key == 'X') {
                if (ssh_connected && channel) {
                    ssh_write_all("\x18", 1);
                }
                return;
            }
            if (key == 'w' || key == 'W') {
                if (ssh_connected && channel) {
                    ssh_write_all("\x17", 1);
                }
                return;
            }
            if (key == 'f' || key == 'F') {
                if (ssh_connected && channel) {
                    ssh_write_all("\x06", 1);
                }
                return;
            }
            if (key == 'y' || key == 'Y') {
                if (ssh_connected && channel) {
                    ssh_write_all("\x19", 1);
                }
                return;
            }
            if (key == 'h' || key == 'H') {
                if (ssh_connected && channel) {
                    ssh_write_all("\x08", 1);
                }
                return;
            }
            if (key == 27) {
                set_alt_mode(false);
                return;
            }
        }
    }

    // ========================================================
    // FULL RAW INTERACTIVE SSH MODE (Desktop Experience)
    // ========================================================
    if (ssh_connected && channel && !is_local_mode_during_ssh) {
        if (key == 27) {
            if (!ssh_cmd_buffer.empty()) {
                ssh_cmd_buffer.clear();
                ssh_at_line_start = true;
                set_activity_status("[ONLINE]", 0x50FF50);
                return;
            }
            ssh_write_all("\x1B", 1);
            set_activity_status("[ESC]", 0x50FFFF);
            return;
        }

        if (key == '\n' || key == '\r') {
            if (!ssh_cmd_buffer.empty()) {
                std::string cmd = ssh_cmd_buffer;
                ssh_cmd_buffer.clear();
                ssh_at_line_start = true;
                set_activity_status("[ONLINE]", 0x50FF50);
                process_escape_command(cmd);
                return;
            }
            ssh_at_line_start = true;
            ssh_write_all("\r", 1);
            set_activity_status("[WAIT...]", 0xFFFF50);
            scroll_to_bottom();
            return;
        }
        else if (key == 8 || key == 127) {
            if (!ssh_cmd_buffer.empty()) {
                ssh_cmd_buffer.pop_back();
                if (ssh_cmd_buffer.empty()) {
                    ssh_at_line_start = true;
                    set_activity_status("[ONLINE]", 0x50FF50);
                } else {
                    std::string hud_text = "[" + ssh_cmd_buffer + "]";
                    if (hud_text.length() > 10) hud_text = hud_text.substr(0, 9) + "]";
                    set_activity_status(hud_text.c_str(), 0xFFFF50);
                }
                return;
            }
            ssh_write_all("\x7F", 1);
            return;
        }
        else if (ssh_at_line_start && ssh_cmd_buffer.empty() && key == ':') {
            ssh_cmd_buffer += key;
            set_activity_status("[:]", 0xFFFF50);
            return;
        }
        else if (!ssh_cmd_buffer.empty()) {
            ssh_cmd_buffer += key;
            std::string prefix = ssh_cmd_buffer.substr(1);
            static const std::vector<std::string> s_esc_cmds = {
                "getkey", "keys", "help", "exit", "quit", "clear"
            };
            bool matches_any = false;
            for (const auto& cmd_name : s_esc_cmds) {
                if (prefix.length() <= cmd_name.length() && cmd_name.rfind(prefix, 0) == 0) {
                    matches_any = true;
                    break;
                }
                if (prefix.length() > cmd_name.length() && prefix.rfind(cmd_name + " ", 0) == 0) {
                    matches_any = true;
                    break;
                }
            }
            if (!matches_any) {
                // Not a recognized escape command - flush entire buffer to remote shell
                libssh2_channel_write(channel, ssh_cmd_buffer.c_str(), ssh_cmd_buffer.size());
                ssh_cmd_buffer.clear();
                ssh_at_line_start = false;
                set_activity_status("[ONLINE]", 0x50FF50);
                return;
            }
            std::string hud_text = "[" + ssh_cmd_buffer + "]";
            if (hud_text.length() > 10) hud_text = hud_text.substr(0, 9) + "]";
            set_activity_status(hud_text.c_str(), 0xFFFF50);
            return;
        }
        else if (key == '\t' || key == 9) {
            ssh_write_all("\t", 1);
            return;
        }
        else if ((unsigned char)key >= 1) {
            ssh_at_line_start = false;
            char k = key;
            ssh_write_all(&k, 1);
            return;
        }
        return;
    }

    // ========================================================
    // LOCAL SHELL MODE (Line-Buffered)
    // ========================================================
    if (key == '\t' || key == 9) {
        handle_tab_completion();
        return;
    }

    if (key == '\n' || key == '\r') {
        scroll_to_bottom();
        std::string prompt_str = (ssh_connected && is_local_mode_during_ssh) ? "[local]> " : "> ";
        if (current_input.empty()) {
            append_text("\n");
            append_text(prompt_str.c_str());
            scroll_to_bottom();
            return;
        }

        append_text("\n");
        append_text(prompt_str.c_str());
        append_text(current_input.c_str());
        append_text("\n");
            
            if (current_input == "scan" || current_input == "wifi scan") {
                scan_wifi();
            }
            else if (current_input == "status" || current_input == "wifi status" || current_input == "wifi") {
                show_wifi_status();
            }
            else if (current_input == "saved" || current_input == "wifi saved") {
                std::string saved_ssid, saved_pass;
                if (load_wifi_config(saved_ssid, saved_pass)) {
                    char buf[160];
                    snprintf(buf, sizeof(buf), "Saved network: \"%s\" (Auto-connect: %s)\nCommands:\n  connect - Connect to saved WiFi\n  forget  - Erase saved credentials\n",
                             saved_ssid.c_str(), get_wifi_autoconnect_enabled() ? "ON" : "OFF");
                    append_text(buf);
                } else {
                    append_text("No saved WiFi network.\nUse 'save <SSID|#> [PASSWORD]' to save a network.\n");
                }
            }
            else if (current_input == "save" || current_input.rfind("save ", 0) == 0 ||
                     current_input == "savewifi" || current_input.rfind("savewifi ", 0) == 0 ||
                     current_input == "wifi save" || current_input.rfind("wifi save ", 0) == 0) {
                // Determine prefix length
                size_t prefix_len = 4;
                if (current_input.rfind("savewifi", 0) == 0) prefix_len = 8;
                else if (current_input.rfind("wifi save", 0) == 0) prefix_len = 9;

                // Parse quoted arguments
                std::vector<std::string> args;
                std::string arg;
                bool in_quotes = false;
                for (size_t i = prefix_len; i < current_input.length(); i++) {
                    char ch = current_input[i];
                    if (ch == '"') {
                        in_quotes = !in_quotes;
                    } else if (ch == ' ' && !in_quotes) {
                        if (!arg.empty()) {
                            args.push_back(arg);
                            arg.clear();
                        }
                    } else {
                        arg += ch;
                    }
                }
                if (!arg.empty()) args.push_back(arg);

                if (args.empty()) {
                    if (wifi_connected && !connected_ssid.empty()) {
                        save_wifi_to_nvs(connected_ssid.c_str(), "");
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Saved active network: \"%s\" (Auto-connect: %s)\n",
                                 connected_ssid.c_str(), get_wifi_autoconnect_enabled() ? "ON" : "OFF");
                        append_text(buf);
                    } else {
                        append_text("Usage:\n  save <SSID|#> [PASSWORD]   - Save network to NVS\n  save \"My WiFi\" password     - Supports quotes for spaces\n  save (saves currently connected WiFi)\n");
                    }
                } else {
                    bool is_num = !args[0].empty() && std::all_of(args[0].begin(), args[0].end(), ::isdigit);
                    std::string target_ssid;
                    std::string target_pass = (args.size() >= 2) ? args[1] : "";

                    if (is_num) {
                        int idx = std::atoi(args[0].c_str());
                        if (idx >= 1 && idx <= (int)cached_scan_results.size()) {
                            target_ssid = (char*)cached_scan_results[idx - 1].ssid;
                        } else {
                            char buf[128];
                            snprintf(buf, sizeof(buf), "Invalid scan index %d. Type 'scan' to see networks.\n", idx);
                            append_text(buf);
                        }
                    } else {
                        target_ssid = args[0];
                    }

                    if (!target_ssid.empty()) {
                        save_wifi_to_nvs(target_ssid.c_str(), target_pass.c_str());
                        char buf[160];
                        snprintf(buf, sizeof(buf), "WiFi network saved to NVS:\n  SSID: \"%s\"\n  Auto-connect: %s\nType 'connect' to connect to it.\n",
                                 target_ssid.c_str(), get_wifi_autoconnect_enabled() ? "ON" : "OFF");
                        append_text(buf);
                    }
                }
            }
            else if (current_input == "forget" || current_input == "wifi forget") {
                clear_wifi_config();
                append_text("Saved WiFi network erased.\n");
            }
            else if (current_input.rfind("autoconnect", 0) == 0 || current_input.rfind("wifi autoconnect", 0) == 0) {
                std::string lower = current_input;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.find("on") != std::string::npos) {
                    set_wifi_autoconnect_enabled(true);
                    append_text("WiFi auto-connect set to ON\n");
                } else if (lower.find("off") != std::string::npos) {
                    set_wifi_autoconnect_enabled(false);
                    append_text("WiFi auto-connect set to OFF\n");
                } else {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Auto-connect is %s. Usage: autoconnect on|off\n",
                             get_wifi_autoconnect_enabled() ? "ON" : "OFF");
                    append_text(buf);
                }
            }
            else if (current_input == "connect" || current_input.rfind("connect ", 0) == 0) {
                if (current_input == "connect") {
                    std::string saved_ssid, saved_pass;
                    if (load_wifi_config(saved_ssid, saved_pass)) {
                        append_text("Connecting to saved WiFi: ");
                        append_text(saved_ssid.c_str());
                        append_text("...\n");
                        esp_err_t res = init_wifi(saved_ssid.c_str(), saved_pass.c_str());
                        if (res == ESP_OK) {
                            append_text("WiFi connected successfully!\n");
                        } else {
                            char err_buf[128];
                            snprintf(err_buf, sizeof(err_buf), "WiFi connection failed!\nReason [%d]: %s\n",
                                     s_last_disconnect_reason, SSHTerminal::wifi_reason_to_string(s_last_disconnect_reason));
                            append_text(err_buf);
                        }
                    } else {
                        append_text("No saved WiFi network found.\nUsage: connect <SSID|#> [PASSWORD]\nType 'scan' to see available networks.\n");
                    }
                } else {
                    // Parse arguments with support for quoted strings (for SSIDs/passwords with spaces)
                    std::vector<std::string> args;
                    std::string arg;
                    bool in_quotes = false;
                    
                    for (size_t i = 8; i < current_input.length(); i++) {
                        char c = current_input[i];
                        if (c == '"') {
                            in_quotes = !in_quotes;
                        } else if (c == ' ' && !in_quotes) {
                            if (!arg.empty()) {
                                args.push_back(arg);
                                arg.clear();
                            }
                        } else {
                            arg += c;
                        }
                    }
                    if (!arg.empty()) {
                        args.push_back(arg);
                    }
                    
                    if (!args.empty()) {
                        bool is_num = !args[0].empty() && std::all_of(args[0].begin(), args[0].end(), ::isdigit);
                        std::string target_ssid;
                        std::string target_pass = (args.size() >= 2) ? args[1] : "";
                        
                        if (is_num) {
                            int idx = std::atoi(args[0].c_str());
                            if (idx >= 1 && idx <= (int)cached_scan_results.size()) {
                                target_ssid = (char*)cached_scan_results[idx - 1].ssid;
                                if (target_pass.empty() && cached_scan_results[idx - 1].authmode != WIFI_AUTH_OPEN) {
                                    std::string saved_ssid, saved_pass;
                                    if (load_wifi_from_nvs(saved_ssid, saved_pass) && saved_ssid == target_ssid) {
                                        target_pass = saved_pass;
                                    } else {
                                        char buf[128];
                                        snprintf(buf, sizeof(buf), "Network '%s' requires a password:\n  connect %d <PASSWORD>\n",
                                                 target_ssid.c_str(), idx);
                                        append_text(buf);
                                        target_ssid.clear(); // abort
                                    }
                                }
                            } else {
                                char buf[128];
                                snprintf(buf, sizeof(buf), "Invalid index %d (%d networks scanned). Type 'scan' to view.\n",
                                         idx, (int)cached_scan_results.size());
                                append_text(buf);
                            }
                        } else {
                            target_ssid = args[0];
                            if (target_pass.empty()) {
                                std::string saved_ssid, saved_pass;
                                if (load_wifi_from_nvs(saved_ssid, saved_pass) && saved_ssid == target_ssid) {
                                    target_pass = saved_pass;
                                }
                            }
                        }
                        
                        if (!target_ssid.empty()) {
                            append_text("Connecting to WiFi: ");
                            append_text(target_ssid.c_str());
                            append_text("...\n");
                            
                            esp_err_t res = init_wifi(target_ssid.c_str(), target_pass.c_str());
                            if (res == ESP_OK) {
                                append_text("WiFi connected successfully!\n");
                                char sbuf[128];
                                snprintf(sbuf, sizeof(sbuf), "[Saved \"%s\" to NVS - Auto-connect: %s]\n",
                                         target_ssid.c_str(), get_wifi_autoconnect_enabled() ? "ON" : "OFF");
                                append_text(sbuf);
                            } else {
                                char err_buf[128];
                                snprintf(err_buf, sizeof(err_buf), "WiFi connection failed!\nReason [%d]: %s\n",
                                         s_last_disconnect_reason, SSHTerminal::wifi_reason_to_string(s_last_disconnect_reason));
                                append_text(err_buf);
                            }
                        }
                    } else {
                        append_text("Usage: connect <SSID|#> [PASSWORD]\n");
                        append_text("  Quotes supported: connect \"My WiFi\" password\n");
                        append_text("  Or by index after scan: connect 1 password\n");
                    }
                }
            }
            else if (current_input.rfind("ssh ", 0) == 0) {
                std::vector<std::string> parts;
                size_t pos = 0;
                std::string temp = current_input;
                
                while ((pos = temp.find(' ')) != std::string::npos) {
                    if (pos > 0) parts.push_back(temp.substr(0, pos));
                    temp.erase(0, pos + 1);
                }
                if (!temp.empty()) parts.push_back(temp);
                
                std::string host, user, pass;
                int port = 22;
                
                if (parts.size() >= 5) {
                    host = parts[1];
                    port = std::atoi(parts[2].c_str());
                    user = parts[3];
                    pass = parts[4];
                } else if (parts.size() == 4) {
                    host = parts[1];
                    user = parts[2];
                    pass = parts[3];
                    port = 22;
                } else if (parts.size() == 3 && parts[1].find('@') != std::string::npos) {
                    size_t at_pos = parts[1].find('@');
                    user = parts[1].substr(0, at_pos);
                    host = parts[1].substr(at_pos + 1);
                    pass = parts[2];
                    port = 22;
                }
                
                if (!host.empty() && !user.empty() && !pass.empty()) {
                    connect(host.c_str(), port, user.c_str(), pass.c_str());
                } else {
                    append_text("Usage:\n  ssh <HOST> [PORT] <USER> <PASS>\n  ssh <USER>@<HOST> <PASS>\n");
                }
            }
            else if (current_input.rfind("sshkey ", 0) == 0) {
                std::vector<std::string> parts;
                size_t pos = 0;
                std::string temp = current_input;
                
                while ((pos = temp.find(' ')) != std::string::npos) {
                    if (pos > 0) parts.push_back(temp.substr(0, pos));
                    temp.erase(0, pos + 1);
                }
                if (!temp.empty()) parts.push_back(temp);
                
                std::string host, user, keyfile;
                int port = 22;
                
                if (parts.size() >= 5) {
                    host = parts[1];
                    port = std::atoi(parts[2].c_str());
                    user = parts[3];
                    keyfile = parts[4];
                } else if (parts.size() == 4) {
                    host = parts[1];
                    user = parts[2];
                    keyfile = parts[3];
                    port = 22;
                } else if (parts.size() == 3 && parts[1].find('@') != std::string::npos) {
                    size_t at_pos = parts[1].find('@');
                    user = parts[1].substr(0, at_pos);
                    host = parts[1].substr(at_pos + 1);
                    keyfile = parts[2];
                    port = 22;
                }
                
                if (!host.empty() && !user.empty() && !keyfile.empty()) {
                    size_t key_len = 0;
                    const char* key_data = get_loaded_key(keyfile.c_str(), &key_len);
                    if (key_data && key_len > 0) {
                        append_text("Using key file: ");
                        append_text(keyfile.c_str());
                        append_text("\n");
                        connect_with_key(host.c_str(), port, user.c_str(), key_data, key_len);
                    } else {
                        append_text("ERROR: Key file not found: ");
                        append_text(keyfile.c_str());
                        append_text("\nAvailable keys: ");
                        for (const auto& kv : loaded_keys) {
                            append_text(kv.first.c_str());
                            append_text(" ");
                        }
                        append_text("\n");
                    }
                } else {
                    append_text("Usage:\n  sshkey <HOST> [PORT] <USER> <KEYFILE>\n  sshkey <USER>@<HOST> <KEYFILE>\n");
                }
            }
            else if (current_input == "disconnect" || current_input == "wifi disconnect") {
                if (wifi_connected) {
                    append_text("Disconnecting WiFi...\n");
                    s_retry_num = WIFI_MAXIMUM_RETRY;
                    if (!m_is_virtual_target) {
                        esp_wifi_disconnect();
                    }
                    wifi_connected = false;
                    connected_ssid.clear();
                    update_status_bar();
                    append_text("WiFi disconnected\n");
                } else {
                    append_text("WiFi not connected\n");
                }
            }
            else if (current_input == "resume" || current_input == "fg" || (current_input == "ssh" && ssh_connected)) {
                if (ssh_connected) {
                    toggle_local_shell_mode();
                } else {
                    append_text("No active SSH session to resume. Use 'ssh' or 'sshkey' to connect.\n");
                }
            }
            else if (current_input == "exit" || current_input == "quit") {
                if (ssh_connected) {
                    disconnect();
                } else {
                    append_text("PocketSSH Local Shell. Not connected to SSH.\n");
                    append_text("Type 'help' for commands, or 'connect' to join WiFi.\n> ");
                    scroll_to_bottom();
                }
            }
            else if (current_input == "clear") {
                clear_terminal();
            }
            else if (current_input == "keys" || current_input == "key") {
                show_loaded_keys();
            }
            else if (current_input == "getkey" || current_input.rfind("getkey ", 0) == 0) {
                if (!ssh_connected || !channel) {
                    append_text("[Error: Must be connected to SSH to fetch key from server]\n");
                    append_text("[Place .pem file in /sdcard/ssh_keys/ or connect with password first]\n");
                } else {
                    std::vector<std::string> args;
                    std::string arg;
                    size_t p = 6;
                    while (p < current_input.size() && current_input[p] == ' ') p++;
                    for (; p < current_input.size(); p++) {
                        if (current_input[p] == ' ') {
                            if (!arg.empty()) { args.push_back(arg); arg.clear(); }
                        } else {
                            arg += current_input[p];
                        }
                    }
                    if (!arg.empty()) args.push_back(arg);

                    if (args.empty()) {
                        show_getkey_dialog();
                    } else {
                        const char* rpath = args[0].c_str();
                        const char* lname = args.size() > 1 ? args[1].c_str() : NULL;
                        start_fetch_remote_key(rpath, lname);
                    }
                }
            }
            else if (current_input == "pgup") {
                scroll_page_up();
            }
            else if (current_input == "pgdn") {
                scroll_page_down();
            }
            else if (current_input == "top") {
                scroll_to_top();
            }
            else if (current_input == "bottom") {
                scroll_to_bottom();
            }
            else if (current_input == "scroll" || current_input == "scrollback") {
                append_text("\n=== Terminal Scrollback Controls ===\n");
                append_text(" Trackball UP/DOWN   - Scroll line-by-line\n");
                append_text(" Trackball CLICK     - Jump to bottom (when scrolled)\n");
                append_text(" Trackball L/R       - History / Move cursor / Remote cursor\n");
                append_text(" Touch Drag          - Drag screen vertically\n");
                append_text(" Swipe Left          - Open Keys panel (PgUp, PgDn, Top, Bottom)\n");
                append_text(" Commands            - pgup, pgdn, top, bottom\n");
                append_text("=====================================\n\n");
            }
            else if (current_input == "version" || current_input == "ver" || current_input == "-v" || current_input == "--version") {
                append_text("\n=== PocketSSH Version ===\n"
#ifdef POCKETSSH_VERSION
                            " Version:  v" POCKETSSH_VERSION "\n"
#else
                            " Version:  v1.3.0\n"
#endif
                            " Target:   ESP32-S3 LilyGO T-Deck / T-Deck Plus\n"
                            " Build:    " __DATE__ " " __TIME__ "\n"
                            " Features: WPA3-SAE, WiFi Scanner, NVS Auto-connect,\n"
                            "           VT100 SSH2 Terminal, SD Key Management\n"
                            "=========================\n\n");
            }
            else if (current_input == "help") {
                append_text("Available commands:\n"
                            "  scan                       - Scan 2.4GHz WiFi networks\n"
                            "  connect [SSID|#] [PASS]    - Connect WiFi (or saved if empty)\n"
                            "  save <SSID|#> [PASS]       - Save WiFi network to NVS\n"
                            "  saved                      - Show saved WiFi network\n"
                            "  forget                     - Forget saved WiFi credentials\n"
                            "  autoconnect <on|off>       - Toggle auto-connect on boot\n"
                            "  status                     - Show WiFi info & IP\n"
                            "  disconnect                 - Disconnect WiFi\n"
                            "  ssh <HOST> <PORT> <U> <P>  - Connect via SSH (password)\n"
                            "  sshkey <H> <P> <U> <KEY>   - Connect via SSH (private key)\n"
                            "  keys                       - List all loaded SSH keys\n"
                            "    Note: Place .pem keys in /sdcard/ssh_keys/ before use\n"
                            "  exit                       - Disconnect SSH session\n"
                            "  clear                      - Clear terminal\n"
                            "  pgup / pgdn / top / bottom - Scroll terminal output\n"
                            "  scroll                     - Show scrollback controls\n"
                            "  Tab / Alt+Space            - Auto-complete command, SSID or keys\n"
                            "  version                    - Show version & build info\n"
                            "  invert                     - Toggle LCD color inversion\n"
                            "  resume / fg                - Resume active SSH session (or Alt+Enter)\n"
                            "  help                       - Show this help\n");
            }
            else if (current_input == "invert" || current_input == "lcd invert" || current_input == "color invert") {
                static bool s_inverted = false;
                s_inverted = !s_inverted;
                bsp_display_invert_color(s_inverted);
                if (bsp_display_lock(500)) {
                    lv_obj_invalidate(lv_scr_act());
                    bsp_display_unlock();
                }
                append_text(s_inverted ? "Display color inversion: ON\n" : "Display color inversion: OFF (Normal)\n");
            }
            else if (ssh_connected) {
                send_command(current_input.c_str());
            } else {
                append_text("Unknown command. Type 'help' for commands.\n");
            }
            
            auto it = std::find(command_history.begin(), command_history.end(), current_input);
            if (it != command_history.end()) {
                command_history.erase(it);
            }
            command_history.push_back(current_input);
            history_needs_save = true;
            current_input.clear();
            cursor_pos = 0;
            history_index = -1;
            if (!terminal_screen) {
                std::string prompt_str = (ssh_connected && is_local_mode_during_ssh) ? "[local]> " : "> ";
                append_text(prompt_str.c_str());
            }
    } else if (key == 8 || key == 127) {
        // Backspace - delete character before cursor
        if (cursor_pos > 0 && !current_input.empty()) {
            current_input.erase(cursor_pos - 1, 1);
            cursor_pos--;
        }
    } else if (key >= 32 && key <= 126) {
        // Insert character at cursor position
        current_input.insert(cursor_pos, 1, key);
        cursor_pos++;
    }
    
    update_input_display();
}

void SSHTerminal::update_input_display()
{
    if (!input_label) {
        return;
    }
    
    if (!bsp_display_lock(500)) {
        return;
    }
    
    // Ensure cursor_pos is within bounds
    if (cursor_pos > current_input.length()) {
        cursor_pos = current_input.length();
    }
    
    std::string prompt_str = (ssh_connected && is_local_mode_during_ssh) ? "[local]> " : "> ";
    std::string full_text = prompt_str + current_input;
    
    // Insert cursor at correct position
    if (cursor_visible) {
        size_t display_pos = prompt_str.length() + cursor_pos;
        full_text.insert(display_pos, "|");
    }
    
    lv_label_set_text(input_label, full_text.c_str());
    
    lv_obj_t* container = lv_obj_get_parent(input_label);
    if (container) {
        lv_obj_scroll_to_x(container, LV_COORD_MAX, LV_ANIM_OFF);
    }
    
    bsp_display_unlock();
}

void SSHTerminal::input_touch_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_obj_t* input_label = (lv_obj_t* )lv_event_get_target(e);
    
    if (!terminal || !input_label) {
        return;
    }
    
    // Get the touch point relative to the label
    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    
    // Convert to label coordinates
    lv_obj_t* label = input_label;
    lv_area_t label_coords;
    lv_obj_get_coords(label, &label_coords);
    
    int32_t click_x = point.x - label_coords.x1;
    
    // Get font and calculate character position
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    
    // Account for "> " prefix (2 characters)
    const char* prefix = "> ";
    lv_point_t prefix_size;
    lv_txt_get_size(&prefix_size, prefix, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t prefix_width = prefix_size.x;
    
    if (click_x <= prefix_width) {
        // Clicked on prefix, set cursor to start
        terminal->cursor_pos = 0;
    } else {
        // Calculate which character was clicked
        int32_t text_x = click_x - prefix_width;
        size_t best_pos = 0;
        int32_t min_distance = INT32_MAX;
        
        // Check each character position
        for (size_t i = 0; i <= terminal->current_input.length(); i++) {
            std::string substr = terminal->current_input.substr(0, i);
            lv_point_t substr_size;
            lv_txt_get_size(&substr_size, substr.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int32_t char_x = substr_size.x;
            int32_t distance = abs(text_x - char_x);
            
            if (distance < min_distance) {
                min_distance = distance;
                best_pos = i;
            }
        }
        
        terminal->cursor_pos = best_pos;
    }
    
    // Reset cursor blink to make it visible
    terminal->cursor_visible = true;
    terminal->update_input_display();
    
    ESP_LOGI(TAG, "Cursor moved to position: %d", terminal->cursor_pos);
}

void SSHTerminal::cursor_blink_cb(lv_timer_t* timer)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_timer_get_user_data(timer);
    if (terminal) {
        terminal->cursor_visible = !terminal->cursor_visible;
        terminal->update_input_display();
    }
}

void SSHTerminal::battery_update_cb(lv_timer_t* timer)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_timer_get_user_data(timer);
    if (terminal) {
        terminal->update_status_bar();
    }
}

void SSHTerminal::history_save_cb(lv_timer_t* timer)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_timer_get_user_data(timer);
    if (terminal && terminal->history_needs_save) {
        terminal->history_needs_save = false;
        terminal->save_history_to_nvs();
    }
}

void SSHTerminal::navigate_history(int direction)
{
    if (command_history.empty()) {
        return;
    }
    
    if (direction > 0) {
        if (history_index < (int)command_history.size() - 1) {
            history_index++;
            current_input = command_history[command_history.size() - 1 - history_index];
        }
    } else if (direction < 0) {
        if (history_index > 0) {
            history_index--;
            current_input = command_history[command_history.size() - 1 - history_index];
        } else if (history_index == 0) {
            history_index = -1;
            current_input.clear();
        }
    }
    
    // Move cursor to end of input
    cursor_pos = current_input.length();
    update_input_display();
}

void SSHTerminal::move_cursor_left()
{
    if (cursor_pos > 0) {
        cursor_pos--;
        cursor_visible = true;
        update_input_display();
    }
}

void SSHTerminal::move_cursor_right()
{
    if (cursor_pos < current_input.length()) {
        cursor_pos++;
        cursor_visible = true;
        update_input_display();
    }
}

void SSHTerminal::move_cursor_home()
{
    cursor_pos = 0;
    cursor_visible = true;
    update_input_display();
}

void SSHTerminal::move_cursor_end()
{
    cursor_pos = current_input.length();
    cursor_visible = true;
    update_input_display();
}

void SSHTerminal::delete_current_history_entry()
{
    if (command_history.empty() || history_index < 0) {
        ESP_LOGW(TAG, "No history entry to delete (empty or not navigating)");
        return;
    }
    
    size_t actual_index = command_history.size() - 1 - history_index;
    
    ESP_LOGI(TAG, "Deleting history entry: '%s' (index %d)", 
             command_history[actual_index].c_str(), (int)actual_index);
    
    command_history.erase(command_history.begin() + actual_index);
    
    history_needs_save = true;
    
    if (command_history.empty()) {
        history_index = -1;
        current_input.clear();
    } else if (history_index >= (int)command_history.size()) {
        history_index = command_history.size() - 1;
        current_input = command_history[command_history.size() - 1 - history_index];
    } else if (actual_index < command_history.size()) {
        current_input = command_history[command_history.size() - 1 - history_index];
    } else {
        history_index = -1;
        current_input.clear();
    }
    
    std::string display_text = "> " + current_input;
    if (input_label) {
        lv_label_set_text(input_label, display_text.c_str());
    }
    
    ESP_LOGI(TAG, "History entry deleted. Remaining entries: %d", (int)command_history.size());
}

void SSHTerminal::send_current_history_command()
{
    if (command_history.empty() || history_index < 0) {
        ESP_LOGW(TAG, "No history command to send (empty or not navigating)");
        return;
    }
    
    size_t actual_index = command_history.size() - 1 - history_index;
    std::string cmd_to_send = command_history[actual_index];
    
    ESP_LOGI(TAG, "Sending history command: '%s'", cmd_to_send.c_str());
    
    current_input = cmd_to_send;
    
    std::string display_text = "> " + current_input;
    if (input_label) {
        lv_label_set_text(input_label, display_text.c_str());
    }
    
    send_command(cmd_to_send.c_str());
    
    auto it = std::find(command_history.begin(), command_history.end(), current_input);
    if (it != command_history.end()) {
        command_history.erase(it);
    }
    command_history.push_back(current_input);
    history_needs_save = true;
    current_input.clear();
    history_index = -1;
    
    display_text = "> ";
    if (input_label) {
        lv_label_set_text(input_label, display_text.c_str());
    }
}

void SSHTerminal::load_history_from_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading history: %s", esp_err_to_name(err));
        return;
    }
    
    uint32_t history_count = 0;
    err = nvs_get_u32(nvs_handle, "hist_count", &history_count);
    if (err != ESP_OK || history_count == 0) {
        ESP_LOGI(TAG, "No command history found in NVS");
        nvs_close(nvs_handle);
        return;
    }
    
    ESP_LOGI(TAG, "Loading %lu commands from NVS...", history_count);
    
    command_history.clear();
    for (uint32_t i = 0; i < history_count && i < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist_%lu", i);
        
        size_t required_size = 0;
        err = nvs_get_str(nvs_handle, key, NULL, &required_size);
        if (err != ESP_OK) {
            continue;
        }
        
        char* cmd = (char*)malloc(required_size);
        if (cmd) {
            err = nvs_get_str(nvs_handle, key, cmd, &required_size);
            if (err == ESP_OK) {
                command_history.push_back(std::string(cmd));
            }
            free(cmd);
        }
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded %d commands from NVS", (int)command_history.size());
}

void SSHTerminal::save_history_to_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing history: %s", esp_err_to_name(err));
        return;
    }
    
    size_t start_idx = 0;
    if (command_history.size() > 100) {
        start_idx = command_history.size() - 100;
    }
    
    uint32_t history_count = command_history.size() - start_idx;
    err = nvs_set_u32(nvs_handle, "hist_count", history_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save history count: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }
    
    int saved_count = 0;
    for (size_t i = start_idx; i < command_history.size() && saved_count < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist_%lu", (unsigned long)(i - start_idx));
        
        err = nvs_set_str(nvs_handle, key, command_history[i].c_str());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save command %zu: %s", i, esp_err_to_name(err));
        } else {
            saved_count++;
        }
        
        if (saved_count % 10 == 0) {
            vTaskDelay(1);
        }
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved %d commands to NVS", saved_count);
    }
    
    nvs_close(nvs_handle);
}

void SSHTerminal::clear_history_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return;
    }
    
    uint32_t history_count = 0;
    nvs_get_u32(nvs_handle, "hist_count", &history_count);
    
    for (uint32_t i = 0; i < history_count && i < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist_%lu", i);
        nvs_erase_key(nvs_handle, key);
    }
    
    nvs_erase_key(nvs_handle, "hist_count");
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

int SSHTerminal::waitsocket(int socket_fd, LIBSSH2_SESSION *session)
{
    struct timeval timeout;
    int rc;
    fd_set fd;
    fd_set *writefd = NULL;
    fd_set *readfd = NULL;
    int dir;

    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    FD_ZERO(&fd);
    FD_SET(socket_fd, &fd);

    dir = libssh2_session_block_directions(session);

    if(dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        readfd = &fd;

    if(dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        writefd = &fd;

    rc = select(socket_fd + 1, readfd, writefd, NULL, &timeout);

    return rc;
}

esp_err_t SSHTerminal::connect(const char* host, int port, const char* username, const char* password)
{
    if (!wifi_connected) {
        ESP_LOGE(TAG, "WiFi not connected");
        append_text("ERROR: WiFi not connected\n");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to %s:%d", host, port);
    append_text("Connecting to ");
    append_text(host);
    append_text("...\n");

    int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "libssh2 initialization failed (%d)", rc);
        append_text("ERROR: libssh2 init failed\n");
        return ESP_FAIL;
    }

    struct sockaddr_in sin;
    ssh_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ssh_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        append_text("ERROR: Failed to create socket\n");
        libssh2_exit();
        return ESP_FAIL;
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(host);
    if (sin.sin_addr.s_addr == INADDR_NONE || sin.sin_addr.s_addr == (in_addr_t)(-1)) {
        struct hostent *hp = gethostbyname(host);
        if (hp && hp->h_addr_list && hp->h_addr_list[0]) {
            memcpy(&sin.sin_addr, hp->h_addr_list[0], hp->h_length);
        } else {
            ESP_LOGE(TAG, "Failed to resolve hostname: %s", host);
            append_text("ERROR: Failed to resolve hostname\n");
            close(ssh_socket);
            ssh_socket = -1;
            libssh2_exit();
            return ESP_FAIL;
        }
    }

    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(ssh_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(ssh_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (::connect(ssh_socket, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in)) != 0) {
        ESP_LOGE(TAG, "Failed to connect socket");
        append_text("ERROR: Failed to connect socket\n");
        close(ssh_socket);
        ssh_socket = -1;
        libssh2_exit();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Socket connected");
    append_text("Socket connected, initializing SSH session...\n");

    session = libssh2_session_init();
    if (!session) {
        ESP_LOGE(TAG, "Failed to create SSH session");
        append_text("ERROR: Failed to create SSH session\n");
        close(ssh_socket);
        ssh_socket = -1;
        libssh2_exit();
        return ESP_FAIL;
    }

    libssh2_session_set_blocking(session, 0);

    append_text("Performing SSH handshake...\n");
    while ((rc = libssh2_session_handshake(session, ssh_socket)) == LIBSSH2_ERROR_EAGAIN);
    
    if (rc) {
        ESP_LOGE(TAG, "SSH handshake failed: %d", rc);
        append_text("ERROR: SSH handshake failed\n");
        disconnect();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SSH handshake successful");
    append_text("SSH handshake successful\n");

    if (ssh_authenticate(username, password) != ESP_OK) {
        append_text("ERROR: Authentication failed\n");
        disconnect();
        return ESP_FAIL;
    }

    append_text("Authentication successful\n");

    if (ssh_open_channel() != ESP_OK) {
        append_text("ERROR: Failed to open channel\n");
        disconnect();
        return ESP_FAIL;
    }

    append_text("SSH channel opened - connected!\n");
    ssh_connected = true;
    update_status_bar();

    xTaskCreate(ssh_receive_task, "ssh_rx", 8192, this, 5, NULL);

    return ESP_OK;
}

esp_err_t SSHTerminal::connect_with_key(const char* host, int port, const char* username, const char* privkey_data, size_t privkey_len)
{
    if (!wifi_connected) {
        ESP_LOGE(TAG, "WiFi not connected");
        append_text("ERROR: WiFi not connected\n");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to %s:%d with public key", host, port);
    append_text("Connecting to ");
    append_text(host);
    append_text(" with public key...\n");

    int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "libssh2 initialization failed (%d)", rc);
        append_text("ERROR: libssh2 init failed\n");
        return ESP_FAIL;
    }

    struct sockaddr_in sin;
    ssh_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ssh_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        append_text("ERROR: Failed to create socket\n");
        libssh2_exit();
        return ESP_FAIL;
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(host);
    if (sin.sin_addr.s_addr == INADDR_NONE || sin.sin_addr.s_addr == (in_addr_t)(-1)) {
        struct hostent *hp = gethostbyname(host);
        if (hp && hp->h_addr_list && hp->h_addr_list[0]) {
            memcpy(&sin.sin_addr, hp->h_addr_list[0], hp->h_length);
        } else {
            ESP_LOGE(TAG, "Failed to resolve hostname: %s", host);
            append_text("ERROR: Failed to resolve hostname\n");
            close(ssh_socket);
            ssh_socket = -1;
            libssh2_exit();
            return ESP_FAIL;
        }
    }

    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(ssh_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(ssh_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (::connect(ssh_socket, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in)) != 0) {
        ESP_LOGE(TAG, "Failed to connect socket");
        append_text("ERROR: Failed to connect socket\n");
        close(ssh_socket);
        ssh_socket = -1;
        libssh2_exit();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Socket connected");
    append_text("Socket connected, initializing SSH session...\n");

    session = libssh2_session_init();
    if (!session) {
        ESP_LOGE(TAG, "Failed to create SSH session");
        append_text("ERROR: Failed to create SSH session\n");
        close(ssh_socket);
        ssh_socket = -1;
        libssh2_exit();
        return ESP_FAIL;
    }

    libssh2_session_set_blocking(session, 0);

    append_text("Performing SSH handshake...\n");
    while ((rc = libssh2_session_handshake(session, ssh_socket)) == LIBSSH2_ERROR_EAGAIN);
    
    if (rc) {
        ESP_LOGE(TAG, "SSH handshake failed: %d", rc);
        append_text("ERROR: SSH handshake failed\n");
        disconnect();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SSH handshake successful");
    append_text("SSH handshake successful\n");

    if (ssh_authenticate_pubkey(username, privkey_data, privkey_len) != ESP_OK) {
        append_text("ERROR: Public key authentication failed\n");
        disconnect();
        return ESP_FAIL;
    }

    append_text("Public key authentication successful\n");

    if (ssh_open_channel() != ESP_OK) {
        append_text("ERROR: Failed to open channel\n");
        disconnect();
        return ESP_FAIL;
    }

    append_text("SSH channel opened - connected!\n");
    ssh_connected = true;
    update_status_bar();

    xTaskCreate(ssh_receive_task, "ssh_rx", 8192, this, 5, NULL);

    return ESP_OK;
}

esp_err_t SSHTerminal::ssh_authenticate(const char* username, const char* password)
{
    append_text("Authenticating as ");
    append_text(username);
    append_text("...\n");

    int rc;
    while ((rc = libssh2_userauth_password(session, username, password)) == LIBSSH2_ERROR_EAGAIN);
    
    if (rc) {
        char *err_msg;
        int err_len;
        libssh2_session_last_error(session, &err_msg, &err_len, 0);
        ESP_LOGE(TAG, "Authentication failed: %s", err_msg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Authentication successful");
    return ESP_OK;
}

esp_err_t SSHTerminal::ssh_authenticate_pubkey(const char* username, const char* privkey_data, size_t privkey_len)
{
    append_text("Authenticating as ");
    append_text(username);
    append_text(" with public key...\n");

    int rc;
    // libssh2_userauth_publickey_frommemory expects the private key, public key (can be NULL), and passphrase
    while ((rc = libssh2_userauth_publickey_frommemory(session, username, strlen(username),
                                                         NULL, 0,  // public key (optional)
                                                         privkey_data, privkey_len,
                                                         NULL)) == LIBSSH2_ERROR_EAGAIN);  // no passphrase
    
    if (rc) {
        char *err_msg;
        int err_len;
        libssh2_session_last_error(session, &err_msg, &err_len, 0);
        ESP_LOGE(TAG, "Public key authentication failed: %s (error code: %d)", err_msg, rc);
        append_text("ERROR: Public key authentication failed\n");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Public key authentication successful");
    return ESP_OK;
}

esp_err_t SSHTerminal::ssh_open_channel()
{
    append_text("Opening SSH channel...\n");

    int rc;
    while ((channel = libssh2_channel_open_session(session)) == NULL &&
           libssh2_session_last_error(session, NULL, NULL, 0) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(ssh_socket, session);
    }

    if (channel == NULL) {
        ESP_LOGE(TAG, "Failed to open channel");
        return ESP_FAIL;
    }

    const char* term_type = "xterm-256color";
    char pty_modes[] = { 
        5, 0, 0, 0, 127, // SSH_PTY_OP_VERASE = 127 (^?, DEL)
        0                // TTY_OP_END
    };
    while ((rc = libssh2_channel_request_pty_ex(channel, term_type, (unsigned int)strlen(term_type),
                                                pty_modes, sizeof(pty_modes),
                                                50, 24, 320, 240)) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(ssh_socket, session);
    }
    
    if (rc) {
        ESP_LOGE(TAG, "Failed to request PTY");
        return ESP_FAIL;
    }

    while ((rc = libssh2_channel_shell(channel)) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(ssh_socket, session);
    }
    
    if (rc) {
        ESP_LOGE(TAG, "Failed to start shell");
        return ESP_FAIL;
    }

    libssh2_channel_set_blocking(channel, 0);

    ESP_LOGI(TAG, "SSH channel opened successfully");
    set_ssh_ui_mode(true);
    return ESP_OK;
}

esp_err_t SSHTerminal::disconnect()
{
    if (!ssh_connected && !channel && !session && ssh_socket < 0) {
        ESP_LOGI(TAG, "Already disconnected");
        set_ssh_ui_mode(false);
        return ESP_OK;
    }

    bool had_connection = ssh_connected || (channel != NULL);
    ssh_connected = false;
    is_local_mode_during_ssh = false;
    set_activity_status("", 0x50FF50);
    
    if (ssh_mutex) xSemaphoreTakeRecursive(ssh_mutex, portMAX_DELAY);

    if (channel) {
        libssh2_channel_free(channel);
        channel = NULL;
    }

    if (session) {
        libssh2_session_disconnect(session, "Normal Shutdown");
        libssh2_session_free(session);
        session = NULL;
    }

    if (ssh_socket >= 0) {
        close(ssh_socket);
        ssh_socket = -1;
    }

    libssh2_exit();

    if (ssh_mutex) xSemaphoreGiveRecursive(ssh_mutex);
    
    ssh_rx_paused = false;
    ssh_rx_is_paused = false;
    if (getkey_dialog) {
        if (bsp_display_lock(500)) {
            if (lv_obj_is_valid(getkey_dialog)) {
                lv_obj_del(getkey_dialog);
            }
            getkey_dialog = NULL;
            getkey_path_ta = NULL;
            bsp_display_unlock();
        }
    }
    
    set_ssh_ui_mode(false);
    if (had_connection) {
        append_text("\n\033[33m[SSH session closed]\033[0m\n> ");
        scroll_to_bottom();
    }
    
    ESP_LOGI(TAG, "Disconnected");
    
    return ESP_OK;
}

bool SSHTerminal::is_connected()
{
    return ssh_connected;
}

void SSHTerminal::send_command(const char* cmd)
{
    if (!channel) {
        return;
    }
    
    bytes_received = 0;
    if (byte_counter_label && bsp_display_lock(500)) {
        lv_label_set_text(byte_counter_label, "0 B");
        bsp_display_unlock();
    }

    std::string full_cmd = std::string(cmd) + "\n";
    ssize_t nwritten = 0;
    int retry_count = 0;
    const int MAX_RETRIES = 50;
    
    ESP_LOGI(TAG, "Sending command: %s", cmd);
    
    while (nwritten < (ssize_t)full_cmd.length() && retry_count < MAX_RETRIES) {
        ssize_t n = libssh2_channel_write(channel, full_cmd.c_str() + nwritten, 
                                          full_cmd.length() - nwritten);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            int wait_result = waitsocket(ssh_socket, session);
            if (wait_result <= 0) {
                retry_count++;
                if (retry_count >= MAX_RETRIES) {
                    ESP_LOGE(TAG, "Send command timeout after %d retries", retry_count);
                    break;
                }
            }
            continue;
        }
        if (n < 0) {
            ESP_LOGE(TAG, "Failed to write to channel: %d", (int)n);
            break;
        }
        nwritten += n;
        retry_count = 0;
    }
    
    ESP_LOGI(TAG, "Command sent: %d bytes", (int)nwritten);
}

void SSHTerminal::ssh_receive_task(void* param)
{
    SSHTerminal* terminal = (SSHTerminal*)param;
    char buffer[1024];
    ssize_t rc;
    uint32_t last_rx_ms = 0;
    bool is_busy = false;
    uint8_t spin_idx = 0;
    static const char* spin_chars[] = {"[ / ]", "[ - ]", "[ \\ ]", "[ | ]"};

    ESP_LOGI(TAG, "SSH receive task started");
    terminal->set_activity_status("[ONLINE]", 0x50FF50);

    while (terminal->ssh_connected && terminal->channel) {
        if (terminal->ssh_rx_paused) {
            terminal->ssh_rx_is_paused = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        terminal->ssh_rx_is_paused = false;

        if (terminal->ssh_mutex) xSemaphoreTakeRecursive(terminal->ssh_mutex, portMAX_DELAY);
        if (!terminal->channel || !terminal->ssh_connected) {
            if (terminal->ssh_mutex) xSemaphoreGiveRecursive(terminal->ssh_mutex);
            break;
        }
        rc = libssh2_channel_read(terminal->channel, buffer, sizeof(buffer) - 1);
        if (terminal->ssh_mutex) xSemaphoreGiveRecursive(terminal->ssh_mutex);
        
        if (rc > 0) {
            buffer[rc] = '\0';
            terminal->process_received_data(buffer, rc);
            
            spin_idx = (spin_idx + 1) % 4;
            terminal->set_activity_status(spin_chars[spin_idx], 0x50FFFF);
            last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
            is_busy = true;
            vTaskDelay(1);
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            terminal->flush_display_buffer();
            if (is_busy) {
                uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                if (now - last_rx_ms > 350) {
                    terminal->set_activity_status("[ONLINE]", 0x50FF50);
                    is_busy = false;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (rc < 0) {
            ESP_LOGE(TAG, "Read error: %d", (int)rc);
            break;
        }

        bool is_eof = false;
        if (terminal->ssh_mutex) xSemaphoreTakeRecursive(terminal->ssh_mutex, portMAX_DELAY);
        if (terminal->channel) {
            is_eof = (libssh2_channel_eof(terminal->channel) != 0);
        }
        if (terminal->ssh_mutex) xSemaphoreGiveRecursive(terminal->ssh_mutex);

        if (is_eof) {
            ESP_LOGI(TAG, "Channel EOF");
            terminal->flush_display_buffer();
            break;
        }
        
        vTaskDelay(1);
    }

    ESP_LOGI(TAG, "SSH receive task ended");
    terminal->disconnect();
    vTaskDelete(NULL);
}

// ==========================================
// Full ANSI SGR 256-Color & Stream Processor
// ==========================================

// Standard 16 ANSI colors tuned for high contrast against black background
static const uint32_t s_ansi_std_colors[8] = {
    0x303030, // 0: Black (dark gray for visibility on black BG)
    0xE04040, // 1: Red
    0x00FF00, // 2: Green (default terminal green)
    0xE5C030, // 3: Yellow
    0x4090FF, // 4: Blue (bright blue for directories)
    0xD050D0, // 5: Magenta
    0x00D0D0, // 6: Cyan
    0xDDDDDD  // 7: White
};

static const uint32_t s_ansi_bright_colors[8] = {
    0x707070, // 8: Bright Black (gray)
    0xFF6060, // 9: Bright Red
    0x50FF50, // 10: Bright Green
    0xFFFF50, // 11: Bright Yellow
    0x70B0FF, // 12: Bright Blue
    0xFF70FF, // 13: Bright Magenta
    0x50FFFF, // 14: Bright Cyan
    0xFFFFFF  // 15: Bright White
};

static uint32_t ansi_256_to_rgb(int index)
{
    if (index < 0 || index > 255) return 0x00FF00;
    if (index < 8) return s_ansi_std_colors[index];
    if (index < 16) return s_ansi_bright_colors[index - 8];
    if (index >= 232) {
        // 24 grayscale steps (232-255)
        uint32_t g = 8 + (index - 232) * 10;
        return (g << 16) | (g << 8) | g;
    }
    // 6x6x6 color cube (16-231)
    int c = index - 16;
    int r = c / 36;
    int g = (c % 36) / 6;
    int b = c % 6;
    static const uint8_t cube_levels[6] = {0, 95, 135, 175, 215, 255};
    return ((uint32_t)cube_levels[r] << 16) | ((uint32_t)cube_levels[g] << 8) | cube_levels[b];
}

static bool s_color_tag_open = false;
static uint32_t s_current_fg_rgb = 0x00FF00;
static int32_t s_saved_cursor_pos = 0;

// Multi-byte UTF-8 stream buffer
static uint8_t s_utf8_buf[4];
static size_t s_utf8_len = 0;
static size_t s_utf8_expected = 0;

static uint32_t decode_utf8(const uint8_t* buf, size_t len)
{
    if (len == 1) return buf[0];
    if (len == 2) {
        return ((buf[0] & 0x1F) << 6) | (buf[1] & 0x3F);
    }
    if (len == 3) {
        return ((buf[0] & 0x0F) << 12) | ((buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
    }
    if (len == 4) {
        return ((buf[0] & 0x07) << 18) | ((buf[1] & 0x3F) << 12) | ((buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
    }
    return 0;
}

struct EmojiBadge {
    uint32_t cp;
    const char* text;
};

static const EmojiBadge s_emoji_table[] = {
    // Status & Result
    {0x2705, "#50FF50 [OK]#"},       // ✅ White Heavy Check Mark
    {0x2714, "#50FF50 [OK]#"},       // ✔ Heavy Check Mark
    {0x2713, "#50FF50 [OK]#"},       // ✓ Check Mark
    {0x274C, "#FF6060 [FAIL]#"},     // ❌ Cross Mark
    {0x2716, "#FF6060 [FAIL]#"},     // ✖ Heavy Multiplication X
    {0x2715, "#FF6060 [FAIL]#"},     // ✕ Multiplication X
    {0x274E, "#FF6060 [FAIL]#"},     // ❎ Negative Squared Cross Mark
    {0x26A0, "#FFFF50 [WARN]#"},     // ⚠️ Warning Sign
    {0x2139, "#50FFFF [INFO]#"},     // ℹ️ Information Source
    {0x2728, "#FFFF50 [FEAT]#"},     // ✨ Sparkles
    {0x1F389, "#50FF50 [DONE]#"},    // 🎉 Party Popper
    {0x1F44D, "#50FF50 [+1]#"},      // 👍 Thumbs Up
    {0x1F44E, "#FF6060 [-1]#"},      // 👎 Thumbs Down

    // DevOps, Git & CI/CD
    {0x1F680, "#70B0FF [DEPLOY]#"},  // 🚀 Rocket
    {0x1F525, "#FF6060 [HOT]#"},     // 🔥 Fire
    {0x1F4E6, "#E5C030 [PKG]#"},     // 📦 Package
    {0x1F41B, "#FF70FF [BUG]#"},     // 🐛 Bug
    {0x1F691, "#FF6060 [HOTFIX]#"},  // 🚑 Ambulance
    {0x1F50D, "#50FFFF [FIND]#"},    // 🔍 Magnifying Glass Left
    {0x1F50E, "#50FFFF [FIND]#"},    // 🔎 Magnifying Glass Right
    {0x1F4A1, "#FFFF50 [TIP]#"},     // 💡 Light Bulb
    {0x1F512, "#E5C030 [SEC]#"},     // 🔒 Lock
    {0x1F510, "#E5C030 [AUTH]#"},    // 🔐 Key with Lock
    {0x1F513, "#50FF50 [OPEN]#"},    // 🔓 Open Lock
    {0x1F511, "#E5C030 [KEY]#"},     // 🔑 Key
    {0x26A1,  "#FFFF50 [FAST]#"},    // ⚡ High Voltage
    {0x1F6D1, "#FF6060 [STOP]#"},    // 🛑 Stop Sign
    {0x1F480, "#FF6060 [FATAL]#"},   // 💀 Skull
    {0x1F4A5, "#FF6060 [BREAK]#"},   // 💥 Collision
    {0x1F3AF, "#FF6060 [TARGET]#"},  // 🎯 Direct Hit
    {0x1F527, "#70B0FF [CONF]#"},    // 🔧 Wrench
    {0x1F528, "#70B0FF [BUILD]#"},   // 🔨 Hammer
    {0x2699,  "#DDDDDD [GEAR]#"},    // ⚙️ Gear
    {0x1F4CC, "#FF6060 [PIN]#"},     // 📌 Pushpin
    {0x1F4DD, "#DDDDDD [DOCS]#"},    // 📝 Memo
    {0x1F4C1, "#4090FF [DIR]#"},     // 📁 Folder
    {0x1F4C2, "#4090FF [DIR]#"},     // 📂 Open Folder
    {0x1F4C4, "#DDDDDD [FILE]#"},    // 📄 Page
    {0x1F4BE, "#50FF50 [SAVE]#"},    // 💾 Floppy Disk
    {0x1F4AC, "#DDDDDD [COMMENT]#"}, // 💬 Speech Balloon
    {0x1F6E1, "#50FFFF [SEC]#"},     // 🛡️ Shield
    {0x1F310, "#50FFFF [NET]#"},     // 🌐 Globe
    {0x1F517, "#70B0FF [LINK]#"},    // 🔗 Link
    {0x23F3,  "#707070 [...]#"},     // ⏳ Hourglass Not Done
    {0x231B,  "#707070 [...]#"},     // ⌛ Hourglass Done
    {0x23F1,  "#707070 [TIME]#"},    // ⏱️ Stopwatch
    {0x231A,  "#707070 [TIME]#"},    // ⌚ Watch
    {0x2757,  "#FF6060 [!]#"},       // ❗ Exclamation
    {0x2753,  "#FFFF50 [?]#"},       // ❓ Question
    {0x203C,  "#FF6060 [!!]#"},      // ‼️ Double Exclamation
    {0x2049,  "#FFFF50 [!?]#"},      // ⁉️ Exclamation Question
    {0x1F6A8, "#FF6060 [ALERT]#"},   // 🚨 Police Siren
    {0x1F6A7, "#FFFF50 [WIP]#"},     // 🚧 Construction
    {0x1F49A, "#50FF50 [CI-OK]#"},   // 💚 Green Heart
    {0x1F477, "#70B0FF [CI]#"},      // 👷 Construction Worker
    {0x1F4C8, "#50FF50 [STATS]#"},   // 📈 Chart
    {0x267B,  "#50FF50 [REFACTOR]#"},// ♻️ Recycling Symbol
    {0x2795,  "#50FF50 [+]#"},       // ➕ Plus
    {0x2796,  "#FF6060 [-]#"},       // ➖ Minus
    {0x1F500, "#70B0FF [MERGE]#"},   // 🔀 Shuffle / Merge
    {0x1F69A, "#4090FF [MOVE]#"},    // 🚚 Truck
    {0x1F5C3, "#4090FF [DB]#"},      // 🗃️ Card Box
    {0x1F507, "#707070 [MUTE]#"},    // 🔇 Muted
    {0x1F50A, "#50FF50 [LOGS]#"},    // 🔊 Speaker High Volume
    {0x1F648, "#707070 [IGNORE]#"},  // 🙈 See-No-Evil
    {0x1F9EA, "#50FFFF [TEST]#"},    // 🧪 Test Tube
    {0x1FA7A, "#50FF50 [HEALTH]#"},  // 🩺 Stethoscope
    {0x1F9F1, "#E5C030 [INFRA]#"},   // 🧱 Brick
    {0x1F4BB, "#70B0FF [DEV]#"},     // 💻 Laptop
    {0x1F5D1, "#FF6060 [DEL]#"},     // 🗑️ Wastebasket
    {0x1F4E1, "#50FFFF [SYNC]#"},    // 📡 Satellite

    // Arrows & Directions
    {0x279C, "->"},                  // ➜ Heavy Round-Tipped Right Arrow
    {0x2794, "->"},                  // ➔ Heavy Wide-Headed Right Arrow
    {0x2192, "->"},                  // → Rightwards Arrow
    {0x2190, "<-"},                  // ← Leftwards Arrow
    {0x2191, "^"},                   // ↑ Upwards Arrow
    {0x2193, "v"},                   // ↓ Downwards Arrow
    {0x21D2, "=>"},                  // ⇒ Double Right Arrow
    {0x21D0, "<="},                  // ⇐ Double Left Arrow
    {0x21D4, "<=>"},                 // ⇔ Double Left Right Arrow
    {0x2B06, "^"},                   // ⬆️ Up Arrow
    {0x2B07, "v"},                   // ⬇️ Down Arrow

    // Punctuation & Quotes
    {0x2022, "*"},                   // • Bullet
    {0x00B7, "."},                   // · Middle Dot
    {0x2026, "..."},                 // … Horizontal Ellipsis
    {0x201C, "\""},                 // “ Left Double Quotation Mark
    {0x201D, "\""},                 // ” Right Double Quotation Mark
    {0x2018, "'"},                   // ‘ Left Single Quotation Mark
    {0x2019, "'"},                   // ’ Right Single Quotation Mark
    {0x2014, "--"},                  // — Em Dash
    {0x2013, "-"},                   // – En Dash
    {0x00A0, " "},                   // Non-breaking space
    {0x00A9, "(c)"},                 // ©
    {0x00AE, "(R)"},                 // ®
    {0x00B0, " deg"},                // °
    {0x00B1, "+/-"},                 // ±
    {0x00D7, "x"},                   // ×
    {0x00F7, "/"},                   // ÷

    // Box drawing & Geometric shapes
    {0x2500, "-"},                   // ─ Light Horizontal
    {0x2501, "="},                   // ━ Heavy Horizontal
    {0x2502, "|"},                   // │ Light Vertical
    {0x2503, "|"},                   // ┃ Heavy Vertical
    {0x250C, "+"},                   // ┌
    {0x2510, "+"},                   // ┐
    {0x2514, "+"},                   // └
    {0x2518, "+"},                   // ┘
    {0x251C, "+"},                   // ├
    {0x2524, "+"},                   // ┤
    {0x252C, "+"},                   // ┬
    {0x2534, "+"},                   // ┴
    {0x253C, "+"},                   // ┼
    {0x25AA, "*"},                   // ▪ Black Small Square
    {0x25AB, "-"},                   // ▫ White Small Square
    {0x25CF, "o"},                   // ● Black Circle
    {0x25CB, "o"},                   // ○ White Circle
    {0x25B6, ">"},                   // ▶ Black Right Triangle
    {0x25C0, "<"},                   // ◀ Black Left Triangle
    {0x25B2, "^"},                   // ▲ Black Up Triangle
    {0x25BC, "v"}                    // ▼ Black Down Triangle
};

static const char* get_emoji_representation(uint32_t cp)
{
    size_t count = sizeof(s_emoji_table) / sizeof(s_emoji_table[0]);
    for (size_t i = 0; i < count; i++) {
        if (s_emoji_table[i].cp == cp) {
            return s_emoji_table[i].text;
        }
    }

    if (cp >= 0x1F000 && cp <= 0x1FAFF) {
        return "#707070 [?]#";
    }
    if ((cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B50 && cp <= 0x2B55)) {
        return "#707070 [?]#";
    }
    if (cp > 0x7F) {
        return " ";
    }
    return "";
}

static void output_badge_or_symbol(lv_obj_t* ta, const char* rep)
{
    if (!ta || !rep || rep[0] == '\0') return;

    int32_t cpos = lv_textarea_get_cursor_pos(ta);
    const char* cur_t = lv_textarea_get_text(ta);
    size_t cur_len = cur_t ? strlen(cur_t) : 0;
    if (cpos >= 0 && cpos < (int32_t)cur_len && cur_t[cpos] != '\n' && cur_t[cpos] != '\0') {
        if (cur_t[cpos] == '#' && cpos + 7 < (int32_t)cur_len && cur_t[cpos + 7] == ' ') {
            for (int k = 0; k < 8; k++) lv_textarea_delete_char_forward(ta);
        } else if (cur_t[cpos] == '#' && cpos + 1 < (int32_t)cur_len && cur_t[cpos + 1] == '#') {
            lv_textarea_delete_char_forward(ta);
            lv_textarea_delete_char_forward(ta);
        } else {
            lv_textarea_delete_char_forward(ta);
        }
    }

    if (rep[0] == '#') {
        if (s_color_tag_open) {
            lv_textarea_add_char(ta, '#');
        }
        lv_textarea_add_text(ta, rep);
        if (s_color_tag_open) {
            char tag[16];
            snprintf(tag, sizeof(tag), "#%06lx ", (unsigned long)s_current_fg_rgb);
            lv_textarea_add_text(ta, tag);
        }
    } else {
        lv_textarea_add_text(ta, rep);
    }
}

void SSHTerminal::erase_to_end_of_line()
{
    if (!terminal_output) return;
    const char* txt = lv_textarea_get_text(terminal_output);
    if (!txt || txt[0] == '\0') return;

    int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
    size_t txt_len = strlen(txt);
    if (cpos < 0) cpos = 0;
    if (cpos >= (int32_t)txt_len) return;

    // Determine the end of the current line (up to next newline or end of buffer)
    int32_t eol_pos = (int32_t)txt_len;
    const char* next_nl = strchr(txt + cpos, '\n');
    if (next_nl != nullptr) {
        eol_pos = (int32_t)(next_nl - txt);
    }

    std::string kept(txt, cpos);
    kept.append(txt + eol_pos);
    lv_textarea_set_text(terminal_output, kept.c_str());
    lv_textarea_set_cursor_pos(terminal_output, cpos);
    s_color_tag_open = false;
    s_utf8_len = 0;
    s_utf8_expected = 0;
}

void SSHTerminal::erase_current_line()
{
    if (!terminal_output) return;
    const char* txt = lv_textarea_get_text(terminal_output);
    if (!txt || txt[0] == '\0') return;
    
    // Find start of current line
    int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
    size_t tlen = strlen(txt);
    if (cpos > (int32_t)tlen) cpos = (int32_t)tlen;
    if (cpos < 0) cpos = 0;

    int32_t sol = 0;
    for (int32_t k = cpos - 1; k >= 0; k--) {
        if (txt[k] == '\n') {
            sol = k + 1;
            break;
        }
    }
    lv_textarea_set_cursor_pos(terminal_output, sol);
    erase_to_end_of_line();
}

std::string SSHTerminal::strip_ansi_codes(const char* data, size_t len)
{
    return std::string(data, len);
}

void SSHTerminal::process_received_data(const char* data, size_t len)
{
    bytes_received += len;
    
    if (!bsp_display_lock(1500)) {
        return;
    }

    bool pending_cr = false;
    bool is_bold = false;

    char text_batch[256];
    size_t batch_len = 0;
    auto flush_batch = [&]() {
        if (batch_len > 0) {
            text_batch[batch_len] = '\0';
            lv_textarea_add_text(terminal_output, text_batch);
            batch_len = 0;
        }
    };

    for (size_t i = 0; i < len; i++) {
        char ch = data[i];

        // 1. ANSI Escape sequences
        if (ch == '\x1B' || ch == '\033') {
            flush_batch();
            if (pending_cr) {
                pending_cr = false;
            }
            if (i + 1 < len && data[i + 1] == 'M') {
                // Reverse Index (RI) -> cursor UP
                i++;
                lv_textarea_cursor_up(terminal_output);
                continue;
            } else if (i + 1 < len && data[i + 1] == '7') {
                // Save Cursor (DECSC)
                i++;
                s_saved_cursor_pos = lv_textarea_get_cursor_pos(terminal_output);
                continue;
            } else if (i + 1 < len && data[i + 1] == '8') {
                // Restore Cursor (DECRC)
                i++;
                lv_textarea_set_cursor_pos(terminal_output, s_saved_cursor_pos);
                continue;
            }
            if (i + 1 < len && data[i + 1] == '[') {
                i += 2;
                size_t csi_start = i;
                while (i < len && !((data[i] >= 'A' && data[i] <= 'Z') || 
                                    (data[i] >= 'a' && data[i] <= 'z') ||
                                    data[i] == '~')) {
                    i++;
                }
                if (i < len) {
                    char cmd = data[i];
                    std::string params = "";
                    if (i > csi_start) {
                        params = std::string(data + csi_start, i - csi_start);
                    }

                    if (cmd == 'm') {
                        // SGR (Select Graphic Rendition) - Colors & Styles
                        std::vector<int> codes;
                        if (params.empty()) {
                            codes.push_back(0); // \033[m -> reset
                        } else {
                            size_t p = 0;
                            while (p < params.size()) {
                                size_t next_semi = params.find(';', p);
                                if (next_semi == std::string::npos) next_semi = params.find(':', p);
                                std::string token = (next_semi == std::string::npos) 
                                    ? params.substr(p) 
                                    : params.substr(p, next_semi - p);
                                codes.push_back(token.empty() ? 0 : atoi(token.c_str()));
                                if (next_semi == std::string::npos) break;
                                p = next_semi + 1;
                            }
                        }

                        for (size_t ci = 0; ci < codes.size(); ci++) {
                            int code = codes[ci];
                            if (code == 0) {
                                // Reset color
                                is_bold = false;
                                if (s_color_tag_open) {
                                    lv_textarea_add_char(terminal_output, '#');
                                    s_color_tag_open = false;
                                }
                            } else if (code == 1) {
                                is_bold = true;
                            } else if (code == 22) {
                                is_bold = false;
                            } else if (code >= 30 && code <= 37) {
                                // Standard 8 foreground colors
                                uint32_t rgb = is_bold 
                                    ? s_ansi_bright_colors[code - 30] 
                                    : s_ansi_std_colors[code - 30];
                                if (s_color_tag_open) {
                                    lv_textarea_add_char(terminal_output, '#');
                                }
                                char tag[16];
                                snprintf(tag, sizeof(tag), "#%06lx ", (unsigned long)rgb);
                                lv_textarea_add_text(terminal_output, tag);
                                s_color_tag_open = true;
                                s_current_fg_rgb = rgb;
                            } else if (code >= 90 && code <= 97) {
                                // Bright foreground colors
                                uint32_t rgb = s_ansi_bright_colors[code - 90];
                                if (s_color_tag_open) {
                                    lv_textarea_add_char(terminal_output, '#');
                                }
                                char tag[16];
                                snprintf(tag, sizeof(tag), "#%06lx ", (unsigned long)rgb);
                                lv_textarea_add_text(terminal_output, tag);
                                s_color_tag_open = true;
                                s_current_fg_rgb = rgb;
                            } else if (code == 39) {
                                // Default foreground
                                if (s_color_tag_open) {
                                    lv_textarea_add_char(terminal_output, '#');
                                    s_color_tag_open = false;
                                }
                            } else if (code == 38 && ci + 2 < codes.size() && codes[ci + 1] == 5) {
                                // 256-color palette (\033[38;5;<n>m)
                                int col_idx = codes[ci + 2];
                                uint32_t rgb = ansi_256_to_rgb(col_idx);
                                if (s_color_tag_open) {
                                    lv_textarea_add_char(terminal_output, '#');
                                }
                                char tag[16];
                                snprintf(tag, sizeof(tag), "#%06lx ", (unsigned long)rgb);
                                lv_textarea_add_text(terminal_output, tag);
                                s_color_tag_open = true;
                                s_current_fg_rgb = rgb;
                                ci += 2;
                            } else if (code == 38 && ci + 4 < codes.size() && codes[ci + 1] == 2) {
                                // 24-bit TrueColor (\033[38;2;<r>;<g>;<b>m)
                                int r = codes[ci + 2];
                                int g = codes[ci + 3];
                                int b = codes[ci + 4];
                                uint32_t rgb = ((uint32_t)(r & 0xFF) << 16) | ((uint32_t)(g & 0xFF) << 8) | (b & 0xFF);
                                if (s_color_tag_open) {
                                    lv_textarea_add_char(terminal_output, '#');
                                }
                                char tag[16];
                                snprintf(tag, sizeof(tag), "#%06lx ", (unsigned long)rgb);
                                lv_textarea_add_text(terminal_output, tag);
                                s_color_tag_open = true;
                                s_current_fg_rgb = rgb;
                                ci += 4;
                            }
                        }
                    } else if (cmd == 'A') {
                        // Cursor Up
                        int count = 1;
                        if (!params.empty()) count = atoi(params.c_str());
                        if (count < 1) count = 1;
                        for (int k = 0; k < count; k++) {
                            lv_textarea_cursor_up(terminal_output);
                        }
                    } else if (cmd == 'B') {
                        // Cursor Down
                        int count = 1;
                        if (!params.empty()) count = atoi(params.c_str());
                        if (count < 1) count = 1;
                        for (int k = 0; k < count; k++) {
                            lv_textarea_cursor_down(terminal_output);
                        }
                    } else if (cmd == 'C') {
                        // Cursor right (clamped to end of current line)
                        int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
                        const char* txt = lv_textarea_get_text(terminal_output);
                        size_t tlen = txt ? strlen(txt) : 0;
                        if (cpos > (int32_t)tlen) cpos = (int32_t)tlen;
                        if (cpos < 0) cpos = 0;
                        int32_t eol = (int32_t)tlen;
                        if (txt) {
                            const char* next_nl = strchr(txt + cpos, '\n');
                            if (next_nl) eol = (int32_t)(next_nl - txt);
                        }
                        int count = 1;
                        if (!params.empty()) count = atoi(params.c_str());
                        if (count < 1) count = 1;
                        int32_t target = cpos + count;
                        if (target > eol) target = eol;
                        lv_textarea_set_cursor_pos(terminal_output, target);
                    } else if (cmd == 'D') {
                        // Non-destructive cursor left (clamped to start of current line)
                        int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
                        const char* txt = lv_textarea_get_text(terminal_output);
                        size_t tlen = txt ? strlen(txt) : 0;
                        if (cpos > (int32_t)tlen) cpos = (int32_t)tlen;
                        if (cpos < 0) cpos = 0;
                        int32_t sol = 0;
                        if (txt) {
                            for (int32_t k = cpos - 1; k >= 0; k--) {
                                if (txt[k] == '\n') {
                                    sol = k + 1;
                                    break;
                                }
                            }
                        }
                        int count = 1;
                        if (!params.empty()) count = atoi(params.c_str());
                        if (count < 1) count = 1;
                        int32_t target = cpos - count;
                        if (target < sol) target = sol;
                        lv_textarea_set_cursor_pos(terminal_output, target);
                    } else if (cmd == 'G' || cmd == '`') {
                        // Cursor Horizontal Absolute (CHA / HPA)
                        int target_col = 1;
                        if (!params.empty()) target_col = atoi(params.c_str());
                        if (target_col < 1) target_col = 1;

                        const char* txt = lv_textarea_get_text(terminal_output);
                        if (txt && txt[0] != '\0') {
                            int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
                            size_t tlen = strlen(txt);
                            if (cpos > (int32_t)tlen) cpos = (int32_t)tlen;
                            if (cpos < 0) cpos = 0;
                            int32_t sol = 0;
                            for (int32_t k = cpos - 1; k >= 0; k--) {
                                if (txt[k] == '\n') {
                                    sol = k + 1;
                                    break;
                                }
                            }
                            int32_t eol = (int32_t)tlen;
                            const char* next_nl = strchr(txt + sol, '\n');
                            if (next_nl) eol = (int32_t)(next_nl - txt);

                            int32_t pos = sol + (target_col - 1);
                            if (pos > eol) pos = eol;
                            lv_textarea_set_cursor_pos(terminal_output, pos);
                        }
                    } else if (cmd == 'H' || cmd == 'f') {
                        // Cursor Position (CUP / HVP)
                        int row = 1;
                        int col = 1;
                        if (!params.empty()) {
                            size_t semi = params.find(';');
                            if (semi != std::string::npos) {
                                row = atoi(params.substr(0, semi).c_str());
                                col = atoi(params.substr(semi + 1).c_str());
                            } else {
                                row = atoi(params.c_str());
                            }
                        }
                        if (row < 1) row = 1;
                        if (col < 1) col = 1;
                        if (row == 1 && col == 1) {
                            const char* txt = lv_textarea_get_text(terminal_output);
                            if (!txt || txt[0] == '\0') {
                                lv_textarea_set_cursor_pos(terminal_output, 0);
                            }
                        }
                    } else if (cmd == 'J') {
                        // Erase in Display (ED)
                        if (params == "2" || params == "3") {
                            lv_textarea_set_text(terminal_output, "");
                            lv_textarea_set_cursor_pos(terminal_output, 0);
                            s_color_tag_open = false;
                            s_utf8_len = 0;
                            s_utf8_expected = 0;
                        } else if (params.empty() || params == "0") {
                            int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
                            const char* txt = lv_textarea_get_text(terminal_output);
                            if (txt && cpos >= 0 && cpos < (int32_t)strlen(txt)) {
                                std::string kept(txt, cpos);
                                lv_textarea_set_text(terminal_output, kept.c_str());
                                lv_textarea_set_cursor_pos(terminal_output, cpos);
                            }
                        }
                    } else if (cmd == 'K') {
                        // Erase in line
                        if (params == "2") {
                            // \033[2K: Erase entire current line
                            erase_current_line();
                        } else {
                            // \033[K or \033[0K: Erase from cursor to end of line
                            erase_to_end_of_line();
                        }
                    } else if (cmd == 'P') {
                        // DCH - Delete character(s) at cursor position
                        int count = 1;
                        if (!params.empty()) count = atoi(params.c_str());
                        if (count < 1) count = 1;
                        for (int k = 0; k < count; k++) {
                            lv_textarea_delete_char_forward(terminal_output);
                        }
                    } else if (cmd == 's') {
                        // Save cursor position
                        s_saved_cursor_pos = lv_textarea_get_cursor_pos(terminal_output);
                    } else if (cmd == 'u') {
                        // Restore cursor position
                        lv_textarea_set_cursor_pos(terminal_output, s_saved_cursor_pos);
                    }
                }
            } else if (i + 1 < len && data[i + 1] == ']') {
                // OSC sequence: skip until BEL (\a) or ST (\x1B\\)
                i += 2;
                while (i < len) {
                    if (data[i] == '\a') break;
                    if (data[i] == '\x1B' && i + 1 < len && data[i + 1] == '\\') {
                        i++;
                        break;
                    }
                    i++;
                }
            } else if (i + 1 < len && (data[i + 1] == '(' || data[i + 1] == ')')) {
                i += 2;
            }
            continue;
        }

        // 2. Carriage return '\r'
        if (ch == '\r') {
            flush_batch();
            s_utf8_expected = 0;
            s_utf8_len = 0;
            if (i + 1 < len && data[i + 1] == '\n') {
                i++;
                ch = '\n';
                // Fall through to handle '\n'
            } else {
                const char* cur_t = lv_textarea_get_text(terminal_output);
                if (cur_t && cur_t[0] != '\0') {
                    int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
                    size_t tlen = strlen(cur_t);
                    if (cpos > (int32_t)tlen) cpos = (int32_t)tlen;
                    if (cpos < 0) cpos = 0;
                    int32_t sol = 0;
                    for (int32_t k = cpos - 1; k >= 0; k--) {
                        if (cur_t[k] == '\n') {
                            sol = k + 1;
                            break;
                        }
                    }
                    lv_textarea_set_cursor_pos(terminal_output, sol);
                }
                ssh_at_line_start = true;
                pending_cr = true;
                continue;
            }
        }

        if (pending_cr) {
            pending_cr = false;
        }

        // 3. Line feed '\n'
        if (ch == '\n') {
            flush_batch();
            ssh_at_line_start = true;
            int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
            const char* cur_t = lv_textarea_get_text(terminal_output);
            size_t cur_len = cur_t ? strlen(cur_t) : 0;
            if (cpos > (int32_t)cur_len) cpos = (int32_t)cur_len;
            if (cpos < 0) cpos = 0;

            const char* next_nl = (cur_t && cpos < (int32_t)cur_len) ? strchr(cur_t + cpos, '\n') : nullptr;
            if (next_nl == nullptr) {
                // On the last line: always append newline at the end
                lv_textarea_set_cursor_pos(terminal_output, LV_TEXTAREA_CURSOR_LAST);
                lv_textarea_add_char(terminal_output, '\n');
            } else {
                // Moving down within existing multi-line screen
                lv_textarea_cursor_down(terminal_output);
            }
            continue;
        }

        // 4. Backspace '\b'
        if (ch == '\b') {
            flush_batch();
            int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
            const char* txt = lv_textarea_get_text(terminal_output);
            int32_t sol = 0;
            if (txt && cpos > 0) {
                for (int32_t k = cpos - 1; k >= 0; k--) {
                    if (txt[k] == '\n') {
                        sol = k + 1;
                        break;
                    }
                }
            }
            if (i + 2 < len && data[i + 1] == ' ' && data[i + 2] == '\b') {
                i += 2;
                if (cpos > sol) {
                    lv_textarea_delete_char(terminal_output);
                }
            } else if (i + 3 < len && data[i + 1] == '\x1B' && data[i + 2] == '[' && data[i + 3] == 'K') {
                i += 3;
                if (cpos > sol) {
                    lv_textarea_delete_char(terminal_output);
                }
                erase_to_end_of_line();
            } else {
                // Non-destructive cursor left (clamped to start of line)
                if (cpos > sol) {
                    lv_textarea_set_cursor_pos(terminal_output, cpos - 1);
                }
            }
            continue;
        }

        // 5. Literal '#' character escape
        if (ch == '#') {
            flush_batch();
            s_utf8_expected = 0;
            s_utf8_len = 0;
            int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
            const char* cur_t = lv_textarea_get_text(terminal_output);
            size_t cur_len = cur_t ? strlen(cur_t) : 0;
            if (cpos >= 0 && cpos < (int32_t)cur_len && cur_t[cpos] != '\n' && cur_t[cpos] != '\0') {
                if (cur_t[cpos] == '#' && cpos + 7 < (int32_t)cur_len && cur_t[cpos + 7] == ' ') {
                    for (int k = 0; k < 8; k++) lv_textarea_delete_char_forward(terminal_output);
                } else if (cur_t[cpos] == '#' && cpos + 1 < (int32_t)cur_len && cur_t[cpos + 1] == '#') {
                    lv_textarea_delete_char_forward(terminal_output);
                    lv_textarea_delete_char_forward(terminal_output);
                } else {
                    lv_textarea_delete_char_forward(terminal_output);
                }
            }
            // In LVGL recolor mode, '##' renders a single literal '#'
            lv_textarea_add_text(terminal_output, "##");
            continue;
        }

        // 6. Bell or control characters
        if (ch == '\a') continue;
        if ((unsigned char)ch < 32 && ch != '\t') continue;

        // 7. UTF-8 Multi-byte decoder & Smart Emoji Engine
        if ((unsigned char)ch >= 0x80) {
            flush_batch();
            uint8_t u = (uint8_t)ch;
            if (u >= 0xC0 && u <= 0xF7) {
                // Start of UTF-8 sequence (lead byte)
                s_utf8_buf[0] = u;
                s_utf8_len = 1;
                if ((u & 0xE0) == 0xC0) s_utf8_expected = 2;
                else if ((u & 0xF0) == 0xE0) s_utf8_expected = 3;
                else if ((u & 0xF8) == 0xF0) s_utf8_expected = 4;
                else { s_utf8_expected = 0; s_utf8_len = 0; }
                continue;
            } else if (s_utf8_expected > 0 && (u & 0xC0) == 0x80) {
                // UTF-8 continuation byte
                s_utf8_buf[s_utf8_len++] = u;
                if (s_utf8_len == s_utf8_expected) {
                    uint32_t codepoint = decode_utf8(s_utf8_buf, s_utf8_len);
                    s_utf8_expected = 0;
                    s_utf8_len = 0;

                    if (codepoint == 0xFE0F || codepoint == 0xFE0E || codepoint == 0x200D) {
                        // Variation selector or zero-width joiner: silent skip
                        continue;
                    }

                    const char* rep = get_emoji_representation(codepoint);
                    if (rep && rep[0] != '\0') {
                        output_badge_or_symbol(terminal_output, rep);
                    }
                }
                continue;
            } else {
                // Stray continuation or invalid byte: discard
                s_utf8_expected = 0;
                s_utf8_len = 0;
                continue;
            }
        }

        // 8. Regular ASCII character or tab
        s_utf8_expected = 0;
        s_utf8_len = 0;
        if (ch == '\t') {
            flush_batch();
            lv_textarea_add_char(terminal_output, '\t');
            continue;
        }

        int32_t cpos = lv_textarea_get_cursor_pos(terminal_output);
        const char* cur_t = lv_textarea_get_text(terminal_output);
        size_t cur_len = cur_t ? strlen(cur_t) : 0;
        if (cpos >= (int32_t)cur_len) {
            // High-speed chunk batching when at the end of the line
            text_batch[batch_len++] = ch;
            if (batch_len >= sizeof(text_batch) - 1) {
                flush_batch();
            }
        } else {
            // Overwrite mode when inserting in the middle of a line
            flush_batch();
            if (cpos >= 0 && cur_t[cpos] != '\n' && cur_t[cpos] != '\0') {
                if (cur_t[cpos] == '#' && cpos + 7 < (int32_t)cur_len && cur_t[cpos + 7] == ' ') {
                    for (int k = 0; k < 8; k++) lv_textarea_delete_char_forward(terminal_output);
                } else if (cur_t[cpos] == '#' && cpos + 1 < (int32_t)cur_len && cur_t[cpos + 1] == '#') {
                    lv_textarea_delete_char_forward(terminal_output);
                    lv_textarea_delete_char_forward(terminal_output);
                } else {
                    lv_textarea_delete_char_forward(terminal_output);
                }
            }
            lv_textarea_add_char(terminal_output, (uint32_t)(unsigned char)ch);
        }
    }

    flush_batch();

    if (!is_user_scrolled_back) {
        lv_obj_scroll_to_y(terminal_output, LV_COORD_MAX, LV_ANIM_OFF);
    }

    // Prune if buffer grows too large (> 12KB with color tags)
    const char* cur_txt = lv_textarea_get_text(terminal_output);
    size_t cur_len = strlen(cur_txt);
    if (cur_len > 12288) {
        const char* p = cur_txt + (cur_len - 6144);
        const char* nl = strchr(p, '\n');
        if (nl) {
            lv_textarea_set_text(terminal_output, nl + 1);
            lv_textarea_set_cursor_pos(terminal_output, LV_TEXTAREA_CURSOR_LAST);
        }
    }

    bsp_display_unlock();

    // Update byte counter
    if (bytes_received > 0 && byte_counter_label && bsp_display_lock(500)) {
        char counter_text[32];
        if (bytes_received < 1024) {
            snprintf(counter_text, sizeof(counter_text), "%zu B", bytes_received);
        } else if (bytes_received < 1024 * 1024) {
            snprintf(counter_text, sizeof(counter_text), "%.1f KB", bytes_received / 1024.0);
        } else {
            snprintf(counter_text, sizeof(counter_text), "%.2f MB", bytes_received / (1024.0 * 1024.0));
        }
        lv_label_set_text(byte_counter_label, counter_text);
        bsp_display_unlock();
    }
}

void SSHTerminal::flush_display_buffer()
{
    if (!bsp_display_lock(500)) {
        return;
    }
    if (terminal_output && !is_user_scrolled_back) {
        lv_obj_scroll_to_y(terminal_output, LV_COORD_MAX, LV_ANIM_OFF);
    }
    bsp_display_unlock();
}

void SSHTerminal::update_status_bar()
{
    if (!status_bar) return;
    
    if (!bsp_display_lock(500)) {
        return;
    }

    std::string status;
    
    if (battery_initialized) {
        float voltage = battery.readBatteryVoltage();
        if (voltage > 0.1f) {
            char voltage_text[32];
            snprintf(voltage_text, sizeof(voltage_text), "%.2fV | ", voltage);
            status = voltage_text;
            ESP_LOGD(TAG, "Battery voltage displayed: %.2fV", voltage);
        } else {
            ESP_LOGW(TAG, "Battery voltage too low or invalid: %.2fV", voltage);
        }
    } else {
        ESP_LOGD(TAG, "Battery not initialized, skipping voltage display");
    }
    
    if (!wifi_connected) {
        status += LV_SYMBOL_WIFI " OFF";
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFF4040), 0);
    } else {
        status += LV_SYMBOL_WIFI;
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0x00FF00), 0);
    }
    
    lv_label_set_text(status_bar, status.c_str());
    
    bsp_display_unlock();
}

void SSHTerminal::update_terminal_display()
{
}

void SSHTerminal::create_side_panel()
{
    side_panel = lv_obj_create(terminal_screen);
    lv_obj_set_size(side_panel, 100, lv_pct(100));
    lv_obj_set_style_bg_color(side_panel, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(side_panel, LV_OPA_80, 0);
    lv_obj_set_style_border_color(side_panel, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(side_panel, 2, 0);
    lv_obj_set_scrollbar_mode(side_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(side_panel, LV_DIR_VER);
    lv_obj_align(side_panel, LV_ALIGN_TOP_RIGHT, 100, 0);
    lv_obj_add_flag(side_panel, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* title = lv_label_create(side_panel);
    lv_label_set_text(title, "Keys");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    
    auto create_key_button = [this](const char* label, const char* key_seq, int y_offset) {
        lv_obj_t* btn = lv_btn_create(side_panel);
        lv_obj_set_size(btn, 85, 30);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y_offset);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x00CC00), LV_STATE_PRESSED);
        
        lv_obj_t* btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, label);
        lv_obj_set_style_text_color(btn_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_10, 0);
        lv_obj_center(btn_label);
        
        lv_obj_set_user_data(btn, (void*)key_seq);
        lv_obj_add_event_cb(btn, special_key_event_cb, LV_EVENT_CLICKED, this);
        
        return btn;
    };

    create_key_button("Up ^", "UP", 35);
    create_key_button("Down v", "DOWN", 70);
    create_key_button("<-", "LEFT", 105);
    create_key_button("->", "RIGHT", 140);
    create_key_button("Tab", "\t", 175);
    create_key_button("Ctrl+C", "\x03", 210);
    create_key_button("Ctrl+Z", "\x1A", 245);
    create_key_button("Ctrl+D", "\x04", 280);
    create_key_button("Ctrl+L", "\x0C", 315);
    create_key_button("Esc", "\x1B", 350);
    create_key_button("~ (Tilde)", "~", 385);
    create_key_button("| (Pipe)", "|", 420);
    create_key_button("\\ (Bslash)", "\\", 455);
    create_key_button("< (Less)", "<", 490);
    create_key_button("> (Great)", ">", 525);
    create_key_button("{ (LBrace)", "{", 560);
    create_key_button("} (RBrace)", "}", 595);
    create_key_button("[ (LBrack)", "[", 630);
    create_key_button("] (RBrack)", "]", 665);
    create_key_button("% (Percent)", "%", 700);
    create_key_button("= (Equals)", "=", 735);
    create_key_button("PgUp", "PGUP", 770);
    create_key_button("PgDn", "PGDN", 805);
    create_key_button("Top", "TOP", 840);
    create_key_button("Bottom", "BOTTOM", 875);
    create_key_button("Exit SSH", "EXIT", 910);
    create_key_button("Clear", "CLEAR", 945);
    create_key_button("Get Key", "GETKEY", 980);
    create_key_button("Local/SSH", "TOGGLE_SHELL", 1015);
}

void SSHTerminal::toggle_side_panel()
{
    if (!side_panel) return;
    
    if (lv_obj_has_flag(side_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(side_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(side_panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        lv_obj_add_flag(side_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(side_panel, LV_ALIGN_TOP_RIGHT, 100, 0);
    }
}

void SSHTerminal::send_special_key(const char* sequence)
{
    if (!sequence || strlen(sequence) == 0) {
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "\t") == 0 || strcmp(sequence, "TAB") == 0) {
        if (ssh_connected && channel) {
            libssh2_channel_write(channel, "\t", 1);
        } else {
            handle_tab_completion();
        }
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "UP") == 0) {
        if (ssh_connected && channel) {
            libssh2_channel_write(channel, "\033[A", 3);
        } else {
            navigate_history(1);
        }
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "DOWN") == 0) {
        if (ssh_connected && channel) {
            libssh2_channel_write(channel, "\033[B", 3);
        } else {
            navigate_history(-1);
        }
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "EXIT") == 0) {
        disconnect();
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "CLEAR") == 0) {
        clear_terminal();
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "GETKEY") == 0) {
        toggle_side_panel();
        if (ssh_connected && channel) {
            show_getkey_dialog();
        } else {
            append_text("\n[Error: Must be connected to SSH to fetch remote key]\n");
        }
        return;
    }


    if (strcmp(sequence, "TOGGLE_SHELL") == 0) {
        toggle_side_panel();
        toggle_local_shell_mode();
        return;
    }
    
    if (strcmp(sequence, "PGUP") == 0) {
        scroll_page_up();
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "PGDN") == 0) {
        scroll_page_down();
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "TOP") == 0) {
        scroll_to_top();
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "BOTTOM") == 0) {
        scroll_to_bottom();
        toggle_side_panel();
        return;
    }
    
    // Handle cursor movement keys
    if (strcmp(sequence, "LEFT") == 0) {
        if (ssh_connected && channel) {
            libssh2_channel_write(channel, "\033[D", 3);
        } else {
            move_cursor_left();
        }
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "RIGHT") == 0) {
        if (ssh_connected && channel) {
            libssh2_channel_write(channel, "\033[C", 3);
        } else {
            move_cursor_right();
        }
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "HOME") == 0) {
        move_cursor_home();
        return;
    }
    
    if (strcmp(sequence, "END") == 0) {
        move_cursor_end();
        return;
    }
    
    if (ssh_connected && channel) {
        libssh2_channel_write(channel, sequence, strlen(sequence));
        ESP_LOGI(TAG, "Sent special key sequence");
    } else {
        for (size_t s = 0; sequence[s]; s++) {
            current_input.insert(cursor_pos++, 1, sequence[s]);
        }
        update_input_display();
    }
    
    toggle_side_panel();
}

void SSHTerminal::gesture_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    
    if (dir == LV_DIR_LEFT) {
        ESP_LOGI(TAG, "Swipe left detected - showing special keys panel");
        if (terminal->side_panel && lv_obj_has_flag(terminal->side_panel, LV_OBJ_FLAG_HIDDEN)) {
            terminal->toggle_side_panel();
        }
    } else if (dir == LV_DIR_RIGHT) {
        ESP_LOGI(TAG, "Swipe right detected - hiding special keys panel");
        if (terminal->side_panel && !lv_obj_has_flag(terminal->side_panel, LV_OBJ_FLAG_HIDDEN)) {
            terminal->toggle_side_panel();
        }
    }
}

void SSHTerminal::special_key_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t* ) lv_event_get_target(e);
    const char* key_seq = (const char*)lv_obj_get_user_data(btn);
    
    if (key_seq) {
        terminal->send_special_key(key_seq);
    }
}

void SSHTerminal::load_key_from_memory(const char* keyname, const char* key_data, size_t key_len)
{
    if (!keyname || !key_data || key_len == 0) {
        ESP_LOGE(TAG, "Invalid key parameters");
        return;
    }
    
    // Convert keyname to lowercase for case-insensitive lookup
    std::string keyname_str(keyname);
    std::transform(keyname_str.begin(), keyname_str.end(), keyname_str.begin(), ::tolower);
    
    std::string key_str(key_data, key_len);
    
    loaded_keys[keyname_str] = key_str;
    
    ESP_LOGI(TAG, "Loaded SSH key: %s (%d bytes)", keyname, key_len);
}

const char* SSHTerminal::get_loaded_key(const char* keyname, size_t* len)
{
    if (!keyname) {
        return NULL;
    }
    
    // Convert keyname to lowercase for case-insensitive lookup
    std::string keyname_str(keyname);
    std::transform(keyname_str.begin(), keyname_str.end(), keyname_str.begin(), ::tolower);
    
    auto it = loaded_keys.find(keyname_str);
    if (it != loaded_keys.end()) {
        if (len) {
            *len = it->second.length();
        }
        return it->second.c_str();
    }

    // Also check with .pem appended
    if (keyname_str.size() <= 4 || keyname_str.substr(keyname_str.size() - 4) != ".pem") {
        it = loaded_keys.find(keyname_str + ".pem");
        if (it != loaded_keys.end()) {
            if (len) *len = it->second.length();
            return it->second.c_str();
        }
    } else {
        // Also check without .pem
        it = loaded_keys.find(keyname_str.substr(0, keyname_str.size() - 4));
        if (it != loaded_keys.end()) {
            if (len) *len = it->second.length();
            return it->second.c_str();
        }
    }
    
    return NULL;
}

std::vector<std::string> SSHTerminal::get_loaded_key_names()
{
    std::vector<std::string> key_names;
    
    for (const auto& pair : loaded_keys) {
        key_names.push_back(pair.first);
    }
    
    return key_names;
}

// ==========================================
// Terminal Scrollback & Navigation Methods
// ==========================================

void SSHTerminal::scroll_up(int32_t pixels)
{
    if (!terminal_output) return;
    if (!bsp_display_lock(500)) return;
    
    // In LVGL: positive dy in lv_obj_scroll_by_bounded scrolls UP towards the top
    lv_obj_scroll_by_bounded(terminal_output, 0, pixels, LV_ANIM_OFF);
    update_scroll_indicator();
    
    bsp_display_unlock();
}

void SSHTerminal::scroll_down(int32_t pixels)
{
    if (!terminal_output) return;
    if (!bsp_display_lock(500)) return;
    
    // In LVGL: negative dy in lv_obj_scroll_by_bounded scrolls DOWN towards the bottom
    lv_obj_scroll_by_bounded(terminal_output, 0, -pixels, LV_ANIM_OFF);
    update_scroll_indicator();
    
    bsp_display_unlock();
}

void SSHTerminal::scroll_page_up()
{
    scroll_up(140);
}

void SSHTerminal::scroll_page_down()
{
    scroll_down(140);
}

void SSHTerminal::scroll_to_top()
{
    if (!terminal_output) return;
    if (!bsp_display_lock(500)) return;
    
    lv_obj_scroll_to_y(terminal_output, 0, LV_ANIM_OFF);
    update_scroll_indicator();
    
    bsp_display_unlock();
}

void SSHTerminal::scroll_to_bottom()
{
    if (!terminal_output) return;
    if (!bsp_display_lock(500)) return;
    
    lv_obj_scroll_to_y(terminal_output, LV_COORD_MAX, LV_ANIM_OFF);
    is_user_scrolled_back = false;
    update_scroll_indicator();
    
    bsp_display_unlock();
}

bool SSHTerminal::is_scrolled_back()
{
    if (!terminal_output) return false;
    if (!bsp_display_lock(500)) return is_user_scrolled_back;
    int32_t bottom = lv_obj_get_scroll_bottom(terminal_output);
    bsp_display_unlock();
    return (bottom > 20);
}

void SSHTerminal::update_scroll_indicator()
{
    if (!terminal_output || !scroll_indicator_label) return;
    
    int32_t bottom = lv_obj_get_scroll_bottom(terminal_output);
    if (bottom > 20) {
        is_user_scrolled_back = true;
        lv_label_set_text(scroll_indicator_label, "[^ SCROLL]");
        lv_obj_clear_flag(scroll_indicator_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        is_user_scrolled_back = false;
        lv_obj_add_flag(scroll_indicator_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void SSHTerminal::terminal_scroll_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    if (terminal) {
        // Only update scrollback state if the scroll was initiated by active user touch
        if (lv_indev_active() != NULL) {
            terminal->update_scroll_indicator();
        }
    }
}

void SSHTerminal::handle_trackball_left()
{
    if (ssh_connected && channel) {
        // Send ANSI cursor left to remote SSH shell
        libssh2_channel_write(channel, "\033[D", 3);
    } else {
        if (history_index >= 0 || current_input.empty()) {
            navigate_history(1);
        } else {
            move_cursor_left();
        }
    }
}

void SSHTerminal::handle_trackball_right()
{
    if (ssh_connected && channel) {
        // Send ANSI cursor right to remote SSH shell
        libssh2_channel_write(channel, "\033[C", 3);
    } else {
        if (history_index >= 0) {
            navigate_history(-1);
        } else if (!current_input.empty()) {
            move_cursor_right();
        }
    }
}

void SSHTerminal::send_special_key_direct(const char* sequence)
{
    if (ssh_connected && channel && sequence) {
        libssh2_channel_write(channel, sequence, strlen(sequence));
        ESP_LOGI(TAG, "Sent special key sequence direct");
    }
}


// ==========================================
// Terminal Tab Auto-Completion
// ==========================================


// ==========================================
// SSH Key Management & Persistence (SD + NVS)
// ==========================================

bool SSHTerminal::save_key_to_sd_and_nvs(const char* keyname, const char* key_data, size_t key_len)
{
    if (!keyname || !key_data || key_len == 0) return false;

    // 1. Save to SD card if mounted
    if (is_sd_card_mounted()) {
        mkdir("/sdcard/ssh_keys", 0755);
        char filepath[256];
        std::string fname = keyname;
        if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".pem") {
            snprintf(filepath, sizeof(filepath), "/sdcard/ssh_keys/%s", fname.c_str());
        } else {
            snprintf(filepath, sizeof(filepath), "/sdcard/ssh_keys/%s.pem", fname.c_str());
        }

        FILE* f = fopen(filepath, "wb");
        if (f) {
            fwrite(key_data, 1, key_len, f);
            fclose(f);
            ESP_LOGI(TAG, "Key saved to SD card: %s", filepath);
        } else {
            ESP_LOGE(TAG, "Failed to write key to SD card: %s", filepath);
        }
    }

    // 2. Save to NVS
    save_key_to_nvs(keyname, key_data);

    // 3. Load into memory
    load_key_from_memory(keyname, key_data, key_len);
    return true;
}

bool SSHTerminal::delete_key_from_sd_and_nvs(const char* keyname)
{
    if (!keyname) return false;

    // Delete from SD card
    if (is_sd_card_mounted()) {
        char filepath[256];
        snprintf(filepath, sizeof(filepath), "/sdcard/ssh_keys/%s", keyname);
        unlink(filepath);
        snprintf(filepath, sizeof(filepath), "/sdcard/ssh_keys/%s.pem", keyname);
        unlink(filepath);
    }

    // Delete from NVS
    delete_key_from_nvs(keyname);

    // Delete from memory
    loaded_keys.erase(keyname);
    std::string pem_name = std::string(keyname) + ".pem";
    loaded_keys.erase(pem_name);

    return true;
}

void SSHTerminal::save_key_to_nvs(const char* keyname, const char* key_data)
{
    if (!keyname || !key_data) return;
    
    // Clean key name for NVS (max 15 characters, alphanumeric + underscore)
    std::string clean_name;
    for (size_t i = 0; keyname[i] != '\0' && clean_name.size() < 15; i++) {
        char c = keyname[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            clean_name += c;
        }
    }
    if (clean_name.empty()) clean_name = "key1";

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("ssh_keys", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace ssh_keys: %s", esp_err_to_name(err));
        return;
    }

    std::string list_str;
    size_t list_len = 0;
    if (nvs_get_str(nvs_handle, "__key_list__", NULL, &list_len) == ESP_OK && list_len > 0) {
        std::vector<char> list_buf(list_len);
        nvs_get_str(nvs_handle, "__key_list__", list_buf.data(), &list_len);
        list_str = list_buf.data();
    }

    std::string key_entry = clean_name + ";";
    if (list_str.find(key_entry) == std::string::npos) {
        list_str += key_entry;
        nvs_set_str(nvs_handle, "__key_list__", list_str.c_str());
    }

    err = nvs_set_str(nvs_handle, clean_name.c_str(), key_data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_str failed for key '%s': %s", clean_name.c_str(), esp_err_to_name(err));
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Key '%s' saved to NVS", clean_name.c_str());
}

void SSHTerminal::load_keys_from_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("ssh_keys", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return;

    size_t list_len = 0;
    if (nvs_get_str(nvs_handle, "__key_list__", NULL, &list_len) != ESP_OK || list_len == 0) {
        nvs_close(nvs_handle);
        return;
    }

    std::vector<char> list_buf(list_len);
    nvs_get_str(nvs_handle, "__key_list__", list_buf.data(), &list_len);
    std::string list_str(list_buf.data());

    size_t start = 0;
    size_t end = list_str.find(';');
    while (end != std::string::npos) {
        std::string kname = list_str.substr(start, end - start);
        if (!kname.empty()) {
            size_t kdata_len = 0;
            if (nvs_get_str(nvs_handle, kname.c_str(), NULL, &kdata_len) == ESP_OK && kdata_len > 0) {
                std::vector<char> kdata_buf(kdata_len);
                nvs_get_str(nvs_handle, kname.c_str(), kdata_buf.data(), &kdata_len);
                load_key_from_memory(kname.c_str(), kdata_buf.data(), kdata_len);
                ESP_LOGI(TAG, "Loaded key '%s' from NVS", kname.c_str());
            }
        }
        start = end + 1;
        end = list_str.find(';', start);
    }

    nvs_close(nvs_handle);
}

bool SSHTerminal::delete_key_from_nvs(const char* keyname)
{
    if (!keyname) return false;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("ssh_keys", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return false;

    size_t list_len = 0;
    if (nvs_get_str(nvs_handle, "__key_list__", NULL, &list_len) == ESP_OK && list_len > 0) {
        std::vector<char> list_buf(list_len);
        nvs_get_str(nvs_handle, "__key_list__", list_buf.data(), &list_len);
        std::string list_str(list_buf.data());

        std::string to_remove = std::string(keyname) + ";";
        size_t pos = list_str.find(to_remove);
        if (pos != std::string::npos) {
            list_str.erase(pos, to_remove.length());
            nvs_set_str(nvs_handle, "__key_list__", list_str.c_str());
        }
    }

    nvs_erase_key(nvs_handle, keyname);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return true;
}

void SSHTerminal::toggle_alt_mode()
{
    set_alt_mode(!alt_mode);
}

void SSHTerminal::set_alt_mode(bool enable)
{
    alt_mode = enable;
    if (bsp_display_lock(500)) {
        if (alt_mode) {
            if (alt_btn) {
                lv_obj_set_style_bg_color(alt_btn, lv_color_hex(0x403000), 0);
                lv_obj_set_style_border_color(alt_btn, lv_color_hex(0xFFCC00), 0);
            }
            if (alt_btn_lbl) {
                lv_label_set_text(alt_btn_lbl, "ALT");
                lv_obj_set_style_text_color(alt_btn_lbl, lv_color_hex(0xFFFF00), 0);
            }
            if (alt_hud_panel) {
                lv_obj_clear_flag(alt_hud_panel, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(alt_hud_panel);
            }
        } else {
            if (alt_btn) {
                lv_obj_set_style_bg_color(alt_btn, lv_color_hex(0x181824), 0);
                lv_obj_set_style_border_color(alt_btn, lv_color_hex(0x5070AA), 0);
            }
            if (alt_btn_lbl) {
                lv_label_set_text(alt_btn_lbl, "ALT");
                lv_obj_set_style_text_color(alt_btn_lbl, lv_color_hex(0x80B0FF), 0);
            }
            if (alt_hud_panel) {
                lv_obj_add_flag(alt_hud_panel, LV_OBJ_FLAG_HIDDEN);
            }
        }
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "Alt Mode: %s", alt_mode ? "ON" : "OFF");
}

void SSHTerminal::toggle_local_shell_mode()
{
    if (!ssh_connected) return;

    is_local_mode_during_ssh = !is_local_mode_during_ssh;
    if (is_local_mode_during_ssh) {
        set_ssh_ui_mode(false);
        set_activity_status("[LOCAL]", 0xFFFF50);
        append_text("\n\033[33m--- [PocketSSH Local Shell Active] ---\033[0m\n");
        append_text("Type local commands ('getkey', 'keys', 'test', 'status', 'help').\n");
        append_text("Type '\033[1;32mresume\033[0m' or press \033[1;36mAlt+Enter\033[0m to return to SSH.\n\n");
    } else {
        set_ssh_ui_mode(true);
        set_activity_status("[ONLINE]", 0x50FF50);
        append_text("\n\033[32m--- [Resumed Interactive SSH Session] ---\033[0m\n");
    }
    scroll_to_bottom();
}

void SSHTerminal::send_arrow_up()
{
    if (ssh_connected && channel) {
        ssh_write_all("\033[A", 3);
    } else {
        navigate_history(1);
    }
}

void SSHTerminal::send_arrow_down()
{
    if (ssh_connected && channel) {
        ssh_write_all("\033[B", 3);
    } else {
        navigate_history(-1);
    }
}

void SSHTerminal::send_arrow_left()
{
    if (ssh_connected && channel) {
        ssh_write_all("\033[D", 3);
    } else {
        move_cursor_left();
    }
}

void SSHTerminal::send_arrow_right()
{
    if (ssh_connected && channel) {
        ssh_write_all("\033[C", 3);
    } else {
        move_cursor_right();
    }
}

void SSHTerminal::send_char_or_insert(char c)
{
    char buf[2] = { c, '\0' };
    if (ssh_connected && channel) {
        ssh_write_all(buf, 1);
    } else {
        current_input.insert(cursor_pos, 1, c);
        cursor_pos++;
        update_input_display();
    }
}

void SSHTerminal::show_loaded_keys()
{
    std::string keys_text = "\n=== Loaded SSH Keys ===\n";
    if (loaded_keys.empty()) {
        keys_text += "  No SSH keys currently loaded.\n";
        keys_text += "  Place .pem in /sdcard/ssh_keys/ or use ':getkey <path>'\n";
    } else {
        for (const auto& pair : loaded_keys) {
            char buf[128];
            snprintf(buf, sizeof(buf), "  * %s (%zu bytes)\n", pair.first.c_str(), pair.second.size());
            keys_text += buf;
        }
    }
    keys_text += "=======================\n\n";
    append_text(keys_text.c_str());
}

struct FetchKeyParams {
    SSHTerminal* terminal;
    std::string remote_path;
    std::string local_name;
};

void SSHTerminal::fetch_key_task(void* param)
{
    FetchKeyParams* params = (FetchKeyParams*)param;
    if (params) {
        SSHTerminal* terminal = params->terminal;
        std::string rpath = params->remote_path;
        std::string lname = params->local_name;
        delete params;
        if (terminal) {
            terminal->fetch_remote_key(rpath.c_str(), lname.empty() ? NULL : lname.c_str());
        }
    }
    vTaskDelete(NULL);
}

void SSHTerminal::start_fetch_remote_key(const char* remote_path, const char* local_name)
{
    if (!ssh_connected || !session) {
        append_text("\n[Error: Must be connected to SSH server to fetch key]\n");
        return;
    }

    FetchKeyParams* params = new FetchKeyParams();
    params->terminal = this;
    params->remote_path = (remote_path && strlen(remote_path) > 0) ? remote_path : "~/.ssh/id_pem";
    if (params->remote_path.find('/') == std::string::npos) {
        params->remote_path = "~/.ssh/" + params->remote_path;
    }
    if (local_name && strlen(local_name) > 0) {
        params->local_name = local_name;
    }

    xTaskCreate(fetch_key_task, "fetch_key", 8192, params, 5, NULL);
}

void SSHTerminal::show_getkey_dialog()
{
    if (!terminal_screen) return;
    if (!bsp_display_lock(1000)) return;

    if (getkey_dialog && lv_obj_is_valid(getkey_dialog)) {
        lv_obj_del(getkey_dialog);
        getkey_dialog = NULL;
        getkey_path_ta = NULL;
    }

    getkey_dialog = lv_obj_create(terminal_screen);
    lv_obj_set_size(getkey_dialog, 290, 195);
    lv_obj_align(getkey_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(getkey_dialog, lv_color_hex(0x061406), 0);
    lv_obj_set_style_bg_opa(getkey_dialog, LV_OPA_90, 0);
    lv_obj_set_style_border_color(getkey_dialog, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(getkey_dialog, 2, 0);
    lv_obj_set_style_radius(getkey_dialog, 6, 0);
    lv_obj_set_style_pad_all(getkey_dialog, 6, 0);
    lv_obj_clear_flag(getkey_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(getkey_dialog);
    lv_label_set_text(title, "FETCH SSH KEY FROM SERVER");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t* hint = lv_label_create(getkey_dialog);
    lv_label_set_text(hint, "Select remote file to retrieve:");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x80D0FF), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 4, 20);

    getkey_path_ta = lv_textarea_create(getkey_dialog);
    lv_obj_set_size(getkey_path_ta, 268, 30);
    lv_obj_align(getkey_path_ta, LV_ALIGN_TOP_MID, 0, 36);
    lv_textarea_set_one_line(getkey_path_ta, true);
    lv_textarea_set_text(getkey_path_ta, "~/.ssh/id_pem");
    lv_obj_set_style_bg_color(getkey_path_ta, lv_color_hex(0x102010), 0);
    lv_obj_set_style_text_color(getkey_path_ta, lv_color_hex(0xFFFF50), 0);
    lv_obj_set_style_border_color(getkey_path_ta, lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_border_width(getkey_path_ta, 1, 0);
    lv_obj_set_style_text_font(getkey_path_ta, &lv_font_montserrat_10, 0);

    lv_obj_t* preset_lbl = lv_label_create(getkey_dialog);
    lv_label_set_text(preset_lbl, "Presets:");
    lv_obj_set_style_text_color(preset_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(preset_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(preset_lbl, LV_ALIGN_TOP_LEFT, 4, 72);

    auto make_preset_btn = [this](const char* label, const char* path, int x_pos) {
        lv_obj_t* b = lv_btn_create(getkey_dialog);
        lv_obj_set_size(b, 84, 26);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, x_pos, 88);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x152515), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x00CC00), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, lv_color_hex(0x60FF60), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_center(l);
        lv_obj_set_user_data(b, (void*)path);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            SSHTerminal* term = (SSHTerminal*)lv_event_get_user_data(e);
            lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
            const char* p = (const char*)lv_obj_get_user_data(btn);
            if (term && term->getkey_path_ta && p) {
                lv_textarea_set_text(term->getkey_path_ta, p);
            }
        }, LV_EVENT_CLICKED, this);
    };

    make_preset_btn("id_pem", "~/.ssh/id_pem", 4);
    make_preset_btn("id_rsa", "~/.ssh/id_rsa", 94);
    make_preset_btn("id_ed25519", "~/.ssh/id_ed25519", 184);

    lv_obj_t* fetch_btn = lv_btn_create(getkey_dialog);
    lv_obj_set_size(fetch_btn, 130, 32);
    lv_obj_align(fetch_btn, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_set_style_bg_color(fetch_btn, lv_color_hex(0x103510), 0);
    lv_obj_set_style_bg_color(fetch_btn, lv_color_hex(0x00AA00), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(fetch_btn, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(fetch_btn, 2, 0);
    lv_obj_set_style_radius(fetch_btn, 4, 0);
    lv_obj_set_style_pad_all(fetch_btn, 0, 0);
    lv_obj_t* fetch_lbl = lv_label_create(fetch_btn);
    lv_label_set_text(fetch_lbl, "FETCH KEY");
    lv_obj_set_style_text_color(fetch_lbl, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(fetch_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(fetch_lbl);

    lv_obj_add_event_cb(fetch_btn, [](lv_event_t* e) {
        SSHTerminal* term = (SSHTerminal*)lv_event_get_user_data(e);
        if (term && term->getkey_path_ta) {
            std::string path = lv_textarea_get_text(term->getkey_path_ta);
            if (path.empty()) path = "~/.ssh/id_pem";
            if (term->getkey_dialog) {
                lv_obj_del(term->getkey_dialog);
                term->getkey_dialog = NULL;
                term->getkey_path_ta = NULL;
            }
            term->start_fetch_remote_key(path.c_str(), NULL);
        }
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* cancel_btn = lv_btn_create(getkey_dialog);
    lv_obj_set_size(cancel_btn, 130, 32);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x351010), 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xAA0000), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_radius(cancel_btn, 4, 0);
    lv_obj_set_style_pad_all(cancel_btn, 0, 0);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "CANCEL");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xFFA0A0), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_add_event_cb(cancel_btn, [](lv_event_t* e) {
        SSHTerminal* term = (SSHTerminal*)lv_event_get_user_data(e);
        if (term && term->getkey_dialog) {
            lv_obj_del(term->getkey_dialog);
            term->getkey_dialog = NULL;
            term->getkey_path_ta = NULL;
        }
    }, LV_EVENT_CLICKED, this);

    lv_obj_move_foreground(getkey_dialog);
    bsp_display_unlock();
}

esp_err_t SSHTerminal::fetch_remote_key(const char* remote_path, const char* local_name)
{
    if (!ssh_connected || !session) {
        append_text("\n[Error: Not connected to SSH server]\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (!remote_path || strlen(remote_path) == 0) {
        remote_path = "~/.ssh/id_pem";
    }

    std::string key_alias;
    if (local_name && strlen(local_name) > 0) {
        key_alias = local_name;
    } else {
        const char* slash = strrchr(remote_path, '/');
        key_alias = (slash != NULL) ? (slash + 1) : remote_path;
        if (key_alias.empty() || key_alias == ".") {
            key_alias = "remote_key";
        }
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "\n[Fetching remote key '%s' from %s...]\n", key_alias.c_str(), remote_path);
    append_text(msg);

    // Pause ssh_receive_task to give exclusive access to session
    ssh_rx_paused = true;
    for (int w = 0; w < 50 && !ssh_rx_is_paused; w++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    LIBSSH2_CHANNEL* exec_ch = NULL;
    int rc = 0;
    int retries = 0;
    while ((exec_ch = libssh2_channel_open_session(session)) == NULL && retries++ < 50) {
        if (libssh2_session_last_error(session, NULL, NULL, 0) == LIBSSH2_ERROR_EAGAIN) {
            waitsocket(ssh_socket, session);
        } else {
            break;
        }
    }

    if (!exec_ch) {
        ssh_rx_paused = false;
        append_text("[Error: Failed to open exec channel for key retrieval]\n");
        return ESP_FAIL;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cat %s", remote_path);
    retries = 0;
    while ((rc = libssh2_channel_exec(exec_ch, cmd)) == LIBSSH2_ERROR_EAGAIN && retries++ < 50) {
        waitsocket(ssh_socket, session);
    }

    if (rc != 0) {
        ssh_rx_paused = false;
        append_text("[Error: Failed to execute read command on remote server]\n");
        libssh2_channel_free(exec_ch);
        return ESP_FAIL;
    }

    std::string key_content;
    char read_buf[512];
    ssize_t nread = 0;
    int read_eagain_retries = 0;
    while (true) {
        nread = libssh2_channel_read(exec_ch, read_buf, sizeof(read_buf));
        if (nread > 0) {
            read_eagain_retries = 0;
            key_content.append(read_buf, nread);
            if (key_content.size() > 16384) {
                append_text("[Error: Remote key file exceeds 16KB limit]\n");
                break;
            }
        } else if (nread == LIBSSH2_ERROR_EAGAIN) {
            if (++read_eagain_retries > 50) break;
            waitsocket(ssh_socket, session);
        } else {
            break;
        }
    }

    libssh2_channel_close(exec_ch);
    libssh2_channel_free(exec_ch);

    // Resume normal SSH receive task
    ssh_rx_paused = false;

    if (key_content.empty() || key_content.find("-----BEGIN") == std::string::npos) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg),
            "\n[Error: Key '%s' not found or empty on server!]\n"
            "[To create a PEM key on remote server, run:]\n"
            "  ssh-keygen -t rsa -b 2048 -m PEM -f %s -N \"\"\n"
            "  cat %s.pub >> ~/.ssh/authorized_keys\n\n",
            remote_path, remote_path, remote_path);
        append_text(err_msg);
        return ESP_ERR_NOT_FOUND;
    }

    if (key_content.find("OPENSSH") != std::string::npos) {
        char warn_msg[320];
        snprintf(warn_msg, sizeof(warn_msg),
            "\n[Warning: '%s' is in OpenSSH format! PocketSSH requires PEM format]\n"
            "[To convert on server, run:]\n"
            "  ssh-keygen -p -m PEM -f %s\n"
            "[Or create a new PEM key:]\n"
            "  ssh-keygen -t rsa -b 2048 -m PEM -f ~/.ssh/id_pem -N \"\"\n"
            "  cat ~/.ssh/id_pem.pub >> ~/.ssh/authorized_keys\n\n",
            remote_path, remote_path);
        append_text(warn_msg);
        return ESP_ERR_INVALID_ARG;
    }

    if (key_content.find("KEY-----") == std::string::npos && key_content.find("PRIVATE") == std::string::npos) {
        append_text("\n[Error: Retrieved content does not appear to be a private key!]\n\n");
        return ESP_ERR_INVALID_ARG;
    }

    save_key_to_sd_and_nvs(key_alias.c_str(), key_content.c_str(), key_content.size());

    char ok_msg[160];
    snprintf(ok_msg, sizeof(ok_msg), "[Key '%s' saved to /sdcard/ssh_keys/ & NVS! Ready to use]\n", key_alias.c_str());
    append_text(ok_msg);
    return ESP_OK;
}

void SSHTerminal::process_escape_command(const std::string& raw_cmd)
{
    std::string cmd = raw_cmd;
    if (!cmd.empty() && (cmd[0] == ':' || cmd[0] == '~')) {
        cmd = cmd.substr(1);
    }
    
    if (cmd == "keys") {
        show_loaded_keys();
    } else if (cmd == "exit" || cmd == "quit") {
        disconnect();
    } else if (cmd == "clear") {
        clear_terminal();
    } else if (cmd == "help" || cmd.empty()) {
        append_text("\n=== PocketSSH Escape Commands ===\n");
        append_text("  :getkey [remote_path] - Fetch key from server (opens dialog if empty)\n");
        append_text("  :keys                 - List all loaded SSH keys\n");
        append_text("  :clear                - Clear terminal output\n");
        append_text("  :exit                 - Disconnect SSH session\n");
        append_text("=================================\n\n");
    } else if (cmd == "getkey" || cmd.rfind("getkey ", 0) == 0) {
        std::vector<std::string> args;
        std::string arg;
        size_t p = 6;
        while (p < cmd.size() && cmd[p] == ' ') p++;
        for (; p < cmd.size(); p++) {
            if (cmd[p] == ' ') {
                if (!arg.empty()) { args.push_back(arg); arg.clear(); }
            } else {
                arg += cmd[p];
            }
        }
        if (!arg.empty()) args.push_back(arg);

        if (args.empty()) {
            show_getkey_dialog();
        } else {
            const char* rpath = args[0].c_str();
            const char* lname = args.size() > 1 ? args[1].c_str() : NULL;
            start_fetch_remote_key(rpath, lname);
        }
    } else {
        append_text("[Unknown command. Use ':getkey [path]', ':keys', or ':exit']\n");
    }
}


void SSHTerminal::handle_tab_completion()
{
    // Helper: case-insensitive prefix match
    auto istarts_with = [](const std::string& str, const std::string& prefix) -> bool {
        if (str.length() < prefix.length()) return false;
        for (size_t i = 0; i < prefix.length(); i++) {
            if (tolower((unsigned char)str[i]) != tolower((unsigned char)prefix[i])) return false;
        }
        return true;
    };

    // Helper: compute longest common prefix (case-preserving based on first candidate)
    auto calc_lcp = [](const std::vector<std::string>& candidates) -> std::string {
        if (candidates.empty()) return "";
        std::string lcp = candidates[0];
        for (size_t i = 1; i < candidates.size(); i++) {
            size_t j = 0;
            while (j < lcp.length() && j < candidates[i].length() &&
                   tolower((unsigned char)lcp[j]) == tolower((unsigned char)candidates[i][j])) {
                j++;
            }
            lcp = lcp.substr(0, j);
            if (lcp.empty()) break;
        }
        return lcp;
    };

    // If input is empty:
    if (current_input.empty()) {
        if (ssh_connected && channel) {
            // In active SSH session with no input: send TAB to remote PTY shell
            libssh2_channel_write(channel, "\t", 1);
            return;
        }

        // In local mode: display available commands
        append_text("\nCommands:\n"
                    "  scan         connect      save         saved\n"
                    "  forget       autoconnect  status       disconnect\n"
                    "  ssh          sshkey       exit         clear\n"
                    "  pgup         pgdn         top          bottom\n"
                    "  scroll       version      help\n");
        scroll_to_bottom();
        return;
    }

    size_t space_pos = current_input.find(' ');

    // ----------------------------------------------------
    // Case 1: No space (Completing primary command name)
    // ----------------------------------------------------
    if (space_pos == std::string::npos) {
        std::string prefix = current_input;
        std::vector<std::string> cmd_list;

        if (!ssh_connected) {
            cmd_list = {
                "autoconnect", "bottom", "clear", "connect", "disconnect", "exit",
                "forget", "help", "keys", "pgdn", "pgup", "save", "saved",
                "scan", "scroll", "ssh", "sshkey", "status", "top", "version", "wifi"
            };
        } else {
            cmd_list = {
                // Local commands available during SSH
                "bottom", "clear", "exit", "help", "pgdn", "pgup", "scroll", "status", "top", "version",
                // Common Linux CLI tools
                "apt", "cat", "cd", "chmod", "chown", "cp", "curl", "df", "dmesg", "docker",
                "find", "free", "git", "grep", "head", "htop", "journalctl", "kill", "ls",
                "mkdir", "mv", "nano", "ping", "ps", "pwd", "reboot", "rm", "shutdown",
                "ssh", "sudo", "systemctl", "tail", "tar", "uptime", "vim", "wget"
            };
        }

        std::vector<std::string> matches;
        for (const auto& cmd : cmd_list) {
            if (istarts_with(cmd, prefix)) {
                matches.push_back(cmd);
            }
        }

        if (matches.empty()) {
            if (ssh_connected && channel) {
                // In SSH, pass unrecognized prefix + TAB to remote shell
                std::string to_send = current_input + "\t";
                libssh2_channel_write(channel, to_send.c_str(), to_send.length());
                current_input.clear();
                cursor_pos = 0;
                update_input_display();
            }
            return;
        }

        if (matches.size() == 1) {
            current_input = matches[0] + " ";
            cursor_pos = current_input.length();
            update_input_display();
            return;
        }

        std::string lcp = calc_lcp(matches);
        if (lcp.length() > prefix.length()) {
            current_input = lcp;
            cursor_pos = current_input.length();
            update_input_display();
        } else {
            std::string out = "\nCandidates:\n  ";
            for (size_t i = 0; i < matches.size(); i++) {
                out += matches[i] + "  ";
                if ((i + 1) % 4 == 0 && i + 1 < matches.size()) out += "\n  ";
            }
            out += "\n";
            append_text(out.c_str());
            scroll_to_bottom();
        }
        return;
    }

    // ----------------------------------------------------
    // Case 2: Subcommands and Arguments (space present)
    // ----------------------------------------------------

    // A) wifi subcommands
    if (istarts_with(current_input, "wifi ")) {
        std::string rest = current_input.substr(5);
        size_t second_space = rest.find(' ');
        if (second_space == std::string::npos) {
            std::vector<std::string> wifi_subcmds = {
                "scan", "connect", "save", "saved", "forget", "autoconnect", "status", "disconnect"
            };
            std::vector<std::string> matches;
            for (const auto& sub : wifi_subcmds) {
                if (istarts_with(sub, rest)) {
                    matches.push_back(sub);
                }
            }
            if (matches.empty()) {
                append_text("\nWiFi subcommands:\n  scan  connect  save  saved  forget  autoconnect  status  disconnect\n");
                scroll_to_bottom();
                return;
            }
            if (matches.size() == 1) {
                current_input = "wifi " + matches[0] + " ";
                cursor_pos = current_input.length();
                update_input_display();
                return;
            }
            std::string lcp = calc_lcp(matches);
            if (lcp.length() > rest.length()) {
                current_input = "wifi " + lcp;
                cursor_pos = current_input.length();
                update_input_display();
            } else {
                std::string out = "\nMatching subcommands: ";
                for (const auto& m : matches) out += m + "  ";
                out += "\n";
                append_text(out.c_str());
                scroll_to_bottom();
            }
            return;
        }
    }

    // B) connect / save (SSID argument completion)
    std::string prefix_base = "";
    if (istarts_with(current_input, "connect ")) prefix_base = "connect ";
    else if (istarts_with(current_input, "wifi connect ")) prefix_base = "wifi connect ";
    else if (istarts_with(current_input, "save ")) prefix_base = "save ";
    else if (istarts_with(current_input, "wifi save ")) prefix_base = "wifi save ";
    else if (istarts_with(current_input, "savewifi ")) prefix_base = "savewifi ";

    if (!prefix_base.empty()) {
        std::string arg = current_input.substr(prefix_base.length());

        std::vector<std::string> candidates;
        for (const auto& ap : cached_scan_results) {
            std::string s = (char*)ap.ssid;
            if (!s.empty() && std::find(candidates.begin(), candidates.end(), s) == candidates.end()) {
                candidates.push_back(s);
            }
        }
        std::string saved_s, saved_p;
        if (load_wifi_from_nvs(saved_s, saved_p) && !saved_s.empty()) {
            if (std::find(candidates.begin(), candidates.end(), saved_s) == candidates.end()) {
                candidates.push_back(saved_s);
            }
        }
        if (wifi_connected && !connected_ssid.empty()) {
            if (std::find(candidates.begin(), candidates.end(), connected_ssid) == candidates.end()) {
                candidates.push_back(connected_ssid);
            }
        }

        if (candidates.empty()) {
            append_text("\n[No networks found. Type 'scan' to discover nearby APs]\n");
            scroll_to_bottom();
            return;
        }

        if (arg.empty()) {
            std::string out = "\nDiscovered networks:\n";
            for (size_t i = 0; i < candidates.size(); i++) {
                char item[64];
                snprintf(item, sizeof(item), "  %d: \"%s\"", (int)(i + 1), candidates[i].c_str());
                out += item;
                if ((i + 1) % 2 == 0 || i == candidates.size() - 1) out += "\n";
            }
            append_text(out.c_str());
            scroll_to_bottom();
            return;
        }

        bool has_quote = (arg[0] == '"');
        std::string clean_arg = has_quote ? arg.substr(1) : arg;

        // Check if user typed a number matching a scan index
        if (!clean_arg.empty() && std::all_of(clean_arg.begin(), clean_arg.end(), ::isdigit)) {
            int idx = std::atoi(clean_arg.c_str());
            if (idx >= 1 && idx <= (int)candidates.size()) {
                std::string chosen = candidates[idx - 1];
                if (chosen.find(' ') != std::string::npos) chosen = "\"" + chosen + "\"";
                current_input = prefix_base + chosen + " ";
                cursor_pos = current_input.length();
                update_input_display();
                return;
            }
        }

        std::vector<std::string> matches;
        for (const auto& ssid : candidates) {
            if (istarts_with(ssid, clean_arg)) {
                matches.push_back(ssid);
            }
        }

        if (matches.empty()) return;

        if (matches.size() == 1) {
            std::string chosen = matches[0];
            if (chosen.find(' ') != std::string::npos) chosen = "\"" + chosen + "\"";
            current_input = prefix_base + chosen + " ";
            cursor_pos = current_input.length();
            update_input_display();
            return;
        }

        std::string lcp = calc_lcp(matches);
        if (lcp.length() > clean_arg.length()) {
            if (has_quote || lcp.find(' ') != std::string::npos) {
                current_input = prefix_base + "\"" + lcp;
            } else {
                current_input = prefix_base + lcp;
            }
            cursor_pos = current_input.length();
            update_input_display();
        } else {
            std::string out = "\nMatching networks:\n";
            for (const auto& m : matches) {
                out += "  \"" + m + "\"\n";
            }
            append_text(out.c_str());
            scroll_to_bottom();
        }
        return;
    }

    // C) autoconnect on / off
    std::string auto_base = "";
    if (istarts_with(current_input, "autoconnect ")) auto_base = "autoconnect ";
    else if (istarts_with(current_input, "wifi autoconnect ")) auto_base = "wifi autoconnect ";

    if (!auto_base.empty()) {
        std::string arg = current_input.substr(auto_base.length());
        if (arg.empty()) {
            append_text("\nOptions: on  off\n");
            scroll_to_bottom();
            return;
        }
        if (istarts_with("on", arg)) {
            current_input = auto_base + "on ";
            cursor_pos = current_input.length();
            update_input_display();
            return;
        }
        if (istarts_with("off", arg)) {
            current_input = auto_base + "off ";
            cursor_pos = current_input.length();
            update_input_display();
            return;
        }
        return;
    }

    // D) sshkey keyfile completion
    if (istarts_with(current_input, "sshkey ")) {
        size_t last_space = current_input.rfind(' ');
        std::string before = current_input.substr(0, last_space + 1);
        std::string key_arg = current_input.substr(last_space + 1);

        std::vector<std::string> keys = get_loaded_key_names();
        if (keys.empty()) {
            append_text("\n[No SSH keys loaded from /sdcard/ssh_keys/]\n");
            scroll_to_bottom();
            return;
        }

        if (key_arg.empty()) {
            std::string out = "\nAvailable key files: ";
            for (const auto& k : keys) out += k + "  ";
            out += "\n";
            append_text(out.c_str());
            scroll_to_bottom();
            return;
        }

        std::vector<std::string> matches;
        for (const auto& k : keys) {
            if (istarts_with(k, key_arg)) matches.push_back(k);
        }

        if (matches.empty()) return;

        if (matches.size() == 1) {
            current_input = before + matches[0] + " ";
            cursor_pos = current_input.length();
            update_input_display();
            return;
        }

        std::string lcp = calc_lcp(matches);
        if (lcp.length() > key_arg.length()) {
            current_input = before + lcp;
            cursor_pos = current_input.length();
            update_input_display();
        } else {
            std::string out = "\nMatching keys: ";
            for (const auto& m : matches) out += m + "  ";
            out += "\n";
            append_text(out.c_str());
            scroll_to_bottom();
        }
        return;
    }

    // E) SSH common tools subcommand completion
    if (ssh_connected) {
        if (istarts_with(current_input, "systemctl ")) {
            std::string sub = current_input.substr(10);
            std::vector<std::string> sys_sub = {"status", "restart", "start", "stop", "enable", "disable", "reload"};
            std::vector<std::string> matches;
            for (const auto& s : sys_sub) if (istarts_with(s, sub)) matches.push_back(s);
            if (matches.size() == 1) {
                current_input = "systemctl " + matches[0] + " ";
                cursor_pos = current_input.length();
                update_input_display();
                return;
            } else if (matches.size() > 1) {
                std::string lcp = calc_lcp(matches);
                if (lcp.length() > sub.length()) {
                    current_input = "systemctl " + lcp;
                    cursor_pos = current_input.length();
                    update_input_display();
                } else {
                    std::string out = "\nCandidates: ";
                    for (const auto& m : matches) out += m + "  ";
                    out += "\n";
                    append_text(out.c_str());
                    scroll_to_bottom();
                }
                return;
            }
        } else if (istarts_with(current_input, "git ")) {
            std::string sub = current_input.substr(4);
            std::vector<std::string> git_sub = {"status", "log", "diff", "commit", "push", "pull", "checkout", "branch", "clone", "add", "reset"};
            std::vector<std::string> matches;
            for (const auto& s : git_sub) if (istarts_with(s, sub)) matches.push_back(s);
            if (matches.size() == 1) {
                current_input = "git " + matches[0] + " ";
                cursor_pos = current_input.length();
                update_input_display();
                return;
            } else if (matches.size() > 1) {
                std::string lcp = calc_lcp(matches);
                if (lcp.length() > sub.length()) {
                    current_input = "git " + lcp;
                    cursor_pos = current_input.length();
                    update_input_display();
                } else {
                    std::string out = "\nCandidates: ";
                    for (const auto& m : matches) out += m + "  ";
                    out += "\n";
                    append_text(out.c_str());
                    scroll_to_bottom();
                }
                return;
            }
        } else if (istarts_with(current_input, "docker ")) {
            std::string sub = current_input.substr(7);
            std::vector<std::string> doc_sub = {"ps", "images", "run", "stop", "rm", "rmi", "logs", "exec", "build", "compose"};
            std::vector<std::string> matches;
            for (const auto& s : doc_sub) if (istarts_with(s, sub)) matches.push_back(s);
            if (matches.size() == 1) {
                current_input = "docker " + matches[0] + " ";
                cursor_pos = current_input.length();
                update_input_display();
                return;
            } else if (matches.size() > 1) {
                std::string lcp = calc_lcp(matches);
                if (lcp.length() > sub.length()) {
                    current_input = "docker " + lcp;
                    cursor_pos = current_input.length();
                    update_input_display();
                } else {
                    std::string out = "\nCandidates: ";
                    for (const auto& m : matches) out += m + "  ";
                    out += "\n";
                    append_text(out.c_str());
                    scroll_to_bottom();
                }
                return;
            }
        } else if (istarts_with(current_input, "apt ")) {
            std::string sub = current_input.substr(4);
            std::vector<std::string> apt_sub = {"update", "upgrade", "install", "remove", "search", "show", "autoremove"};
            std::vector<std::string> matches;
            for (const auto& s : apt_sub) if (istarts_with(s, sub)) matches.push_back(s);
            if (matches.size() == 1) {
                current_input = "apt " + matches[0] + " ";
                cursor_pos = current_input.length();
                update_input_display();
                return;
            } else if (matches.size() > 1) {
                std::string lcp = calc_lcp(matches);
                if (lcp.length() > sub.length()) {
                    current_input = "apt " + lcp;
                    cursor_pos = current_input.length();
                    update_input_display();
                } else {
                    std::string out = "\nCandidates: ";
                    for (const auto& m : matches) out += m + "  ";
                    out += "\n";
                    append_text(out.c_str());
                    scroll_to_bottom();
                }
                return;
            }
        }

        // Generic SSH remote tab fallback
        if (channel) {
            std::string to_send = current_input + "\t";
            libssh2_channel_write(channel, to_send.c_str(), to_send.length());
            current_input.clear();
            cursor_pos = 0;
            update_input_display();
            return;
        }
    }
}

void SSHTerminal::set_ssh_ui_mode(bool ssh_active)
{
    if (!terminal_screen || !terminal_output || !input_container) return;
    if (!bsp_display_lock(1000)) return;

    if (ssh_active) {
        // Full Interactive Desktop SSH Mode: hide bottom local bar and expand terminal
        is_local_mode_during_ssh = false;
        lv_obj_add_flag(input_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(terminal_output, lv_pct(100), lv_pct(88));
        lv_obj_align(terminal_output, LV_ALIGN_TOP_MID, 0, 25);
        is_user_scrolled_back = false;
        lv_obj_add_state(terminal_output, LV_STATE_FOCUSED);
        lv_textarea_set_cursor_pos(terminal_output, LV_TEXTAREA_CURSOR_LAST);
        lv_obj_scroll_to_y(terminal_output, LV_COORD_MAX, LV_ANIM_OFF);
        if (scroll_indicator_label) {
            lv_obj_add_flag(scroll_indicator_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // Return to Local Terminal Shell: restore bottom prompt bar
        lv_obj_clear_state(terminal_output, LV_STATE_FOCUSED);
        lv_obj_clear_flag(input_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(input_container);
        lv_obj_set_size(terminal_output, lv_pct(100), lv_pct(75));
        lv_obj_align(terminal_output, LV_ALIGN_TOP_MID, 0, 25);
        current_input.clear();
        cursor_pos = 0;
        history_index = -1;
        is_user_scrolled_back = false;
        lv_obj_scroll_to_y(terminal_output, LV_COORD_MAX, LV_ANIM_OFF);
    }
    
    bsp_display_unlock();
    update_status_bar();
    if (!ssh_active) {
        update_input_display();
    }
}
