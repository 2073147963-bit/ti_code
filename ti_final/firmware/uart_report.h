/**
 * @file    uart_report.h
 * @brief   MSPM0G3507 波形识别结果 UART 上报模块 —— 对外接口
 *
 * @details
 *  2026 年第十六届 TI 杯数字系统创新大赛 · 赛道一初赛
 *  本模块负责链路中"单片机发送端"这一环：
 *
 *      MSPM0G3507(算得结果) --UART--> XDS110(UART 转 USB 虚拟串口)
 *          --> PC(识别为 COM 端口) --> CCS 终端 / 上位机接收程序
 *
 *  物理层参数(《数据通信部分算法》2.1 节, 需与 SysConfig 中一致):
 *      波特率 115200 | 数据位 8 | 校验位 无(N) | 停止位 1 | 无硬件流控
 *      简写: 115200、8-N-1、无流控
 *
 *  模块提供两种上报模式, 二者共用同一套底层发送函数:
 *      模式 A  结果帧 —— 单片机本地算完, 直接上报一行识别结果(报名评分用)
 *               report_send_result()
 *      模式 B  原始数据 —— 核心算法放上位机时, 把 ADC 采样序列整段送出
 *               report_raw_begin() / report_raw_put*() / report_raw_end()
 *
 * @note  全模块不使用 printf 及其任何变体。嵌入式默认编译链不含浮点
 *        格式化支持, "%f" 可能输出乱码或空白; 这里的浮点转字符串
 *        (format3dec) 全程只用整数运算手工实现。
 *
 * @date    2026-10-07
 */

#ifndef UART_REPORT_H
#define UART_REPORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 *  一、编译期配置开关
 * ===================================================================== */

/**
 * @brief 结果帧是否带 "RESULT," 前缀。
 *
 * 1 = RESULT,sine,200.000,2.500,0.000   <-- 《初赛评分规则》3 节规定格式(默认)
 * 0 = sine,200.000,2.500,0.000          <-- 《数据通信部分算法》2.3 节格式
 *
 * 两份文档格式不一致, 故做成开关。现场以裁判认可的格式为准, 改这一行即可。
 */
#ifndef REPORT_FRAME_WITH_PREFIX
#define REPORT_FRAME_WITH_PREFIX   1
#endif

/**
 * @brief 占空比的【输出】单位。协议要求"定死一种", 本工程定为百分数。
 *
 * 1 = 百分数 0~100, 如 68.200 表示 68.2%  <-- 《初赛评分规则》3 节(默认)
 * 0 = 归一化 0~1,   如 0.682  表示 68.2%  <-- 《数据通信部分算法》示例
 *
 * @warning 改成 0 会与官方评分规则的"单位为 %"不符, 有被判分风险。
 */
#ifndef REPORT_DUTY_PERCENT
#define REPORT_DUTY_PERCENT        1
#endif

/**
 * @brief 占空比的【输入】单位 —— 即上层算法填进 result_t::duty 的量纲。
 *
 * 1 = 上层给归一化值 0~1(交越法数 高电平样点/周期样点 直接得到, 默认)
 * 0 = 上层已经自己乘好 100, 给的是 0~100
 *
 * 输入与输出单位不一致时, 本模块自动换算(×100 或 ÷100), 上层不用操心。
 * 两者一致时不做任何换算, 原值输出。
 */
#ifndef REPORT_DUTY_INPUT_RATIO
#define REPORT_DUTY_INPUT_RATIO    1
#endif

/**
 * @brief 非方波的占空比是否强制输出 0。
 *
 * 1 = 强制 0(默认)。协议规定"方波为实际值, 其余波形输出 0", 由本模块
 *     兜底, 上层即便传了脏值也不会违反协议。
 * 0 = 原样输出上层给的 duty, 便于算法调试时观察中间量。
 */
#ifndef REPORT_FORCE_ZERO_DUTY
#define REPORT_FORCE_ZERO_DUTY     1
#endif

/**
 * @brief 非方波的占空比是否输出裸 "0" 而不是 "0.000"。
 *
 * 0 = 输出 "0.000"(默认)。字段宽度统一, 肉眼和脚本都好对齐。
 * 1 = 输出 "0"。与《初赛评分规则》3 节的输出示例逐字符一致
 *     (示例为 RESULT,sine,137.2,1.18,0)。
 *
 * 两者在数值上完全等价("0" 与 "0.000" 用 strtod 读出来都是 0.0),
 * 评分按相对误差算, 所以选哪个都不影响得分。默认取 0.000 是因为
 * 定宽字段在串口终端里更容易肉眼核对。若现场裁判要求与评分规则
 * 示例逐字符一致, 把本宏改成 1 即可。
 *
 * @note 只影响"被兜底成 0 的非方波"。方波的实际占空比永远走三位小数。
 */
