/*
 * Deck Base Application
 * Main application entry point for PocketSSH. Initializes hardware peripherals including
 * GPIO, display, touch input, and manages FreeRTOS tasks for keyboard input and trackball navigation.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

#include "esp_lcd_touch_gt911.h"
#include "driver/gpio.h"

#include "utilities.h"
#include "c3_keyboard.hpp"
#include "ssh_terminal.hpp"

#include "lvgl.h"

// Pepboy splash screen images
LV_IMG_DECLARE(pepboy_0);
LV_IMG_DECLARE(pepboy_1);
LV_IMG_DECLARE(pepboy_2);
LV_IMG_DECLARE(pepboy_3);
LV_IMG_DECLARE(pepboy_4);
LV_IMG_DECLARE(pepboy_5);
LV_IMG_DECLARE(pepboy_6);
LV_IMG_DECLARE(pepboy_7);

#if defined(BSP_LCD_DRAW_BUFF_SIZE)
#define DRAW_BUF_SIZE BSP_LCD_DRAW_BUFF_SIZE
#else
#define DRAW_BUF_SIZE (BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT)
#endif

static const char *TAG = "main";

static i2c_master_bus_handle_t i2c_handle;
static lv_obj_t *ssh_screen;
static SSHTerminal *ssh_terminal = NULL;

// Splash screen variables
static lv_obj_t *splash_screen = NULL;
static lv_obj_t *splash_img = NULL;
static lv_timer_t *splash_timer = NULL;
static int splash_frame = 0;
static const lv_image_dsc_t* pepboy_frames[] = {
    &pepboy_0, &pepboy_1, &pepboy_2, &pepboy_3,
    &pepboy_4, &pepboy_5, &pepboy_6, &pepboy_7
};

// Dismiss splash screen (called by touch or keyboard)
void dismiss_splash_screen()
{
    if (splash_timer) {
        lv_timer_delete(splash_timer);
        splash_timer = NULL;
    }
    if (splash_screen) {
        bsp_display_lock(0);
        lv_scr_load(ssh_screen);
        lv_obj_delete(splash_screen);
        splash_screen = NULL;
        bsp_display_unlock();
    }
}

// Touch event handler for splash screen - skip on touch
void splash_touch_cb(lv_event_t * e)
{
    dismiss_splash_screen();
}

// Splash screen animation callback
void splash_timer_cb(lv_timer_t * timer)
{
    // Update to next frame
    splash_frame++;
    
    // Wrap frame index (8 frames total) - loop indefinitely
    if (splash_frame >= 8) {
        splash_frame = 0;
    }
    
    // Update image
    lv_image_set_src(splash_img, pepboy_frames[splash_frame]);
}

void show_splash_screen()
{
    // Create splash screen
    splash_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash_screen, lv_color_black(), 0);
    
    // Add touch event to skip splash screen
    lv_obj_add_event_cb(splash_screen, splash_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(splash_screen, LV_OBJ_FLAG_CLICKABLE);
    
    // Create image object centered on screen
    splash_img = lv_image_create(splash_screen);
    lv_image_set_src(splash_img, &pepboy_0);
    lv_obj_align(splash_img, LV_ALIGN_CENTER, 0, 0);
    
    // Load splash screen
    lv_scr_load(splash_screen);
    
    // Reset counter
    splash_frame = 0;
    
    // Create timer for animation (100ms per frame for smooth walking animation)
    splash_timer = lv_timer_create(splash_timer_cb, 150, NULL);
}

void keypad_task(void *param)
{
    C3Keyboard keyboard(i2c_handle);
    if (keyboard.init() != ESP_OK)
    {
        ESP_LOGE("KEYPAD", "Failed to initialize keypad!");
        vTaskDelete(NULL);
        return;
    }

    while (1)
    {
        uint32_t key = keyboard.get_key();
        if (key)
        {
            ESP_LOGI("KEYPAD", "Key Pressed: %c", (unsigned char)key);

            // If splash screen is active, dismiss it on any key press
            if (splash_screen) {
                dismiss_splash_screen();
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;  // Skip processing this key
            }

            // Send key input to SSH terminal
            if (ssh_terminal && ssh_screen) {
                ssh_terminal->handle_key_input((char)key);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // Shorter delay for better responsiveness
    }
}

void trackball_task(void *param)
{
    static bool last_up = true;
    static bool last_down = true;
    static bool last_left = true;
    static bool last_right = true;
    static bool last_press = true;
    static uint32_t press_start_time = 0;
    const uint32_t LONG_PRESS_MS = 1000; // 1 second for long press
    
    while (1) {
        bool up = gpio_get_level(BOARD_TBOX_G01);
        bool down = gpio_get_level(BOARD_TBOX_G03);
        bool left = gpio_get_level(BOARD_TBOX_G04);
        bool right = gpio_get_level(BOARD_TBOX_G02);
        bool press = gpio_get_level(BOARD_BOOT_PIN);
        
        // Detect falling edge on Up (roller scrolled up) -> Scroll Terminal Up
        if (!up && last_up) {
            if (ssh_terminal) {
                ssh_terminal->scroll_up(28);
            }
        }
        
        // Detect falling edge on Down (roller scrolled down) -> Scroll Terminal Down
        if (!down && last_down) {
            if (ssh_terminal) {
                ssh_terminal->scroll_down(28);
            }
        }

        // Detect falling edge on Left (roller scrolled left) -> History older or move cursor left
        if (!left && last_left) {
            if (ssh_terminal) {
                ssh_terminal->handle_trackball_left();
            }
        }

        // Detect falling edge on Right (roller scrolled right) -> History newer or move cursor right
        if (!right && last_right) {
            if (ssh_terminal) {
                ssh_terminal->handle_trackball_right();
            }
        }
        
        // Trackball press button handling
        if (!press && last_press) {
            // Button just pressed - record start time
            press_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        } else if (press && !last_press) {
            // Button released - check duration
            uint32_t press_duration = (xTaskGetTickCount() * portTICK_PERIOD_MS) - press_start_time;
            
            if (ssh_terminal) {
                if (press_duration >= LONG_PRESS_MS) {
                    // Long press: Delete current history entry
                    ESP_LOGI("TRACKBALL", "Long press detected (%lu ms) - deleting command", press_duration);
                    ssh_terminal->delete_current_history_entry();
                } else {
                    // Short press:
                    // If terminal is scrolled back, snap immediately back to bottom!
                    // If already at bottom, execute current input (like Enter key)
                    if (ssh_terminal->is_scrolled_back()) {
                        ESP_LOGI("TRACKBALL", "Short press: returning to bottom");
                        ssh_terminal->scroll_to_bottom();
                    } else {
                        ESP_LOGI("TRACKBALL", "Short press: executing current input");
                        ssh_terminal->handle_key_input('\n');
                    }
                }
            }
        }
        
        last_up = up;
        last_down = down;
        last_left = left;
        last_right = right;
        last_press = press;
        
        vTaskDelay(pdMS_TO_TICKS(15)); // Fast 15ms sampling for responsive trackball spin
    }
}

static uint8_t s_detected_touch_addr = 0;

esp_err_t _bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    /* Initialize I2C */
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Initialize I2C Fail: %s", esp_err_to_name(ret));
        return ret;
    }

    // Allow peripheral power rail (GT911) to settle
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_handle = bsp_i2c_get_handle();

    // GT911 can power up with address 0x5D or 0x14 depending on boot strapping
    const uint8_t try_addrs[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,        // 0x5D
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP  // 0x14
    };

    for (size_t i = 0; i < sizeof(try_addrs) / sizeof(try_addrs[0]); i++) {
        uint8_t addr = try_addrs[i];
        ESP_LOGI("Touch", "Probing GT911 touch at I2C address 0x%02X...", addr);

        esp_lcd_touch_io_gt911_config_t gt911_driver_cfg = {
            .dev_addr = addr,
        };

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = BSP_LCD_V_RES, // 240
            .y_max = BSP_LCD_H_RES, // 320
            .rst_gpio_num = GPIO_NUM_NC,
            // Use polling mode (GPIO_NUM_NC) so LVGL periodically polls every 30ms.
            // Hardware interrupt edge detection on GPIO 16 is notoriously unreliable
            // across T-Deck hardware revisions, which locks up touch in event mode.
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = true,
                .mirror_x = true,
                .mirror_y = false,
            },
            .process_coordinates = NULL,
            .interrupt_callback = NULL,
            .user_data = NULL,
            .driver_data = &gt911_driver_cfg,
        };

        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = addr;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 16;
        tp_io_config.flags.disable_control_phase = 1;
        tp_io_config.scl_speed_hz = 100000; // 100 kHz standard for GT911 on shared bus

        esp_err_t io_ret = esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle);
        if (io_ret != ESP_OK) {
            ESP_LOGW("Touch", "Failed to create panel IO for addr 0x%02X: %s", addr, esp_err_to_name(io_ret));
            continue;
        }

        esp_err_t touch_ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
        if (touch_ret == ESP_OK && *ret_touch != NULL) {
            s_detected_touch_addr = addr;
            ESP_LOGI("Touch", "GT911 touch controller detected and initialized at 0x%02X!", addr);
            return ESP_OK;
        } else {
            ESP_LOGW("Touch", "GT911 not responding at 0x%02X (%s)", addr, esp_err_to_name(touch_ret));
            esp_lcd_panel_io_del(tp_io_handle);
        }
    }

    ESP_LOGW("Touch", "No GT911 touch controller detected on I2C bus (standard non-touch T-Deck)");
    *ret_touch = NULL;
    return ESP_ERR_NOT_FOUND;
}

