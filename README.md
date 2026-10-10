# TI_codes — 2026 TI 杯赛道一 · 波形识别

2026 年第十六届 TI 杯数字系统创新大赛赛道一（初赛）相关代码与文档。

单片机采集信号发生器输出的周期信号，识别**波形类别、频率、峰峰值**（方波另报**占空比**），
按协议经串口上报给 PC。

```
RESULT,<type>,<frequency>,<Vpp>,<duty>
```

例：`RESULT,square,223.400,1.560,68.200`

---

## 一、目录结构

```
TI_codes/
├── ti_final/          ★ 交付版本：完整、可直接编译烧录
├── ti_wave/           ★ 调试工作区：CCS 工程 + 采集脚本 + 实测数据
├── docs/
│   └── 算法开发接口文档.md
├── ti_cup/            早期版本（仅 main.c，存档）
│
├── uart_report.c/.h   通信模块源文件（与 ti_final/firmware/ 下同名文件一致）
├── main_template.c    主程序模板（ti_final/firmware/main.c 的前身）
├── pc_receive.c       PC 端接收程序（接 ti_wave 的原始数据流）
├── pc_receiver.c      PC 端接收程序（接 ti_final 的结果帧）
├── test_*.c           离线自测用例
│
├── 交接文档_PC端对接.md      PC 端对接说明
├── 交接要点.md               交接概要
├── 串口协议_发给PC端同学.md   协议速查
├── 协议差异与改进建议.md      协议对比分析
└── 操作指南_单片机端.md       单片机端操作说明
```

> 根目录的 `uart_report.c/.h` 与 `ti_final/firmware/` 下的同名文件内容完全一致
> （md5 相同），是调试期的副本。

---

## 二、`ti_final/` 与 `ti_wave/` 的关系

这两个目录记录的是**同一个固件工程的两个阶段**，不是两份独立代码。

| | `ti_final/` | `ti_wave/` |
|---|---|---|
| 定位 | 交付版本 | 调试工作区 |
| 源码 | ✅ 完整（`firmware/` 下齐全） | ❌ 不含 `main.c` / `wave_algo.c` / `ti_wave.syscfg` |
| 内容 | 固件源码、PC 端程序、测试用例、烧录指南 | CCS 工程配置、采集/校验脚本、实测波形数据 |
| 能否编译 | ✅ 是 | ❌ 否，需 `../` 父目录源码 |

### 为什么 `ti_wave/` 没有源码

`ti_wave/.cproject` 和 `ti_wave/Debug/subdir_vars.mk` 引用的源文件路径是 `../main.c`、
`../wave_algo.c`、`../uart_report.c`、`../ti_wave.syscfg`——都指向**父目录**。
工程最初建在 `TI_codes/` 根目录，后来源码整批迁进 `ti_final/firmware/`，而 CCS
工作区目录 `ti_wave/` 与旧构建产物 `Debug/` 保留了下来。

因此单独克隆 `ti_wave/` **无法编译**。要构建请用 `ti_final/firmware/`，
见 [`ti_final/README.md`](ti_final/README.md) 和
[`ti_final/CCS烧录操作指南.md`](ti_final/CCS烧录操作指南.md)。

### 两边的配置已分叉

`ti_wave/Debug/ti_msp_dl_config.h` 与 `ti_final/firmware/ti_msp_dl_config.h`
内容不同（前者 13 个外设实例引用，后者 12 个）。**以 `ti_final` 为准**——
那才是交付版本对应的配置，`ti_wave/Debug/` 下的是旧构建残留。

`ti_wave/` 里保留且有价值的内容：

- `capture_wave.js` / `validate_wave.js` / `flash_wave.js` — CCS 自动化脚本
- `log_wave.ps1` — 串口日志采集
- `adc_capture.json` / `algorithm_validation.json` — 实测采集与算法校验数据
- `live_uart.csv` / `verification_uart.csv` / `wave_*.csv` — 实测波形记录
- `UART_FIX.md` — UART 问题排查记录

---

## 三、从哪里开始

| 你想做的事 | 看这里 |
|---|---|
| 编译、烧录、看串口输出 | [`ti_final/CCS烧录操作指南.md`](ti_final/CCS烧录操作指南.md) |
| 了解固件结构、算法、协议、踩坑点 | [`ti_final/README.md`](ti_final/README.md) |
| 对接 PC 端 | [`交接文档_PC端对接.md`](交接文档_PC端对接.md)、[`串口协议_发给PC端同学.md`](串口协议_发给PC端同学.md) |
| 单片机端操作 | [`操作指南_单片机端.md`](操作指南_单片机端.md) |
| 算法接口约定 | [`docs/算法开发接口文档.md`](docs/算法开发接口文档.md) |

**硬件接入**：信号 → **PA25**（板上丝印 `PA25/A0_2`），地 → **GND**，**必须共地**。
信号须为 0~3.3 V、无负压；超范围**必须**先加偏置/衰减，否则损坏 ADC 引脚。

**关键参数**（三处已统一，改动需同步）：

