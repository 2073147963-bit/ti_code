# TI 杯赛道一 · MSPM0G3507 波形识别

2026 年第十六届 TI 杯数字系统创新大赛 · 赛道一初赛。

单片机采集信号发生器输出的周期信号，识别**波形类别、频率、峰峰值**（方波另报**占空比**），
按协议经串口上报给 PC。

```
信号发生器 ──> PA25 ──┐
                      │  MSPM0G3507               USB 线
                      │  ├ ADC 10 kHz 采样 1 秒     │
                      └──┤ 去直流→FFT+Goertzel 估频 ├──> XDS110 ──> PC
                         │ 周期折叠→谐波比分类      │            (虚拟串口)
                         └─ 参数估计 ──> UART 115200 ┘
```

输出一行结果：

```
RESULT,<type>,<frequency>,<Vpp>,<duty>
```

例：`RESULT,square,223.400,1.560,68.200`

---

## 一、目录结构

```
ti_final/
├── firmware/                        ← 烧进单片机的全部代码
│   ├── main.c                       主程序：采样 + 主循环 + ADC 中断
│   ├── wave_algo.c / .h             算法：识别与参数估计（纯 DSP，不依赖 TI SDK）
│   ├── uart_report.c / .h           通信：拼帧 + UART 发送
│   ├── ti_wave.syscfg               SysConfig 外设配置（ADC / 定时器 / 串口）
│   ├── ti_wave.projectspec          CCS 工程导入文件
│   ├── ti_msp_dl_config.c / .h      ← SysConfig 生成，勿手改
│   ├── device.opt / .cmd.genlibs    ← SysConfig 生成，勿手改
│   ├── device_linker.cmd            ← SysConfig 生成，勿手改
│   ├── startup_mspm0g350x_ticlang.c 启动文件
│   └── makefile                     命令行构建（CCS 里用不到）
│
├── pc/
│   └── pc_receiver.c                PC 端接收程序：解析结果帧 + 原始数据
│
├── test/                            离线自测，不需要硬件
│   ├── test_uart_report.c           37 个用例：格式化 / 帧格式
│   ├── test_e2e.c                   27 个用例：单片机发 → PC 收 闭环
│   └── e2e_sender_shim.c            端到端测试的适配层
│
├── README.md                        本文件
├── CCS烧录操作指南.md               从零开始：建工程 → 编译 → 烧录 → 看输出
└── 2026TI杯赛道一初赛评分规则.pdf   赛题评分规则
```

---

## 二、当前状态（已实测）

| 项目 | 结果 |
|---|---|
| 固件编译（tiarmclang，`-Wall`） | ✅ 通过，**0 告警** |
| 链接 | ✅ 通过，产出 `ti_wave.out` |
| Flash 占用 | 19.2 KB / 128 KB |
| SRAM 占用 | 29.8 KB / 32 KB（余量 2.2 KB） |
| `test_uart_report` | ✅ 37 通过 / 0 失败 |
| `test_e2e`（端到端闭环） | ✅ 27 通过 / 0 失败 |
| `pc_receiver.c` 编译 | ✅ 通过 |

### 采样参数（三处已统一）

| 参数 | 值 | 定义位置 |
|---|---|---|
| 采样率 | 10 kHz | `wave_algo.h` 的 `WAVE_FS_HZ`，`ti_wave.syscfg` 的 TIMG0 周期 |
| 采样点数 | 10000（1 秒） | `wave_algo.h` 的 `WAVE_NSAMP` |
| ADC 参考电压 | 3.3 V | `wave_algo.h` 的 `WAVE_ADC_VREF` |

**改了采样率，定时器周期必须同步改**，否则频率换算全错。

### 信号接入

- 信号 → **PA25**（板上丝印 `PA25/A0_2`），地 → **GND**，**必须共地**。
- 信号范围 0~3.3 V、无负压 → 可直接接入，不需要任何调理电路。
- 若信号超过 3.3 V 或有负压，**必须**先加偏置/衰减，否则会损坏 ADC 引脚。

---

## 三、怎么用