void device_init(void)
{
    /* Initialize power on pin */
    gpio_reset_pin(BOARD_POWERON);
    gpio_set_direction(BOARD_POWERON, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_POWERON, 1);

    /* Initialize GPIO for SD card CS */
    gpio_reset_pin(BOARD_SDCARD_CS);
    gpio_set_direction(BOARD_SDCARD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_SDCARD_CS, 1);

    /* Initialize radio CS pin */
    gpio_reset_pin(RADIO_CS_PIN);
    gpio_set_direction(RADIO_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RADIO_CS_PIN, 1);

    /* Initialize TFT CS pin */
    gpio_reset_pin(BOARD_TFT_CS);
    gpio_set_direction(BOARD_TFT_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_TFT_CS, 1);

    /* Configure MISO with pull-up for SD card (must be done before SPI bus init) */
    gpio_reset_pin(BOARD_SPI_MISO);
    gpio_set_direction(BOARD_SPI_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_SPI_MISO, GPIO_PULLUP_ONLY);

    // Initialize trackball GPIOs for scrolling and navigation (all 4 directions)
    gpio_reset_pin(BOARD_TBOX_G01);
    gpio_set_direction(BOARD_TBOX_G01, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_TBOX_G01, GPIO_PULLUP_ONLY);
    
    gpio_reset_pin(BOARD_TBOX_G03);
    gpio_set_direction(BOARD_TBOX_G03, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_TBOX_G03, GPIO_PULLUP_ONLY);

    gpio_reset_pin(BOARD_TBOX_G04);
    gpio_set_direction(BOARD_TBOX_G04, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_TBOX_G04, GPIO_PULLUP_ONLY);

    gpio_reset_pin(BOARD_TBOX_G02);
    gpio_set_direction(BOARD_TBOX_G02, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_TBOX_G02, GPIO_PULLUP_ONLY);
    
    // Initialize trackball press button (BOOT button on GPIO 0)
    gpio_reset_pin(BOARD_BOOT_PIN);
    gpio_set_direction(BOARD_BOOT_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_BOOT_PIN, GPIO_PULLUP_ONLY);
}

