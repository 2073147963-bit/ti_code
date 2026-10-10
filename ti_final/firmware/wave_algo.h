/**
 * @file    wave_algo.h
 * @brief   波形识别与参数估计算法 —— 对外接口
 *
 * @details
 *  2026 年第十六届 TI 杯数字系统创新大赛 · 赛道一初赛
 *
 *  本模块是纯 DSP 代码: 输入一段 ADC 原始采样, 输出波形类别 + 频率 +
 *  峰峰值 + 占空比。**不包含任何 TI SDK 依赖**, 不碰寄存器、不碰串口。
 *
 *  这样拆分有两个实打实的好处:
 *    1. 同一份算法源码既能被 CCS 编进 MSPM0G3507, 也能被 gcc 编进 PC
 *       仿真程序(test/sim_track1.c), 两边跑的是同一个翻译单元, 不存在
 *       "仿真通过但真机不一样"的风险;
 *    2. 算法可以脱离硬件单独回归测试 —— 不用抢板子、不用信号发生器。
 *
 *  算法链路(详见 wave_algo.c 文件头与 ../README.md):
 *      去直流 -> FFT 粗估频率 -> Goertzel 细化 -> 周期折叠去噪
 *             -> 双电平比例 / DFT 谐波比分类 -> 分波形估参数
 *
 *  数据类型直接复用 uart_report.h 的 wave_type_t 与 result_t ——
 *  两者的枚举值顺序本来就一致(sine=0/square=1/triangle=2/sawtooth=3),
 *  共用同一套类型可以彻底消除"算法枚举"与"通信枚举"错位的隐患。
 *
 * @date    2026-10-08
 */

#ifndef WAVE_ALGO_H
#define WAVE_ALGO_H

#include <stdint.h>
#include "uart_report.h"   /* wave_type_t / result_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 *  一、采样与量程配置
 *
 *  这几个宏同时被 mcu/main.c(配 ADC/定时器)和 test/sim_track1.c
 *  (造仿真信号)使用, 改一处两边同步, 不会出现"采样率对不上"。
 * ===================================================================== */

/** 采样率 Hz。必须与 SysConfig 里 TIMG0 的周期严格一致(100 us -> 10 kHz)。 */
#define WAVE_FS_HZ        (10000.0f)

/** 采样点数。10000 点 @ 10 kHz = 1.00 s。
 *  赛题频率 100~400 Hz, 1 秒内有 100~400 个完整周期, 周期折叠的平均
 *  次数足够多, 去噪效果好。int16 存储占 20 KB, 在 32 KB SRAM 内。 */
#define WAVE_NSAMP        (10000)

/** ADC 参考电压 V。LaunchPad 默认用 VDD = 3.3 V。
 *  若在 SysConfig 里改用内部 2.5 V 基准, 这里必须同步改, 否则 Vpp 全错。 */
#define WAVE_ADC_VREF     (3.3f)

/** 12 位 ADC 满量程码值。电压 = 码值 / 4095 * WAVE_ADC_VREF。 */
#define WAVE_ADC_FS_CODE  (4095.0f)

/** 频率搜索下限 Hz。赛题标称 100~400 Hz, 两端各留余量防止边界误判。 */
#define WAVE_FMIN_HZ      (80.0f)

/** 频率搜索上限 Hz。 */
#define WAVE_FMAX_HZ      (500.0f)

/* ===================================================================== *
 *  二、返回码
 * ===================================================================== */

#define WAVE_OK            0    /**< 识别成功, *out 已填好              */
#define WAVE_ERR_NULL    (-1)   /**< 入参为 NULL                        */
#define WAVE_ERR_NOSIG   (-2)   /**< 找不到有效基频(没接线/幅度太小)   */

/* ===================================================================== *
 *  三、对外 API
 * ===================================================================== */

/**
 * @brief 对一段 ADC 采样做完整的波形识别与参数估计。
 *
 * @param[in,out] x    ADC 原始码值数组(12 位, 0~4095)。
 *                     **会被原地修改** —— 函数第一步就减去均值做去直流,
 *                     调用后数组内容变成去直流后的有符号样本。
 *                     若调用方后续还要用原始数据(例如模式 B 上传),
 *                     必须先自行备份。
 * @param[in]     n    样点个数, 通常传 WAVE_NSAMP。
 * @param[out]    out  识别结果。字段量纲:
 *                       type —— 四类枚举之一
 *                       freq —— Hz
 *                       vpp  —— V(已按 WAVE_ADC_VREF 换算成物理电压)
 *                       duty —— **归一化 0~1**, 仅方波有效, 其余填 0。
 *                               正是 uart_report 期望的入参量纲
 *                               (REPORT_DUTY_INPUT_RATIO = 1), 直接
 *                               把 *out 交给 report_send_result() 即可。
 *
 * @return WAVE_OK / WAVE_ERR_NULL / WAVE_ERR_NOSIG
 *
 * @note 本函数耗时约 60~80 ms(M0+ @80 MHz 软浮点), 加上 1 s 采样, 距
 *       评分规则的 10 s 限时有充足余量。
 * @warning 不要在中断里调用 —— 运算时间长, 会打乱采样时序。
 */
int wave_analyze(int16_t *x, uint32_t n, result_t *out);

/* --------------------------------------------------------------------- *
 *  以下是链路中间步骤, 单独暴露出来便于定位问题:
 *  现场如果结果不对, 可以先单独看频率估得对不对, 再看折叠波形长什么样。
 * --------------------------------------------------------------------- */

/**
 * @brief 去直流: 原地减去样本均值。
 * @param x  样本数组, 原地修改
 * @param n  样点个数
 */
void wave_remove_dc(int16_t *x, uint32_t n);

/**
 * @brief 基频估计(FFT 粗估 + Goertzel 细化)。
 * @param x  **已去直流**的样本数组
 * @param n  样点个数
 * @return 频率 Hz; 落在 [WAVE_FMIN_HZ, WAVE_FMAX_HZ] 外视为无效信号
 * @note 实测误差 ≤ 0.02 Hz。周期折叠对频率精度极敏感, 这一步是整条
 *       链路的基础, 它错了后面全错。
 */
float wave_estimate_freq(const int16_t *x, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* WAVE_ALGO_H */
