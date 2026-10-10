/**
 * @file    test_uart_report.c
 * @brief   uart_report 模块的 PC 离线自测程序
 *
 * @details
 *  在 PC 上验证不依赖硬件的那部分逻辑, 省去反复烧板子:
 *      - format3dec() 的四舍五入、补零、负号、负零、异常值
 *      - report_send_result() 的帧格式与字段顺序
 *      - 模式 B 原始数据帧的帧头/帧尾与校验和
 *
 *  编译(MinGW / MSYS2 / Linux gcc 均可), 在 ti_final 根目录执行:
 *      gcc -DREPORT_SIM_STDOUT=1 -std=c99 -Wall -Wextra \
 *          -Ifirmware test/test_uart_report.c firmware/uart_report.c \
 *          -o test_uart_report
 *
 *  运行:
 *      ./test_uart_report
 *
 *  REPORT_SIM_STDOUT=1 会把 uart_send_bytes() 重定向到 stdout, 因此
 *  不需要 ti_msp_dl_config.h, 也不需要任何 TI SDK。
 *
 * @date    2026-10-07
 */

#include <stdio.h>
#include <string.h>
#include "uart_report.h"

static int g_pass = 0;      /* 通过的用例数 */
static int g_fail = 0;      /* 失败的用例数 */

/**
 * @brief 校验 format3dec() 的一个用例。
 * @param v         输入浮点数
 * @param expect    期望输出字符串
 */
static void check_fmt(float v, const char *expect)
{
    char got[REPORT_NUM_BUF_SIZE];
    int  len = format3dec(got, v);

    if (strcmp(got, expect) == 0 && len == (int)strlen(expect)) {
        ++g_pass;
        printf("  [ OK ] %-14s -> \"%s\"\n", "format3dec", got);
    } else {
        ++g_fail;
        printf("  [FAIL] format3dec  got \"%s\" (len=%d), want \"%s\"\n",
               got, len, expect);
    }
}

/* 整帧比对只在默认配置下做, 条件与下面第 6b 节一致 —— 否则本函数
   没有调用点, -Wunused-function 会报警。 */
#if (REPORT_FRAME_WITH_PREFIX == 1) && (REPORT_DUTY_PERCENT == 1) && \
    (REPORT_DUTY_INPUT_RATIO == 1) && (REPORT_DUTY_BARE_ZERO == 0) && \
    (REPORT_FORCE_ZERO_DUTY == 1)
#define TEST_DEFAULT_CONFIG  1
#else
#define TEST_DEFAULT_CONFIG  0
#endif

#if TEST_DEFAULT_CONFIG
/**
 * @brief 校验 report_format_result() 拼出的整帧字符串。
 *
 * @param type    波形类别
 * @param freq    频率 Hz
 * @param vpp     峰峰值 V
 * @param duty    占空比, 单位按 REPORT_DUTY_INPUT_RATIO(默认归一化 0~1)
 * @param expect  期望的完整帧, 含结尾 "\r\n"
 */
static void check_frame(wave_type_t type, float freq, float vpp, float duty,
                        const char *expect)
{
    result_t r;
    char     got[REPORT_FRAME_BUF_SIZE];
    int      len = 0;
    int      rc;

    r.type = type;
    r.freq = freq;
    r.vpp  = vpp;
    r.duty = duty;

    rc = report_format_result(got, (int)sizeof(got), &r, &len);

    if (rc == REPORT_OK && strcmp(got, expect) == 0 &&
        len == (int)strlen(expect)) {
        ++g_pass;
        /* 打印时把 \r\n 显示成可见的转义, 免得输出乱跳行。 */
        printf("  [ OK ] %.*s\\r\\n\n", (int)strlen(got) - 2, got);
    } else {
        ++g_fail;
        printf("  [FAIL] rc=%d len=%d\n         got  \"%s\"\n"
               "         want \"%s\"\n", rc, len, got, expect);
    }
}
#endif /* TEST_DEFAULT_CONFIG */

