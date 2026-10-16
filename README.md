<div align="center">

<img src="docs/assets/img/drag.gif" alt="窗口被拖动，逐帧由 QuickDraw 引擎渲染" width="460">

# MosaiMac

**QuickDraw for ESP32-S31**

1984 年 Macintosh 的 1-bit 图形核心（BitBlt、Region、GrafPort），重写为可移植 C11，跑在 ESP-Mosaico 的 480×480 CO5300 上。

[![ESP32-S31](https://img.shields.io/badge/chip-ESP32--S31-ff5a1f?style=flat-square)](https://www.espressif.com/en/products/socs/esp32-s31)
[![tests](https://img.shields.io/badge/tests-3124_passing-5cb85c?style=flat-square)](test)
[![License](https://img.shields.io/badge/license-Apache--2.0-f1e8d8?style=flat-square)](LICENSE)

[项目主页](https://hkgood.github.io/MosaiMac/) · [设计决策与实测](docs/DESIGN.md) · [English](README.en.md)

</div>

---

## 构建

**主机** — 不需要硬件

```sh
make test     # 1509 + 1615 checks
make bench    # 吞吐测量
make asan     # ASan + UBSan
make sim      # 无头模拟器，60 步拖动逐像素校验
make sim-asan # 同上，ASan + UBSan + LeakSan
make render   # 重新生成静态截图
make replay   # 重新生成上面的拖动动画
```

**macOS 模拟器** — 零依赖，用系统自带 Cocoa

```sh
make app && open build/MosaiMac.app
```

拖窗口就能看。和固件共用 `src/*.c`，不是另写一份。

**ESP32-S31**

```sh
cd platform/esp32s31
. $IDF_PATH/export.sh
idf.py set-target esp32s31
idf.py build flash monitor
```

或者走 [esp-mosaico-vibe](https://github.com/esp-mosaico/esp-mosaico-vibe) 工作区（同目录的 [mosaico-film](https://github.com/hkgood/mosaico-film) 就是这么做的），能顺带拿到 `mosaico.py project sim` PC 模拟器。

板级初始化用 [esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp) 的 `bsp_display_new()` / `bsp_touch_new()`。

## 状态

| | |
|---|---|
| ROP / CopyBits / Region / GrafPort / 脏矩形回路 | ✅ 主机测试覆盖，3124 checks，ASan + UBSan 干净 |
| 窗口管理器（拖动 + 损伤回路） | ✅ 主机测试 + 模拟器逐像素校验 |
| macOS 模拟器 | ✅ `make app`，零依赖；本机无 macOS，未实际运行过 |
| ESP-IDF 平台层（CO5300 + CST9220 + 推送） | ✅ 编译通过 |
| `.irisfw` 打包 | ✅ **产出 bundle**，237.7 KB |
| 上机安装与验证 | ❌ 没有硬件 |

在 [esp-mosaico-vibe](https://github.com/esp-mosaico/esp-mosaico-vibe) 工作区里用 `mosaico.py iris system-update` 打出：

```
qd_esp32s31-system-update.irisfw    237.7 KB
  manifest.json         esp-iris-system-update/0.2
  partition-table.bin   4 KB
  ota_0.bin             466 KB
```

编译环境：ESP-IDF **v6.2.0**（master @ `7b9cc1ac`，和 workspace 钉的同一 commit），target `esp32s31`，16 MB flash。

**但没有硬件，所以没装过、没跑过。** CO5300 初始化时序、CST9220 坐标校准、偶数对齐窗口在真屏上的行为 —— 三样全部未验证。

这段动画不是录屏。`make sim-frames` 用桌面模拟器驱动一次真实的拖动，导出全部 74 帧，每一帧都是跑在固件同一份 `src/*.c` 上的输出，包括 480×480 面板真实的上下黑边。

## 三条设计要点

**内部 480×320，不缩放。** 480/320 = 1.5，512/342 ≈ 1.497，宽高比本来就一致，省掉整个重采样阶段。

**Region 用 span list。** 原版把 Region 塞进 16 字节做 packed RLE；S31 有 16 MB PSRAM，用内存换可验证性。矩形 Region 仍然零堆分配。

**脏的是区域不是矩形。** 窗口移动的包围盒是**整个窗口**（1 像素拖动也是 40,800 px / 82 KB），按 region 存并切 run 后只要 **818 px / 1.6 KB**。同一块硬件上这就是 46 fps 和拖得动的区别。

细节、实测数据和引擎踩过的坑都在 [docs/DESIGN.md](docs/DESIGN.md)。

## 目录

```text
include/qd/qd.h      公开 API
src/                 引擎（可移植 C11）
test/                主机测试
bench/               吞吐测量
sim/                 模拟器：核心 + headless + Cocoa 后端 + .app 打包
tools/               渲染器、字体、PPM→PNG、API 推送
platform/esp32s31/   ESP-IDF 工程
docs/                设计文档 + GitHub Pages
```

---

[Apache-2.0](LICENSE) · 2026 · Apple、Macintosh、QuickDraw 是 Apple Inc. 的商标，本项目与其无关联。QuickDraw 是根据可观察行为重新实现的，未使用任何 Apple 源码。
