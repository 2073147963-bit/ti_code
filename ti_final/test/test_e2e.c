/**
 * @file    test_e2e.c
 * @brief   端到端联调自测: 单片机端生成的帧 -> PC 端解析, 不需要硬件
 *
 * @details
 *  把 uart_report.c 发出的字节流直接灌给 pc_receiver.c 的帧同步层和
 *  解析层, 验证两端对协议的理解完全一致。涵盖:
 *      - 四类波形的结果帧往返
 *      - 模式 B 原始数据帧的帧头/数据体/帧尾与校验和
 *      - 协议 2.8 节的五条容错规则
 *      - 行同步的边界: 只有 \n、连续空行、CRLF 被拆成两批到达
 *
 *  编译(MinGW), 在 ti_final 根目录执行:
 *      gcc -std=c99 -Wall -Wextra -DREPORT_SIM_STDOUT=1 \
 *          -Ifirmware -Ipc \
 *          test/test_e2e.c test/e2e_sender_shim.c firmware/uart_report.c \
 *          -o test_e2e.exe
 *
 *  注意: 本文件用 #include 的方式把 pc_receiver.c 拖进同一个翻译单元,
 *  以便复用其中的 static 函数, 所以不要把它加入正式工程的源文件列表。
 *  发送端则走 e2e_sender_shim.c 这个独立翻译单元(两端的 wave_type_t
 *  同名, 不能放一起), 详见下面的声明处注释。
 *
 * @date    2026-10-07
 */

#include <stdio.h>
#include <string.h>

/* ---- 把 pc_receiver 的 main 改名, 避免与本文件的 main 冲突 ---- */
#define main  receiver_main_unused
#include "pc_receiver.c"
#undef main

/* ---- 单片机端 ----
 *  uart_report.h 与 pc_receiver.c 各自定义了同名的 wave_type_t /
 *  result_t, 不能放进同一个翻译单元。所以发送端走 e2e_sender_shim.c
 *  这层薄壳: 它单独编译, 只用基础类型对外, 这里声明原型即可。
 *
 *  这样下面的"期望帧"不再是手写的字符串字面量, 而是 uart_report.c
 *  当场拼出来的真实输出 —— 发送端改坏了格式, 本测试会立刻失败,
 *  而不是两边照着同一份手写常量一起"通过"。            ---- */
int         sender_make_frame(char *dst, int cap, int type,
                              float freq, float vpp, float duty);
const char *sender_wave_word(int type);

/** @brief 发送端拼帧用的缓冲区大小, 与 REPORT_FRAME_BUF_SIZE 对齐。 */
#define SENDER_BUF  96

static int g_case_pass = 0;
static int g_case_fail = 0;

/** @brief 把一段文本当作串口收到的字节流灌入帧同步层。 */
static void inject(const char *s)
{
    feed_bytes(s, (DWORD)strlen(s));
}

/** @brief 断言当前累计解析成功/丢弃计数。 */
static void expect_counts(const char *what, long ok, long bad)
{
    if (g_total_ok == ok && g_total_bad == bad) {
        ++g_case_pass;
        printf("  [ OK ] %s  (ok=%ld bad=%ld)\n", what, ok, bad);
    } else {
        ++g_case_fail;
        printf("  [FAIL] %s  期望 ok=%ld bad=%ld, 实际 ok=%ld bad=%ld\n",
               what, ok, bad, g_total_ok, g_total_bad);
    }
}

/** @brief 断言环形缓冲区中最近一条结果的内容。 */
static void expect_last(const char *what, wave_type_t t,
                        double f, double v, double d)
{
    int       idx = (g_ring_head - 1 + RESULT_RING_SIZE) % RESULT_RING_SIZE;
    result_t *r   = &g_ring[idx];
    double    eps = 1e-6;

    if (r->type == t &&
        (r->freq - f) < eps && (f - r->freq) < eps &&
        (r->vpp  - v) < eps && (v - r->vpp)  < eps &&
        (r->duty - d) < eps && (d - r->duty) < eps) {
        ++g_case_pass;
        printf("  [ OK ] %s\n", what);
    } else {
        ++g_case_fail;
        printf("  [FAIL] %s  得到 type=%d f=%.3f v=%.3f d=%.3f\n",
               what, (int)r->type, r->freq, r->vpp, r->duty);
    }
}

/**
 * @brief 完整往返一帧: 发送端拼帧 -> 喂给接收端 -> 核对解析结果。
 *
 * @param what    用例名, 打印用
 * @param type    发送端的波形类别码(0=sine 1=square 2=triangle 3=sawtooth)
 * @param freq    发送的频率 Hz
 * @param vpp     发送的峰峰值 V
 * @param duty    发送的占空比, 归一化 0~1
 * @param et      期望接收端解出的类别
 * @param ef      期望接收端解出的频率
 * @param ev      期望接收端解出的峰峰值
 * @param ed      期望接收端解出的占空比(百分数)
 */
