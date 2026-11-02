#include "c3_keyboard.hpp"
#include "esp_log.h"

#define I2C_KEYPAD_ADDR 0x55      
#define I2C_MASTER_FREQ_HZ 100000

static const char *TAG = "C3_KEYBOARD";

// Constructor: Store the I2C bus handle
C3Keyboard::C3Keyboard(i2c_master_bus_handle_t i2c_handle)
{
    this->i2c_handle = i2c_handle;
}

// Keypad Initialization
esp_err_t C3Keyboard::init()
{
    if (i2c_handle == NULL)
    {
        ESP_LOGE(TAG, "I2C not initialized! Call bsp_i2c_init() first.");
        return ESP_FAIL;
    }

    // Probe the I2C bus with a 50ms timeout to detect if the physical C3 keyboard coprocessor is present.
    // In Wokwi simulation or environments without the physical keyboard hardware, probe fails gracefully
    // so we avoid polling a non-existent device and flooding the console with I2C NACK errors.
    esp_err_t probe_ret = i2c_master_probe(i2c_handle, I2C_KEYPAD_ADDR, 50);
    if (probe_ret != ESP_OK)
    {
        ESP_LOGI(TAG, "Keypad coprocessor not detected at I2C address 0x%02X (%s) - running in Virtual/Serial mode",
                 I2C_KEYPAD_ADDR, esp_err_to_name(probe_ret));
        return probe_ret;
    }

    // Configure the keypad device
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, // Address Length (7-bit)
        .device_address = I2C_KEYPAD_ADDR,     // Keypad I2C Address
        .scl_speed_hz = I2C_MASTER_FREQ_HZ     // I2C Speed (100kHz)
    };

    // Register the keypad device on the existing I2C bus
    esp_err_t err = i2c_master_bus_add_device(i2c_handle, &dev_config, &keypad_dev);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Keypad initialized successfully on I2C.");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to initialize keypad: %s", esp_err_to_name(err));
    }

    return err;
}

// Read a key press from the keypad
uint32_t C3Keyboard::get_key()
{
    uint8_t key_ch = 0;
    uint8_t dummy_register = 0x00; // Some keypads require register address before reading

    // Perform I2C read operation using new driver API
    esp_err_t ret = i2c_master_transmit_receive(
        keypad_dev,
        &dummy_register, 1, // Send a register address before reading
        &key_ch, 1,         // Read 1 byte
        1000                // Timeout in ms
    );

    if (ret != ESP_OK)
    {
        static int s_fail_count = 0;
        if (s_fail_count < 3) {
            ESP_LOGW(TAG, "Failed to read key from keypad: %s", esp_err_to_name(ret));
            s_fail_count++;
        }
        return 0;
    }

    // ESP_LOGI(TAG, "Keypad Pressed: 0x%02X", key_ch);
    return key_ch;
}
