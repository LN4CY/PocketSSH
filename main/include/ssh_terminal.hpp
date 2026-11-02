/*
 * SSHTerminal Header
 * Provides LVGL-based SSH terminal interface with WiFi connectivity.
 * Supports command history, battery monitoring, and interactive shell sessions.
 */

#ifndef SSH_TERMINAL_HPP
#define SSH_TERMINAL_HPP

#include "lvgl.h"
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include "libssh2.h"
#include "battery_measurement.hpp"

#define SSH_MAX_LINE_LENGTH 128
#define SSH_MAX_LINES 100
#define SSH_BUFFER_SIZE 4096

// External check for SD card status from deck_base
bool is_sd_card_mounted();

// Splash screen WiFi status update function in deck_base
void deck_update_splash_wifi(const char* status_text, uint32_t color_hex);

class SSHTerminal
{
public:
    SSHTerminal();
    ~SSHTerminal();

    lv_obj_t* create_terminal_screen();
    
    esp_err_t connect(const char* host, int port, const char* username, const char* password);
    esp_err_t connect_with_key(const char* host, int port, const char* username, const char* privkey_data, size_t privkey_len);
    esp_err_t disconnect();
    bool is_connected();
    
    void append_text(const char* text);
    void clear_terminal();
    void handle_key_input(char key, bool from_serial = false);
    void handle_tab_completion();
    void set_ssh_ui_mode(bool ssh_active);
    void send_command(const char* cmd);
    void navigate_history(int direction);
    void delete_current_history_entry();
    void send_current_history_command();
    void move_cursor_left();
    void move_cursor_right();
    void move_cursor_home();
    void move_cursor_end();
    
    // Terminal scrollback navigation
    void scroll_up(int32_t pixels = 28);
    void scroll_down(int32_t pixels = 28);
    void scroll_page_up();
    void scroll_page_down();
    void scroll_to_top();
    void scroll_to_bottom();
    bool is_scrolled_back();
    void handle_trackball_left();
    void handle_trackball_right();
    bool is_ssh_active() const { return ssh_connected; }
    void send_special_key_direct(const char* sequence);
    
    esp_err_t init_wifi(const char* ssid, const char* password);
    bool is_wifi_connected();
    esp_err_t scan_wifi();
    void show_wifi_status();
    void try_autoconnect();
    
    // WiFi configuration & persistence (Dual SD Card + NVS)
    void save_wifi_config(const char* ssid, const char* password);
    bool load_wifi_config(std::string& ssid, std::string& password);
    void clear_wifi_config();
    void save_wifi_to_nvs(const char* ssid, const char* password);
    bool load_wifi_from_nvs(std::string& ssid, std::string& password);
    void clear_wifi_nvs();
    bool get_wifi_autoconnect_enabled();
    void set_wifi_autoconnect_enabled(bool enabled);
    static const char* wifi_reason_to_string(uint8_t reason);
    
    void set_virtual_target(bool is_virtual) { m_is_virtual_target = is_virtual; }
    bool is_virtual_target() const { return m_is_virtual_target; }
    
    lv_obj_t* get_screen() { return terminal_screen; }
    
    // Status bar & splash integration
    void update_status_bar();
    
    // SSH key management
    void load_key_from_memory(const char* keyname, const char* key_data, size_t key_len);
    const char* get_loaded_key(const char* keyname, size_t* len);
    std::vector<std::string> get_loaded_key_names();
    esp_err_t fetch_remote_key(const char* remote_path, const char* local_name = NULL);
    void start_fetch_remote_key(const char* remote_path, const char* local_name = NULL);
    void show_getkey_dialog();
    void close_getkey_dialog();
    bool save_key_to_sd_and_nvs(const char* keyname, const char* key_data, size_t key_len);
    bool delete_key_from_sd_and_nvs(const char* keyname);
    void save_key_to_nvs(const char* keyname, const char* key_data);
    void load_keys_from_nvs();
    bool delete_key_from_nvs(const char* keyname);
    void show_loaded_keys();
    void erase_current_line();
    void erase_to_end_of_line();
    void send_interrupt();
    void set_activity_status(const char* text, uint32_t color_hex);
    ssize_t ssh_write_all(const char* data, size_t len);
    
