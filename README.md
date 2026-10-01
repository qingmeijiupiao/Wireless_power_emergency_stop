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

OLED 使用 I2C 100 kHz，自动探测 0x3C/0x3D，列偏移 2。
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

Pro V2 Shell 执行 `espnow pair 120`，随后向急停板 USB 串口发送单字符 `r`。
两端配对成功后退出配对模式，急停板先确认关闭，再开始每秒请求电压、电流和输出状态。

| 急停板命令 | 行为 |
|---|---|
| r | 主动配对，不清除已有记录 |
| s | 打印远端状态、电压、电流和保护掩码 |
| f | 请求关闭并保持重试，不产生自动开启 |
| o | 诊断开启请求；物理触点闭合时忽略 |
| m / v | 返回真实状态和仪表页面 |
| h | GPIO 和电池 ADC 快照 |
| c | 临时切到错误信道 6，用于恢复测试 |
| 0–9 / t | DEMO 状态页 / 边框页传输诊断，结束后发送 m |

串口 115200，无需回车。物理急停控制在诊断显示期间仍有效，闭合与关闭处理中会覆盖诊断页面。
`FRAME_OK` 只确认 I2C 帧传输；视觉效果仍须在实物上确认。

## 真实 UI 和可靠性

仪表页显示真实电压、电流和计算功率；数据超过 3 秒未更新则显示离线和横线。
已确认开启、已确认关闭、关闭处理中、开启处理中、通信中断、未配对采用大图标提示。
收到 Pro V2 详细反馈时显示短路、OTP / OVP / UVP / OCP、冷却、忙碌、检测异常等原因。
原协议只返回通用拒绝时显示“开启拒绝 / 原因未提供”，不推测短路或保护。

关闭必须收到业务成功且实际输出为关闭才解除待确认状态。1 秒未确认会重试，并触发加密信道恢复；通信恢复后不需要再次按按钮。
开启只处理一次稳定释放，拒绝后不会反复尝试开启；新的急停闭合可抢占待开启事务。
当前没有通信断开后由 Pro V2 自主执行的看门狗关闭协议，链路中断期间仍只能等待关闭送达。

验证记录见 [Pro V2 联调记录](docs/prov2-integration.md)，协议见 [详细响应](docs/switch-detail-protocol.md)。
正式像素资源由 `scripts/generate_product_ui.py` 生成，正常编译无需图像库；当前布局说明见 [产品 UI](docs/ui-design.md)。预览脚本的图片输出到忽略跟踪的 `tmp/ui-previews/`。
历史 DEMO 像素与生成脚本保留作诊断。

## 结构和后续范围

```text
main/                         高层启动入口
components/
  app/                        急停产品应用组件
    app_runtime/              启动上下文、诊断和休眠收尾工具
    battery_voltage/          电池电压采样与校准
    button_input/             按键事务与操作反馈
    power_manager/            唤醒来源和深度休眠管理
    emergency_ui/           产品页面、菜单、休眠策略和 USB 诊断
    shell_command/            维护命令注册
    status_led/               状态反馈
  bsp/                        板级专用驱动（sh1106、Temperature）
```