#ifndef REPORT_DUTY_BARE_ZERO
#define REPORT_DUTY_BARE_ZERO      0
#endif

/**
 * @brief 类别识别失败(枚举越界)时输出什么。
 *
 * 0 = 输出 "unknown"(默认)。诚实反映"没识别出来", 便于调试;
 *     《数据通信部分算法》2.8 节也把 unknown 列为接收端要处理的情形。
 * 1 = 退化成 "sine"。《初赛评分规则》3 节只允许四个小写单词, 且类别
 *     识别与参数估计分开计分 —— 输出 unknown 必定拿 0 分, 输出一个
 *     合法单词至少有 1/4 概率命中, 期望分不会更差。
 *
 * @warning 默认 0。这个开关只在"算法有可能返回越界枚举"时才有意义;
 *          正常情况下 classify() 必定返回四个合法值之一, 走不到这里。
 */
#ifndef REPORT_UNKNOWN_FALLBACK_SINE
#define REPORT_UNKNOWN_FALLBACK_SINE  0
#endif

/**
 * @brief 目标 UART 外设实例。
 *
 * SysConfig 中添加 UART0 外设、实例名保持默认 "UART_0" 时, 生成的宏即
 * UART_0_INST。若你在 SysConfig 里改了实例名, 在工程的编译选项里
 * 预定义 REPORT_UART_INST 覆盖即可, 不必动本文件。
 */
#ifndef REPORT_UART_INST
#define REPORT_UART_INST           UART_0_INST
#endif

/**
 * @brief PC 仿真开关 —— 把底层发送重定向到 stdout。
 *
 * 0 = 真机(默认), 调用 MSPM0 DriverLib 发送。
 * 1 = PC 上用 gcc/MSVC 编译, 发送内容打到 stdout, 用于离线验证
 *     format3dec 的进位、补零、负零等边界。见 test_uart_report.c。
 */
#ifndef REPORT_SIM_STDOUT
#define REPORT_SIM_STDOUT          0
#endif

/**
 * @brief 结果帧拼接缓冲区长度。
 *
 * 最坏情况逐项相加(全部按上限取, 即便那些值说明算法已经出错):
 *     "RESULT,"            7
 *     "triangle"           8   (四个单词里最长)
 *     3 × (',' + 数值)     3 × (1 + 15) = 48
 *                              数值上限 15 = 负号 1 + 整数 10 + '.' 1 + 小数 3
 *     "\r\n"               2
 *     '\0'                 1
 *     ----------------------------
 *     合计                66
 *
 * 《数据通信部分算法》3.5 节写的是 char buf[64] —— 按赛题实际量程
 * (频率 100~400 Hz、峰峰值几伏、占空比 0~100)绝对够用, 但把三个
 * 数值都按 int32 上限取满时会差 2 字节。这里取 96 留足余量, 使
 * "容量不足"这条分支在任何输入下都走不到, 而不是靠量程假设兜着。
 * 代价是栈上多 32 字节, 对 32 KB SRAM 可忽略。
 */
#define REPORT_FRAME_BUF_SIZE      96

/* ===================================================================== *
 *  二、数据类型(《数据通信部分算法》3.2 节)
 * ===================================================================== */

/** @brief 波形类别枚举。顺序与 wave_to_str() 内的映射表严格对应。 */
typedef enum {
    WAVE_SINE     = 0,   /**< 正弦波   -> "sine"     */
    WAVE_SQUARE   = 1,   /**< 方波     -> "square"   */
    WAVE_TRIANGLE = 2,   /**< 三角波   -> "triangle" */
    WAVE_SAWTOOTH = 3,   /**< 锯齿波   -> "sawtooth" */
    WAVE_TYPE_NUM        /**< 哨兵: 枚举个数, 不是合法类别 */
} wave_type_t;

