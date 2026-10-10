# CCS 烧录操作指南 —— 从零开始

面向第一次接触单片机的同学。每一步都写了**点哪里、看到什么才算对、出错怎么办**。

你机器上的软件已经装好了，**第一节可以跳过**：

| 软件 | 版本 | 路径 |
|---|---|---|
| Code Composer Studio | 21.0.1 | `D:\AppGallery\ccs\ccs` |
| MSPM0 SDK | 2.11.00.07 | `D:\AppGallery\ccs\mspm0_sdk_2_11_00_07` |
| SysConfig | 1.26.2 | `D:\AppGallery\ccs\sysconfig_1.26.2` |
| TI ARM Clang | 4.0.2.LTS | `D:\AppGallery\ccs\ti_cgt_arm_llvm_4.0.2.LTS` |

---

## 0. 先搞清楚在做什么

```
信号发生器 ──> PA25 ──> [单片机: 采样 + 识别] ──UART──> XDS110 ──USB──> 电脑
                                                                      (显示结果)
```

板子上有颗 **XDS110** 芯片，同时干两件事：

1. **调试器** —— 把电脑里编译好的程序烧进单片机
2. **背板串口（backchannel UART）** —— 把单片机的串口信号转成 USB，电脑上认成一个 **COM 口**

所以你**只需要一根 USB 线**，不用额外的 USB-TTL 模块。

最终要在电脑上看到这样一行：

```
RESULT,sine,200.000,2.500,0.000
```

---

## 1. 接板子和认串口

1. 用 Micro-USB 线把 LaunchPad 接到电脑，**插靠近 XDS110 丝印的那个口**。
2. 板子上电源灯应亮起。
3. 打开**设备管理器**（Win+X → 设备管理器），展开「端口 (COM 和 LPT)」，应该看到：

   | 设备名 | 用途 |
   |---|---|
   | `XDS110 Class Application/User UART (COMx)` | ✅ **这个才是要用的** |
   | `XDS110 Class Auxiliary Data Port (COMy)` | ❌ 别选，能打开但收不到数据 |

   **记下 Application/User UART 的号码**（比如 COM5），后面要用。

> **看不到端口？** 说明 XDS110 驱动没装好。装 CCS 时会自动装驱动，若仍是黄色感叹号，
> 到 `D:\AppGallery\ccs\ccs\install_scripts\` 或设备管理器里手动指向 CCS 的驱动目录。

---

## 2. 新建工程

> 本包提供 `firmware/ti_wave.projectspec`，熟练的话可以 **Project → Import CCS Projects**
> 一键导入。但第一次建议按下面的向导手建，每一步看得见，出问题好定位。

1. 双击 `D:\AppGallery\ccs\CCS 21.0.1.lnk` 启动 CCS。
2. 第一次启动会让你选 **workspace**（工作区目录）。选**纯英文、无空格**的路径，比如
   `D:\ti_workspace`。**不要用中文路径，也不要放在 `C:\Program Files` 下面。**
3. 菜单 **File → New → CCS Project**，在弹出的对话框里填：

   | 选项 | 填什么 |
   |---|---|
   | Target | `MSPM0G3507` |
   | Connection | `Texas Instruments XDS110 USB Debug Probe` |
   | Project name | `ti_wave`（英文，无空格） |
   | Compiler version | 默认 `TI ARM Clang 4.0.2.LTS` |
   | Project templates | **`Empty Project (with SysConfig)`** |

   > ⚠️ **模板必须选带 SysConfig 的那个**。选错成不带 SysConfig 的模板，
   > 后面双击 `.syscfg` 打不开图形界面。

4. 点 **Finish**。左侧 Project Explorer 里出现 `ti_wave` 工程和 `ti_wave.syscfg`。

---

## 3. 加入源码

工程根目录现在只有一个空的 `main.c`。把本包的源码复制进去：

1. 打开文件管理器，进到 `ti_final\firmware\`。
2. **选中这些文件**，Ctrl+C：

   ```
   main.c
   wave_algo.c
   wave_algo.h
   uart_report.c
   uart_report.h
   ti_wave.syscfg
   startup_mspm0g350x_ticlang.c
   ```

   > **不要**复制 `ti_wave.projectspec`、`makefile`。
   > `ti_msp_dl_config.c/.h`、`device_linker.cmd` 也**不要**复制 ——
   > 它们由 SysConfig 自动生成，手抄一份反而会冲突。

3. 到工程所在目录（默认 `D:\ti_workspace\ti_wave\`）粘贴，**提示覆盖 `main.c` 时选「是」**。
4. 回到 CCS，右键工程名 → **Refresh**（或按 **F5**）。
   文件应该出现在工程树里，**图标上没有斜杠标记**（有斜杠 = 被排除了，右键 → Exclude from Build 取消）。

5. **设置 include 路径**：右键工程 → **Properties** → `Build → Arm Compiler → Include Options`
   → 在 `Add dir to #include search path` 里加一条 `.`（点号，表示工程根目录）。
   点 **Apply and Close**。

   > 不加这条，编译会报 `cannot open source file "ti_msp_dl_config.h"`。

