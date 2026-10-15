<div align="center">

<img src="docs/assets/img/workspace.png" alt="MosaiMac — 用 QuickDraw 引擎渲染的 480×320 桌面" width="480">

# MosaiMac

**QuickDraw for ESP32-S31**

把 1984 年那套 1-bit 图形系统，塞进一块 480×480 的 AMOLED。

[![项目主页](https://img.shields.io/badge/项目主页-GitHub_Pages-ff9a3c?style=for-the-badge)](https://hkgood.github.io/MosaiMac/)
[![ESP32-S31](https://img.shields.io/badge/chip-ESP32--S31-ff5a1f?style=flat-square)](https://www.espressif.com/en/products/socs/esp32-s31)
[![ESP-Mosaico](https://img.shields.io/badge/board-ESP--Mosaico-ff5a1f?style=flat-square)](https://www.espressif.com)
[![1-bit](https://img.shields.io/badge/framebuffer-1bpp-1f1f1f?style=flat-square)](include/qd/qd.h)
[![tests](https://img.shields.io/badge/tests-3124_passing-5cb85c?style=flat-square)](test)
[![License](https://img.shields.io/badge/license-Apache--2.0-f1e8d8?style=flat-square)](LICENSE)

**中文** · [English](README.en.md)

</div>

---

<p align="center">
  <img src="docs/assets/img/workspace.png" alt="菜单栏、下拉菜单、文档窗口、硬阴影、反白选区、滚动条、桌面图标 — 全部由本仓库的 ROP、Region 和 GrafPort 代码绘制，没有一行 SVG 或 CSS 参与" width="480">
</p>

上面这张图**不是设计稿**。它由本仓库的渲染器用**同一套** ROP / Region / GrafPort 代码画出来 —— 和跑在 ESP32-S31 上的是同一份 `src/*.c`。如果 span XOR 算错了，图上会出现拖影；事实上这张图就是引擎正确性的一个可视证明。

| | |
| :-: | :-: |
| **1 bit/pixel** · 18.75 KB 帧缓冲 | **480×320** 逻辑屏幕，嵌在 480×480 面板中央 |
| **Region 裁剪** · Span list | **脏矩形**按 region 累积，不按矩形 |

---

## 这为什么不是「在单片机上画点东西」

1984 年的 Macintosh 用 68000 @ 7.836 MHz、**128 KB 总内存**，跑出了一个完整的窗口系统：可拖动的窗口、区域裁剪、脏矩形重绘、滚动条、下拉菜单。那套系统的核心就是 BitBlt 和 Region，而它们今天依然是你在 480×480 屏幕上做「经典 Mac 风格」界面时唯一正确的选择 —— 因为**1-bit 是唯一一种你能在没有 GPU、没有 DMA 引擎、没有 alpha 的设备上，把整屏重绘压进 16 ms 的位深**。

MosaiMac 把那个核心重新实现了一遍。

## 三个决定

**内部 480×320，不缩放 512×342。**
原版屏幕是 512×342。这里是 480×480。`480/320 = 1.5`，`512/342 ≈ 1.497` —— 宽高比本来就是一致的。所以直接用 480×320，省掉整个重采样阶段。代价是行距 60 字节，让每行都 16 字节不对齐；这个代价值得付。

**Region 用 span list，不用 1984 年的 packed RLE。**
原版把 Region 塞进 `Region` record 允许的 16 字节里，每个集合运算都是手写的位运算。S31 有 16 MB PSRAM，用内存换可验证性是白赚的。矩形 Region 原地存储、零堆分配，`qd_region_is_rect()` 为真时 ROP 直接跳过逐扫描线遍历。

**脏的是区域，不是矩形。**
这是最重要的一条，下面单独说。

## 脏矩形：1 像素拖动 vs 40,800 像素

窗口移动损伤的是新旧位置的对称差。**如果把这个区域存成矩形**（取包围盒），哪怕只拖 1 像素，包围盒也是**整个窗口** —— 40,800 px，每帧 82 KB。按 QSPI 的 14–28 MB/s 算，那是 46 fps。

**存成 region 并按 scanline 切 run**，1 像素移动只推 **818 px，1.6 KB**：

| 移动 | 推送量 |
|---|---|
| 40 px 斜向 | 25,600 px（51 KB） |
| 1 px 斜向 | **818 px（1.6 KB）** |
| 无操作 | 0 px |

`qd_bsp_flush()` 会把竖直方向相同的 run 合并成极大矩形，所以一次窗口移动的损伤会塌缩成大约 4 个矩形，而不是每条边每行两个。同一块硬件上，这就是 46 fps 和「拖得动」之间的全部差别。

`test/test_window.c` 在没有硬件的情况下验证了整条回路：拖动窗口、每一步之后断言窗口外**没有任何残留黑像素**（那就是漏掉的损伤），断言标题栏和边框确实画出来了，断言被跨过的下层窗口完好。

## 实测数据

主机：x86-64、GCC 12、`-O2`。S31 会更慢 —— **有参考价值的是比例，不是绝对值。**

```
CopyBits 全屏 480×320
  srcXor   对齐        0.007 ms      22,000 Mpixel/s
  srcXor   非对齐      0.047 ms       3,270 Mpixel/s
  srcXor   L 形 region  0.014 ms      10,600 Mpixel/s
  200×150 窗口移动     0.011 ms      90,600 ops/s

1bpp → RGB565
  整帧                 0.150 ms       1,030 Mpixel/s
  120×90 脏矩形        0.011 ms

面板开销（模型估算，不是实测）
  @ 40 MHz QSPI: 整帧 21.9 ms (46 fps) | 120×90 脏矩形 1.54 ms (648 fps)
  @ 80 MHz QSPI: 整帧 11.0 ms (91 fps) | 120×90 脏矩形 0.77 ms (1296 fps)
```

**1bpp 每字节 8 像素**，所以整屏 XOR 只有 18.75 KB，是微秒级的事。

两处优化是被实测数据逼出来的，不是提前设计出来的：

- **非对齐 BitBlt 曾经比对齐慢 14 倍**（走逐字节分块路径）。拖窗口天生非对齐，而且是 Mac 式 UI 里最频繁的 BitBlt，所以不能忍。改成把源数据过 64 位窗口滑动：**0.104 → 0.047 ms**。
- **`qd_region_spans()` 原本每次查询是 O(y)** —— 要把前面所有行的 span 数累加一遍。整屏 region 裁剪因此是**平方复杂度**。换成前缀和 offset 表：**0.040 → 0.014 ms**。

## 引擎里踩过的坑

都记在 README.en.md 和代码注释里，因为每一个在常见路径上都看不见：

- **`rop_byte_chunk()` 少了一次按目标位相位的移位。** 只在 `dp == 0` 时正确 —— 也就是**所有非对齐 span 全错**。整字节路径把它盖住了。
- **`qd_fill_span_1bpp()` 的部分字节掩码忽略相位。** 只在 head/tail 被裁剪时错，第一次暴露是在 `frameRect` 一个 3 px 边框被裁成 3 px 的时候。
- **XOR 结果被截断到操作数 bounds 的交集。** 但只有 INTERSECT 会被交集框住，XOR 和 UNION 都会伸出去。
- **`spans_xor` 会丢 span。** 头尾合并和双游标都不对，因为一个 span 可能压住另一个列表的*下一个* span。改成对两组边界并集做事件扫描。
- **`qd_port_set_white()` 存在但 `qd_fill_rect()` 完全不理它。** API 摆在那儿不生效，调用方设了白笔期待擦除、实际得到黑色，而且**没有任何办法察觉**。
- **空 Region 是个哨兵指针** `(int32_t*)1`，忘了初始化会变成堆损坏而不是一个空 region。

护栏都在：`test_qd.c` 里 16×16 相位组合扫描，`test_qd.c` 里 fill span 穷举 16,512 个对参考模型，`test_qd.c` 里专门验笔的用例。ASan + UBSan 干净。

## 目录

```text
include/qd/qd.h      全部公开 API
src/                 引擎：ROP / Region / GrafPort（可移植 C11）
test/                主机端正确性测试，1509 + 1615 checks
bench/               主机端吞吐测量
tools/               渲染器、字体、PPM→PNG、API 推送
platform/esp32s31/   ESP-IDF 工程：CO5300 + CST9217 + 推送路径
docs/                GitHub Pages 与图片
```

核心是可移植的 C11，没有平台依赖。主机上构建测试，交叉编译到 S31，同一份 `src/*.c`。

### 位图约定

- 1 bit/pixel，**MSB-first**，行首像素是 bit 7
- `rowBytes` 是**行距，包含行尾 slack**。这是最容易错的地方：引擎靠它找下一行，所以必须包含 slack；`QD_SCREEN_ROWBYTES`（60）是有效载荷宽度，位运算基于它。把 `rowBytes` 设成有效载荷宽度、却按行距分配内存，会静默地把图像剪切成斜的 —— 写的人和读的人对「第 N+1 行从哪开始」意见不一致
- 每行需要 `QD_ROW_SLACK`（8）字节可读余量，成本 8 B × 高度

## 构建

**主机（测试和基准，不需要硬件）**

```sh
make test     # 1509 + 1615 checks
make bench    # 吞吐测量
make asan     # ASan + UBSan
make render   # 重新生成 docs/assets/img/workspace.png
```

**ESP32-S31**

推荐走 [esp-mosaico-vibe](https://github.com/esp-mosaico/esp-mosaico-vibe) 工作区，同目录下的 [mosaico-film](https://github.com/hkgood/mosaico-film) 就是这么做的，能顺带拿到工作区固定的 IDF 版本和 `mosaico.py project sim` PC 模拟器：

```sh
git clone https://github.com/esp-mosaico/esp-mosaico-vibe.git
cd esp-mosaico-vibe
git submodule update --init \
  submodule/esp-mosaico-utils submodule/esp-mosaico-bsp
git clone https://github.com/hkgood/MosaiMac.git projects/qd_esp32s31
```

或者独立 ESP-IDF 工程：

```sh
cd platform/esp32s31
. $IDF_PATH/export.sh
idf.py set-target esp32s31
idf.py build flash monitor
```

板级初始化交给 [esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp)（`bsp_display_new()` / `bsp_touch_new()`），不自己猜 CO5300 和 CST9217 的引脚。

## 当前状态

| 部分 | 状态 |
|---|---|
| ROP / CopyBits / fill / frameRect | ✅ 完成，主机测试覆盖 |
| Region：union / intersect / xor / offset / invert | ✅ 完成，主机测试覆盖 |
| GrafPort：clip、origin、Save/RestoreGState、矩形 + region 裁剪 | ✅ 完成，主机测试覆盖 |
| 窗口移动脏矩形回路 | ✅ 完成，主机测试覆盖 |
| 1bpp → RGB565 展开、分带推送 | ✅ 完成，主机测试覆盖 |
| ESP-IDF / ESP32-S31 移植 | ⚠️ **写完了，从未编译过** |

核心在主机上过 **3124 个检查**，ASan + UBSan 干净。平台层是经过 review 的骨架 —— 没有 ESP-IDF 工具链可编译它。**「能编译」是第一个待解决的问题，不是「能用」。**

## 备选路线，以及为什么没走

**Linux on S31。** 首批产品不合适。S31 没有 MIPI DSI，QSPI 屏要自己写 Linux SPI panel 驱动，而 Espressif 官方的 Linux BSP 面向的是无屏的 Function-CoreBoard。产物是一个 fbdev 控制台，不如一个真正的 1-bit 窗口系统有用。

**`systemless`。** 一个活跃的 Rust 经典 Macintosh 运行时，是我找到的**最接近的先例**。GPL-3.0-or-later，而且 Cranelift JIT 那条线目前主要是 x86-64，对 S31 不是直接路径。兼容层值得研究，图形引擎不值得抄。

**Apple 的 QuickDraw 源码。** 没有使用，也正是本仓库在许可边界确定前默认私有的原因。**接口**是重新实现的，**没有一行代码是抄的**。

---

[Apache License 2.0](LICENSE) · 2026

Apple、Macintosh 和 QuickDraw 是 Apple Inc. 的商标。本项目独立开发，与 Apple 无关联或背书关系。
