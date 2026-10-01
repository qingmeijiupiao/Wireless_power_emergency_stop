# Wireless Power Emergency Stop

从 `Wireless_power_switch_button` 复制建立的 ESP32-C3 急停开关工程。
当前阶段：新板运行固件与 Pro V2 无线联调。COM17 急停板接收真实测量数据；触点闭合优先关闭并持续重试，稳定释放后尝试开启，屏幕显示业务确认和拒绝原因。
已实现 BOOT 菜单、常亮设置、休眠判断、连接超时和电量回传；电池供电下的实际深睡电流与唤醒仍需实物验证。
Lite 使用原协议的兼容路径，尚未进行实物联调。

## 仓库与 CI/CD

本工程为私有仓库，仅授权成员可以获取源码和固件。
推送 main、提交 PR 或手动运行会触发 CI，分别编译默认屏幕模式和共享遥控组件检查模式，并验证合并固件布局、上传 Actions artifacts。
推送 `vMAJOR.MINOR` 或 `vMAJOR.MINOR.0` 标签触发发布构建，生成 APP、merged 和 SHA256SUMS，发布到本私有仓库的 GitHub prerelease。
当前发布内容为 急停开关固件。没有公共 CDN、Launchpad 或 firmware-dist 分发；需登录并具有仓库访问权限才能下载。

## 当前硬件（2026-10-01）

| 功能 | GPIO / 端口 |
|---|---|
| SH1106 SDA / SCL | GPIO1 / GPIO2 |
| OLED 供电使能 | GPIO6，高电平开启低侧 Q1 |
| 急停触点 | GPIO5，闭合接地，低电平关闭；高电平释放 |
| USB 供电检测 | GPIO4 |
| 电池 ADC / 分压下端 | GPIO0 / GPIO10，采样期间将 GPIO10 拉低 |
| BOOT 菜单 / 唤醒 | GPIO3，同时连接 GPIO9 |
| 急停板原生 USB | COM17 |
| Pro V2 原生 USB | COM16 |

OLED 使用 I2C 400 kHz，自动探测 0x3C/0x3D，列偏移 2。
GPIO5 下降沿在 ISR 中锁存，控制任务独立于 OLED 传输，每 10 ms 检查输入。
释放须稳定 100 ms；即使已释放，也会先取得关闭业务确认再发送开启。
启动或配对后先同步关闭，启动时的高电平不会自动开启输出。

原 COM3 核心板接线 SDA=21 / SCL=20 已退出默认配置；可在 menuconfig 中调整 I2C 引脚。

## 编译与烧录

ESP-IDF v6.0：

```powershell
idf.py build
idf.py -p COM17 flash
```

本机封装入口：

```powershell
python scripts/idf_local.py build
python scripts/idf_local.py -p COM17 flash
python scripts/verify_display.py --port COM17
python scripts/validate_firmware.py
```

封装脚本仅对子进程关闭 Git CRLF 转换，以保持公共组件哈希与 Git blob 一致。
烧录不主动擦除 NVS；配对记录保留。曾观察到 USB 复位停在下载模式，拔插 USB 后恢复，仍需后续复位稳定性检查。

## 配对和串口诊断

Pro V2 Shell 执行 `espnow pair 120`，随后在急停板 Shell 执行 `remote pair`。
两端配对成功后退出配对模式，急停板先确认关闭，再以 5 Hz（每 200 ms）请求电压、电流和输出状态。

| 急停板命令 | 行为 |
|---|---|
| `remote pair` | 主动配对，不清除已有记录 |
| `remote status` | 打印远端状态、电压、电流和保护掩码 |
| `remote stop` | 请求关闭并保持重试，不产生自动开启 |
| `remote on` | 诊断开启请求；物理触点闭合时忽略 |
| `remote retry` | 重新开始连接尝试 |
| `battery status` | 电池电压、电量和校准快照 |
| `remote test-channel` | 临时切到错误信道 6，用于恢复测试 |

USB 串口 115200，先按回车进入公共 Shell 的 `ESTOP>` 提示符，以上均为整行命令；输入 `exit` 返回日志模式。物理急停控制在诊断期间仍有效，闭合与关闭处理会覆盖诊断页面。屏幕刷新成功不再打印日志；视觉效果须在实物上确认。