// Forward declarations
esp_err_t mount_sdcard(void);
void load_ssh_keys_from_sd(SSHTerminal* terminal);

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize device GPIOs */
    device_init();

    /* Initialize display and LVGL (Initializes SPI2_HOST with display) */
    lv_display_t *disp = bsp_display_start();

    /* Set display brightness to 100% */
    bsp_display_backlight_on();

    /* Mount SD card on shared SPI2 bus concurrently with display */
    esp_err_t sd_status = mount_sdcard();

    /* Initialize touch (auto-detects GT911 on 0x5D or 0x14 with polling mode) */
    esp_lcd_touch_handle_t touch_handle = NULL;
    const bsp_touch_config_t bsp_touch_cfg = {};
    esp_err_t touch_status = _bsp_touch_new(&bsp_touch_cfg, &touch_handle);

    if (touch_status == ESP_OK && touch_handle != NULL) {
        /* Add touch input (for selected screen) */
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = touch_handle,
            .scale = {.x = 0, .y = 0},
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch input registered with LVGL");
    } else {
        ESP_LOGI(TAG, "Touch controller not found - running in Trackball & Keyboard mode");
    }

    bsp_display_lock(0);

    // Show splash screen animation
    show_splash_screen();

    // Create terminal instance
    ssh_terminal = new SSHTerminal();
    load_ssh_keys_from_sd(ssh_terminal);
    ssh_screen = ssh_terminal->create_terminal_screen();
    
    // Display version and initial instructions
#ifdef POCKETSSH_VERSION
    ssh_terminal->append_text("PocketSSH v" POCKETSSH_VERSION "\n");
#else
    ssh_terminal->append_text("PocketSSH Terminal Ready\n");
