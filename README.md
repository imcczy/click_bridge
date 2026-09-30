# Click Bridge 1.0

让原版 Zwift Click V1 的两个按键通过 ESP32-C3 变成 iPad 蓝牙键盘输入，
用于 MyWhoosh 等支持相应键盘快捷键的应用。刷写完成后，C3 只需供电，
不需要电脑运行转发程序，也不需要 Wi-Fi 或互联网来完成按键桥接。

Click 通过 BLE 连接 C3；C3 同时作为 BLE HID 键盘连接 iPad。
iPad 配对的是桥接器 `Click Bridge V1`，不是 Click 本身。

| Click 按键 | 发送的键 | 本项目的目标用途 |
| --- | --- | --- |
| `+` | `K` | 加挡 |
| `-` | `I` | 减挡 |

普通文档里通常显示小写 `k/i`，本项目不发送 Shift。
实际升降挡效果取决于目标应用的快捷键及当前模式；文档输入测试只验证键盘链路。

目标环境：ESP-IDF 6.1、ESP32-C3、Click V1 固件 1.1.0、iPad。
蓝牙设备名为 `Click Bridge V1`，版本号为 `1.0.0`，维护分支为
`release/1.0`。

## 所需硬件

| 硬件 | 数量 | 说明 |
| --- | --- | --- |
| ESP32-C3 开发板 | 1 | 当前固件目标是 ESP32-C3，控制台使用原生 USB Serial/JTAG。 |
| Zwift Click V1 | 1 | 原版双按键型号，本项目验证所用固件为 1.1.0；需有可用电池。 |
| iPad | 1 | 开启蓝牙；准备可编辑文档，以及需要使用的骑行应用。 |
| USB 数据线和电源 | 1 套 | 刷写时连接电脑，日常可用开发板支持的 USB 电源供电。 |
| 开发电脑 | 1 | 构建和刷写时使用，需安装 ESP-IDF 6.1。 |

无需焊接，也无需把 Click 或 iPad 接到 C3 的 GPIO。BLE 承担两端通信，
USB 用于给 C3 供电、刷写及可选的故障排查。购买或使用其他 C3 板型时，
请确认其 USB 接口连接方式；仅带 USB-UART 转接芯片的板子不能直接使用
本项目配置的 USB Serial/JTAG 日志控制台。

目前只针对 Click V1；未验证 Click V2、Zwift Play、其他 ESP32 芯片或其他
主机平台。一次支持一个 Click 和一个 HID 主机；扫描按设备名匹配，不支持
在多个同名 Click 中指定一个。

## 构建和刷写

先安装并激活 ESP-IDF 6.1，再在本仓库根目录执行。
下面是当前 macOS 开发机的示例，激活脚本和串口路径需按自己的环境替换：

```sh
source "/Users/imcczy/.espressif/tools/activate_idf_v6.1.sh"
idf.py -B build/release-quiet \
  -D SDKCONFIG="$PWD/build/release-quiet/sdkconfig" \
  -D IDF_TARGET=esp32c3 build
```

首次构建时，独立的 `build/release-quiet/sdkconfig` 从版本控制中的
`sdkconfig.defaults` 生成，避免开发阶段留下的本地配置覆盖发布配置。输出为
`build/release-quiet/click_bridge.bin`，另有 bootloader、分区表和刷写参数。
以下命令只刷写，不启动串口 monitor：

```sh
idf.py -B build/release-quiet \
  -D SDKCONFIG="$PWD/build/release-quiet/sdkconfig" \
  -D IDF_TARGET=esp32c3 \
  -p /dev/cu.usbmodem83201 flash
```

## 使用方式

1. 给刷好固件的 C3 供电，确保 Click 没有被其他应用占用。
2. 按一下 Click 的任意按键将其唤醒，等待 C3 自动连接。
3. 在 iPad 的“设置 → 蓝牙”中连接 `Click Bridge V1`。桥接器在 Click
   链路就绪后才开始广播，因此刚上电时可能不会立即出现。
4. 打开备忘录或其他可编辑文档，切换英文输入并将光标置于正文中。
   分别短按 `+` 和 `-`，应能看到 `k` 和 `i`；自动大写或 Caps Lock
   可能影响显示大小写。长按时是否重复输入由主机处理。
5. 文档测试通过后，切回 MyWhoosh，在支持 `K/I` 快捷键的模式下验证升降挡。
   如果文档能输入而骑行应用不响应，应检查应用的焦点、模式和快捷键支持。