**烧到板子上** → 看 [`CCS烧录操作指南.md`](CCS烧录操作指南.md)。
你机器上 CCS 21.0.1 与 MSPM0 SDK 2.11.00.07 已装在 `D:\AppGallery\ccs\`，指南里路径都是按这个写的。

**PC 端收结果**（MinGW 或 MSVC 编译）：

```bash
gcc -std=c11 -Wall -Wextra pc/pc_receiver.c -o pc_receiver.exe
pc_receiver.exe COM5                  # COM 口号改成设备管理器里看到的
pc_receiver.exe COM5 115200 raw.csv   # 第三个参数可选，把原始数据存成 CSV
```

> COM 口选 **`XDS110 Class Application/User UART`**，不是 `Auxiliary Data Port`。
> CCS 的串口终端和 `pc_receiver.exe` **不能同时占用同一个 COM 口**。

**不接硬件，先验证协议逻辑**（在 `ti_final` 根目录执行）：

```bash
gcc -DREPORT_SIM_STDOUT=1 -std=c99 -Wall -Wextra -Ifirmware \
    test/test_uart_report.c firmware/uart_report.c -o test_uart_report
./test_uart_report

gcc -std=c99 -Wall -Wextra -DREPORT_SIM_STDOUT=1 -Ifirmware -Ipc \
    test/test_e2e.c test/e2e_sender_shim.c firmware/uart_report.c -o test_e2e
./test_e2e
```

**命令行编译固件**（不开 CCS，改完想快速验证时用）：

```bash
cd firmware
"D:/AppGallery/ccs/ccs/utils/bin/gmake.exe"
```

---

## 四、几个容易踩的坑

**1. 占空比的单位**

`wave_algo.c` 交给通信层的占空比是**归一化 0~1**，`uart_report.c` 负责乘 100 变成百分数。
这是协议要求。所以 `wave_algo` 里方波算出 0.682，串口上打出的是 `68.200`。
两边都改成百分数会输出 `6820.000`。

**2. `wave_analyze()` 会改掉输入数组**

函数第一步减均值去直流，是**原地修改**。所以 `main.c` 里那份 `g_adc` 每轮只能用一次。
要同时发送原始数据（模式 B）的话，得在调用前备份——但 20 KB 的备份在 32 KB SRAM 上放不下。

**3. 不能在中断里发送**

`report_*()` 是阻塞发送，`wave_analyze()` 要 60~80 ms。在 ADC 中断里调用任何一个是致命的——
会让后续采样点全部错位，频率精度直接崩掉。代码里中断只做三件事：取值、存数组、计数。

**4. SRAM 只剩约 2.2 KB**

主要开销是 `g_adc[10000]`（20 KB）和两个 1024 点 float FFT 数组（各 4 KB）。
**不要再加大数组**。要加大采样点数，先减 FFT 点数。

**5. 分类阈值是实测标定的**

`wave_algo.c` 里 `tlv > 0.7`（判方波）、`r1 > 0.3`（判锯齿）、`r2 > 0.08`（判三角）是经验值。
理论值假设理想波形，实测受采样和折叠影响会偏离。现场如果偶发误判，优先调这三个数。

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

`uart_report.h` 顶部有几个编译期开关（前缀、占空比单位、零的写法）。若现场裁判要求与评分规则
示例逐字符一致，改宏重新编译即可：

```c
#define REPORT_FRAME_WITH_PREFIX   1   // 1 = 带 "RESULT," 前缀
#define REPORT_DUTY_PERCENT        1   // 1 = 占空比输出百分数
#define REPORT_DUTY_BARE_ZERO      0   // 1 = 零占空比输出 "0" 而非 "0.000"
```

---

## 六、时间预算

| 阶段 | 耗时 |
|---|---|
| 采样 10000 点 @ 10 kHz | 1.00 s |
| 算法（M0+ @80 MHz 软浮点） | 0.06 ~ 0.08 s |
| 发送约 32 字节 @115200 | < 0.003 s |
| **合计** | **≈ 1.1 s** |

赛题限时 10 秒，余量充足。程序是**连续循环识别**的：采 1 秒 → 出结果 → 再采。
比赛时看最新一行即可；若裁判要求"按键触发一次"，改 `main.c` 主循环等待按键即可。