---

## 4. 生成外设配置

双击工程里的 **`ti_wave.syscfg`**，会打开图形化配置界面。你应该能看到已经配好的三个外设：

| 外设 | 实例名 | 关键配置 |
|---|---|---|
| ADC12 | `ADC12_0`（= ADC0） | 12 位，输入通道 **A0_2 / PA25**，由 TIMG0 事件触发，开 MEM0 中断 |
| TIMER | `TIMER_0`（= TIMG0） | 周期模式，**100 µs**（即 10 kHz 采样率） |
| UART | `UART_0`（= UART0） | **115200-8-N-1**，无流控，TX=PA10，RX=PA11 |

**验收标准**：界面上没有红色报错，三个外设都在。直接关掉即可（配置已经是对的）。

> 如果双击打不开或提示找不到 SDK：右键工程 → Properties → **Products**，
> 勾选 `MSPM0-SDK 2.11.00.07` 和 SysConfig，再重试。

---

## 5. 编译

点工具栏的**锤子图标**（或按 **Ctrl+B**）。

看下方 Console 窗口，成功时最后几行是：

```
  CC   main.c
  CC   wave_algo.c
  CC   uart_report.c
  LINK ti_wave.out
**** Build Finished ****
```

**验收标准**：Console 显示 `Build Finished`，Problems 窗口为空（0 errors, 0 warnings）。

> 可以对照参考：本包在命令行下用同一套 tiarmclang 编译过，**0 告警通过**，
> Flash 占 19.6 KB / 128 KB，SRAM 占 30.5 KB / 32 KB。

### 常见编译错误

| 报错 | 原因 / 解决 |
|---|---|
| `cannot open source file "ti_msp_dl_config.h"` | 第 3.5 步的 include 路径没加 |
| `identifier "UART_0_INST" is undefined` | SysConfig 没生成。双击 `.syscfg` 保存一次，或 Project → Build 触发重新生成 |
| `identifier "ADC12_0_INST" is undefined` | 同上。**注意** ADC 实例名在 SDK 里可能是 `ADC0` 或 `ADC12_0`，看一眼 `ti_msp_dl_config.h` 里实际生成的宏名 |
| `undefined symbol: interruptVectors` | 启动文件没加进来。把 `startup_mspm0g350x_ticlang.c` 加入工程 |
| `undefined reference to wave_analyze` | `wave_algo.c` 没加入工程，或被 Exclude 了 |
| `region SRAM overflowed` | 数组太大。见 README 第四节第 4 条 |
| `errors encountered during linking` | 看 Console 里具体的 undefined symbol，多半是上面某一条 |

---

## 6. 烧录

1. 确认板子 USB 已连、设备管理器能看到 XDS110。
2. 点工具栏的**绿色甲虫图标**（或按 **F11**）进入 Debug。
3. 第一次会弹出选择调试探针的对话框：

   | 选项 | 选什么 |
   |---|---|
   | Connection | `Texas Instruments XDS110 USB Debug Probe` |
   | Board or Device | `MSPM0G3507` |

   勾选「记住这个选择」，点 **OK**。

4. CCS 会自动执行：编译 → 烧写进 Flash → 复位 → **停在 `main()` 第一行**。

> ⚠️ **这里最容易困惑**：此刻程序是**暂停**的，板子上什么都没在跑。
> **Debug 只是「把程序放进去并停住」，Resume 才是「开始跑」。**

5. 点工具栏的 **Resume**（绿色三角形 ▶，或按 **F8**），程序才开始运行。

