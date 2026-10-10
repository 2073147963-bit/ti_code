/**
 * @file    pc_receiver.c
 * @brief   PC 端串口接收与解析程序(Windows)
 *
 * @details
 *  实现《数据通信部分算法》第 4 章。按文档的四层架构组织:
 *      4.2/4.3/4.4  串口驱动层  -> open_and_config() / close_port()
 *      4.5          帧同步层    -> feed_bytes()
 *      4.6          协议解析层  -> parse_result_line()
 *      4.7          数据服务层  -> store_result() + 环形缓冲区
 *
 *  同时能识别单片机的两种上报模式:
 *      结果帧   RESULT,sine,200.000,2.500,0.000
 *               (或无前缀的 sine,200.000,2.500,0.000)
 *      原始数据 #RAW,<n>,<fs>  ...  #END,<checksum>
 *               收齐后校验累加和, 可选落盘成 CSV 供 MATLAB/Python 分析
 *
 *  编译(MinGW):
 *      gcc -std=c99 -Wall -Wextra pc_receiver.c -o pc_receiver.exe
 *  编译(MSVC):
 *      cl /W4 pc_receiver.c
 *
 *  用法:
 *      pc_receiver.exe [COM口] [波特率] [原始数据输出文件]
 *  例:
 *      pc_receiver.exe COM5
 *      pc_receiver.exe COM7 115200 raw.csv
 *
 *  COM 口号用设备管理器确认: XDS110 会枚举出两个串口, 要选
 *  "XDS110 Class Application/User UART" 那一个。
 *
 *  Ctrl+C 退出。
 *
 * @date    2026-10-07
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===================================================================== *
 *  配置常量
 * ===================================================================== */

#define LINE_BUF_SIZE     4096      /* 行缓冲区。原始数据一行 16 个样点,
                                       远小于此; 留大是为了容错 */
#define READ_CHUNK_SIZE   512       /* 单次 ReadFile 的字节数 */
#define RESULT_RING_SIZE  32        /* 结果环形缓冲区容量(文档 4.7 节) */
#define MAX_RAW_SAMPLES   65536     /* 原始数据最多缓存的样点数 */

/* ===================================================================== *
 *  数据类型 —— 与单片机端 uart_report.h 的 result_t 字段一致
 * ===================================================================== */

typedef enum {
    WAVE_SINE = 0,
    WAVE_SQUARE,
    WAVE_TRIANGLE,
    WAVE_SAWTOOTH,
    WAVE_UNKNOWN                    /* 协议 2.8 节: 类别不在枚举内的兜底 */
} wave_type_t;

typedef struct {
    wave_type_t type;
    double      freq;               /* Hz   */
    double      vpp;                /* V    */
    double      duty;               /* %    */
} result_t;

/* ===================================================================== *
 *  全局状态
 * ===================================================================== */

static HANDLE   g_port = INVALID_HANDLE_VALUE;

/* --- 帧同步层状态(文档 4.5 节) --- */
static char     g_line[LINE_BUF_SIZE];
static int      g_line_len   = 0;
static int      g_line_ovf   = 0;   /* 本行是否已溢出, 溢出则整行丢弃 */

/* --- 数据服务层: 结果环形缓冲区(文档 4.7 节) --- */
static result_t g_ring[RESULT_RING_SIZE];
static int      g_ring_head  = 0;   /* 下一个写入位置 */
static int      g_ring_count = 0;   /* 已存条数, 上限 RESULT_RING_SIZE */
static long     g_total_ok   = 0;   /* 累计成功解析的结果帧数 */
static long     g_total_bad  = 0;   /* 累计丢弃的行数 */

/* --- 原始数据接收状态 --- */
static int      g_raw_active   = 0; /* 是否正在接收 #RAW ... #END 之间 */
static uint32_t g_raw_expect_n = 0; /* 帧头声明的样点数 */
static uint32_t g_raw_fs       = 0; /* 帧头声明的采样率 */
static uint32_t g_raw_got      = 0; /* 实收样点数 */
static uint32_t g_raw_sum      = 0; /* 实收样点的 32 位累加和 */
static uint16_t *g_raw_buf     = NULL;
static const char *g_raw_outfile = NULL;