- `components/bsp/sh1106`：独立 SH1106 页式帧传输驱动。
- `components/app/emergency_ui`：产品页面、菜单、休眠策略和 USB 诊断。
- `main/app_main.cpp`：进入急停产品应用入口。
- `components/app/emergency_remote`：急停事务、关闭重试、信道恢复和真实数据快照。
- 配对、协议、电量估算、日志和遥控链路等共用组件已统一迁移到
  [wireless-power-components](https://github.com/qingmeijiupiao/wireless-power-components)
  公共仓库，由 `main/idf_component.yml` 固定引用，原本地副本已移除。
- SH1106 仅供本工程使用，按要求保留本地。Lite 与 Pro V2 未修改。

后续：物理负载与真实短路测试、Lite 实物联调、深度休眠与常亮模式，以及硬件上电与电池策略。

## 后续运行模式

- 插电或用户选择常亮时常驻真实电压/电流主页；目前仅验证 USB 常驻场景。
- 日常深度休眠及休眠唤醒后的输入处理尚未接入当前入口。
- 保护和通信异常优先提示，开启失败不会自动重新开启。
- Lite / Pro V2 测量单位沿用 mV / uA，数据超时当前为 3 秒。

驱动命令参考：[SH1106 V2.6 数据手册](https://www.displayfuture.com/Display/datasheet/controller/SH1106.pdf)。

## 共用组件验证

`battery_level`、`blackbox_service`、`espnow_remote`、`espnow_service_remote`、`espnow_link` 等遥控相关组件，以及 `HXC_NVS`、`PWM`、`circular_flash_buffer`、`Interp`、`blackbox`、`ADC`、`wifi_manager`、`shell`、`diagnostic_log` 等通用组件来自固定 Git 版本的公共仓库，位置见 `main/idf_component.yml`。原本地副本已移除。

```powershell
python scripts/idf_local.py -D ESTOP_VALIDATE_SHARED_REMOTE=ON build
python scripts/idf_local.py -D ESTOP_VALIDATE_SHARED_REMOTE=OFF build
```

第一条额外编译原按钮遥控组件；第二条恢复默认配置。两种配置均使用急停无线联调入口。
遥控相关组件固定到 `15525b7a2d3cc0694bbc0b65dcac8335cd73d454`，其余通用组件固定到 `79d506e686ec743ad961ab76c732af96313db54a`，通过 Component Manager 自动下载，不依赖本地相邻目录。

## 静置、连接失败与参数 Shell

BOOT 短按打开菜单并切换，长按确认；确认默认取消。连接失败主页的短按改为重新连接，长按进入菜单。菜单 15 秒无操作返回主页。

开机/重新连接默认允许 10 秒尝试，失败后停止发送和信道恢复，关闭射频。屏幕提示“连接失败，短按按键重试”，默认等待 180 秒。只有用户操作才重新开始连接；即使之后对端开机也不会自动重试。上述停止重试用于开机连接失败或普通断线；如果已知输出可能开启、且急停关闭尚未确认，则保留关闭重试，窗口到期后按 `off_retry_ms` 降低频率，避免松开再按才能关闭。此时仍禁止深睡，静置到期只熄屏。

电池供电时，连接失败静置到期可深睡（即便没有取得关闭确认），待关闭意图保留到下次启动，重新连接先关闭；曾发送开启且尚未确认关闭的输出按“可能开启”处理，禁止深睡。插电也禁止深睡，这两种阻止条件下，失败提示到期只关闭 OLED。没有定时唤醒，GPIO3 按键、GPIO5 触点变化和 GPIO4 插电可以唤醒。

正常联机且输出已确认关闭、没有待处理控制时，非长亮模式静置 15 秒自动深睡。电池供电、非长亮模式下，如果因为输出开启等原因不能深睡，则静置到期只关闭 OLED，控制任务继续工作，BOOT 或急停操作会恢复显示。菜单、故障提示等临时页面不无限延长静置时间。常亮设置保存在 NVS，手动休眠不会清除它；连接失败超时省电优先于常亮设置。

USB 串口支持以下整行命令（115200，回车执行，命令前无需单字符切换）：

```text
estop config
estop set connect_ms 5000
estop set fail_idle_ms 180000
estop set idle_ms 15000
estop set menu_idle_ms 15000
estop set release_ms 100
estop set debounce_ms 30
estop set long_ms 1000
estop set battery_ms 30000
estop set report_ms 30000
estop set low_mv 3500
estop battery
estop status
estop gpio
estop retry
```

`estop config` 列出全部参数及有效范围，设置成功会打印 `SAVED` 并保存 NVS。超出范围、非法参数或保存失败不会更新运行值。`connect_ms` 为连接窗口；`fail_idle_ms` 为失败等待时间；其余 `_ms` 参数单位为毫秒。不存在运动传感器，“静置”依据 BOOT 输入、急停控制状态变化及用户串口操作；周期采样与数据刷新不重置静置计时。

电池 GPIO0 采样时才将 GPIO10 拉低，采样后恢复高阻。状态页显示估算电压、百分比和 USB 状态，顶部电池图标表示余量，低压附加警告。联机时每 30 秒通过原 0x0202 协议回传百分比，兼容 Lite/Pro V2；失败等待期间不回传、不重新连接。GPIO4 仅代表外部供电，不能据此证明电池正在充电。`bat_gain_ppm` 可调整电压比例，默认 1000000；电压和电量需实测校准，当前百分比为估算。

## 产品 UI（已接入）

采用已确认的左右布局：左侧四位数字与固定单位列，x=71 竖线，右侧显示实际输出/连接状态与电池。四个数字不包含正负号和小数点，跨量程时调整小数位（如 3.250、20.00、1176），超范围显示横杠。电池内部显示 SOC；USB 供电且电量不足 100% 时显示闪电提示。当前硬件仅检测 VBUS，这个图案是外部供电推导的充电提示，并非独立充电状态检测；满电显示 100%。

正在关闭/开启显示沙漏与“待确认”，取得业务确认后才显示开关图案。失联显示断链及“未连接”。保护原因使用独立图案和简短说明，旧设备未提供原因时不推测原因。只有协议明确报告阶段才显示短路检测，不能通过固定延时假定已进入检测。

菜单显示前一项/当前项/后一项，当前项反白，短按选择、长按确认。确认默认取消，重新配对第二次确认明确写“删除配对”。常亮显示保存后的实际设置；休眠拒绝显示具体条件。

正常熄屏后第一次 BOOT 操作只点亮主页，不进入菜单、不发送开启请求；连接失败短按仍用于重新尝试。保护页短按先确认提示、查看数据，长按可进入菜单，不重试开启。提示默认保留 30 秒，重复状态数据不会无限延长时间：

```text
estop set notice_ms 30000
estop last_fault
```

最后一次保护/拒绝原因保留在 RTC；深睡唤醒后显示历史原因时标记“上次原因”，与当前输出状态分开。只在实际原因变化时重新开始提示计时。低电量给一次提示并保留图标提醒，不抢占急停控制、不强制关闭或深睡。

资源生成入口 `python scripts/generate_product_ui.py` 使用 Windows 字体和 Pillow，从已确认预览生成 `product_pages.h`；CI 直接使用保存的位图资源，不依赖系统字体。主机测试验证数字进位、负数、页面绘制范围、待确认图案以及提示/唤醒策略。
