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
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

#include "esp_lcd_touch_gt911.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "driver/uart_vfs.h"

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
static lv_obj_t *splash_countdown_label = NULL;
static lv_obj_t *splash_bar = NULL;
static lv_timer_t *splash_timer = NULL;
static int splash_frame = 0;
static uint32_t splash_start_ms = 0;
static const lv_image_dsc_t* pepboy_frames[] = {
    &pepboy_0, &pepboy_1, &pepboy_2, &pepboy_3,
    &pepboy_4, &pepboy_5, &pepboy_6, &pepboy_7
};

// Dismiss splash screen (called by touch, keyboard, trackball, or 30s timeout)
void dismiss_splash_screen()
{
    if (!splash_screen) return;

    bool locked = bsp_display_lock(pdMS_TO_TICKS(500));
    if (locked) {
        if (splash_timer) {
            lv_timer_delete(splash_timer);
            splash_timer = NULL;
        }
        if (splash_screen) {
            lv_scr_load(ssh_screen);
            lv_obj_delete(splash_screen);
            splash_screen = NULL;
            splash_img = NULL;
            splash_countdown_label = NULL;
            splash_bar = NULL;
        }
        bsp_display_unlock();
    }
}

// Touch event handler for splash screen - skip on touch after 1.5s grace period
void splash_touch_cb(lv_event_t * e)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - splash_start_ms >= 1500) {
        dismiss_splash_screen();
    }
}