/* ===================================================================== *
 *  一、串口驱动层(文档 4.2 / 4.3 / 4.4 节)
 * ===================================================================== */

/**
 * @brief 打开并配置串口。
 *
 * @param port_name  端口名, 如 "COM5"
 * @param baud       波特率, 如 115200
 * @return 0 成功, 非 0 失败
 */
static int open_and_config(const char *port_name, DWORD baud)
{
    char          full[32];
    DCB           dcb;
    COMMTIMEOUTS  to;

    /* COM10 及以上必须用 "\\\\.\\COM10" 这种前缀形式, CreateFile 才认。
       COM1~COM9 两种写法都行, 所以统一加前缀最省事。 */
    (void)snprintf(full, sizeof(full), "\\\\.\\%s", port_name);

    /* 只读打开 —— 本任务是单向上报, 不需要向单片机写。 */
    g_port = CreateFileA(full,
                         GENERIC_READ,
                         0,                 /* 独占, 不共享 */
                         NULL,
                         OPEN_EXISTING,
                         0,                 /* 同步(非 OVERLAPPED)I/O */
                         NULL);

    if (g_port == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[错误] 打开 %s 失败 (GetLastError=%lu)\n",
                port_name, (unsigned long)GetLastError());
        fprintf(stderr, "       请在设备管理器确认 COM 口号, 并确保该口\n");
        fprintf(stderr, "       没有被 CCS 终端或串口助手占用。\n");
        return 1;
    }

    /* ---- 读出当前配置, 只改需要改的字段 ---- */
    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(g_port, &dcb)) {
        fprintf(stderr, "[错误] GetCommState 失败\n");
        return 2;
    }

    /* ---- 与协议 2.1 节对齐: 115200, 8-N-1, 无流控 ---- */
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;

    /* 关闭所有流控。XDS110 虚拟串口不需要流控, 若开了 RTS/CTS 或
       XON/XOFF, 会出现"收一会儿就卡住"的现象。 */
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl  = DTR_CONTROL_ENABLE;
    dcb.fRtsControl  = RTS_CONTROL_ENABLE;
    dcb.fOutX        = FALSE;
    dcb.fInX         = FALSE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(g_port, &dcb)) {
        fprintf(stderr, "[错误] SetCommState 失败, 参数可能不被支持\n");
        return 3;
    }

    /* ---- 读超时(文档 4.4 节) ----
     * 目标: 无数据时 ReadFile 尽快返回, 由上层轮询, 不无限阻塞。
     * ReadTotalTimeoutConstant = 50 ms 意味着最坏 50 ms 返回一次, 既不
     * 空转烧 CPU, 也不会让结果延迟到肉眼可见。 */
    ZeroMemory(&to, sizeof(to));
    to.ReadIntervalTimeout        = 10;   /* 字节间隔超时 ms */
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant   = 50;   /* 总超时常数 ms */
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 0;

    if (!SetCommTimeouts(g_port, &to)) {
        fprintf(stderr, "[错误] SetCommTimeouts 失败\n");
        return 4;
    }

    /* 丢掉打开前积压在驱动缓冲里的旧数据, 避免一上来就解析到半截帧。 */
    (void)PurgeComm(g_port, PURGE_RXCLEAR | PURGE_RXABORT);

    printf("[信息] %s 已打开: %lu-8-N-1, 无流控\n",
           port_name, (unsigned long)baud);
    return 0;
}

/**
 * @brief 关闭串口, 释放资源(文档 4.8 节)。
 */
static void close_port(void)
{
    DWORD  errs = 0;
    COMSTAT st;

    if (g_port != INVALID_HANDLE_VALUE) {
        /* 清理串口错误标志。不清的话某些驱动下错误会黏住, 影响下次打开。*/
        (void)ClearCommError(g_port, &errs, &st);
        (void)CloseHandle(g_port);
        g_port = INVALID_HANDLE_VALUE;
        printf("[信息] 串口已关闭\n");
    }

    if (g_raw_buf != NULL) {
        free(g_raw_buf);
        g_raw_buf = NULL;
    }
}