---

## 7. 看输出

### 方式一：CCS 自带终端（推荐）

1. 菜单 **View → Terminal**，打开终端面板。
2. 点终端面板的 **+** 号 → 选 **Serial Terminal**。
3. 填参数：

   | 项 | 值 |
   |---|---|
   | Port | 第 1 步记下的 COM 号（Application/User UART 那个） |
   | Baud Rate | `115200` |
   | Data Bits | `8` |
   | Parity | `None` |
   | Stop Bits | `1` |
   | Flow Control | `None` |

4. 点 **OK**。程序在跑的话，应该看到：

   ```
   #BOOT,MSPM0G3507 wave recognizer ready
   RESULT,sine,200.000,2.500,0.000
   RESULT,sine,200.000,2.500,0.000
   ...
   ```

   `#BOOT` 是上电标识，用来确认链路通了。之后每约 1.1 秒出一行结果。

### 方式二：PC 接收程序（能存 CSV）

```bash
gcc -std=c11 -Wall -Wextra pc/pc_receiver.c -o pc_receiver.exe
pc_receiver.exe COM5                  # COM 号改成你自己的
pc_receiver.exe COM5 115200 raw.csv   # 可选：把原始数据存成 CSV
```

输出示例：

```
[#1] sine    (正弦波)  f =   200.000 Hz   Vpp =   2.500 V
[#2] square  (方波)    f =   223.400 Hz   Vpp =   1.560 V   duty = 68.200 %
```

> **CCS 终端和 `pc_receiver.exe` 不能同时开** —— 同一个 COM 口只能被一个程序占用。
> 打开失败时报 `CreateFile failed` 就是这个原因。

---

## 8. 接线

| 信号发生器 | 开发板 |
|---|---|
| 输出 (OUT / HI) | **PA25**（板上丝印 `PA25/A0_2`，位于 BoosterPack 排针） |
| 地 (GND) | **GND** |

**必须共地**，否则读数会乱跳。

信号要求：**0 ~ 3.3 V，无负压**。你的信号已加直流偏置、最高不超过 3.3 V，
**可以直接接入，不需要任何调理电路**。

> ⚠️ 若信号超过 3.3 V 或含负压，**必须先做偏置/衰减**，直接接会损坏 ADC 引脚。
> 板上有 OPA2365 缓冲电路，但默认配置走的是 ADC0.4 通道，用 PA25（ADC0.2）不受影响。

---

## 9. 排查表

| 现象 | 可能原因 / 解决 |
|---|---|
| 点 Debug 后没反应 / 程序停不住 | USB 没连好；或调试探针选错。检查设备管理器 |
| 编译过了，串口没输出 | ① J21 跳线是否在 1-2 位（PA10 接 XDS）② 波特率是不是 115200 ③ COM 口选错（应是 Application/User UART） |
| 串口有输出但是乱码 | 波特率不对，确认是 115200 且数据位 8、无校验 |
| 一直输出 `#ERR,no valid signal` | 信号没接对 / 幅度太小 / 频率超出 80~500 Hz。检查 PA25 和共地 |
| 输出值是错的但格式对 | 采样率对不上。确认 `ti_wave.syscfg` 里 TIMG0 是 100 µs |
| 频率误差大 | 信号源不稳，或信号幅度太小（低于几百 mV 时信噪比差） |
| 烧录报错 `Error connecting to the target` | 板子没上电；或调试器被别的程序占用（关掉 CCS 的其他调试会话） |
| Debug 报 `Error: Flash programming failed` | 芯片被锁。按住 S3 (RESET) 再点 Debug，或用 BSL 擦除 |

---

## 10. 改了代码之后

| 改了什么 | 要做什么 |
|---|---|
| `main.c` / `wave_algo.c` / `uart_report.c` | 直接 Build → Debug |
| `ti_wave.syscfg`（改引脚、采样率、波特率） | 保存后 SysConfig 自动重生成配置；**若改了采样率，同步改 `wave_algo.h` 的 `WAVE_FS_HZ`** |
| 只想验证算法逻辑，不想烧板子 | 编译运行 `test/` 下的两个测试，不需要硬件 |

**比赛现场流程**：接好信号 → 点 Debug → 点 Resume → 在终端看最新一行结果。
程序是连续识别的，信号稳定后直接读最新一行即可。
