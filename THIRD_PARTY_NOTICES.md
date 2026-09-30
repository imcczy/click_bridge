# 第三方来源与许可说明

本项目原创部分使用根目录 [LICENSE](LICENSE) 中的 MIT License。
以下说明保留参考项目的来源、原始版权和许可边界，不变更第三方权利。

## Zwift Click Handler

- 作者：Joshua Taillon（GitHub：`jat255`）。
- 仓库：[jat255/zwift_click_handling](https://github.com/jat255/zwift_click_handling)。
- 核对日期：2026-09-30。
- 核对版本：[`7cf896da3785d2971f5c7b489aeb810404d65cec`](https://github.com/jat255/zwift_click_handling/tree/7cf896da3785d2971f5c7b489aeb810404d65cec)。
- 参考内容：[GATT UUID](https://github.com/jat255/zwift_click_handling/blob/7cf896da3785d2971f5c7b489aeb810404d65cec/characteristics.py)、
  [消息类型及握手常量](https://github.com/jat255/zwift_click_handling/blob/7cf896da3785d2971f5c7b489aeb810404d65cec/constants.py)、
  [握手及按键通知处理](https://github.com/jat255/zwift_click_handling/blob/7cf896da3785d2971f5c7b489aeb810404d65cec/app.py)。

本项目将相关协议处理实现为 ESP32-C3 上的 C 固件，增加独立的 iPad BLE HID
链路、报告长度处理、双链路安全策略和输入边界检查；不包含上游 Python 运行时。
以下完整保留上游 [LICENSE](https://github.com/jat255/zwift_click_handling/blob/7cf896da3785d2971f5c7b489aeb810404d65cec/LICENSE)
中的 MIT 文本，适用于来自该项目的内容，不表示上游作者参与了本项目的全部开发。

```text
MIT License

Copyright (c) 2024 Joshua Taillon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

上游另外引用了 [ajchellew/zwiftplay](https://github.com/ajchellew/zwiftplay)
与 [Makinolo 的协议研究](https://www.makinolo.com/blog/2023/10/08/connecting-to-zwift-play-controllers/)。
这些研究来源也应获得致谢；此处的致谢不是将这些项目重新授权为 MIT。

## ESP-IDF / NimBLE / HID 示例

构建使用 ESP-IDF 6.1，本仓库不包含 SDK 源码副本。

- [ESP-IDF](https://github.com/espressif/esp-idf)：框架主要采用
  [Apache-2.0](https://github.com/espressif/esp-idf/blob/master/LICENSE)，
  各组件可能使用其他许可证，应以实际构建所用版本中的文件声明为准。
- [NimBLE Central 示例](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/nimble/blecent)：
  BLE central 的初始化和事件处理参考；本地 SDK 的 `main/main.c` 声明为
  Apache-2.0，不属于本项目 MIT 声明覆盖的上游内容。
- [ESP-IDF HID 示例](https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/esp_hid_device/main/esp_hid_device_main.c)：
  `main/hid_keyboard.c` 的键盘 Report Map 参考来源。本地 SDK 文件声明为
  `SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD`，
  许可证为 `Unlicense OR CC0-1.0`。

本地 SDK 中的 NimBLE `NOTICE` 原文如下：

```text
Apache Mynewt NimBLE
Copyright 2015-2022 The Apache Software Foundation
Modifications Copyright 2017-2022 Espressif Systems (Shanghai) CO., LTD.

This product includes software developed at
The Apache Software Foundation (http://www.apache.org/).

Portions of this software were developed at
Runtime Inc, copyright 2015.
```

MIT 许可仅覆盖有权按 MIT 授权的本项目内容，不替换 SDK/协议栈或其他依赖的
许可。分发固件或复制第三方实现时，应保留其适用的 LICENSE、NOTICE 和源文件
声明；本文件不是所有链接组件的完整许可清单，也不代替逐项检查。