/* ===================================================================== *
 *  二、协议解析层(文档 4.6 节)
 * ===================================================================== */

/** @brief 波形类别字符串 -> 枚举。无法识别返回 WAVE_UNKNOWN。 */
static wave_type_t str_to_wave(const char *s)
{
    if (strcmp(s, "sine")     == 0) return WAVE_SINE;
    if (strcmp(s, "square")   == 0) return WAVE_SQUARE;
    if (strcmp(s, "triangle") == 0) return WAVE_TRIANGLE;
    if (strcmp(s, "sawtooth") == 0) return WAVE_SAWTOOTH;
    return WAVE_UNKNOWN;            /* 协议 2.8 节第 5 条 */
}

/** @brief 枚举 -> 中文名, 仅用于控制台显示。 */
static const char *wave_to_cn(wave_type_t w)
{
    switch (w) {
        case WAVE_SINE:     return "正弦波";
        case WAVE_SQUARE:   return "方波";
        case WAVE_TRIANGLE: return "三角波";
        case WAVE_SAWTOOTH: return "锯齿波";
        default:            return "未知";
    }
}

/** @brief 原地去掉字符串首尾空白。 */
static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    if (*s == '\0') {
        return s;
    }

    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r')) {
        *end-- = '\0';
    }
    return s;
}

/**
 * @brief 把字符串整体转为 double, 要求整串都是合法数字。
 *
 * @param[in]  s    待转换字符串
 * @param[out] out  转换结果
 * @return 0 成功, 非 0 失败
 *
 * @note 用 strtod + 尾指针检查, 比 atof 严格 —— "1.2abc" 会被判失败,
 *       符合协议 2.8 节第 4 条"含非法字符则丢弃该帧"。
 */
static int str_to_double(const char *s, double *out)
{
    char   *endp = NULL;
    double  v;

    if (s == NULL || *s == '\0') {
        return 1;
    }

    v = strtod(s, &endp);

    /* endp 必须走到串尾(允许尾部空白, 但 trim 已处理) */
    if (endp == s || *endp != '\0') {
        return 2;
    }

    *out = v;
    return 0;
}

/**
 * @brief 解析一行结果帧。
 *
 * 可接受两种形式(单片机端 REPORT_FRAME_WITH_PREFIX 的两种取值):
 *     RESULT,<type>,<freq>,<vpp>,<duty>
 *     <type>,<freq>,<vpp>,<duty>
 *
 * @param[in]  line 行文本(会被 strtok_s 修改), 不含 \r\n
 * @param[out] out  解析结果
 * @return 0 成功, 非 0 失败
 */
static int parse_result_line(char *line, result_t *out)
{
    char       *fields[8];
    int         n = 0;
    char       *tok;
    char       *ctx = NULL;
    int         base;               /* 类别字段的下标 */
    wave_type_t t;
    double      freq, vpp, duty;

    /* ---- 协议 2.8 节第 1 条: 空行或纯空白行丢弃 ---- */
    line = trim(line);
    if (*line == '\0') {
        return 1;
    }

    /* ---- 按逗号拆分。Windows 线程安全版本是 strtok_s ---- */
#if defined(_MSC_VER)
    tok = strtok_s(line, ",", &ctx);
#else
    (void)ctx;
    tok = strtok(line, ",");
#endif
    while (tok != NULL && n < 8) {
        fields[n++] = trim(tok);
#if defined(_MSC_VER)
        tok = strtok_s(NULL, ",", &ctx);
#else
        tok = strtok(NULL, ",");
#endif
    }

    /* ---- 判断有无 "RESULT" 前缀, 定位类别字段 ---- */
    if (n >= 1 && strcmp(fields[0], "RESULT") == 0) {
        base = 1;
    } else {
        base = 0;
    }

    /* ---- 协议 2.8 节第 2 条: 字段不足则判为不完整帧, 丢弃 ----
     * 需要 类别 + 3 个数值 = base + 4 个字段。
     * 第 3 条"字段多于 4 个取前 4 个"自然满足 —— 多出来的不读。 */
    if (n < base + 4) {
        return 2;
    }

    /* ---- 字段 1: 波形类别 ---- */
    t = str_to_wave(fields[base + 0]);
    /* 注意: 类别为 unknown 时不丢帧。评分规则明确"类别识别得分与参数
       估计得分相互独立", 参数仍需照常显示, 所以这里保留整帧。 */

    /* ---- 字段 2/3/4: 三个数值。任一转换失败则整帧丢弃(协议 2.8 节 4) ---- */
    if (str_to_double(fields[base + 1], &freq) != 0) return 3;
    if (str_to_double(fields[base + 2], &vpp)  != 0) return 4;
    if (str_to_double(fields[base + 3], &duty) != 0) return 5;

    out->type = t;
    out->freq = freq;
    out->vpp  = vpp;
    out->duty = duty;
    return 0;
}

