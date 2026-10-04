/**
 * @file shell_command.cpp
 * @brief 急停控制器 Shell 命令集中注册与参数解析
 *
 * 设计约束：
 * - 命令层只做参数解析、字符串输出与调用应用服务，不持有 ESP-NOW 请求状态；
 * - 涉及硬件的操作都通过各服务的线程安全接口提交，避免阻塞 Shell 任务；
 * - 输出文本保持 ASCII 英文，便于串口工具与上位机稳定解析。
 */
#include "shell_command.h"
#include "app_diagnostics.h"

#include <cctype>
#include <cinttypes>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "battery_status.h"
#include "battery_voltage.h"
#include "blackbox.h"
#include "blackbox_service.h"
#include "emergency_remote.h"
#include "emergency_ui.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware.h"
#include "power_manager.h"
#include "runtime_settings.h"
#include "shell.h"

namespace ShellCommand {
namespace {

// 日志与黑匣子人工标记来源所使用的标签。
constexpr char TAG[] = "ShellCommand";

// 将可能包含控制字符的文本转义后输出，保证 dump 结果单行可解析。
void print_escaped_text(const char* text) {
    putchar('"');
    for (const uint8_t* cursor = reinterpret_cast<const uint8_t*>(text); *cursor != '\0'; ++cursor) {
        switch (*cursor) {
            case '\\':
                printf("\\\\");
                break;
            case '"':
                printf("\\\"");
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
                if (std::isprint(*cursor)) {
                    putchar(*cursor);
                } else {
                    printf("\\x%02X", *cursor);
                }
                break;
        }
    }
    putchar('"');
}

void print_hex(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        printf("%02X", data[i]);
    }
}

} // namespace