/** @brief 一次识别的完整结果。与 PC 接收端 result_t 保持字段一致。 */
typedef struct {
    wave_type_t type;    /**< 波形类别                                  */
    float       freq;    /**< 基波频率, 单位 Hz                         */
    float       vpp;     /**< 峰峰值, 单位 V                            */
    float       duty;    /**< 占空比, 入参单位见 REPORT_DUTY_INPUT_RATIO
                              (默认 0~1); 非方波本模块兜底输出 0        */
} result_t;

/* ===================================================================== *
 *  三、对外 API
 * ===================================================================== */

/**
 * @brief 模块初始化。
 *
 * UART 外设本身由 SysConfig 生成的 SYSCFG_DL_init() 初始化, 本函数只做
 * 模块内部状态复位, 必须在 SYSCFG_DL_init() 之后调用。
 */
void report_init(void);

/**
 * @brief 波形类别枚举 -> 协议规定的英文小写单词。
 *
 * @param  w  波形类别
 * @return 指向静态常量字符串的指针, 调用方不得释放。
 *         非法枚举值返回 "unknown"。
 */
const char *wave_to_str(wave_type_t w);

/**
 * @brief 浮点数 -> "整数.三位小数" 字符串(手工实现, 不依赖 printf)。
 *
 * @param[out] dst  目标缓冲区, 容量至少 REPORT_NUM_BUF_SIZE 字节
 * @param[in]  v    待格式化的浮点数
 * @return 写入的字符个数(不含结尾 '\0')
 *
 * @details 四舍五入到小数点后三位, 不足补零; 负数保留负号, 且 -0.004
 *          这类"整数部分为 0 的负数"也能正确带出负号。
 *          示例: 200.12345 -> "200.123"   -0.004 -> "-0.004"
 *                2.5       -> "2.500"      0.0   -> "0.000"
 */
int format3dec(char *dst, float v);

/** 单个数值字符串的最小缓冲区: 符号 1 + 整数最多 10 + '.' 1 + 小数 3 + '\0' 1 */
#define REPORT_NUM_BUF_SIZE        16

/**
 * @brief 【模式 A】按协议发送一帧识别结果。
 *
 * 帧格式(REPORT_FRAME_WITH_PREFIX = 1 时):
 *     RESULT,<type>,<freq>,<vpp>,<duty>\r\n
 * 例:
 *     RESULT,sine,200.000,2.500,0.000\r\n
 *     RESULT,square,223.400,1.560,68.200\r\n
 *
 * @param r  识别结果; 传 NULL 则函数直接返回, 不发送任何字节
 *
 * @note 阻塞发送, 一帧约 32 字节, 115200 bps 下耗时 < 3 ms。
 * @warning 不要在 ADC 采样或定时器中断里调用, 以免打乱采样时序;
 *          应在采样结束、参数估计完成之后在主循环中调用。
 */
void report_send_result(const result_t *r);

/**
 * @brief 【模式 A·便捷重载】不构造结构体, 直接给四个量发送。
 *
 * @param type  波形类别
 * @param freq  频率 Hz
 * @param vpp   峰峰值 V
 * @param duty  占空比(单位见 REPORT_DUTY_INPUT_RATIO), 非方波可随意传
 */
void report_send_values(wave_type_t type, float freq, float vpp, float duty);

/* --- 返回值: report_format_result() / report_send_result_checked() 共用 --- */
#define REPORT_OK            0    /**< 成功                                 */
#define REPORT_ERR_NULL     (-1)  /**< 入参为 NULL                          */
#define REPORT_ERR_CAPACITY (-2)  /**< 目标缓冲区容量不足, 未写入完整帧     */

/**
 * @brief 只拼帧, 不发送 —— 把一帧结果写成字符串放进调用方的缓冲区。
 *
 * report_send_result() 内部就是"调本函数 + 把结果丢给 uart_send_str()",
 * 单独暴露出来有三个用处:
 *   1. 离线单元测试可以直接比对整帧字符串, 不必捕获 stdout;
 *   2. 想改用 DMA / 中断发送时, 自己拿到帧内容交给 DMA 即可;
 *   3. 需要把同一帧同时送往串口和 OLED / SD 卡时, 只拼一次。
 *
 * @param[out] dst  目标缓冲区, 建议直接给 char[REPORT_FRAME_BUF_SIZE]
 * @param[in]  cap  dst 的容量(字节), 含结尾 '\0' 的位置
 * @param[in]  r    识别结果
 * @param[out] out_len  非 NULL 时写回帧长度(不含 '\0'); 失败时写 0
 *
 * @return REPORT_OK / REPORT_ERR_NULL / REPORT_ERR_CAPACITY
 *
 * @note 帧含结尾的 "\r\n", 但不含 '\0' 之后的任何内容。容量不足时
 *       dst 被置为空串并返回 REPORT_ERR_CAPACITY —— 绝不返回半帧,
 *       这是协议 2.6 节"不得只发送半帧"在 API 层面的体现。
 */
