/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: ESP32-C3 GPIO 唤醒与深度休眠实现
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-07
 */
#include "power_manager.h"

#include <algorithm>

#include "battery_voltage.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led.h"

namespace PowerManager {
namespace {

constexpr gpio_num_t BUTTON_GPIO = GPIO_NUM_4;
constexpr gpio_num_t USB_DETECT_GPIO = GPIO_NUM_5;
// 深睡前将与当前唤醒无关的引脚统一切换为高阻，避免内部上下拉或输出保持
// 形成额外漏电路径。列表包含板级改线和 USB/JTAG 相关引脚。
constexpr gpio_num_t HIGH_IMPEDANCE_SLEEP_GPIOS[] = {
    GPIO_NUM_2,  // 电池分压下端控制
    GPIO_NUM_3,  // 电池分压 ADC 中点
    GPIO_NUM_7,  // 板级改线后与 USB 检测信号同网
    GPIO_NUM_8,  // 外部 10 kOhm 上拉
    GPIO_NUM_9,  // BOOT，外部 10 kOhm 上拉
    GPIO_NUM_18, // USB D-
    GPIO_NUM_19, // USB D+
};

int64_t app_started_us;
WakeSource source = WakeSource::OTHER;

esp_err_t configure_inputs() {
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << BUTTON_GPIO) | (1ULL << USB_DETECT_GPIO);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

esp_err_t configure_wakeup_inputs_for_sleep() {
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << BUTTON_GPIO) | (1ULL << USB_DETECT_GPIO);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

esp_err_t isolate_unused_sleep_pins() {
    for (const gpio_num_t gpio : HIGH_IMPEDANCE_SLEEP_GPIOS) {
        gpio_config_t config = {};
        config.pin_bit_mask = 1ULL << gpio;
        config.mode = GPIO_MODE_DISABLE;
        config.pull_up_en = GPIO_PULLUP_DISABLE;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.intr_type = GPIO_INTR_DISABLE;

        const esp_err_t ret = gpio_config(&config);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

void release_sleep_pin_holds() {
    gpio_deep_sleep_hold_dis();
}

} // namespace

esp_err_t init() {
    app_started_us = esp_timer_get_time();
    release_sleep_pin_holds();
    ESP_RETURN_ON_ERROR(configure_inputs(), "PowerManager", "wakeup GPIO config failed");

    const uint32_t causes = esp_sleep_get_wakeup_causes();
    const uint64_t gpio_status = esp_sleep_get_gpio_wakeup_status();
    const bool button_wakeup =
        (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) != 0 &&
        (gpio_status & (1ULL << BUTTON_GPIO)) != 0;
    const bool usb_wakeup =
        (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) != 0 &&
        (gpio_status & (1ULL << USB_DETECT_GPIO)) != 0;

    if (button_wakeup && usb_wakeup) {
        source = WakeSource::BUTTON_AND_USB;
    } else if (button_wakeup) {
        source = WakeSource::BUTTON;
    } else if (usb_wakeup) {
        source = WakeSource::USB;
    } else if (causes == 0) {
        source = WakeSource::POWER_ON;
    } else {
        source = WakeSource::OTHER;
    }
    return ESP_OK;
}

WakeSource wake_source() {
    return source;
}

bool usb_connected() {
    return gpio_get_level(USB_DETECT_GPIO) != 0;
}

bool button_pressed() {
    return gpio_get_level(BUTTON_GPIO) == 0;
}

uint32_t button_press_elapsed_ms() {
    const int64_t elapsed_us = std::max<int64_t>(0, esp_timer_get_time() - app_started_us);
    return static_cast<uint32_t>(elapsed_us / 1000);
}

esp_err_t enter_deep_sleep() {
    if (button_pressed() || usb_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(StatusLed::prepare_for_sleep(),
                        "PowerManager", "LED sleep state failed");
    ESP_RETURN_ON_ERROR(BatteryVoltage::disable_sample_path(),
                        "PowerManager", "battery divider disable failed");
    ESP_RETURN_ON_ERROR(configure_wakeup_inputs_for_sleep(),
                        "PowerManager", "wakeup GPIO reconfigure failed");
    ESP_RETURN_ON_ERROR(isolate_unused_sleep_pins(),
                        "PowerManager", "unused GPIO isolation failed");
    ESP_RETURN_ON_ERROR(
        esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
            1ULL << BUTTON_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW),
        "PowerManager", "button wakeup config failed");
    ESP_RETURN_ON_ERROR(
        esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
            1ULL << USB_DETECT_GPIO, ESP_GPIO_WAKEUP_GPIO_HIGH),
        "PowerManager", "USB wakeup config failed");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

} // namespace PowerManager