## 真实 UI 和可靠性

仪表页显示真实电压、电流和计算功率；数据超过 3 秒未更新则显示离线和横线。
已确认开启、已确认关闭、关闭处理中、开启处理中、通信中断、未配对采用大图标提示。
收到 Pro V2 详细反馈时显示短路、OTP / OVP / UVP / OCP、冷却、忙碌、检测异常等原因。
原协议只返回通用拒绝时显示“开启拒绝 / 原因未提供”，不推测短路或保护。

关闭必须收到业务成功且实际输出为关闭才解除待确认状态。1 秒未确认会重试，并触发加密信道恢复；通信恢复后不需要再次按按钮。
开启只处理一次稳定释放，拒绝后不会反复尝试开启；新的急停闭合可抢占待开启事务。
当前没有通信断开后由 Pro V2 自主执行的看门狗关闭协议，链路中断期间仍只能等待关闭送达。

正式像素资源由 `scripts/generate_product_ui.py` 生成，正常编译无需图像库。预览脚本的图片输出到忽略跟踪的 `tmp/ui-previews/`。
历史 DEMO 像素与生成脚本保留作诊断。

## 结构和后续范围

```text
main/
  app_main.cpp                高层启动入口、服务初始化和协调循环
components/
  app/                        急停产品应用组件
    battery_voltage/          电池电压采样与校准
    button_input/             BOOT 消抖、短长按和唤醒按键抑制
    power_manager/            静置计时、倒计时、休眠安全检查及进入/恢复
    emergency_remote/         急停触点、无线控制事务和数据快照
    emergency_ui/             页面、菜单、提示状态和显示刷新
    shell_command/            命令注册、参数解析和输出
    runtime_settings/         参数及常亮模式的 HXC_NVS 持久化
    boot_diagnostics/         固件、复位/唤醒、配置和电池启动快照
  bsp/
    hardware/                 引脚、电气状态、板级 GPIO 和 USB PHY
    sh1106/                   屏幕 I2C 驱动
```