| 参数 | 值 | 定义位置 |
|---|---|---|
| 采样率 | 10 kHz | `wave_algo.h` 的 `WAVE_FS_HZ`、`ti_wave.syscfg` 的 TIMG0 周期 |
| 采样点数 | 10000（1 秒） | `wave_algo.h` 的 `WAVE_NSAMP` |
| ADC 参考电压 | 3.3 V | `wave_algo.h` 的 `WAVE_ADC_VREF` |

---

## 四、构建与测试速查

**烧录**：见 `ti_final/CCS烧录操作指南.md`。本机 CCS 与 MSPM0 SDK 装在 `D:\AppGallery\ccs\`。

**离线自测**（不需要硬件，在 `ti_final/` 下执行）：

```bash
gcc -DREPORT_SIM_STDOUT=1 -std=c99 -Wall -Wextra -Ifirmware \
    test/test_uart_report.c firmware/uart_report.c -o test_uart_report
./test_uart_report          # 37 用例：格式化 / 帧格式

gcc -std=c99 -Wall -Wextra -DREPORT_SIM_STDOUT=1 -Ifirmware -Ipc \
    test/test_e2e.c test/e2e_sender_shim.c firmware/uart_report.c -o test_e2e
./test_e2e                  # 27 用例：单片机发 → PC 收 闭环
```

**PC 端收结果**：

```bash
gcc -std=c11 -Wall -Wextra pc/pc_receiver.c -o pc_receiver.exe
pc_receiver.exe COM5                  # 改成设备管理器里的实际 COM 号
pc_receiver.exe COM5 115200 raw.csv   # 第三参数可选，存原始数据 CSV
```

> COM 口选 **`XDS110 Class Application/User UART`**，不是 `Auxiliary Data Port`。
> CCS 串口终端与 `pc_receiver.exe` 不能同时占用同一 COM 口。

**命令行编译固件**（不开 CCS，快速验证用）：`cd ti_final/firmware && "D:/AppGallery/ccs/ccs/utils/bin/gmake.exe"`

---

## 五、协议速查

| 项 | 约定 |
|---|---|
| 物理层 | 115200，8 位数据，无校验，1 位停止，**无流控** |
| 帧边界 | 一帧 = 一行，`\r\n` 结尾，英文半角逗号分隔，纯 ASCII |
| 类别词 | `sine` / `square` / `triangle` / `sawtooth`，固定小写 |
| 数值格式 | 固定三位小数，不带单位字符，不用科学计数法 |
| 占空比 | 单位 **%**；**非方波恒为 `0.000`** |
| 控制行 | 以 `#` 开头（`#BOOT` / `#ERR`），按行首字符与结果帧区分 |

`ti_final/firmware/uart_report.h` 顶部有编译期开关（前缀、占空比单位、零的写法）。
若现场裁判要求与评分规则示例逐字符一致，改宏重新编译即可。

---

## 六、当前状态

| 项目 | 结果 |
|---|---|
| 固件编译（tiarmclang，`-Wall`） | ✅ 通过，**0 告警** |
| 链接 | ✅ 通过，产出 `ti_wave.out` |
| Flash 占用 | 19.2 KB / 128 KB |
| SRAM 占用 | 29.8 KB / 32 KB（余量 2.2 KB） |
| `test_uart_report` | ✅ 37 通过 / 0 失败 |
| `test_e2e`（端到端闭环） | ✅ 27 通过 / 0 失败 |
| `pc_receiver.c` 编译 | ✅ 通过 |
| 单轮耗时 | ≈ 1.1 s（采样 1.00 s + 算法 0.06~0.08 s + 发送 < 0.003 s），限时 10 s |

> 编译产物（`Debug/`、`*.out`、`*.map`、`.obj`、`.exe` 等）与 PDF、压缩包已通过
> `.gitignore` 排除，仓库只保留源码、配置、文档和实测数据。

---

## 七、容易踩的坑

**1. 占空比单位** —— `wave_algo.c` 交给通信层的是**归一化 0~1**，`uart_report.c`
负责乘 100 变百分数。方波算出 0.682，串口打 `68.200`。两边都改成百分数会输出 `6820.000`。

**2. `wave_analyze()` 会原地修改输入数组** —— 第一步减均值去直流是**原地**操作，
`main.c` 里那份 `g_adc` 每轮只能用一次。

**3. 不能在中断里发送** —— `report_*()` 是阻塞发送、`wave_analyze()` 要 60~80 ms，
在 ADC 中断里调用会让后续采样点错位、频率精度崩掉。中断只做取值、存数组、计数。

**4. SRAM 只剩约 2.2 KB** —— 主要开销是 `g_adc[10000]`（20 KB）和两个 1024 点 float
FFT 数组（各 4 KB）。**不要再加大数组**；要加采样点数，先减 FFT 点数。

**5. 分类阈值是实测标定的** —— `wave_algo.c` 里 `tlv > 0.7`（方波）、`r1 > 0.3`（锯齿）、
`r2 > 0.08`（三角）为经验值。现场偶发误判时优先调这三个数。