/* ===================================================================== *
 *  三、数据服务层(文档 4.7 节)
 * ===================================================================== */

/**
 * @brief 把结果存入环形缓冲区并打印到控制台。
 */
static void store_result(const result_t *r)
{
    g_ring[g_ring_head] = *r;
    g_ring_head = (g_ring_head + 1) % RESULT_RING_SIZE;
    if (g_ring_count < RESULT_RING_SIZE) {
        ++g_ring_count;
    }
    ++g_total_ok;

    /* 控制台展示。方波才显示占空比, 其余波形该字段按协议恒为 0, 无意义。*/
    printf("[#%ld] %-8s(%-6s)  f = %9.3f Hz   Vpp = %7.3f V",
           g_total_ok,
           (r->type == WAVE_SINE)     ? "sine"     :
           (r->type == WAVE_SQUARE)   ? "square"   :
           (r->type == WAVE_TRIANGLE) ? "triangle" :
           (r->type == WAVE_SAWTOOTH) ? "sawtooth" : "unknown",
           wave_to_cn(r->type),
           r->freq, r->vpp);

    if (r->type == WAVE_SQUARE) {
        printf("   duty = %6.3f %%", r->duty);
    }
    printf("\n");
    (void)fflush(stdout);
}

/* ===================================================================== *
 *  四、原始数据帧处理(模式 B)
 * ===================================================================== */

/** @brief 处理 "#RAW,<n>,<fs>" 帧头。 */
static void raw_begin(const char *line)
{
    uint32_t n = 0, fs = 0;

    if (sscanf(line, "#RAW,%u,%u", &n, &fs) != 2) {
        fprintf(stderr, "[警告] #RAW 帧头格式错误: %s\n", line);
        return;
    }

    if (n == 0 || n > MAX_RAW_SAMPLES) {
        fprintf(stderr, "[警告] #RAW 样点数 %u 超出范围(1~%d)\n",
                n, MAX_RAW_SAMPLES);
        return;
    }

    if (g_raw_buf == NULL) {
        g_raw_buf = (uint16_t *)malloc(sizeof(uint16_t) * MAX_RAW_SAMPLES);
        if (g_raw_buf == NULL) {
            fprintf(stderr, "[错误] 原始数据缓冲区分配失败\n");
            return;
        }
    }

    g_raw_active   = 1;
    g_raw_expect_n = n;
    g_raw_fs       = fs;
    g_raw_got      = 0;
    g_raw_sum      = 0;

    printf("[信息] 开始接收原始数据: %u 点 @ %u Hz\n", n, fs);
    (void)fflush(stdout);
}