- `components/bsp/sh1106`：独立 SH1106 页式帧传输驱动。
- `components/app/emergency_ui`：只拥有页面、菜单、故障提示和帧缓存；渲染资源位于 `private_include/`。不操作 ADC、GPIO、NVS 或休眠入口。
- `main/app_main.cpp`：编排服务初始化并运行唯一的协调循环，不通过独立运行模块间接调度。
- `components/app/emergency_remote`：急停事务、关闭重试、信道恢复和真实数据快照。
- 配对、协议、电量估算、日志和遥控链路等共用组件已统一迁移到
  [wireless-power-components](https://github.com/qingmeijiupiao/wireless-power-components)
  公共仓库，由 `main/idf_component.yml` 固定引用，原本地副本已移除。
- SH1106 仅供本工程使用，按要求保留本地。Lite 与 Pro V2 未修改。

模块采用 `include/` 公开接口、`src/` 实现和明确的 CMake 公开/私有依赖。电池采样资源归 `BatteryVoltage`，休眠时间与入口归 `PowerManager`，无线事务归 `EmergencyRemote`，菜单和显示状态归 `EmergencyUi`。这些服务由 `app_main` 的协调循环驱动；Shell 命令直接调用线程安全的服务请求，无线工作任务与急停 ISR 独立运行。

休眠入口通过显示回调协作，不依赖 UI 或 SH1106。进入顺序保持为通信静默确认、显示关闭、黑匣子同步、安全复查、释放 ADC、隔离 GPIO/USB、深睡；中止时恢复对应资源。周期采样和遥测不重置静置计时。

## 运行模式与待验证项

- 插电或用户选择常亮时常驻真实电压/电流主页；目前仅验证 USB 常驻场景。
- 非插电且输出关闭、状态可确认、没有控制事务时，按静置设置进入深睡；最后一分钟显示倒计时。USB 插电或输出开启时禁止深睡并保持显示。
- 保护和通信异常优先提示，开启失败不会自动重新开启。
- Lite / Pro V2 测量单位沿用 mV / uA，数据超时当前为 3 秒。
- 物理负载、真实短路、Lite 实物联调、深睡电流及唤醒仍需对应硬件回归。

驱动命令参考：[SH1106 V2.6 数据手册](https://www.displayfuture.com/Display/datasheet/controller/SH1106.pdf)。

## 共用组件验证

`battery_level`、`blackbox_service`、`espnow_remote`、`espnow_service_remote`、`espnow_link` 等遥控相关组件，以及 `HXC_NVS`、`circular_flash_buffer`、`Interp`、`blackbox`、`ADC`、`wifi_manager`、`shell`、`diagnostic_log` 等通用组件来自固定 Git 版本的公共仓库，位置见 `main/idf_component.yml`。原本地副本已移除。

```powershell
python scripts/idf_local.py -D ESTOP_VALIDATE_SHARED_REMOTE=ON build
python scripts/idf_local.py -D ESTOP_VALIDATE_SHARED_REMOTE=OFF build
```

第一条额外编译原按钮遥控组件；第二条恢复默认配置。两种配置均使用急停无线联调入口。
所有 `wireless-power-components` 组件统一固定到 `14662fc40ddc8ed5de18a347ef27d42f2ebdbbe1`，通过 Component Manager 自动下载，不依赖本地相邻目录。`main/idf_component.yml` 使用 YAML 锚点集中定义仓库地址与版本，升级时只需修改锚点处一处；`PWM` 未被本工程使用，已移除以减少下载。

## 静置、连接失败与参数 Shell

BOOT 短按打开菜单并切换，长按确认；确认默认取消。连接失败主页的短按改为重新连接，长按进入菜单。菜单 15 秒无操作返回主页。

开机/重新连接默认允许 10 秒尝试，失败后停止普通发送和信道恢复，关闭射频，屏幕提示“连接失败，短按重试”。只有用户操作才重新开始连接；即使之后对端开机也不会自动重试。如果输出可能开启、且急停关闭尚未确认，则保留关闭重试，窗口到期后按 `off_retry_ms` 降低频率，避免松开再按才能关闭；此时禁止深睡，屏幕保持点亮。

电池供电时，连接失败静置到期可深睡（即便没有取得关闭确认），待关闭意图保留到下次启动，重新连接先关闭；曾发送开启且尚未确认关闭的输出按“可能开启”处理，禁止深睡。插电也禁止深睡。禁止深睡时屏幕保持点亮，不使用单独熄屏或浅睡模式。没有定时唤醒，GPIO3 按键、GPIO5 触点变化和 GPIO4 插电可以唤醒。

正常联机且输出已确认关闭、没有待处理控制时，非长亮模式无操作默认 5 分钟自动深睡。菜单“休眠时间”提供 5m、10m、30m、1h，短按选择、长按保存到 NVS；正常与连接失败使用同一时长。自动深睡前显示完整 60 秒倒计时，按键操作重置静置时间；即使深睡阻止条件在超时后解除，也先给完整倒计时。插拔外部电源会重新开始静置计时。常亮设置保存在 NVS，手动休眠不会清除它；连接失败省电仍优先于常亮设置。

产品命令统一在 `components/app/shell_command` 注册，均为独立命令，不使用 `estop` 前缀。公共 `bsp/shell` 提供命令补全和历史记录；USB 串口按回车进入交互模式后支持以下整行命令（115200，回车执行）：

```text
version
config
set connect_ms 5000
set idle_ms 300000
set menu_idle_ms 15000
set release_ms 100
set debounce_ms 30
set long_ms 1000
set battery_ms 30000
set report_ms 30000
set low_mv 3500
battery status
battery reset-calibration
remote status
remote stop
remote on
remote retry
remote pair
remote repair
remote test-channel
gpio
fault
blackbox status
blackbox dump 50
blackbox clear
blackbox mark <text>
```

`config` 列出全部参数及有效范围，设置成功会打印 `SAVED` 并保存 NVS。超出范围、非法参数或保存失败不会更新运行值。`connect_ms` 为连接窗口；`idle_ms` 为统一静置休眠时间，范围 300000..3600000；其余 `_ms` 参数单位为毫秒。旧版本不足 5 分钟的已保存休眠时长升级后回退至默认 5 分钟。不存在运动传感器，“静置”依据 BOOT 输入、急停控制状态变化及用户串口操作；周期采样与数据刷新不重置静置计时。

电池 GPIO0 采样时才将 GPIO10 拉低，采样后恢复高阻。状态页显示估算电压、百分比和 USB 状态，顶部电池图标表示余量，低压附加警告。联机时每 30 秒通过原 0x0202 协议回传百分比，兼容 Lite/Pro V2；失败等待期间不回传、不重新连接。GPIO4 仅代表外部供电，不能据此证明电池正在充电。分压倍率默认 2.0（Q16 值 131072），USB 满电 4.2 V 自动校准并保存到 NVS，范围限制 1.8~2.2；电压和电量需实测校准，当前百分比为估算。

## 产品 UI（已接入）

采用已确认的左右布局：左侧四位数字与固定单位列，x=71 竖线，右侧显示实际输出/连接状态与电池。四个数字不包含正负号和小数点，跨量程时调整小数位（如 3.250、20.00、1176），超范围显示横杠。电池内部始终显示 SOC；USB 供电且电量不足 100% 时在电池左侧显示闪电，不覆盖百分比。当前硬件仅检测 VBUS，这个图案是外部供电推导的充电提示，并非独立充电状态检测；满电显示 100%。

正在关闭/开启显示沙漏与“待确认”，取得业务确认后才显示开关图案。失联显示断链及“未连接”。保护原因使用独立图案和简短说明，旧设备未提供原因时不推测原因。只有协议明确报告阶段才显示短路检测，不能通过固定延时假定已进入检测。

菜单显示前一项/当前项/后一项，当前项反白，短按选择、长按确认。确认默认取消，重新配对第二次确认明确写“删除配对”。常亮显示保存后的实际设置；休眠拒绝显示具体条件。

菜单包含返回主页、常亮模式、手动休眠、休眠时间、开始配对、重新配对、设备信息，没有“关闭输出”菜单项。设备信息短按循环电池电压与供电、固件版本与编译时间、黑匣子记录数/容量/待写队列、日志捕获/丢失/写入失败四页，长按返回主页。关键状态变化通过公共 `common/diagnostic_log` 的 `DEVICE_EVENT_I("ProductEvent", ...)` 标记输出，由黑匣子 hook 按白名单持久化；`ESP_LOGW` / `ESP_LOGE` 也由 hook 自动写入。初始化失败时信息页明确显示“未启用”。

关键事件包括启动/复位原因与深睡唤醒 GPIO、固件及编译信息、全部运行配置、常亮模式、已保存对端 MAC/信道、电池电压/SOC、USB 插拔、BOOT 短按/长按、菜单操作、急停触发/释放、开关请求/确认/拒绝、保护及连接状态变化、配对和设置变更。自动休眠倒计时开始/取消、休眠请求/拒绝/中止均有记录；深睡进入前同步写入 Flash。

ESP-NOW 提交失败、MAC 发送失败、ACK 超时、非法接收包和队列溢出通过链路计数检测；业务超时、请求失败、信道恢复及屏幕传输错误也有记录。周期遥测和屏幕刷新成功不写入黑匣子。Shell 命令 `blackbox status` 显示记录数、捕获/丢失/写入失败计数，`blackbox dump [count]` 同步并导出持久化文本事件。

连接失败短按用于重新尝试。保护页短按先确认提示、查看数据，长按可进入菜单，不重试开启。提示默认保留 30 秒，重复状态数据不会无限延长时间：

```text
set notice_ms 30000
fault
```

最后一次保护/拒绝原因保留在 RTC；深睡唤醒后显示历史原因时标记“上次原因”，与当前输出状态分开。只在实际原因变化时重新开始提示计时。低电量给一次提示并保留图标提醒，不抢占急停控制、不强制关闭或深睡。

资源生成入口 `python scripts/generate_product_ui.py` 使用 Windows 字体和 Pillow，从已确认预览生成 `product_pages.h`；CI 直接使用保存的位图资源，不依赖系统字体。主机测试验证数字进位、负数、页面绘制范围、待确认图案以及提示/唤醒策略。