首次配对成功后，密钥保存在 NVS；后续刷写应用不会擦除该分区。
从旧的、未保存密钥的开发固件升级时，可能需要重新配对一次。
正常运行时不打印连接或按键日志，串口安静是预期行为，不代表桥接器未工作。

本项目是按键桥接器，不直接控制骑行台阻力，也不实现 FTMS 或 Zwift 的
完整虚拟变速协议。骑行台兼容性和变速行为由目标应用决定。

## 设计约束

- `main/click_bridge.c`：扫描、连接、发现 Click 服务、订阅通知和握手。
- `main/click_protocol.c`：不依赖 ESP-IDF 的 Click V1 报文解析。
- `main/hid_keyboard.c`：HID 服务、iPad 配对、广播和键盘报告。

两个连接共享一个 NimBLE host。C3 对 Click 是 central/GATT client，
对 iPad 是 peripheral/GATT server。状态只由 NimBLE host 事件队列访问，
无需额外任务、互斥锁或按键轮询。

Click 使用已验证的 `RideOn` 明文握手及 `0x37` 按键通知；只订阅按键和
握手响应，不读取电量或执行其他周期任务。收到相同按键状态不会重复发送。
Click 断线时释放 HID 按键，然后重新扫描。HID 尚未订阅或加密时，保留最新
按键状态，待链路准备好后发送。

**两条链路的安全要求不同。** `CONFIG_BT_NIMBLE_SM_LVL=2` 保证 SDK 的
HID 特征要求加密；运行时必须设置 `ble_hs_cfg.sm_sec_lvl=0`，否则 NimBLE
会在 `ble_att_svr_rx_notify()` 中丢弃 Click 的未加密通知。HID 发送前仍显式
检查 iPad 链路是否已加密。这里的 `0` 是由服务自行指定安全要求，不能换成
会拒绝配对请求的 `1`。

沿用实测的 Report Map、GATT 服务布局和启动顺序：Click 链路稳定后开启
HID 广播，收到按键通知也可触发广播。Report 模式发送 7 字节，Boot 模式
发送 8 字节。保留 SDK 生成的兼容性描述符；桥接器不提供电量服务，也不
宣称完整 HOGP 认证或对其他主机的兼容性。

省电目标是 **Click 的电池**。每次连接只做必要的服务发现、两项通知订阅
及一次 `RideOn` 握手；不发送心跳，不轮询按键或电量，不主动要求更短的
连接间隔，接受 Click 的连接参数更新请求。保持已验证的连接行为，不增加
定时断连或重连。已建立的 BLE 链路仍有连接事件，不能将“无业务轮询”等同于
Click 零耗电；未进行 Click 电流实测，不承诺具体续航改善。

C3 的扫描、广播和电源配置保持原样：扫描占空比为 10%，连接后停止扫描；
HID 广播间隔为 300 ms，连接后停止广播；控制器使用 modem sleep 和主晶振
低功耗时钟。本次没有新增 C3 降频或轻睡眠策略。

正式版的应用、bootloader 和 NimBLE 默认仅输出 WARN/ERROR；正常连接、
断开和按键不输出。应用控制台仅使用 USB Serial/JTAG，不再同时输出到 UART；
ROM 启动信息不在此设置范围内。需要排障时可调整日志配置恢复 INFO 日志。
HID 广播失败按 200 ms 起步、翻倍至最多 5 s 的间隔重试，连接成功后复位；
等待重试期间，Click 通知不会触发额外广播尝试。HID 广播/连接失败与报告发送
失败分别限流：首次立即记录，同一路径后续最多每 30 s 记录一次，无日志定时器。
这些改动减少无用输出和失败重试，并不直接降低 Click 正常连接时的耗电。

## 代码规范和检查