// Splash screen animation and 30-second countdown callback (125ms per frame)
void splash_timer_cb(lv_timer_t * timer)
{
    if (!splash_screen) return;

    // Update Pip-Boy walking frame
    splash_frame = (splash_frame + 1) % 8;
    if (splash_img) {
        lv_image_set_src(splash_img, pepboy_frames[splash_frame]);
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t elapsed_ms = now_ms - splash_start_ms;

    // 30 seconds reached: automatically transition to main screen
    if (elapsed_ms >= 30000) {
        dismiss_splash_screen();
        return;
    }

    // Update live countdown
    if (splash_countdown_label) {
        int remaining_sec = (30000 - elapsed_ms + 999) / 1000;
        if (remaining_sec < 0) remaining_sec = 0;
        char buf[32];
        snprintf(buf, sizeof(buf), "Starting in %ds...", remaining_sec);
        lv_label_set_text(splash_countdown_label, buf);
    }

    // Update progress bar (0 to 300)
    if (splash_bar) {
        lv_bar_set_value(splash_bar, elapsed_ms / 100, LV_ANIM_OFF);
    }
}

void show_splash_screen()
{
    // Create splash screen
    splash_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(splash_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(splash_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Add touch event to skip splash screen (after grace period)
    lv_obj_add_event_cb(splash_screen, splash_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(splash_screen, LV_OBJ_FLAG_CLICKABLE);

    // Left side: Animated Pip-Boy Walking Character (109 x 180)
    splash_img = lv_image_create(splash_screen);
    lv_image_set_src(splash_img, &pepboy_0);
    lv_obj_align(splash_img, LV_ALIGN_LEFT_MID, 8, 0);

    // Right side: Pip-Boy Retro Fallout / Terminal Info Card
    lv_obj_t* info_card = lv_obj_create(splash_screen);
    lv_obj_set_size(info_card, 192, 216);
    lv_obj_align(info_card, LV_ALIGN_TOP_RIGHT, -8, 12);
    lv_obj_set_style_bg_color(info_card, lv_color_hex(0x081208), 0);
    lv_obj_set_style_border_color(info_card, lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_border_width(info_card, 1, 0);
    lv_obj_set_style_radius(info_card, 6, 0);
    lv_obj_set_style_pad_all(info_card, 8, 0);
    lv_obj_clear_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Title
    lv_obj_t* title_lbl = lv_label_create(info_card);
    lv_label_set_text(title_lbl, "POCKET SSH");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 2, 0);

    // 2. Subtitle / App Description
    lv_obj_t* desc_lbl = lv_label_create(info_card);
    lv_label_set_text(desc_lbl, "Handheld SSH Client\nLilyGO T-Deck Plus");
    lv_obj_set_style_text_color(desc_lbl, lv_color_hex(0x60E060), 0);
    lv_obj_set_style_text_font(desc_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(desc_lbl, LV_ALIGN_TOP_LEFT, 2, 26);

    // 3. Divider Line 1
    lv_obj_t* div1 = lv_obj_create(info_card);
    lv_obj_set_size(div1, 172, 1);
    lv_obj_set_style_bg_color(div1, lv_color_hex(0x005500), 0);
    lv_obj_set_style_border_width(div1, 0, 0);
    lv_obj_align(div1, LV_ALIGN_TOP_LEFT, 2, 58);

    // 4. Version Number
    lv_obj_t* ver_lbl = lv_label_create(info_card);
#ifdef POCKETSSH_VERSION
    lv_label_set_text(ver_lbl, "Version: v" POCKETSSH_VERSION);
#else
    lv_label_set_text(ver_lbl, "Version: v1.3.0");
#endif
    lv_obj_set_style_text_color(ver_lbl, lv_color_hex(0xFFFF50), 0);
    lv_obj_set_style_text_font(ver_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(ver_lbl, LV_ALIGN_TOP_LEFT, 2, 66);

    // 5. Author
    lv_obj_t* author_lbl = lv_label_create(info_card);
    lv_label_set_text(author_lbl, "Author: LN4CY");
    lv_obj_set_style_text_color(author_lbl, lv_color_hex(0x50FFFF), 0);
    lv_obj_set_style_text_font(author_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(author_lbl, LV_ALIGN_TOP_LEFT, 2, 86);

    // 6. Author Subtitle
    lv_obj_t* author_sub = lv_label_create(info_card);
    lv_label_set_text(author_sub, "David Remba");
    lv_obj_set_style_text_color(author_sub, lv_color_hex(0x38B0B0), 0);
    lv_obj_set_style_text_font(author_sub, &lv_font_montserrat_10, 0);
    lv_obj_align(author_sub, LV_ALIGN_TOP_LEFT, 2, 104);

    // 7. Divider Line 2
    lv_obj_t* div2 = lv_obj_create(info_card);
    lv_obj_set_size(div2, 172, 1);
    lv_obj_set_style_bg_color(div2, lv_color_hex(0x005500), 0);
    lv_obj_set_style_border_width(div2, 0, 0);
    lv_obj_align(div2, LV_ALIGN_TOP_LEFT, 2, 122);

    // 8. 30-Second Countdown Label
    splash_countdown_label = lv_label_create(info_card);
    lv_label_set_text(splash_countdown_label, "Starting in 30s...");
    lv_obj_set_style_text_color(splash_countdown_label, lv_color_hex(0x00FF50), 0);
    lv_obj_set_style_text_font(splash_countdown_label, &lv_font_montserrat_12, 0);
    lv_obj_align(splash_countdown_label, LV_ALIGN_TOP_LEFT, 2, 130);

    // 9. 30-Second Progress Bar
    splash_bar = lv_bar_create(info_card);
    lv_obj_set_size(splash_bar, 172, 7);
    lv_bar_set_range(splash_bar, 0, 300);
    lv_bar_set_value(splash_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(splash_bar, lv_color_hex(0x003300), 0);
    lv_obj_set_style_bg_color(splash_bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
    lv_obj_set_style_border_color(splash_bar, lv_color_hex(0x008800), 0);
    lv_obj_set_style_border_width(splash_bar, 1, 0);
    lv_obj_set_style_radius(splash_bar, 3, 0);
    lv_obj_align(splash_bar, LV_ALIGN_TOP_LEFT, 2, 150);

    // 10. Skip Hint
    lv_obj_t* skip_hint = lv_label_create(info_card);
    lv_label_set_text(skip_hint, "[ Tap / Key to Skip ]");
    lv_obj_set_style_text_color(skip_hint, lv_color_hex(0x408040), 0);
    lv_obj_set_style_text_font(skip_hint, &lv_font_montserrat_10, 0);
    lv_obj_align(skip_hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Load splash screen
    lv_scr_load(splash_screen);

    // Reset frame and record start time
    splash_frame = 0;
    splash_start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // Timer: every 125ms for smooth 8fps animation and live countdown
    splash_timer = lv_timer_create(splash_timer_cb, 125, NULL);
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

            // If splash screen is active, dismiss it on any key press after grace period (1.5s)
            if (splash_screen) {
                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                if (now_ms - splash_start_ms >= 1500) {
                    dismiss_splash_screen();
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;  // Skip processing this key
                }
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
        
        // Detect falling edge on Up
        if (!up && last_up) {
            if (splash_screen) {
                dismiss_splash_screen();
            } else if (ssh_terminal) {
                if (ssh_terminal->is_alt_mode()) {
                    ssh_terminal->send_arrow_up();
                } else {
                    ssh_terminal->scroll_up(28);
                }
            }
        }
        
        // Detect falling edge on Down
        if (!down && last_down) {
            if (splash_screen) {
                dismiss_splash_screen();
            } else if (ssh_terminal) {
                if (ssh_terminal->is_alt_mode()) {
                    ssh_terminal->send_arrow_down();
                } else {
                    ssh_terminal->scroll_down(28);
                }
            }
        }

        // Detect falling edge on Left
        if (!left && last_left) {
            if (splash_screen) {
                dismiss_splash_screen();
            } else if (ssh_terminal) {
                if (ssh_terminal->is_alt_mode()) {
                    ssh_terminal->send_arrow_left();
                } else {
                    ssh_terminal->handle_trackball_left();
                }
            }
        }

        // Detect falling edge on Right
        if (!right && last_right) {
            if (splash_screen) {
                dismiss_splash_screen();
            } else if (ssh_terminal) {
                if (ssh_terminal->is_alt_mode()) {
                    ssh_terminal->send_arrow_right();
                } else {
                    ssh_terminal->handle_trackball_right();
                }
            }
        }
        
        // Trackball press button handling
        if (!press && last_press) {
            // Button just pressed - record start time
            press_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        } else if (press && !last_press) {
            // Button released - check duration
            uint32_t press_duration = (xTaskGetTickCount() * portTICK_PERIOD_MS) - press_start_time;
            
            if (splash_screen) {
                dismiss_splash_screen();
            } else if (ssh_terminal) {
                if (press_duration >= 400) {
                    // Long press (>400ms):
                    if (ssh_terminal->is_connected()) {
                        ESP_LOGI("TRACKBALL", "Long press in SSH (%lu ms) - sending Ctrl+C", press_duration);
                        ssh_terminal->send_interrupt();
                    } else {
                        ESP_LOGI("TRACKBALL", "Long press detected (%lu ms) - deleting command", press_duration);
                        ssh_terminal->delete_current_history_entry();
                    }
                } else {
                    // Short press:
                    if (ssh_terminal->is_alt_mode()) {
                        ESP_LOGI("TRACKBALL", "Short press in Alt Mode: toggling Alt Mode OFF");
                        ssh_terminal->set_alt_mode(false);
                    } else if (ssh_terminal->is_scrolled_back()) {
                        ESP_LOGI("TRACKBALL", "Short press: returning to bottom");
                        ssh_terminal->scroll_to_bottom();
                    } else {
                        ESP_LOGI("TRACKBALL", "Short press: toggling Alt Mode ON");
                        ssh_terminal->toggle_alt_mode();
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

void serial_terminal_task(void *param)
{
    uart_vfs_dev_port_set_rx_line_endings(0, ESP_LINE_ENDINGS_LF);
    uart_vfs_dev_port_set_tx_line_endings(0, ESP_LINE_ENDINGS_CRLF);
    uart_vfs_dev_use_nonblocking(0);

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    static bool last_was_cr = false;
    while (1) {
        int c = fgetc(stdin);
        if (c != EOF && c >= 0) {
            char ch = (char)c;
            if (ch == '\n' && last_was_cr) {
                last_was_cr = false;
                continue;
            }
            last_was_cr = (ch == '\r');
            if (ch == '\r') {
                ch = '\n';
            }

            // If splash screen is active, dismiss it
            if (splash_screen) {
                dismiss_splash_screen();
            }

            // Echo character for interactive serial shell
            if (ch >= 32 && ch <= 126) {
                esp_rom_printf("%c", ch);
            } else if (ch == 8 || ch == 127) {
                esp_rom_printf("\b \b");
            } else if (ch == '\n') {
                esp_rom_printf("\r\n");
            }

            if (ssh_terminal) {
                ssh_terminal->handle_key_input(ch);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
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
    esp_rom_printf("\n====================================\n[PocketSSH] Virtual Target Boot OK!\n====================================\n\n");
    printf("\n====================================\n[PocketSSH] Starting on ESP32-S3...\n====================================\n");
    fflush(stdout);
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize device GPIOs */
    device_init();

    bool virtual_target = false;
#if CONFIG_SPIRAM
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        virtual_target = true;
    }
#endif

    lv_display_t *disp = NULL;
    esp_err_t sd_status = ESP_FAIL;
    esp_lcd_touch_handle_t touch_handle = NULL;
    esp_err_t touch_status = ESP_FAIL;

    if (!virtual_target) {
        /* Initialize display and LVGL (Initializes SPI2_HOST with display) */
        disp = bsp_display_start();

        /* Set display brightness to 100% */
        bsp_display_backlight_on();

        /* Mount SD card on shared SPI2 bus concurrently with display */
        sd_status = mount_sdcard();

        /* Initialize touch (auto-detects GT911 on 0x5D or 0x14 with polling mode) */
        const bsp_touch_config_t bsp_touch_cfg = {};
        touch_status = _bsp_touch_new(&bsp_touch_cfg, &touch_handle);

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
            // In Wokwi simulation, wokwi-ili9341 is used instead of ST7789 IPS.
            // ST7789 IPS requires hardware color inversion (INVON 0x21), but ILI9341 is a TN panel
            // where INVON inverts colors into negative (black becomes white, green becomes magenta).
            // Disable color inversion for Wokwi simulation so colors are accurate:
            bsp_display_invert_color(false);
            ESP_LOGI(TAG, "Wokwi simulation detected: display color inversion disabled (true colors)");
        }

        bsp_display_lock(0);

        // Show splash screen animation
        show_splash_screen();
    } else {
        ESP_LOGI(TAG, "Virtual Target (QEMU) detected: running Headless Serial Terminal");
    }

    // Create terminal instance
    ssh_terminal = new SSHTerminal();
    if (virtual_target) {
        ssh_terminal->set_virtual_target(true);
        const char* vkey = "-----BEGIN OPENSSH PRIVATE KEY-----\n"
                           "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW\n"
                           "QyNTUxOQAAACD0a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6q7r8s9t0uQAAAJj8e1X1/HtV\n"
                           "9QAAAAtzc2gtZWQyNTUxOQAAACD0a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6q7r8s9t0uQ\n"
                           "AAAEBLM1N2O3P4Q5R6S7T8U9V0W1X2Y3Z0a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6q7r8\n"
                           "s9t0uPRrVvZzd4e5f6g7h8i9j0k1l2m3n4o5p6q7r8s9t0uQAAAAp2aXJ0dWFsX3Rlc3QB\n"
                           "AgMEBQ==\n"
                           "-----END OPENSSH PRIVATE KEY-----\n";
        ssh_terminal->load_key_from_memory("id_ed25519_virtual", vkey, strlen(vkey));
    }
    load_ssh_keys_from_sd(ssh_terminal);
    if (disp) {
        ssh_screen = ssh_terminal->create_terminal_screen();
    }
    
    // Display version and initial instructions
#ifdef POCKETSSH_VERSION
    ssh_terminal->append_text("PocketSSH v" POCKETSSH_VERSION "\n");
#else
    ssh_terminal->append_text("PocketSSH Terminal Ready\n");
#endif

    if (!virtual_target) {
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
    } else {
        ssh_terminal->append_text("Environment: Virtual ESP32-S3 Target (Headless Serial Terminal)\n");
        ssh_terminal->append_text("Storage: NVS Flash\n");
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

    if (disp) {
        bsp_display_unlock();
    } else {
        ssh_terminal->append_text("> ");
    }

    if (!virtual_target) {
        // Check for saved WiFi network and auto-connect
        ssh_terminal->try_autoconnect();

        xTaskCreate(keypad_task, "keypad_task", 4096, NULL, 5, NULL);
        xTaskCreate(trackball_task, "trackball_task", 4096, NULL, 5, NULL);
    }

    xTaskCreate(serial_terminal_task, "serial_terminal_task", 4096, NULL, 5, NULL);
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