int main(void)
{
    uint16_t raw[20];
    int      i;

    report_init();

    /* ================================================================= *
     *  1. format3dec —— 基本量程
     * ================================================================= */
    puts("== 1. format3dec 基本用例 ==");
    check_fmt(0.0f,        "0.000");      /* 零                        */
    check_fmt(2.5f,        "2.500");      /* 末尾补零                  */
    check_fmt(200.0f,      "200.000");    /* 整数值                    */
    check_fmt(1.0f,        "1.000");
    check_fmt(137.2f,      "137.200");    /* 评分规则示例中的频率      */
    check_fmt(1.18f,       "1.180");      /* 评分规则示例中的峰峰值    */
    check_fmt(68.2f,       "68.200");     /* 评分规则示例中的占空比    */

    /* ================================================================= *
     *  2. format3dec —— 四舍五入与补零(文档 3.4 节验证示例)
     * ================================================================= */
    puts("\n== 2. 四舍五入与补零 ==");
    check_fmt(200.12345f,  "200.123");    /* 文档示例: 截到三位        */
    check_fmt(0.0005f,     "0.001");      /* 向上进位                  */
    check_fmt(0.0004f,     "0.000");      /* 不进位                    */
    check_fmt(1.9999f,     "2.000");      /* 进位跨整数                */
    check_fmt(0.001f,      "0.001");      /* 小数部分需补两个零        */
    check_fmt(0.01f,       "0.010");      /* 小数部分需补一个零        */
    check_fmt(9.9995f,     "10.000");     /* 进位改变整数位数          */

    /* ================================================================= *
     *  3. format3dec —— 负数与负零(文档 3.4 节步骤 4)
     * ================================================================= */
    puts("\n== 3. 负数与负零 ==");
    check_fmt(-0.004f,     "-0.004");     /* 文档示例: ip=0 也要带负号 */
    check_fmt(-2.5f,       "-2.500");
    check_fmt(-200.123f,   "-200.123");
    check_fmt(-0.0001f,    "0.000");      /* 舍入为 0, 不输出 "-0.000" */

    /* ================================================================= *
     *  4. format3dec —— 大数与异常值
     * ================================================================= */
    puts("\n== 4. 大数与异常值 ==");
    check_fmt(400.0f,      "400.000");    /* 赛题频率上限附近          */
    check_fmt(123456.789f, "123456.789"); /* 远超赛题量程, 仍需正确    */

    /* ================================================================= *
     *  5. wave_to_str 映射
     * ================================================================= */
    puts("\n== 5. wave_to_str 映射 ==");
    {
        struct { wave_type_t t; const char *s; } map[] = {
            { WAVE_SINE,     "sine"     },
            { WAVE_SQUARE,   "square"   },
            { WAVE_TRIANGLE, "triangle" },
            { WAVE_SAWTOOTH, "sawtooth" },
            /* 非法枚举的兜底值由 REPORT_UNKNOWN_FALLBACK_SINE 决定:
               默认报 "unknown", 置 1 则退化成 "sine" 搏概率。 */
#if REPORT_UNKNOWN_FALLBACK_SINE
            { (wave_type_t)99, "sine"    },
#else
            { (wave_type_t)99, "unknown" },
#endif
        };
        for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); ++i) {
            const char *got = wave_to_str(map[i].t);
            if (strcmp(got, map[i].s) == 0) {
                ++g_pass;
                printf("  [ OK ] wave_to_str  -> \"%s\"\n", got);
            } else {
                ++g_fail;
                printf("  [FAIL] wave_to_str  got \"%s\", want \"%s\"\n",
                       got, map[i].s);
            }
        }
    }

    /* ================================================================= *
     *  6. 模式 A 结果帧 —— 四类波形各发一帧, 人眼核对格式
     *
     *  注意 duty 入参按默认 REPORT_DUTY_INPUT_RATIO=1, 即归一化 0~1;
     *  方波传 0.682 应输出 68.200。
     * ================================================================= */
    puts("\n== 6. 模式 A 结果帧(下面每行应与评分规则格式一致) ==");
    report_send_values(WAVE_SINE,     200.0f,   2.5f,  0.0f);
    report_send_values(WAVE_SQUARE,   223.4f,   1.56f, 0.682f);  /* ->68.200 */
    report_send_values(WAVE_TRIANGLE, 150.25f,  4.0f,  0.0f);
    report_send_values(WAVE_SAWTOOTH, 120.5f,   3.0f,  0.0f);

    /* 方波占空比 50% 的常见情形。 */
    puts("\n   (方波 duty=0.5, 应输出 50.000)");
    report_send_values(WAVE_SQUARE,   100.0f,   3.3f,  0.5f);

    /* 非方波即便上层传了 duty, 协议要求输出 0 —— 验证兜底是否生效。 */
    puts("\n   (非方波传入 duty=0.555, 应被兜底为 0.000)");
    report_send_values(WAVE_SINE,     137.2f,   1.18f, 0.555f);

    /* 空指针防御: 不应崩溃, 也不应输出任何字节。 */
    puts("\n   (传 NULL, 下面一行应为空)");
    report_send_result(NULL);
    puts("");

    /* ================================================================= *
     *  6b. report_format_result —— 整帧字符串比对
     *
     *  比 "人眼核对第 6 节的输出" 严格: 直接断言整帧内容, 回归时
     *  改坏了格式会立刻失败, 不依赖看屏幕。
     *
     *  下面的期望串是按【默认配置】写死的:
     *      REPORT_FRAME_WITH_PREFIX = 1   -> 带 "RESULT," 前缀
     *      REPORT_DUTY_PERCENT      = 1   -> 占空比输出 0~100
     *      REPORT_DUTY_INPUT_RATIO  = 1   -> 入参占空比为 0~1
     *      REPORT_DUTY_BARE_ZERO    = 0   -> 零占空比输出 "0.000"
     *      REPORT_FORCE_ZERO_DUTY   = 1   -> 非方波兜底为 0
     *  改了任一开关, 期望串自然对不上, 所以整段只在默认配置下编译。
     *  非默认配置想回归, 请照抄本段并改期望串。
     * ================================================================= */
