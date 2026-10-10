/**
 * @file    main.c
 * @brief   MSPM0G3507 波形识别主程序 —— 采样 -> 识别 -> 串口上报
 *
 * @details
 *  2026 年第十六届 TI 杯数字系统创新大赛 · 赛道一初赛
 *
 *  主流程(每轮约 1.1 秒, 远在 10 秒限时内):
 *      启动 TIMG0 -> ADC 每 100 us 转换一次 -> 中断把结果填进 g_adc
 *      -> 采满 10000 点 -> 停止定时器
 *      -> wave_analyze() 识别
 *      -> report_send_result() 按协议发一行
 *      -> 下一轮
 *
 *  时间预算:
 *      采样           10000 点 @ 10 kHz      = 1.00 s
 *      算法(含 FFT)   M0+ @ 80 MHz 软浮点    ≈ 0.06~0.08 s
 *      发送           约 32 字节 @115200     < 0.003 s
 *      -------------------------------------------------
 *      合计                                  ≈ 1.1 s
 *
 *  三个外设都由 SysConfig 生成(见 ti_wave.syscfg), 本文件不碰寄存器:
 *      ADC12_0_INST  = ADC0,  PA25 / ADC0.2, TIMG0 零事件触发, MEM0 中断
 *      TIMER_0_INST  = TIMG0, 100 us 周期 => 10 kHz
 *      UART_0_INST   = UART0, 115200-8-N-1, TX=PA10 RX=PA11 -> XDS110 -> USB
 *
 * @date    2026-10-10
 */

#include "ti_msp_dl_config.h"
#include "uart_report.h"
#include "wave_algo.h"

/* ===================================================================== *
 *  采样缓冲与完成标志
 * ===================================================================== */

/**
 * @brief ADC 采样缓冲。
 *
 * 10000 点 @ 10 kHz = 1 秒。赛题频率 100~400 Hz, 1 秒内有 100~400 个完整
 * 周期, 周期折叠的平均次数足够多, 去噪效果才够(白噪声按 sqrt(N) 衰减)。
 *
 * 用 int16_t 存 12 位码值: 去直流后数值落在 -2048~2047, 仍在 int16 范围内,
 * 且只占 20 KB —— 这是 32 KB SRAM 上唯一放得下的选择。
 *
 * 放 static 让它落 .bss, 不占栈。
 */
static int16_t g_adc[WAVE_NSAMP];

/** 已采到的样点数。ISR 写, 主循环读, 必须 volatile。 */
static volatile uint32_t g_adc_idx = 0u;

/** 采样完成标志。ISR 在采满时置位, 主循环轮询等待。 */
static volatile uint8_t g_adc_done = 0u;

/** 本次识别结果。全局放置, 不占栈。 */
static result_t g_result;

/** 识别失败计数, 仅用于串口提示, 不参与协议。 */
static uint32_t g_err_count = 0u;

/* ===================================================================== *
 *  ADC 中断 —— 定时器每 100 us 触发一次转换, 转换完成进这里
 * ===================================================================== */

/**
 * @brief ADC0 中断服务程序。名字由 SysConfig 生成(ADC12_0_INST_IRQHandler)。
 *
 * @note 本 ISR 只做"取值 + 存数组 + 计数"三件极短的事。绝不能在这里
 *       调 report_*() 或 wave_analyze() —— 前者阻塞发送会拖到 3 ms,
 *       后者要 60 ms, 都会让后续采样点全部错位, 直接毁掉频率精度。
 */