    // Alt Mode & Trackball Arrow Navigation
    bool is_alt_mode() const { return alt_mode; }
    void toggle_alt_mode();
    void set_alt_mode(bool enable);
    void send_arrow_up();
    void send_arrow_down();
    void send_arrow_left();
    void send_arrow_right();
    void send_char_or_insert(char c);
    void toggle_local_shell_mode();
    bool is_local_shell_active() const { return is_local_mode_during_ssh; }
    
private:
    lv_obj_t* terminal_screen;
    lv_obj_t* terminal_output;
    lv_obj_t* input_label;
    lv_obj_t* input_container;
    lv_obj_t* status_bar;
    lv_obj_t* byte_counter_label;
    lv_obj_t* side_panel;
    lv_obj_t* scroll_indicator_label;
    lv_obj_t* ctrl_c_btn;
    lv_obj_t* esc_btn;
    lv_obj_t* alt_btn;
    lv_obj_t* alt_btn_lbl;
    lv_obj_t* keys_btn;
    lv_obj_t* activity_label;
    lv_obj_t* alt_hud_panel;
    bool alt_mode;
    static void ctrl_c_btn_event_cb(lv_event_t* e);
    static void esc_btn_event_cb(lv_event_t* e);
    static void alt_btn_event_cb(lv_event_t* e);
    static void keys_btn_event_cb(lv_event_t* e);
    bool is_user_scrolled_back;
    void update_scroll_indicator();
    static void terminal_scroll_event_cb(lv_event_t* e);
    
    std::string current_input;
    std::string ssh_cmd_buffer;
    bool ssh_at_line_start;
    bool ssh_pending_cr;
    bool is_local_mode_during_ssh;
    void process_escape_command(const std::string& raw_cmd);
    size_t cursor_pos;
    size_t bytes_received;
    std::vector<std::string> command_history;
    int history_index;
    
    lv_timer_t* cursor_blink_timer;
    bool cursor_visible;
    
    lv_timer_t* battery_update_timer;
    
    bool history_needs_save;
    lv_timer_t* history_save_timer;
    
    std::string text_buffer;
    int64_t last_display_update;
    
    bool wifi_connected;
    std::string connected_ssid;
    std::vector<wifi_ap_record_t> cached_scan_results;
    
    bool ssh_connected;
    bool battery_initialized;
    
    BatteryMeasurement battery;
    
    int ssh_socket;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    SemaphoreHandle_t ssh_mutex;
    
    char* hostname;
    int port_number;
    
    // SSH key storage: keyname -> key content
    std::map<std::string, std::string> loaded_keys;
    
    void update_terminal_display();
    void update_input_display();
    void process_received_data(const char* data, size_t len);
    void flush_display_buffer();
    
    void load_history_from_nvs();
    void save_history_to_nvs();
    void clear_history_nvs();
    std::string strip_ansi_codes(const char* data, size_t len);
    void send_special_key(const char* sequence);
    void create_side_panel();
    void toggle_side_panel();
    static void gesture_event_cb(lv_event_t* e);
    static void special_key_event_cb(lv_event_t* e);
    static void input_touch_event_cb(lv_event_t* e);
    static void cursor_blink_cb(lv_timer_t* timer);
    static void battery_update_cb(lv_timer_t* timer);
    static void history_save_cb(lv_timer_t* timer);
    static void ssh_receive_task(void* param);
    static void autoconnect_task(void* param);
    static void fetch_key_task(void* param);
    
    volatile bool ssh_rx_paused;
    volatile bool ssh_rx_is_paused;
    bool m_is_virtual_target;
    lv_obj_t* getkey_dialog;
    lv_obj_t* getkey_path_ta;
    
    static esp_err_t ensure_wifi_driver_started(SSHTerminal* terminal);
    
    static int waitsocket(int socket_fd, LIBSSH2_SESSION *session);
    esp_err_t ssh_authenticate(const char* username, const char* password);
    esp_err_t ssh_authenticate_pubkey(const char* username, const char* privkey_data, size_t privkey_len);
    esp_err_t ssh_open_channel();
};

#endif
