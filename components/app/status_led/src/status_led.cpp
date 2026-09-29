/*
 * @version: 2.0
 * @LastEditors: qingmeijiupiao
 * @Description: GPIO10 低有效状态灯与配对/命令反馈实现
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-07
 */
#include "status_led.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "espnow_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace StatusLed {
namespace {

constexpr gpio_num_t LED_GPIO = GPIO_NUM_10;
constexpr uint32_t PAIRING_BLINK_MS = 250;

SemaphoreHandle_t led_mutex;
bool initialized;

esp_err_t set_lit(bool lit) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return gpio_set_level(LED_GPIO, lit ? 0 : 1);
}

void pairing_task(void*) {
    bool phase = false;
    bool was_pairing = false;
    while (true) {
        // 配对提示使用非阻塞加锁，按键确认和错误反馈拥有更高显示优先级。
        if (EspNowLink::is_pairing() && xSemaphoreTake(led_mutex, 0) == pdTRUE) {
            was_pairing = true;
            phase = !phase;
            set_lit(phase);
            xSemaphoreGive(led_mutex);
            vTaskDelay(pdMS_TO_TICKS(PAIRING_BLINK_MS));
        } else {
            if (was_pairing && xSemaphoreTake(led_mutex, 0) == pdTRUE) {
                set_lit(false);
                xSemaphoreGive(led_mutex);
                was_pairing = false;
            }
            phase = false;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

} // namespace

esp_err_t init() {
    gpio_hold_dis(LED_GPIO);

    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << LED_GPIO;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), "StatusLed", "GPIO config failed");

    initialized = true;
    ESP_RETURN_ON_ERROR(off(), "StatusLed", "initial off failed");

    led_mutex = xSemaphoreCreateMutex();
    if (led_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(pairing_task, "status_led", 2048, nullptr, 2, nullptr) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t on() {
    return set_lit(true);
}

esp_err_t off() {
    return set_lit(false);
}

esp_err_t blink(uint32_t count, uint32_t on_ms, uint32_t off_ms) {
    if (!initialized || led_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(led_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    for (uint32_t i = 0; i < count; ++i) {
        ret = on();
        if (ret != ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        ret = off();
        if (ret != ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
    off();
    xSemaphoreGive(led_mutex);
    return ret;
}

esp_err_t prepare_for_sleep() {
    ESP_RETURN_ON_ERROR(off(), "StatusLed", "LED off failed");

    // GPIO10 属于 VDDSDIO 数字 IO 电源域。深睡时保持高电平输出可能导致该
    // 电源域无法关闭；低有效 LED 切换为高阻后仍保持熄灭，无需使用 GPIO hold。
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << LED_GPIO;
    config.mode = GPIO_MODE_DISABLE;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

} // namespace StatusLed