int report_format_result(char *dst, int cap, const result_t *r, int *out_len);

/**
 * @brief 【模式 A·带返回值版】发送一帧结果并告知是否真的发出去了。
 *
 * 与 report_send_result() 行为完全一致, 只是多了返回值。主程序想在
 * 发送失败时点个 LED 或重试, 用这个版本。
 *
 * @param r  识别结果
 * @return REPORT_OK 已完整发出; 负值见 REPORT_ERR_*, 此时一个字节都没发
 */
int report_send_result_checked(const result_t *r);

/**
 * @brief 发送一行以 '#' 开头的注释/状态行。
 *
 * 协议只定义了结果帧的格式, 没规定调试信息怎么办。本工程约定: 凡是
 * 非结果的行一律以 '#' 开头, 接收端按行首字符就能一眼区分, 不会把
 * 调试输出误判成解析失败。#BOOT / #RAW / #END 都遵循这个约定。
 *
 * @param text  '#' 之后的正文, 不含换行; 传 NULL 则只发 "#\r\n"
 *
 * @note 正文必须是纯 ASCII 且不含 '\r'、'\n'、',' 以外的控制字符,
 *       否则会破坏接收端的行同步。本函数不做过滤, 由调用方保证。
 */
void report_send_comment(const char *text);

/**
 * @brief 【模式 B·1/3】发送原始数据帧头, 开启一次原始数据上传。
 *
 * 帧头格式:  #RAW,<n>,<fs>\r\n
 *            n  = 本次将发送的样点个数
 *            fs = 采样率, 单位 Hz(整数)
 *
 * @param n_samples  样点总数, 必须与随后 put 的个数一致
 * @param fs_hz      采样率 Hz
 *
 * @note 内部会清零累加校验和, 所以必须先调用本函数再 put。
 */
void report_raw_begin(uint32_t n_samples, uint32_t fs_hz);

/**
 * @brief 【模式 B·2/3】送出一个样点(ASCII 十进制 + 逗号)。
 *
 * @param code  ADC 原始码值(12 位 ADC 为 0~4095)
 *
 * @note 为便于人眼在串口终端核对, 每 16 个样点自动换行。
 */
void report_raw_put(uint16_t code);

/**
 * @brief 【模式 B·2/3 批量版】送出一段连续样点。
 *
 * @param buf    样点数组, 传 NULL 则直接返回
 * @param count  样点个数
 */
void report_raw_put_block(const uint16_t *buf, uint32_t count);

/**
 * @brief 【模式 B·3/3】发送帧尾, 结束本次原始数据上传。
 *
 * 帧尾格式:  #END,<checksum>\r\n
 *            checksum = 所有已 put 样点码值的 32 位无符号累加和(自然溢出)
 *
 * 上位机把自己收到的样点同样累加, 比对一致即认为整段无丢字节。
 */
void report_raw_end(void);

/**
 * @brief 【模式 B·一把梭】帧头 + 全部样点 + 帧尾, 一次发完。
 *
 * @param buf        样点数组
 * @param n_samples  样点个数
 * @param fs_hz      采样率 Hz
 *
 * @warning ASCII CSV 单点约 5 字节, 4096 点 ≈ 20 KB, 115200 bps 下
 *          约 1.8 s。初赛限时 10 s, 若样点数远大于 4096 需先评估耗时。
 */
void report_send_raw(const uint16_t *buf, uint32_t n_samples, uint32_t fs_hz);

/* --------------------------------------------------------------------- *
 *  底层发送原语 —— 也可被其它模块直接拿去打调试信息
 * --------------------------------------------------------------------- */

/**
 * @brief 阻塞发送 len 个字节。
 * @param data  待发数据; NULL 或 len == 0 时直接返回
 * @param len   字节数
 */
void uart_send_bytes(const uint8_t *data, uint32_t len);

/**
 * @brief 阻塞发送一个以 '\0' 结尾的字符串(不含 '\0' 本身)。
 * @param s  字符串; NULL 时直接返回
 */
void uart_send_str(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* UART_REPORT_H */
