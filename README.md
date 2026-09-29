# Click Bridge 1.0

ESP32-C3 将 Zwift Click V1 的两个按键转换成 iPad 蓝牙键盘输入：
`+ → K`，`- → I`。普通文档里默认显示小写 `k/i`，不发送 Shift。

目标环境：ESP-IDF 6.1、ESP32-C3、Click V1 固件 1.1.0、iPad。
蓝牙设备名为 `Click Bridge V1`，版本号为 `1.0.0`，维护分支为
`release/1.0`。

## 构建和刷写

在本仓库目录执行。下面的激活脚本和串口路径对应当前开发机：

```sh
source "/Users/imcczy/.espressif/tools/activate_idf_v6.1.sh"
idf.py -B build/release \
  -D SDKCONFIG="$PWD/build/release/sdkconfig" \
  -D IDF_TARGET=esp32c3 build
```

独立的 `build/release/sdkconfig` 从版本控制中的 `sdkconfig.defaults`
生成，避免开发阶段留下的本地配置覆盖发布配置。输出为
`build/release/click_bridge.bin`，另有 bootloader、分区表和刷写参数。

```sh
idf.py -B build/release \
  -D SDKCONFIG="$PWD/build/release/sdkconfig" \
  -D IDF_TARGET=esp32c3 \
  -p /dev/cu.usbmodem83201 flash monitor
```

先让 C3 连接 Click，再在 iPad 蓝牙设置中连接 `Click Bridge V1`。
首次配对成功后，密钥保存在 NVS；后续刷写应用不会擦除该分区。
从旧的、未保存密钥的开发固件升级时，可能需要重新配对一次。

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

保持控制器 modem sleep 和主晶振低功耗时钟。扫描占空比为 10%，连接后
停止扫描；HID 广播间隔为 300 ms，连接后停止广播。正式版保留连接和错误
日志，关闭 NimBLE 的逐包 INFO 日志，不打印每次按键。未进行电流实测，
不据此承诺具体续航。

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

## 验证范围

整理前的功能基线 `9c2b466` 已完成实机连接和 iPad 文档 `k/i` 输入测试。
1.0 整理版执行独立配置的 ESP32-C3 构建、格式检查和宿主机协议测试。
这些检查不能替代整理版的实机回归；以下场景仍需在刷入整理版后确认：

1. iPad 文档中短按、长按、释放和同时按键。
2. C3 断电重启后利用已保存密钥重新连接 iPad。
3. 按住键时 Click 断线，iPad 不持续收到重复输入。
4. iPad 断开后重连、休眠后恢复，以及 MyWhoosh 中实际升降挡。

项目未修改 ESP-IDF 或 NimBLE 源码。升级 SDK 时，需重新检查通知安全过滤、
HID 服务描述符和双角色 GAP 回调的行为。