/** @brief 处理原始数据体的一行(逗号分隔的若干样点)。 */
static void raw_data_line(char *line)
{
    char *tok;
    char *ctx = NULL;
    long  v;
    char *endp;

#if defined(_MSC_VER)
    tok = strtok_s(line, ",", &ctx);
#else
    (void)ctx;
    tok = strtok(line, ",");
#endif
    while (tok != NULL) {
        tok = trim(tok);
        if (*tok != '\0') {
            v = strtol(tok, &endp, 10);
            if (*endp == '\0' && v >= 0 && v <= 65535) {
                if (g_raw_got < g_raw_expect_n) {
                    g_raw_buf[g_raw_got++] = (uint16_t)v;
                    g_raw_sum += (uint32_t)v;
                }
            } else {
                fprintf(stderr, "[警告] 原始数据中有非法样点: \"%s\"\n", tok);
            }
        }
#if defined(_MSC_VER)
        tok = strtok_s(NULL, ",", &ctx);
#else
        tok = strtok(NULL, ",");
#endif
    }
}

/** @brief 处理 "#END,<checksum>" 帧尾, 校验并可选落盘。 */
static void raw_end(const char *line)
{
    uint32_t expect_sum = 0;
    FILE    *fp;
    uint32_t i;

    g_raw_active = 0;

    if (sscanf(line, "#END,%u", &expect_sum) != 1) {
        fprintf(stderr, "[警告] #END 帧尾格式错误: %s\n", line);
        return;
    }

    printf("[信息] 原始数据接收完毕: 收到 %u / %u 点\n",
           g_raw_got, g_raw_expect_n);

    if (g_raw_got != g_raw_expect_n) {
        fprintf(stderr, "[警告] 样点数不符, 可能丢字节\n");
    }

    if (g_raw_sum == expect_sum) {
        printf("[信息] 校验和一致 (%u), 数据完整\n", g_raw_sum);
    } else {
        fprintf(stderr, "[警告] 校验和不符: 收到累加 %u, 帧尾声明 %u\n",
                g_raw_sum, expect_sum);
    }

    /* ---- 可选落盘, 便于拿去 MATLAB / Python 跑算法 ---- */
    if (g_raw_outfile != NULL && g_raw_got > 0) {
        fp = fopen(g_raw_outfile, "w");
        if (fp == NULL) {
            fprintf(stderr, "[警告] 无法写入 %s\n", g_raw_outfile);
        } else {
            /* 第一行写采样率做注释, MATLAB/Python 读时跳过即可。 */
            fprintf(fp, "# fs_hz=%u n=%u\n", g_raw_fs, g_raw_got);
            for (i = 0; i < g_raw_got; ++i) {
                fprintf(fp, "%u\n", (unsigned)g_raw_buf[i]);
            }
            (void)fclose(fp);
            printf("[信息] 原始数据已保存到 %s\n", g_raw_outfile);
        }
    }
    (void)fflush(stdout);
}

/* ===================================================================== *
 *  五、帧同步层(文档 4.5 节)
 * ===================================================================== */

/**
 * @brief 把收到的一整行交给相应的处理分支。
 */
static void dispatch_line(char *line)
{
    result_t r;
    int      rc;

    /* 行内容可能被解析函数就地改写, 所以每个分支各自拿副本或按序处理。*/

    /* ---- '#' 开头的是控制/原始数据帧 ---- */
    if (line[0] == '#') {
        if (strncmp(line, "#RAW,", 5) == 0) {
            raw_begin(line);
        } else if (strncmp(line, "#END,", 5) == 0) {
            raw_end(line);
        } else {
            /* #BOOT 之类的提示行, 原样回显, 方便确认链路通了。 */
            printf("[单片机] %s\n", line);
            (void)fflush(stdout);
        }
        return;
    }

    /* ---- 正在接收原始数据期间, 非 '#' 行都是数据体 ---- */
    if (g_raw_active) {
        raw_data_line(line);
        return;
    }

    /* ---- 其余按结果帧解析 ---- */
    rc = parse_result_line(line, &r);
    if (rc == 0) {
        store_result(&r);
    } else {
        ++g_total_bad;
        /* 空行不值得报警(协议明确要求静默丢弃), 其它情况提示一下,
           便于调试时发现格式问题。 */
        if (rc != 1) {
            fprintf(stderr, "[丢弃] 第 %ld 个无效行 (code=%d)\n",
                    g_total_bad, rc);
        }
    }
}