#endif

    // Display storage status
    if (sd_status == ESP_OK) {
        ssh_terminal->append_text("Storage: MicroSD mounted (/sdcard)\n");
    } else {
        ssh_terminal->append_text("Storage: MicroSD not mounted (NVS fallback)\n");
    }

    // Display touch controller status
    if (touch_status == ESP_OK && touch_handle != NULL) {
        char touch_info[64];
        snprintf(touch_info, sizeof(touch_info), "Touch: GT911 active (0x%02X)\n", s_detected_touch_addr);
        ssh_terminal->append_text(touch_info);
    } else {
        ssh_terminal->append_text("Touch: Hardware not detected (Trackball navigation mode)\n");
    }

    // Display loaded SSH keys
    auto loaded_keys = ssh_terminal->get_loaded_key_names();
    if (!loaded_keys.empty()) {
        ssh_terminal->append_text("\nLoaded SSH keys:\n");
        for (const auto& keyname : loaded_keys) {
            ssh_terminal->append_text("  - ");
            ssh_terminal->append_text(keyname.c_str());
            ssh_terminal->append_text("\n");
        }
        ssh_terminal->append_text("\n");
    } else {
        ssh_terminal->append_text("\nNo SSH keys found in /sdcard/ssh_keys/ or NVS.\n");
        ssh_terminal->append_text("Use 'getkey <path>' or copy .pem files to /sdcard/ssh_keys/\n\n");
    }
    
    // Update status bar to show initial battery voltage
    ssh_terminal->update_status_bar();

    bsp_display_unlock();

    // Check for saved WiFi network and auto-connect
    ssh_terminal->try_autoconnect();

    xTaskCreate(keypad_task, "keypad_task", 4096, NULL, 5, NULL);
    
    xTaskCreate(trackball_task, "trackball_task", 4096, NULL, 5, NULL);
}

static sdmmc_card_t* s_sdcard = nullptr;
static bool s_sdcard_mounted = false;

bool is_sd_card_mounted() {
    return s_sdcard_mounted;
}

esp_err_t mount_sdcard(void)
{
    if (s_sdcard_mounted) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting SD card on shared SPI2 bus (CS=%d)...", BOARD_SDCARD_CS);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000; // 20 MHz

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)BOARD_SDCARD_CS;
    slot_config.host_id = SPI2_HOST;

    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &s_sdcard);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount SD card on SPI2: %s", esp_err_to_name(ret));
        s_sdcard_mounted = false;
        return ret;
    }

    s_sdcard_mounted = true;
    ESP_LOGI(TAG, "SD card successfully mounted at /sdcard on SPI2_HOST");

    // Ensure ssh_keys directory exists
    mkdir("/sdcard/ssh_keys", 0755);

    return ESP_OK;
}

/*
 * Load SSH private keys from SD card
 * Reads all .pem files from /sdcard/ssh_keys/ directory into memory.
 * SD card remains permanently mounted for continuous read/write.
 */
void load_ssh_keys_from_sd(SSHTerminal* terminal)
{
    if (!terminal) {
        ESP_LOGE(TAG, "Terminal pointer is NULL");
        return;
    }

    if (!s_sdcard_mounted) {
        ESP_LOGW(TAG, "SD card is not mounted, skipping SD key scan");
        return;
    }

    const char* keys_dir = "/sdcard/ssh_keys";
    ESP_LOGI(TAG, "Scanning for keys in: %s", keys_dir);
    DIR* dir = opendir(keys_dir);
    if (!dir) {
        ESP_LOGW(TAG, "Directory %s not found - creating it", keys_dir);
        mkdir(keys_dir, 0755);
        dir = opendir(keys_dir);
        if (!dir) {
            ESP_LOGE(TAG, "Cannot open or create %s", keys_dir);
            return;
        }
    }

    struct dirent* entry;
    int keys_loaded = 0;
    int files_found = 0;

    while ((entry = readdir(dir)) != NULL) {
        files_found++;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t name_len = strlen(entry->d_name);
        if (name_len < 5) continue;

        const char* ext = entry->d_name + name_len - 4;
        if (strcasecmp(ext, ".pem") != 0 && strcasecmp(ext, ".key") != 0 && strcasecmp(ext, ".rsa") != 0) {
            continue;
        }

        char filepath[256];
        snprintf(filepath, sizeof(filepath), "%s/%s", keys_dir, entry->d_name);

        FILE* f = fopen(filepath, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open key file: %s", filepath);
            continue;
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (file_size <= 0 || file_size > 16384) {
            ESP_LOGW(TAG, "Invalid key file size: %s (%ld bytes)", entry->d_name, file_size);
            fclose(f);
            continue;
        }

        char* key_data = (char*)malloc(file_size + 1);
        if (!key_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for key: %s", entry->d_name);
            fclose(f);
            continue;
        }

        size_t bytes_read = fread(key_data, 1, file_size, f);
        fclose(f);

        if (bytes_read != (size_t)file_size) {
            ESP_LOGE(TAG, "Failed to read complete key file: %s", entry->d_name);
            free(key_data);
            continue;
        }

        key_data[file_size] = '\0';
        terminal->load_key_from_memory(entry->d_name, key_data, file_size);
        free(key_data);
        keys_loaded++;
    }

    closedir(dir);
    ESP_LOGI(TAG, "SD key scan complete: %d keys loaded from %d files", keys_loaded, files_found);
}