#if TEST_DEFAULT_CONFIG
    puts("\n== 6b. report_format_result 整帧比对 ==");
    check_frame(WAVE_SINE,     200.0f,  2.5f,  0.0f,
                "RESULT,sine,200.000,2.500,0.000\r\n");
    check_frame(WAVE_SQUARE,   223.4f,  1.56f, 0.682f,
                "RESULT,square,223.400,1.560,68.200\r\n");
    check_frame(WAVE_TRIANGLE, 150.25f, 4.0f,  0.0f,
                "RESULT,triangle,150.250,4.000,0.000\r\n");
    check_frame(WAVE_SAWTOOTH, 120.5f,  3.0f,  0.0f,
                "RESULT,sawtooth,120.500,3.000,0.000\r\n");
    /* 评分规则 3 节的两个示例, 换算成三位小数后的样子 */
    check_frame(WAVE_SINE,     137.2f,  1.18f, 0.0f,
                "RESULT,sine,137.200,1.180,0.000\r\n");
    /* 非方波脏 duty 必须被兜底 */
    check_frame(WAVE_SINE,     137.2f,  1.18f, 0.555f,
                "RESULT,sine,137.200,1.180,0.000\r\n");

    /* ================================================================= *
     *  6c. 占空比钳位与异常输入
     * ================================================================= */
    puts("\n== 6c. 占空比钳位 ==");
    /* 交越法数错, 给出 1.3(=130%) -> 钳到 100.000 */
    check_frame(WAVE_SQUARE, 100.0f, 3.3f, 1.3f,
                "RESULT,square,100.000,3.300,100.000\r\n");
    /* 给出负占空比 -> 钳到 0.000 */
    check_frame(WAVE_SQUARE, 100.0f, 3.3f, -0.2f,
                "RESULT,square,100.000,3.300,0.000\r\n");
    /* 边界: 恰好 1.0 -> 100.000 */
    check_frame(WAVE_SQUARE, 100.0f, 3.3f, 1.0f,
                "RESULT,square,100.000,3.300,100.000\r\n");
#else
    puts("\n== 6b/6c 整帧比对已跳过(当前为非默认配置) ==");
#endif

    /* ================================================================= *
     *  6d. 容量不足时绝不输出半帧(协议 2.6 节)
     * ================================================================= */
    puts("\n== 6d. 容量不足的行为 ==");
    {
        result_t r;
        char     small[10];
        int      len = -1;
        int      rc;

        r.type = WAVE_TRIANGLE;
        r.freq = 150.25f;
        r.vpp  = 4.0f;
        r.duty = 0.0f;

        rc = report_format_result(small, (int)sizeof(small), &r, &len);
        if (rc == REPORT_ERR_CAPACITY && small[0] == '\0' && len == 0) {
            ++g_pass;
            puts("  [ OK ] 容量不足 -> 返回 ERR_CAPACITY 且输出空串");
        } else {
            ++g_fail;
            printf("  [FAIL] 容量不足  rc=%d buf=\"%s\" len=%d\n",
                   rc, small, len);
        }

        /* NULL 防御 */
        rc = report_format_result(NULL, 64, &r, NULL);
        if (rc == REPORT_ERR_NULL) {
            ++g_pass;
            puts("  [ OK ] dst=NULL -> 返回 ERR_NULL");
        } else {
            ++g_fail;
            printf("  [FAIL] dst=NULL  rc=%d\n", rc);
        }

        rc = report_send_result_checked(NULL);
        if (rc == REPORT_ERR_NULL) {
            ++g_pass;
            puts("  [ OK ] r=NULL   -> 返回 ERR_NULL, 未发送");
        } else {
            ++g_fail;
            printf("  [FAIL] r=NULL   rc=%d\n", rc);
        }
    }

    /* ================================================================= *
     *  7. 模式 B 原始数据帧
     * ================================================================= */
    puts("\n== 7. 模式 B 原始数据帧(20 点, 应在第 16 点后换行) ==");
    for (i = 0; i < 20; ++i) {
        raw[i] = (uint16_t)(2048 + i * 37);   /* 造一段递增的假采样 */
    }
    report_send_raw(raw, 20u, 20000u);

    /* 手工算一遍校验和, 与上面 #END 后的数字核对。 */
    {
        uint32_t sum = 0u;
        for (i = 0; i < 20; ++i) {
            sum += raw[i];
        }
        printf("   (期望 checksum = %lu)\n", (unsigned long)sum);
    }

    /* ================================================================= *
     *  汇总
     * ================================================================= */
    printf("\n=============================\n");
    printf("自动用例: %d 通过, %d 失败\n", g_pass, g_fail);
    printf("=============================\n");

    return (g_fail == 0) ? 0 : 1;
}