/**
 * @brief 字节流 -> 行。文档 4.5 节步骤 2 的实现。
 *
 * @param buf  本批字节
 * @param n    字节数
 */
static void feed_bytes(const char *buf, DWORD n)
{
    DWORD i;
    char  c;

    for (i = 0; i < n; ++i) {
        c = buf[i];

        if (c == '\n') {
            /* ---- (a) 遇 LF: 当前缓冲即一个完整帧 ---- */
            if (!g_line_ovf) {
                g_line[g_line_len] = '\0';
                /* '\r' 已在下面的分支里跳过, 这里缓冲区末尾是干净的 */
                dispatch_line(g_line);
            } else {
                /* 这一行曾经溢出过, 内容已不可信, 整行丢弃 */
                ++g_total_bad;
                fprintf(stderr, "[丢弃] 行超长(> %d 字节)\n", LINE_BUF_SIZE - 1);
            }
            g_line_len = 0;
            g_line_ovf = 0;

        } else if (c == '\r') {
            /* ---- (b) 遇 CR: 忽略, 等随后的 LF ---- */
            continue;

        } else {
            /* ---- (c) 其它字节: 存入行缓冲 ---- */
            if (g_line_len < LINE_BUF_SIZE - 1) {
                g_line[g_line_len++] = c;
            } else {
                /* 溢出。打标记而不是立刻报错, 等到行结束再统一丢弃,
                   这样不会把后半截内容误当成新的一行。 */
                g_line_ovf = 1;
            }
        }
    }
}

/* ===================================================================== *
 *  六、主循环(文档 4.9 节)
 * ===================================================================== */

/** Ctrl+C 退出标志。 */
static volatile int g_quit = 0;

static BOOL WINAPI on_ctrl_c(DWORD type)
{
    (void)type;
    g_quit = 1;
    return TRUE;
}

int main(int argc, char **argv)
{
    const char *port = "COM5";
    DWORD       baud = 115200;
    char        tmp[READ_CHUNK_SIZE];
    DWORD       got;

    /* ---- 命令行参数 ---- */
    if (argc >= 2) {
        port = argv[1];
    }
    if (argc >= 3) {
        baud = (DWORD)strtoul(argv[2], NULL, 10);
    }
    if (argc >= 4) {
        g_raw_outfile = argv[3];
    }

    printf("====================================================\n");
    printf(" TI 杯赛道一 · 波形识别结果接收程序\n");
    printf(" 端口 %s  波特率 %lu  (Ctrl+C 退出)\n",
           port, (unsigned long)baud);
    if (g_raw_outfile != NULL) {
        printf(" 原始数据将保存到 %s\n", g_raw_outfile);
    }
    printf("====================================================\n");

    (void)SetConsoleCtrlHandler(on_ctrl_c, TRUE);

    /* ---- 1. 打开并配置串口 ---- */
    if (open_and_config(port, baud) != 0) {
        return 1;
    }

    /* ---- 2. 初始化行缓冲区 ---- */
    g_line_len = 0;
    g_line_ovf = 0;

    /* ---- 3. 轮询读取 ---- */
    while (!g_quit) {
        got = 0;
        if (!ReadFile(g_port, tmp, (DWORD)sizeof(tmp), &got, NULL)) {
            /* 读失败。清一下错误标志继续试, 不直接退出 ——
               USB 虚拟串口偶发错误比较常见。 */
            DWORD   errs = 0;
            COMSTAT st;
            (void)ClearCommError(g_port, &errs, &st);
            fprintf(stderr, "[警告] ReadFile 失败 (err=%lu), 继续\n",
                    (unsigned long)GetLastError());
            Sleep(100);
            continue;
        }

        /* got == 0 是读超时, 属正常现象(文档 4.8 节第 2 条), 不是错误。*/
        if (got > 0) {
            feed_bytes(tmp, got);
        }
    }

    /* ---- 4. 退出: 关闭串口, 释放资源 ---- */
    printf("\n[统计] 成功解析 %ld 帧, 丢弃 %ld 行\n", g_total_ok, g_total_bad);
    close_port();
    return 0;
}