使用 C，采用 [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
中适用于 C 的规则，而不是将项目改写为 C++：

- 2 空格缩进、80 列、Google clang-format；函数和类型用 `PascalCase`，
  常量用 `kPascalCase`，变量和字段用 `snake_case`。
- 头文件自包含且有 include guard；模块内部符号为 `static`；接口说明
  初始化顺序、线程约束、返回值和状态语义。
- 保留 ESP-IDF/NimBLE 的外部名称及 `app_main` 入口名称。
- 使用 C 的 `struct`、`enum`、指定初始化器和 `_Static_assert`；整数常量
  表达式采用枚举，避免把 C++ 的 `constexpr` 规则机械套用到 C。
- 固件本地代码启用 `-Wall -Wextra -Werror`、原型检查。

```sh
make check
# 若 clang-format 不在 PATH，可以显式指定已安装的工具：
make check CLANG_FORMAT=/path/to/clang-format
```

`make check` 检查格式，使用宿主机 C 编译器、AddressSanitizer 和
UndefinedBehaviorSanitizer 运行协议测试。包括按下/释放、同时按键、部分
字段、未知 varint 字段、截断和整数溢出，以及 50,000 组生成输入的状态
完整性检查。需要支持上述 sanitizer 的 Clang 或 GCC。

`make format` 自动格式化源文件。修改 `sdkconfig.defaults` 后，应使用新的
独立配置文件验证，不能仅依赖可能仍含旧值的开发用 `sdkconfig`。
例如将上述构建命令中的两处 `build/release-quiet` 都换成尚未使用的
`build/release-check`；后续刷写也使用同一构建目录和配置路径。

## 验证范围

整理前的功能基线 `9c2b466` 已完成实机连接和 iPad 文档 `k/i` 输入测试。
1.0 整理版执行独立配置的 ESP32-C3 构建、格式检查和宿主机协议测试。
发布代码 `09df0b1` 已刷入开发板且刷写校验通过，未启动 monitor。
刷写成功不能替代功能回归；以下场景仍需在当前发布固件上确认：

1. iPad 文档中短按、长按、释放和同时按键。
2. C3 断电重启后利用已保存密钥重新连接 iPad。
3. 按住键时 Click 断线，iPad 不持续收到重复输入。
4. iPad 断开后重连、休眠后恢复，以及 MyWhoosh 中实际升降挡。

项目未修改 ESP-IDF 或 NimBLE 源码。升级 SDK 时，需重新检查通知安全过滤、
HID 服务描述符和双角色 GAP 回调的行为。

## 参考与致谢

Click 通信实现参考了 Joshua Taillon（[@jat255](https://github.com/jat255)）的
[Zwift Click Handler](https://github.com/jat255/zwift_click_handling)。
感谢该项目整理和演示 Click 的 GATT UUID、`RideOn` 明文握手及 `0x37`
按键通知解析：字段 `1/2` 对应 `+/-`，值 `0/1` 对应按下/释放。

本项目据此在 ESP-IDF/NimBLE 上实现 C 固件，并新增面向 iPad 的 BLE HID
链路；不依赖其 Python 程序、Bleak 或桌面键盘模拟库。主要对应实现为
`main/click_bridge.c` 和 `main/click_protocol.c`。

上游 README 还致谢了 [ajchellew/zwiftplay](https://github.com/ajchellew/zwiftplay)
和 [Makinolo 的协议研究](https://www.makinolo.com/blog/2023/10/08/connecting-to-zwift-play-controllers/)。
这里一并保留研究来源的致谢，不将协议逆向成果归为本项目原创。
蓝牙双角色及 HID 实现也参考了 Espressif 的 NimBLE Central 和 HID 示例。
具体引用版本、来源和第三方许可边界见 [第三方说明](THIRD_PARTY_NOTICES.md)。

## 许可证

本项目的原创代码与文档采用 [MIT License](LICENSE)，与
[Zwift Click Handler 的许可证](https://github.com/jat255/zwift_click_handling/blob/7cf896da3785d2971f5c7b489aeb810404d65cec/LICENSE)
相同。MIT 允许使用、修改、再发布和商业使用；复制或分发上游软件及其实质性
部分时，必须保留原版权声明和完整许可声明，不能仅保留仓库链接。
上游的原始 MIT 文本与 `Copyright (c) 2024 Joshua Taillon` 已保存在
[第三方说明](THIRD_PARTY_NOTICES.md)，本项目自己的版权声明不会替代它。

ESP-IDF、NimBLE 及其他第三方组件仍遵循其各自许可证，不因本仓库新增 MIT
而被重新授权。发布包含这些组件的固件时，须另行随附适用的许可及 NOTICE；
本仓库的第三方说明不是整个 SDK 的完整依赖许可清单。

本项目为非官方项目，与 Zwift、MyWhoosh、Apple 或 Espressif 无隶属或背书
关系。产品名称仅用于描述兼容目标，相关商标属于各自权利人。软件按现状提供，
无任何保证；开源许可不等于对第三方商标、专利或服务条款的额外授权。