void ADC12_0_INST_IRQHandler(void)
{
    switch (DL_ADC12_getPendingInterrupt(ADC12_0_INST)) {
        case DL_ADC12_IIDX_MEM0_RESULT_LOADED:
            DL_ADC12_clearInterruptStatus(
                ADC12_0_INST, DL_ADC12_INTERRUPT_MEM0_RESULT_LOADED);

            if (g_adc_idx < (uint32_t)WAVE_NSAMP) {
                /* 取 12 位结果。& 0xFFF 是防御性写法: 若将来改成 14 位分辨率
                 * 而忘了改这里, 会得到被截断的错值, 但至少不会越界。 */
                g_adc[g_adc_idx] = (int16_t)(
                    DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_0)
                    & 0xFFFu);
                ++g_adc_idx;

                if (g_adc_idx >= (uint32_t)WAVE_NSAMP) {
                    g_adc_done = 1u;
                }
            }
            break;

        default:
            break;
    }
}

/* ===================================================================== *
 *  主程序
 * ===================================================================== */

int main(void)
{
    /* ---- 1. 系统初始化 ----
     * SYSCFG_DL_init() 由 SysConfig 生成, 内含时钟、GPIO、ADC、定时器、
     * UART 的全部初始化。必须最先调用。 */
    SYSCFG_DL_init();

    /* ---- 2. 通信模块状态复位(必须在 SYSCFG_DL_init 之后) ---- */
    report_init();

    /* ---- 3. 使能 ADC 中断 ----
     * SysConfig 只配好了中断使能位, NVIC 层面的开关要自己打开。 */
    NVIC_EnableIRQ(ADC12_0_INST_INT_IRQN);

    /* ---- 4. 上电打一行标识, 确认链路通了 ----
     * 以 '#' 开头, 上位机按控制行处理, 不会误判成结果帧。 */
    report_send_comment("BOOT,MSPM0G3507 wave recognizer ready");

    /* ---- 5. 主循环 ---- */
    for (;;) {
        /* 5.a 复位状态, 启动定时器开始本轮采样。
         *     顺序要紧: 必须先清 g_adc_done 再开定时器, 否则可能丢掉
         *     上一轮的完成标志, 或者更糟 —— 把上一轮当成这一轮。 */
        g_adc_idx  = 0u;
        g_adc_done = 0u;
        DL_TimerG_startCounter(TIMER_0_INST);

        /* 5.b 忙等采样完成。1 秒左右, 期间 CPU 无事可做。
         *     __WFI() 让 CPU 进睡眠等中断, 比空转省电; 中断照常响应,
         *     所以采满 10000 点后会被唤醒并看到 g_adc_done 置位。 */
        while (g_adc_done == 0u) {
            __WFI();
        }

        /* 5.c 停止定时器, 不再产生触发。 */
        DL_TimerG_stopCounter(TIMER_0_INST);

        /* 5.d 识别。
         *     注意 wave_analyze() 会【原地修改】g_adc(减均值去直流),
         *     所以每轮采样后这一份数据只能用一次, 不能重复分析。
         *     这里也不做备份 —— 备份要多占 20 KB, 32 KB SRAM 放不下。 */
        if (wave_analyze(g_adc, (uint32_t)WAVE_NSAMP, &g_result) == WAVE_OK) {
            /* 5.e 按协议发一帧:
             *     RESULT,<type>,<freq>,<vpp>,<duty>\r\n
             *     此刻已不在任何中断上下文, 采样也已结束, 阻塞发送安全。 */
            if (report_send_result_checked(&g_result) != REPORT_OK) {
                report_send_comment("ERR,frame format failed");
            }
        } else {
            /* 没采到有效信号(没接线、幅度太小、频率超出 80~500 Hz)。
             * 打一行提示而不是静默 —— 现场看终端时, "有一行报错"
             * 比 "什么都没有" 好排查得多。 */
            ++g_err_count;
            report_send_comment("ERR,no valid signal");
        }

        /* 5.f 到这里本轮结束, 直接进入下一轮。
         *     比赛时裁判发令后只需要一帧结果, 连续跑便于观察稳定性;
         *     若要求"按键触发一次识别", 把上面的 for(;;) 改成等待按键即可。 */
    }

    /* 不会执行到这里。 */
}
