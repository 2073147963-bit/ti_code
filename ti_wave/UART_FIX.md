# CCS 串口输出修复说明

## 已修复
- 原 ADC 为单通道单次转换，转换结束硬件清除 ENC。main.c 的 ADC 中断现在重新使能下一次定时器触发；每轮采样开始也重新使能。
- 采满后在中断中停止定时器。
- 添加 1 ms SysTick 和 2 秒采样超时，避免无限等待，并为 WFI 提供定期唤醒。
- log_wave.ps1 自动选择 XDS110 Application/User UART，115200、8N1、无流控；断线或端口占用时自动重试；5 秒无数据提示检查运行状态。
- CSV 正确转义结果中的逗号，并记录完整接收日期时间。

## 使用
双击 open_wave_terminal.cmd 打开接收终端，保持窗口开启即可等待开发板插入并自动接收。多个开发板时手动指定：

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\log_wave.ps1 -Port COM8

CCS 重新编译和下载后如停在 main，需要按 Resume/F8。退出调试后，已烧录固件可以独立运行。普通 CCS 构建控制台不会显示 UART 字节。

用本机 CCS 工具重新编译和烧录：

    D:\AppGallery\ccs\ccs\utils\bin\gmake.exe -C Debug -j4 all
    D:\AppGallery\ccs\ccs\scripting\run.bat flash_wave.js

烧录时若 CCS 已占用探针，先结束该调试会话。

## 实测（2026-10-10）
CCS TI Arm Clang 编译链接成功；通过 CCS scripting 下载并运行成功，随后断开调试连接。
COM8 接收到 BOOT 和连续 RESULT，接收记录见 verification_uart.csv、live_uart.csv。实际识别输出间隔约 4 秒；main.c 顶部原有 80 MHz/1.1 秒估算不适用于当前 32 MHz 时钟。

当前实测 RESULT 的 Vpp 为 0.000 V，频率跳动，不能当作有效波形测量。需向 PA25/ADC0.2 接入有效信号并共地后验证测量参数；当前算法仅按频率范围判断无信号，可能将低幅噪声误判为方波。本次未修改分类算法。
自动重连逻辑已实现，但未进行人工拔插 USB 的物理复测。

硬件依据：TI MSPM0 G-Series Technical Reference Manual，单通道单次转换结束清除 ENC：
https://www.ti.com/lit/ug/slau846a/slau846a.pdf