esp_err_t init() {
    // Shell 是进程内单例；先完成底层初始化，再设置应用提示符。
    auto& shell = Shell::instance();
    ESP_RETURN_ON_ERROR(shell.init(), TAG, "shell init failed");
    shell.set_prompt("ESTOP> ");

    /**
     * @brief version - 获取固件版本号与编译时间
     * @usage version
     */
    shell.register_command(ShellCommand_t(
        "version", "Get firmware version and build time", "",
        [](int, char**) {
            // PATCH=99 表示本地构建，显式标注以避免被误当作正式固件。
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
     * @note 采样期间 GPIO10 开漏拉低，结束后立即恢复高阻。
     */
    shell.register_command(ShellCommand_t(
        "battery", "Battery level and calibration",
        "[status|reset-calibration|reset-level]",
        [](int argc, char** argv) {
            // 无参数时默认查询状态；其余为显式的维护动作。
            const char* action = argc > 1 ? argv[1] : "status";
            if (!strcmp(action, "reset-calibration")) {
                const esp_err_t ret = BatteryVoltage::reset_calibration("shell");
                printf("battery calibration reset: %s\n", esp_err_to_name(ret));
                return ret == ESP_OK ? 0 : 1;
            }
            if (!strcmp(action, "reset-level")) {
                BatteryStatus::reset();
                APP_LOGI("ProductEvent", "battery reset_level source=shell result=ok");
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
                    BatteryStatus::update(voltage_mv, Hardware::usb_connected());
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
                   "monitor=%u stable=%u/60 range=%d..%d mV generation=%lu save_error=%s\n",
                   static_cast<unsigned long>(calibration.divider_scale_q16),
                   calibration.divider_scale_q16 / 65536.0,
                   calibration.stored_calibration_valid ? 1U : 0U,
                   calibration.monitor_running ? 1U : 0U,
                   static_cast<unsigned>(calibration.stable_sample_count),
                   calibration.stable_min_mv,
                   calibration.stable_max_mv,
                   static_cast<unsigned long>(calibration.generation),
                   esp_err_to_name(calibration.last_error));
            return ret == ESP_OK ? 0 : 1;
        }));

    /**
     * @brief config - 列出全部运行参数及有效范围
     * @usage config
     */
    shell.register_command(ShellCommand_t(
        "config", "Show runtime settings and valid ranges", "",
        [](int, char**) {
            RuntimeSettings::print();
            return 0;
        }));

    /**
     * @brief set - 保存运行参数到 NVS
     * @usage set <name> <value>
     */
    shell.register_command(ShellCommand_t(
        "set", "Save a runtime setting to NVS", "<name> <value>",
        [](int argc, char** argv) {
            if (argc != 3) {
                printf("Usage: set <name> <value>\n");
                return 1;
            }
            // 严格校验：必须为完整十进制正整数且落在参数定义范围内，
            // 拒绝空串、尾随字符、负号与溢出。
            char* end = nullptr;
            const unsigned long value = strtoul(argv[2], &end, 10);
            if (!*argv[2] || *end || argv[2][0] == '-' || value > UINT32_MAX ||
                !RuntimeSettings::set(argv[1], static_cast<uint32_t>(value), "shell")) {
                printf("ERROR invalid name/range or NVS write failure; use config\n");
                return 1;
            }
            printf("SAVED %s=%lu\n", argv[1], value);
            return 0;
        }));

    /**
     * @brief remote - 查询链路状态并执行急停/开启/重连/配对操作
     * @usage remote [status|stop|on|retry|pair|repair|test-channel]
     * @note 命令只提交线程安全请求，实际事务由 EmergencyRemote 工作线程完成。
     */
    shell.register_command(ShellCommand_t(
        "remote", "Emergency remote status and control",
        "[status|stop|on|retry|pair|repair|test-channel]",
        [](int argc, char** argv) {
            // 命令只提交线程安全请求，实际事务由 EmergencyRemote 工作线程完成。
            const char* action = argc > 1 ? argv[1] : "status";
            if (!strcmp(action, "status")) {
                const auto s = EmergencyRemote::snapshot();
                printf("REMOTE state=%u paired=%u online=%u stop=%u output=%u confirmed=%u close_timeout=%u protect=%u "
                       "voltage_mv=%u current_ua=%ld\n",
                       static_cast<unsigned>(s.state), s.paired, s.online, s.stop_closed, s.output_on, s.output_confirmed, s.stop_timed_out,
                       s.protection_mask, s.data.voltage_mv, static_cast<long>(s.data.current_ua));
                printf("RUNTIME usb=%d always_on=%u busy=%u pairing=%u sleep_block=%u\n",
                       Hardware::usb_connected(), RuntimeSettings::always_on(), s.busy, s.pairing,
                       static_cast<unsigned>(PowerManager::block()));
                return 0;
            }
            if (!strcmp(action, "stop")) {
                APP_LOGI("ProductEvent", "remote command source=shell action=stop result=submitted");
                EmergencyRemote::request_stop();
                printf("remote stop requested\n");
                return 0;
            }
            if (!strcmp(action, "on")) {
                APP_LOGI("ProductEvent", "remote command source=shell action=on result=submitted");
                EmergencyRemote::request_on();
                printf("remote on requested\n");
                return 0;
            }
            if (!strcmp(action, "retry")) {
                APP_LOGI("ProductEvent", "remote command source=shell action=retry result=submitted");
                EmergencyRemote::retry_connection();
                printf("remote retry requested\n");
                return 0;
            }
            if (!strcmp(action, "pair")) {
                APP_LOGI("ProductEvent", "remote command source=shell action=pair result=submitted");
                EmergencyRemote::start_pairing(false);
                printf("remote pairing started\n");
                return 0;
            }
            if (!strcmp(action, "repair")) {
                APP_LOGI("ProductEvent", "remote command source=shell action=repair result=submitted");
                EmergencyRemote::start_pairing(true);
                printf("remote pairing started (peers cleared)\n");
                return 0;
            }
            if (!strcmp(action, "test-channel")) {
                EmergencyRemote::test_wrong_channel();
                printf("remote channel test requested\n");
                return 0;
            }
            printf("Usage: remote [status|stop|on|retry|pair|repair|test-channel]\n");
            return 1;
        }));

    /**
     * @brief gpio - 输出板级 GPIO 配置
     * @usage gpio
     */
    shell.register_command(ShellCommand_t(
        "gpio", "Dump board GPIO configuration", "",
        [](int, char**) {
            Hardware::dump_gpio();
            return 0;
        }));

    /**
     * @brief fault - 显示最近一次保护/拒绝原因页面
     * @usage fault
     */
    shell.register_command(ShellCommand_t(
        "fault", "Show the historical fault page", "",
        [](int, char**) {
            printf("LAST_FAULT page=%d (historical reason; not a current output assertion)\n",
                   EmergencyUi::last_fault_page());
            return 0;
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

            // status：输出容量、统计与系统运行信息。
            if (!strcmp(action, "status")) {
                BlackboxService::Statistics statistics = {};
                BlackboxService::get_statistics(&statistics);
                printf("Blackbox status: enabled=%d records=%lu/%lu "
                       "captured=%lu pending=%u dropped=%lu persist_failures=%lu async_log_dropped=%lu gestures_dropped=%lu\n",
                       Blackbox::is_enabled(),
                       static_cast<unsigned long>(Blackbox::count()),
                       static_cast<unsigned long>(Blackbox::capacity()),
                       static_cast<unsigned long>(statistics.captured_logs),
                       static_cast<unsigned>(statistics.pending_logs),
                       static_cast<unsigned long>(statistics.dropped_logs),
                       static_cast<unsigned long>(statistics.persist_failures),
                       static_cast<unsigned long>(AppDiagnostics::dropped()),
                       static_cast<unsigned long>(EmergencyUi::dropped_gestures()));
                printf("Chip uptime_ms=%llu heap_free=%lu heap_min=%lu\n",
                       static_cast<unsigned long long>(esp_timer_get_time() / 1000),
                       static_cast<unsigned long>(esp_get_free_heap_size()),
                       static_cast<unsigned long>(esp_get_minimum_free_heap_size()));
                return 0;
            }

            if (!strcmp(action, "clear")) {
                const esp_err_t ret = Blackbox::erase_all();
                printf("Blackbox clear: %s, persisted_records=%lu\n",
                       esp_err_to_name(ret),
                       static_cast<unsigned long>(Blackbox::count()));
                return ret == ESP_OK ? 0 : 1;
            }

            if (!strcmp(action, "mark")) {
                if (argc < 3) {
                    printf("Usage: blackbox mark <text>\n");
                    return 1;
                }
                // 将后续参数拼成单行文本，并把控制字符替换为空格，
                // 避免破坏黑匣子的记录格式。
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

            // dump/pull：按最新优先顺序读取记录；默认最多 100 条，
            // 第二个参数可为条数或 all。
            if (strcmp(action, "dump") == 0 || strcmp(action, "pull") == 0) {
                uint32_t limit = 100;
                const char* limit_label = "100";
                if (argc >= 3) {
                    if (strcmp(argv[2], "all") == 0) {
                        limit = UINT32_MAX;
                        limit_label = "all";
                    } else {
                        char*         end    = nullptr;
                        uint32_t parsed = strtoul(argv[2], &end, 10);
                        if (argv[2][0] == '\0' || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
                            printf("Usage: blackbox %s [count|all]\n", action);
                            return 1;
                        }
                        limit       = static_cast<uint32_t>(parsed);
                        limit_label = argv[2];
                    }
                }
                if (argc >= 4) {
                    printf("Usage: blackbox %s [count|all]\n", action);
                    return 1;
                }

                // 读取前先冲刷待持久化日志，确保 dump 反映已落盘内容。
                if (!AppDiagnostics::flush()) {
                    printf("App diagnostics flush timed out\n");
                    return 1;
                }
                const esp_err_t sync_result = BlackboxService::sync();
                if (sync_result != ESP_OK) {
                    printf("Blackbox sync failed: %s\n", esp_err_to_name(sync_result));
                    return 1;
                }

                const uint32_t raw_count = Blackbox::count();
                printf("BLACKBOX_DUMP_BEGIN persisted_records=%" PRIu32 " limit=%s order=newest_first\n",
                       static_cast<uint32_t>(raw_count), limit_label);
                uint32_t emitted = 0;
                uint32_t index = 0;
                // 拉取大日志时定期让出 CPU：每 100ms 至少 vTaskDelay(1)，
                // 避免长时间独占单核触发任务看门狗复位。
                constexpr int64_t DUMP_YIELD_INTERVAL_US = 100 * 1000;
                int64_t           last_yield_us          = esp_timer_get_time();
                for (; index < raw_count && emitted < limit;) {
                    if (esp_timer_get_time() - last_yield_us >= DUMP_YIELD_INTERVAL_US) {
                        last_yield_us = esp_timer_get_time();
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                    const Blackbox::Record record = Blackbox::read(index);
                    if (record.header.sof != CircularFlashBuffer::BLOCK_SOF) {
                        printf("record=%" PRIu32 " type=INVALID\n", static_cast<uint32_t>(index));
                        ++index;
                        ++emitted;
                        continue;
                    }

                    if (record.header.type == Blackbox::LogType::STRING) {
                        const Blackbox::TextRecord text = Blackbox::read_text(index);
                        if (text.record_count != 0) {
                            printf("record=%" PRIu32 " timestamp_ms=%" PRIu32
                                   " type=STRING fragments=%" PRIu32 " text=",
                                   static_cast<uint32_t>(index),
                                   static_cast<uint32_t>(record.header.timestamp),
                                   static_cast<uint32_t>(text.record_count));
                            print_escaped_text(text.str);
                            putchar('\n');
                            index += text.record_count;
                            ++emitted;
                            continue;
                        }
                    }

                    printf("record=%" PRIu32 " timestamp_ms=%" PRIu32 " type=%s payload=", static_cast<uint32_t>(index),
                           static_cast<uint32_t>(record.header.timestamp),
                           record.header.type == Blackbox::LogType::STRUCTURED ? "STRUCTURED" : "UNKNOWN");
                    print_hex(record.payload.bytes, Blackbox::PAYLOAD_SIZE);
                    putchar('\n');
                    ++index;
                    ++emitted;
                }
                printf("BLACKBOX_DUMP_END emitted=%" PRIu32 " consumed_records=%" PRIu32
                       " remaining_records=%" PRIu32 "\n",
                       static_cast<uint32_t>(emitted), static_cast<uint32_t>(index),
                       static_cast<uint32_t>(raw_count - index));
                return 0;
            }

            printf("Usage: blackbox [status|dump [count|all]|pull [count|all]|clear|mark <text>]\n");
            return 1;
        }));

    return ESP_OK;
}

} // namespace ShellCommand