static void roundtrip(const char *what, int type,
                      float freq, float vpp, float duty,
                      wave_type_t et, double ef, double ev, double ed)
{
    char frame[SENDER_BUF];
    int  len = sender_make_frame(frame, (int)sizeof(frame),
                                 type, freq, vpp, duty);

    if (len <= 0) {
        ++g_case_fail;
        printf("  [FAIL] %s  发送端拼帧失败\n", what);
        return;
    }

    /* 打印发送端的真实输出, 便于出错时一眼看出是哪边的问题。
       末尾两个字符是 \r\n, 不打印出来免得乱跳行。 */
    printf("  发送端输出: \"%.*s\\r\\n\"\n", len - 2, frame);

    feed_bytes(frame, (DWORD)len);
    expect_last(what, et, ef, ev, ed);
}

int main(void)
{
    g_line_len = 0;
    g_line_ovf = 0;

    /* ================================================================= *
     *  1. 四类波形结果帧往返 —— 真·闭环
     *
     *  帧不是手写的, 而是调 uart_report.c 当场拼出来的。所以这一节
     *  同时验证了三件事:
     *      a. 发送端拼出的帧确实符合协议;
     *      b. 接收端能正确解析发送端的输出;
     *      c. 往返之后数值没有走样(发 0.682 -> 收 68.2)。
     *
     *  注意 duty 入参是归一化的 0~1(REPORT_DUTY_INPUT_RATIO=1),
     *  而期望收到的是百分数(REPORT_DUTY_PERCENT=1), 所以方波
     *  发 0.682 应该收到 68.2 —— 这个换算也在闭环里被验证了。
     * ================================================================= */
    puts("== 1. 结果帧往返(发送端真实输出 -> 接收端) ==");

    roundtrip("sine 帧",     0, 200.0f,  2.5f,  0.0f,
              WAVE_SINE,     200.0,  2.5,  0.0);
    roundtrip("square 帧",   1, 223.4f,  1.56f, 0.682f,
              WAVE_SQUARE,   223.4,  1.56, 68.2);
    roundtrip("triangle 帧", 2, 150.25f, 4.0f,  0.0f,
              WAVE_TRIANGLE, 150.25, 4.0,  0.0);
    roundtrip("sawtooth 帧", 3, 120.5f,  3.0f,  0.0f,
              WAVE_SAWTOOTH, 120.5,  3.0,  0.0);

    expect_counts("前 4 帧全部成功", 4, 0);

    /* 两端的类别用词必须逐字符一致 —— 这是最容易出岔子的地方:
     * 发送端写 "sine"、接收端比 "sin" 的话, 正弦波永远识别不出来,
     * 而且因为其它三类都对, 很容易被误判成"算法把正弦认错了"。 */
    puts("\n== 1b. 两端类别用词一致性 ==");
    {
        static const struct { int code; wave_type_t t; } words[] = {
            { 0, WAVE_SINE }, { 1, WAVE_SQUARE },
            { 2, WAVE_TRIANGLE }, { 3, WAVE_SAWTOOTH },
        };
        size_t k;
        for (k = 0; k < sizeof(words) / sizeof(words[0]); ++k) {
            /* 发送端会写出的单词 */
            const char *word = sender_wave_word(words[k].code);
            /* 接收端拿这个单词反查枚举 */
            char        tmp[32];
            wave_type_t back;

            strncpy(tmp, word, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            back = str_to_wave(tmp);

            if (back == words[k].t) {
                ++g_case_pass;
                printf("  [ OK ] \"%s\" 两端一致\n", word);
            } else {
                ++g_case_fail;
                printf("  [FAIL] \"%s\" 接收端反查得到 %d, 期望 %d\n",
                       word, (int)back, (int)words[k].t);
            }
        }
    }

    /* ================================================================= *
     *  2. 无前缀格式(REPORT_FRAME_WITH_PREFIX=0 时的输出)也要能收
     * ================================================================= */
    puts("\n== 2. 无前缀格式兼容 ==");
    inject("sine,137.200,1.180,0.000\r\n");
    expect_last("无前缀 sine 帧", WAVE_SINE, 137.2, 1.18, 0.0);
    expect_counts("无前缀帧被接受", 5, 0);

    /* ================================================================= *
     *  3. 行同步边界
     * ================================================================= */
    puts("\n== 3. 行同步边界 ==");

    /* 3.1 只有 LF 没有 CR(串口助手有时会这样发) */
    inject("RESULT,square,100.000,3.300,50.000\n");
    expect_last("仅 LF 结尾", WAVE_SQUARE, 100.0, 3.3, 50.0);

    /* 3.2 一帧被拆成三批到达 —— 模拟 ReadFile 在帧中间返回 */
    inject("RESULT,tri");
    inject("angle,99.");
    inject("500,1.000,0.000\r\n");
    expect_last("跨 3 批到达的帧", WAVE_TRIANGLE, 99.5, 1.0, 0.0);

    /* 3.3 CR 与 LF 分两批到达 */
    inject("RESULT,sine,1.000,1.000,0.000\r");
    inject("\n");
    expect_last("CR/LF 分批到达", WAVE_SINE, 1.0, 1.0, 0.0);

    expect_counts("边界用例全部成功", 8, 0);

    /* ================================================================= *
     *  4. 协议 2.8 节容错规则
     * ================================================================= */
    puts("\n== 4. 容错规则(协议 2.8 节) ==");

    /* 4.1 空行与纯空白行: 丢弃, 且不报警 */
    inject("\r\n");
    inject("   \t  \r\n");
    expect_counts("空行被静默丢弃", 8, 2);

    /* 4.2 字段不足 4 个: 判为不完整帧, 丢弃 */
    inject("RESULT,sine,200.000\r\n");
    expect_counts("字段不足被丢弃", 8, 3);

    /* 4.3 字段多于 4 个: 取前 4 个, 忽略多余 */
    inject("RESULT,sine,200.000,2.500,0.000,99,88\r\n");
    expect_last("多余字段被忽略", WAVE_SINE, 200.0, 2.5, 0.0);
    expect_counts("多字段帧被接受", 9, 3);

    /* 4.4 数字字段含非法字符: 丢弃该帧 */
    inject("RESULT,sine,20x.000,2.500,0.000\r\n");
    expect_counts("非法数字被丢弃", 9, 4);

    /* 4.5 波形类别不在枚举内: 标记为 unknown, 参数照常解析
     *     (评分规则: 类别识别与参数估计得分相互独立) */
    inject("RESULT,noise,200.000,2.500,0.000\r\n");
    expect_last("未知类别标记 unknown", WAVE_UNKNOWN, 200.0, 2.5, 0.0);
    expect_counts("未知类别帧仍被接受", 10, 4);

    /* ================================================================= *
     *  5. 模式 B 原始数据帧
     * ================================================================= */
    puts("\n== 5. 模式 B 原始数据帧 ==");

    /* 下面这段就是 test_uart_report.exe 第 7 节的真实输出。
       20 个样点, 2048 起、步长 37, 累加和 47990。 */
    inject("#RAW,20,20000\r\n");
    inject("2048,2085,2122,2159,2196,2233,2270,2307,"
           "2344,2381,2418,2455,2492,2529,2566,2603\r\n");
    inject("2640,2677,2714,2751\r\n");
    inject("#END,47990\r\n");

    if (g_raw_got == 20u && g_raw_sum == 47990u && g_raw_active == 0) {
        ++g_case_pass;
        printf("  [ OK ] 原始数据 20 点收齐, 校验和一致\n");
    } else {
        ++g_case_fail;
        printf("  [FAIL] 原始数据: got=%u sum=%u active=%d\n",
               g_raw_got, g_raw_sum, g_raw_active);
    }

    /* 原始数据体不应被误计入结果帧统计 */
    expect_counts("原始数据不污染结果计数", 10, 4);

    /* 原始数据接收结束后, 结果帧应能正常恢复解析 */
    inject("RESULT,sawtooth,321.000,2.000,0.000\r\n");
    expect_last("原始数据之后恢复解析", WAVE_SAWTOOTH, 321.0, 2.0, 0.0);
    expect_counts("恢复后计数正确", 11, 4);

    /* ================================================================= *
     *  6. #BOOT 提示行不应被当成结果帧
     * ================================================================= */
    puts("\n== 6. 控制行 ==");
    inject("#BOOT,MSPM0G3507 wave recognizer ready\r\n");
    expect_counts("#BOOT 既不计成功也不计丢弃", 11, 4);

    /* ================================================================= *
     *  汇总
     * ================================================================= */
    printf("\n=============================\n");
    printf("端到端用例: %d 通过, %d 失败\n", g_case_pass, g_case_fail);
    printf("=============================\n");

    if (g_raw_buf != NULL) {
        free(g_raw_buf);
        g_raw_buf = NULL;
    }

    return (g_case_fail == 0) ? 0 : 1;
}
