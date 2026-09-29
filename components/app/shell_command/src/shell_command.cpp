/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: 遥控器 Shell 命令集中注册与参数解析
 * @author: qingmeijiupiao
 * @LastEditTime: 2026-06-06
 */
#include "shell_command.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "battery_level.h"
#include "battery_voltage.h"
#include "blackbox.h"
#include "blackbox_service.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "espnow_remote.h"
#include "espnow_service.h"
#include "shell.h"
#include "power_manager.h"
#include "status_led.h"

namespace ShellCommand {
namespace {

constexpr char TAG[] = "ShellCommand";

void print_escaped_text(const char* text) {
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
            case '\\':
                printf("\\\\");
                break;
            case '\r':
                printf("\\r");
                break;
            case '\n':
                printf("\\n");
                break;
            case '\t':
                printf("\\t");
                break;
            default:
                putchar(*cursor);
                break;
        }
    }
}

} // namespace

esp_err_t init() {
    auto& shell = Shell::instance();
    ESP_RETURN_ON_ERROR(shell.init(), "ShellCommand", "shell init failed");
    shell.set_prompt("ESP@wireless_power_switch_button> ");

    /**
     * @brief version - 获取固件版本号与编译时间
     * @usage version
     */
    shell.register_command(ShellCommand_t(
        "version", "Get firmware version and build time", "",
        [](int, char**) {
            if (VERSION_PATCH == 99) {
                printf("Firmware: %d.%d.%d (Local build not official firmware!)\n",
                       VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
            } else {
                printf("Firmware: %d.%d.%d\n",
                       VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
            }
            printf("Build:    %s\n", BUILD_TIME);
            return 0;
        }));

    /**
     * @brief battery - 查询本机电池电压、电量和校准状态
     * @usage battery [status|reset-calibration|reset-level]
     * @note 采样期间 GPIO2 开漏拉低，结束后立即恢复高阻。
     */
    shell.register_command(ShellCommand_t(
        "battery", "Battery level and calibration",
        "[status|reset-calibration|reset-level]",
        [](int argc, char** argv) {
            const char* action = argc > 1 ? argv[1] : "status";
            if (!strcmp(action, "reset-calibration")) {
                const esp_err_t ret = BatteryVoltage::reset_calibration();
                printf("battery calibration reset: %s\n", esp_err_to_name(ret));
                return ret == ESP_OK ? 0 : 1;
            }
            if (!strcmp(action, "reset-level")) {
                BatteryLevel::reset();
                printf("battery RTC level reset\n");
                return 0;
            }
            if (strcmp(action, "status") != 0) {
                printf("Usage: battery [status|reset-calibration|reset-level]\n");
                return 1;
            }

            int voltage_mv = 0;
            const esp_err_t ret = BatteryVoltage::read_mv(voltage_mv);
            if (ret == ESP_OK) {
                const BatteryLevel::Status level =
                    BatteryLevel::update(
                        voltage_mv, PowerManager::usb_connected());
                printf("battery voltage=%d mV estimated=%u%% displayed=%u%% "
                       "charging=%u rtc_restored=%u\n",
                       voltage_mv,
                       static_cast<unsigned>(level.estimated_percent),
                       static_cast<unsigned>(level.displayed_percent),
                       level.charging ? 1U : 0U,
                       level.restored_from_rtc ? 1U : 0U);
            } else {
                printf("battery read failed: %s\n", esp_err_to_name(ret));
            }

            BatteryVoltage::CalibrationStatus calibration = {};
            BatteryVoltage::get_calibration_status(calibration);
            printf("calibration scale_q16=%lu scale=%.6f stored=%u "
                   "monitor=%u stable=%u/60 range=%d..%d mV\n",
                   static_cast<unsigned long>(calibration.divider_scale_q16),
                   calibration.divider_scale_q16 / 65536.0,
                   calibration.stored_calibration_valid ? 1U : 0U,
                   calibration.monitor_running ? 1U : 0U,
                   static_cast<unsigned>(calibration.stable_sample_count),
                   calibration.stable_min_mv,
                   calibration.stable_max_mv);
            return ret == ESP_OK ? 0 : 1;
        }));

    /**
     * @brief espnow - 查询链路状态并管理配对记录
     * @usage espnow status|pair|stop|clear|peers
     * @note 配对扫描和 peer 持久化由 EspNowRemote/EspNowLink 负责。
     */
    shell.register_command(ShellCommand_t(
        "espnow", "ESP-NOW status and pairing", "status|pair|stop|clear|peers",
        [](int argc, char** argv) {
            const char* sub = argc > 1 ? argv[1] : "status";
            if (!strcmp(sub, "status") || !strcmp(sub, "peers")) {
                EspNowRemote::print_status();
                return 0;
            }
            if (!strcmp(sub, "pair")) {
                return EspNowRemote::start_pairing(false) == ESP_OK ? 0 : 1;
            }
            if (!strcmp(sub, "stop")) {
                EspNowRemote::stop_pairing();
                printf("[pair] stopped\n");
                return 0;
            }
            if (!strcmp(sub, "clear")) {
                const esp_err_t ret = EspNowRemote::clear_peers();
                printf("[pair] clear=%s\n", esp_err_to_name(ret));
                return ret == ESP_OK ? 0 : 1;
            }
            printf("Usage: espnow status|pair|stop|clear|peers\n");
            return 1;
        }));

    /**
     * @brief remote - 执行远程开关、数据读取和信道恢复测试
     * @usage remote on|off|toggle|read|find|channel <1-13>|recover|test [count]
     * @note 命令处理函数运行在 Shell 调用任务中，可同步等待链路和业务响应。
     */
    shell.register_command(ShellCommand_t(
        "remote", "Remote meter test commands",
        "on|off|toggle|read|find|channel <1-13>|recover|test [count]",
        [](int argc, char** argv) {
            if (argc < 2) {
                printf("Usage: remote on|off|toggle|read|find|channel <1-13>|recover|test [count]\n");
                return 1;
            }
            if (!strcmp(argv[1], "on")) {
                return EspNowRemote::send_switch(EspNowService::SwitchAction::ON) == ESP_OK ? 0 : 1;
            }
            if (!strcmp(argv[1], "off")) {
                return EspNowRemote::send_switch(EspNowService::SwitchAction::OFF) == ESP_OK ? 0 : 1;
            }
            if (!strcmp(argv[1], "toggle")) {
                return EspNowRemote::send_switch(EspNowService::SwitchAction::TOGGLE) == ESP_OK ? 0 : 1;
            }
            if (!strcmp(argv[1], "read")) {
                return EspNowRemote::read_data() == ESP_OK ? 0 : 1;
            }
            if (!strcmp(argv[1], "find")) {
                return EspNowRemote::start_pairing(false) == ESP_OK ? 0 : 1;
            }
            if (!strcmp(argv[1], "channel") && argc == 3) {
                const long channel = strtol(argv[2], nullptr, 10);
                const esp_err_t ret = channel >= 1 && channel <= 13
                                          ? EspNowRemote::set_channel(channel)
                                          : ESP_ERR_INVALID_ARG;
                printf("[channel] set=%ld result=%s\n", channel, esp_err_to_name(ret));
                return ret == ESP_OK ? 0 : 1;
            }
            if (!strcmp(argv[1], "recover")) {
                return EspNowRemote::recover_channel() == ESP_OK ? 0 : 1;
            }
            if (!strcmp(argv[1], "test")) {
                const int count = argc >= 3 ? atoi(argv[2]) : 1;
                return EspNowRemote::run_test(count) == ESP_OK ? 0 : 1;
            }
            printf("Usage: remote on|off|toggle|read|find|channel <1-13>|recover|test [count]\n");
            return 1;
        }));

    /**
     * @brief blackbox - 查询、拉取、清空黑匣子或写入人工标记
     * @usage blackbox [status|dump [count|all]|pull [count|all]|clear|mark <text>]
     */
    shell.register_command(ShellCommand_t(
        "blackbox", "Blackbox log control",
        "[status|dump [count|all]|pull [count|all]|clear|mark <text>]",
        [](int argc, char** argv) {
            const char* action = argc >= 2 ? argv[1] : "status";

            if (strcmp(action, "status") == 0) {
                BlackboxService::Statistics statistics = {};
                BlackboxService::get_statistics(&statistics);
                printf("Blackbox status: enabled=%d records=%lu/%lu "
                       "captured=%lu pending=%u dropped=%lu persist_failures=%lu\n",
                       Blackbox::is_enabled(),
                       static_cast<unsigned long>(Blackbox::count()),
                       static_cast<unsigned long>(Blackbox::capacity()),
                       static_cast<unsigned long>(statistics.captured_logs),
                       static_cast<unsigned>(statistics.pending_logs),
                       static_cast<unsigned long>(statistics.dropped_logs),
                       static_cast<unsigned long>(statistics.persist_failures));
                printf("Chip uptime_ms=%llu heap_free=%lu heap_min=%lu\n",
                       static_cast<unsigned long long>(esp_timer_get_time() / 1000),
                       static_cast<unsigned long>(esp_get_free_heap_size()),
                       static_cast<unsigned long>(esp_get_minimum_free_heap_size()));
                return 0;
            }

            if (strcmp(action, "clear") == 0) {
                const esp_err_t ret = Blackbox::erase_all();
                printf("Blackbox clear: %s, persisted_records=%lu\n",
                       esp_err_to_name(ret),
                       static_cast<unsigned long>(Blackbox::count()));
                return ret == ESP_OK ? 0 : 1;
            }

            if (strcmp(action, "mark") == 0) {
                if (argc < 3) {
                    printf("Usage: blackbox mark <text>\n");
                    return 1;
                }
                char text[96] = {};
                size_t position = 0;
                for (int i = 2; i < argc && position < sizeof(text) - 1; ++i) {
                    if (i > 2 && position < sizeof(text) - 1) {
                        text[position++] = ' ';
                    }
                    for (const char* cursor = argv[i];
                         *cursor != '\0' && position < sizeof(text) - 1;
                         ++cursor) {
                        const char character = *cursor;
                        text[position++] =
                            (character == '\r' || character == '\n' || character == '\t')
                                ? ' '
                                : character;
                    }
                }
                const esp_err_t ret =
                    BlackboxService::append_text_event("mark: source=%s text=%s", TAG, text);
                printf("Blackbox mark added: %s\n", text);
                return ret == ESP_OK ? 0 : 1;
            }

            if (strcmp(action, "dump") == 0 || strcmp(action, "pull") == 0) {
                uint32_t limit = 100;
                const char* limit_label = "100";
                if (argc >= 3) {
                    if (strcmp(argv[2], "all") == 0) {
                        limit = UINT32_MAX;
                        limit_label = "all";
                    } else {
                        char* end = nullptr;
                        const unsigned long parsed = strtoul(argv[2], &end, 10);
                        if (argv[2][0] == '\0' || *end != '\0' ||
                            parsed == 0 || parsed > UINT32_MAX) {
                            printf("Usage: blackbox %s [count|all]\n", action);
                            return 1;
                        }
                        limit = static_cast<uint32_t>(parsed);
                        limit_label = argv[2];
                    }
                }
                if (argc >= 4) {
                    printf("Usage: blackbox %s [count|all]\n", action);
                    return 1;
                }

                const esp_err_t sync_result = BlackboxService::sync();
                if (sync_result != ESP_OK) {
                    printf("Blackbox sync failed: %s\n", esp_err_to_name(sync_result));
                    return 1;
                }

                const uint32_t raw_count = Blackbox::count();
                printf("BLACKBOX_DUMP_BEGIN persisted_records=%lu limit=%s order=newest_first\n",
                       static_cast<unsigned long>(raw_count),
                       limit_label);
                uint32_t emitted = 0;
                uint32_t index = 0;
                while (index < raw_count && emitted < limit) {
                    const Blackbox::Record record = Blackbox::read(index);
                    const Blackbox::TextRecord text = Blackbox::read_text(index);
                    if (text.record_count != 0) {
                        printf("r=%lu t_ms=%lu n=%u ",
                               static_cast<unsigned long>(index),
                               static_cast<unsigned long>(record.header.timestamp),
                               static_cast<unsigned>(text.record_count));
                        print_escaped_text(text.str);
                        putchar('\n');
                        index += text.record_count;
                        ++emitted;
                        continue;
                    }
                    printf("r=%lu invalid\n", static_cast<unsigned long>(index));
                    ++index;
                    ++emitted;
                }
                printf("BLACKBOX_DUMP_END emitted=%lu consumed_records=%lu remaining_records=%lu\n",
                       static_cast<unsigned long>(emitted),
                       static_cast<unsigned long>(index),
                       static_cast<unsigned long>(raw_count - index));
                return 0;
            }

            printf("Usage: blackbox [status|dump [count|all]|pull [count|all]|clear|mark <text>]\n");
            return 1;
        }));

    return ESP_OK;
}

} // namespace ShellCommand
