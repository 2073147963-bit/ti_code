/**
 * @file    uart_report.c
 * @brief   MSPM0G3507 波形识别结果 UART 上报模块 —— 实现
 *
 * @details
 *  实现《数据通信部分算法》第 3 章"单片机发送端"的全部内容:
 *      3.2 数据类型定义        -> uart_report.h
 *      3.3 波形类别到字符串    -> wave_to_str()
 *      3.4 浮点三位小数格式化  -> format3dec()
 *      3.5 帧拼接与发送        -> report_send_result()
 *      3.6 调用时机            -> 见 main_template.c 注释
 *
 *  设计约束:
 *    1. 全程不用 printf/sprintf —— Cortex-M0+ 无 FPU, 软浮点格式化既慢
 *       又可能因链接了精简版 libc 而输出乱码。
 *    2. 浮点运算只在"输入换算"一处出现(乘 1000 并四舍五入), 之后全是
 *       整数运算。
 *    3. 发送阻塞但时间可控, 不在中断里调用。
 *
 * @date    2026-10-07
 */

#include "uart_report.h"

#if REPORT_SIM_STDOUT
/* ---- PC 仿真: 底层发送重定向到 stdout, 用于离线验证格式化逻辑 ---- */
#include <stdio.h>
#else
/* ---- 真机: 依赖 SysConfig 生成的外设句柄与 MSPM0 DriverLib ---- */
#include "ti_msp_dl_config.h"
#endif

/* ===================================================================== *
 *  模块内部状态
 * ===================================================================== */

/** 模式 B 的累加校验和。report_raw_begin() 清零, 每 put 一点累加一次。 */
static uint32_t s_raw_checksum = 0u;

/** 模式 B 已发送的样点计数, 用于每 16 点插入换行, 便于人眼核对。 */
static uint32_t s_raw_count = 0u;

/** 模式 B 每行样点数。只影响可读性, 不影响协议解析(上位机按逗号拆分)。 */
#define RAW_SAMPLES_PER_LINE   16u

/* ===================================================================== *
 *  一、底层发送原语
 * ===================================================================== */

void uart_send_bytes(const uint8_t *data, uint32_t len)
{
    /* 入参防御: 空指针或零长度视为"无事可做", 静默返回。
       这样上层即使在异常分支里误调也不会跑飞。 */
    if ((data == NULL) || (len == 0u)) {
        return;
    }

#if REPORT_SIM_STDOUT
    /* PC 仿真分支: 逐字节写 stdout。 */
    for (uint32_t i = 0u; i < len; ++i) {
        (void)fputc((int)data[i], stdout);
    }
#else
    /* 真机分支: DriverLib 逐字节阻塞发送。
     *
     * 关于 API 名字: SDK 里真正的实现叫 DL_UART_transmitDataBlocking(),
     * 带 _Main_ 的那个是它的 #define 别名, 两种写法完全等价。
     * mspm0g350x.h 同时定义了 __MSPM0_HAS_UART_MAIN__, 所以 UART0 用哪种
     * 拼法都能编过。这里用 _Main_ 版本, 与 SysConfig 生成的示例风格一致。
     * (若你的 SDK 版本报 undefined, 去掉 Main_ 即可)
     *
     * DL_UART_transmitDataBlocking() 的实现等价于:
     *     while (DL_UART_isTXFIFOFull(uart)) { }   // 等 FIFO 有空位
     *     DL_UART_transmitData(uart, data);        // 写入
     *     while (DL_UART_isBusy(uart)) { }         // 等字节移出移位寄存器
     * 即它每字节都等到完全发完才返回, 所以 FIFO 实际没被用上。
     *
     * 为什么不用 DL_UART_fillTXFIFO() 批量填 FIFO?
     * 因为发送耗时由波特率决定, 不由 CPU 决定: 115200 bps / 10 bit每字节
     * = 11520 字节/秒, 20 KB 原始数据无论如何都要 1.74 s。FIFO 只能省下
     * CPU 的忙等时间, 而我们在发送期间本来也没有别的事要做(采样已结束)。
     * 换成 fillTXFIFO 要处理"返回值小于请求数时继续填"的循环, 增加出错
     * 面却换不到墙上时间, 不值得。
     */
    for (uint32_t i = 0u; i < len; ++i) {
        DL_UART_Main_transmitDataBlocking(REPORT_UART_INST, data[i]);
    }

    /* 再确认一次线路空闲。
     * 上面的 Blocking 版本内部已经等过 isBusy, 所以这一句通常立刻返回,
     * 属于冗余保险 —— 保留它是为了: 万一有人按上面注释把循环体换成
     * 非阻塞的 DL_UART_transmitData(), 这里仍能保证末字节完整送出。
     * 协议明确要求"不得只发送半帧", 半帧会让上位机的行同步错乱。 */
    while (DL_UART_Main_isBusy(REPORT_UART_INST)) {
        /* 忙等。一个字节 @115200 约 87 us。 */
    }
#endif
}

void uart_send_str(const char *s)
{
    uint32_t len = 0u;

    if (s == NULL) {
        return;
    }

    /* 自行求长, 不引入 <string.h>, 保持模块零依赖。 */
    while (s[len] != '\0') {
        ++len;
    }

    uart_send_bytes((const uint8_t *)s, len);
}

/* ===================================================================== *
 *  二、波形类别 -> 字符串(《数据通信部分算法》3.3 节)
 * ===================================================================== */

const char *wave_to_str(wave_type_t w)
{
    /* 用 switch-case 而非数组下标, 原因:
       1. 编译器能检查枚举是否漏分支;
       2. 非法枚举值(如上层算法返回越界)天然落到 default, 不会越界读。 */
    switch (w) {
        case WAVE_SINE:     return "sine";
        case WAVE_SQUARE:   return "square";
        case WAVE_TRIANGLE: return "triangle";
        case WAVE_SAWTOOTH: return "sawtooth";
        default:
            /* 走到这里说明上层 classify() 返回了越界枚举 —— 正常不会发生。
             *
             * "unknown" 是《数据通信部分算法》2.8 节第 5 条列出的值, 接收端
             * 认得; 但《初赛评分规则》3 节只允许四个小写单词, 输出 unknown
             * 类别项必定 0 分。两份文档在这里是冲突的, 做成开关:
             * 默认报 unknown(诚实, 好调试), 需要搏概率时改宏退化成 sine。
             *
             * 注意无论输出哪个, 三个数值字段都照常输出 —— 评分规则 1 节
             * 明确"类别识别得分与参数估计得分相互独立", 类别错了频率和
             * 峰峰值仍然按实际误差计分, 所以绝不能因为类别未知就不发帧。 */
#if REPORT_UNKNOWN_FALLBACK_SINE
            return "sine";
#else
            return "unknown";
#endif
    }
}

/* ===================================================================== *
 *  三、浮点三位小数格式化(《数据通信部分算法》3.4 节)
 * ===================================================================== */

/**
 * @brief 把无符号整数按十进制写入缓冲区, 可指定最少位数(左补 '0')。
 *
 * @param[out] dst       目标缓冲区
 * @param[in]  value     待转换的值
 * @param[in]  min_width 最少输出位数, 不足则左补 '0'(用于 "%03ld" 效果)
 * @return 写入的字符个数
 *
 * @note 内部静态函数, 纯整数运算, 不写 '\0'(由调用方负责)。
 */
static int u32_to_dec(char *dst, uint32_t value, int min_width)
{
    char    tmp[10];           /* uint32_t 最大 4294967295, 10 位 */
    int     n = 0;
    int     i;

    /* 逆序取各位十进制数字。value == 0 时至少产出一个 '0'。 */
    do {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    /* 左补 '0' 到 min_width。这是 "%03ld" 中补零语义的手工实现。 */
    while (n < min_width) {
        tmp[n++] = '0';
    }

    /* 反转写入目标缓冲区。 */
    for (i = 0; i < n; ++i) {
        dst[i] = tmp[n - 1 - i];
    }

    return n;
}

int format3dec(char *dst, float v)
{
    int      pos = 0;          /* 已写入字符数 */
    int      negative;         /* 是否需要输出负号 */
    double   scaled;           /* v * 1000 并偏移半个 LSB 后的值 */
    int64_t  t;                /* round(v * 1000), 用 64 位避免中间溢出 */
    uint32_t ip;               /* 整数部分 */
    uint32_t fp;               /* 小数部分, 恒为 0~999 */

    if (dst == NULL) {
        return 0;
    }

    /* ---- 步骤 0: 异常值防御 ----
     * NaN 的特性: 与自身比较也为假。Inf 则超出我们关心的量程。
     * 频率/峰峰值/占空比一旦出现这类值说明上游算法出了问题, 这里输出
     * "0.000" 保证帧结构完整 —— 协议规定不得发半帧, 宁可值错也不能
     * 让上位机的行同步乱掉。 */
    if (!(v == v)) {                       /* NaN */
        dst[0] = '0'; dst[1] = '.';
        dst[2] = '0'; dst[3] = '0'; dst[4] = '0'; dst[5] = '\0';
        return 5;
    }

    /* ---- 步骤 1: 判符号, 转为正值处理 ----
     * 先记下符号再取绝对值, 可以让后面的四舍五入和取模都在正数域进行,
     * 避开 C 语言中负数整除/取模的实现相关行为。 */
    negative = (v < 0.0f) ? 1 : 0;
    if (negative) {
        v = -v;
    }

    /* ---- 量程保护 ----
     * 本赛题频率 100~400 Hz、峰峰值几伏、占空比 0~100, 正常值远小于此。
     *
     * 钳位到 2^31 - 1 毫单位(约 2.147e6), 理由:
     *   - 上限对应整数部分最多 10 位, 正好匹配 REPORT_NUM_BUF_SIZE = 16
     *     (负号 1 + 整数 10 + '.' 1 + 小数 3 + '\0' 1), 缓冲区不会被写爆;
     *   - t 用 int64_t 存, 乘 1000 这一步本身不会溢出, 钳位纯粹是为了
     *     约束输出字符串的长度, 不是为了防算术溢出。
     *
     * 走到钳位分支一定意味着上游算法出了问题(正常值差了四个数量级),
     * 此时输出什么都是错的, 但帧结构必须完整 —— 协议 2.6 节规定不得
     * 发半帧, 半帧会让接收端的行同步错乱, 影响后续所有帧。
     * Inf 也在这里被一并钳住: Inf > 上限恒为真。 */
    if (!(v <= 2147483.647f)) {        /* 写成否定式, 顺手把 Inf 收进来 */
        v = 2147483.647f;
    }

    /* ---- 步骤 2: 放大 1000 倍并四舍五入 ----
     * 加 0.5 后向零取整, 即为"四舍五入到小数点后三位"。
     * 因为上面已取绝对值, 这里统一加 0.5 即可(文档所述"按正负分别
     * 加 0.5 或减 0.5"在取绝对值后合并为一种情形)。
     *
     * 这一步用 double 而非 float: float 只有 24 位有效位(约 7 位十进制),
     * 乘 1000 后若结果超过 2^24 ≈ 1.68e7, 末位就会丢精度 —— 例如
     * 123456.789f * 1000.0f 会算出 123456792 而非 123456789。
     * 用 double 做这一次乘法即可彻底避开, 代价是每帧 3 次软件双精度
     * 乘法(Cortex-M0+ 无 FPU), 量级在几微秒, 对 10 s 的赛题限时可忽略。
     * 本赛题频率 100~400 Hz、峰峰值几伏, 实际远在 float 精度范围内,
     * 这里只是把边界情形也做对。 */
    scaled = (double)v * 1000.0 + 0.5;
    t      = (int64_t)scaled;              /* 向零取整 */

    /* ---- 步骤 3/4: 拆整数部分与小数部分 ---- */
    ip = (uint32_t)(t / 1000);
    fp = (uint32_t)(t % 1000);             /* 必然落在 0~999 */

    /* ---- 步骤 5: 负号处理, 含"负零"情形 ----
     * v = -0.004 时 ip == 0, 若此处不补负号就会输出 "0.004", 符号丢失。
     * 但 v = -0.0001 四舍五入后 t == 0(ip 与 fp 全为 0), 此时输出
     * "-0.000" 没有意义, 故要求 t != 0 才补负号。 */
    if (negative && (t != 0)) {
        dst[pos++] = '-';
    }

    /* ---- 步骤 6: 拼接 "<ip>.<fp 三位>" ---- */
    pos += u32_to_dec(&dst[pos], ip, 1);   /* 整数部分至少 1 位 */
    dst[pos++] = '.';
    pos += u32_to_dec(&dst[pos], fp, 3);   /* 小数部分固定 3 位, 不足补零 */
    dst[pos]   = '\0';

    return pos;
}

/* ===================================================================== *
 *  四、模块初始化
 * ===================================================================== */

void report_init(void)
{
    /* UART 外设由 SysConfig 生成的 SYSCFG_DL_init() 负责初始化,
       本函数只复位模式 B 的内部状态。 */
    s_raw_checksum = 0u;
    s_raw_count    = 0u;
}

/* ===================================================================== *
 *  五、模式 A: 结果帧拼接与发送(《数据通信部分算法》3.5 节)
 * ===================================================================== */

/**
 * @brief 把源字符串追加到目标缓冲区, 带剩余容量检查。
 *
 * @param[out] dst       目标缓冲区
 * @param[in,out] pos    当前写入位置, 成功后被推进
 * @param[in]  cap       dst 总容量(含结尾 '\0' 的位置)
 * @param[in]  src       待追加字符串
 * @return 1 追加成功; 0 容量不足, 未写入任何字符
 *
 * @note REPORT_FRAME_BUF_SIZE = 96 已覆盖最坏情况(见头文件里的逐项
 *       计算), 所以返回 0 这条分支在正常使用下走不到。保留检查是因为
 *       report_format_result() 允许调用方自带缓冲区并指定 cap, 容量由
 *       调用方决定时就必须真的检查。宁可不发也不发半帧(协议 2.6 节)。
 */
static int str_append(char *dst, int *pos, int cap, const char *src)
{
    int i    = 0;
    int need = 0;

    while (src[need] != '\0') {
        ++need;
    }

    /* 留 1 字节给结尾 '\0'。 */
    if ((*pos + need) >= cap) {
        return 0;
    }

    for (i = 0; i < need; ++i) {
        dst[*pos + i] = src[i];
    }
    *pos += need;
    dst[*pos] = '\0';

    return 1;
}

/**
 * @brief 按配置把上层给的 duty 换算成协议要求的输出值。
 *
 * 分三步, 顺序不能换:
 *   1. 单位换算(0~1 <-> 0~100), 由两个宏的组合决定;
 *   2. 非方波兜底置 0 —— 必须在换算之后, 否则 0 乘 100 还是 0, 看不出
 *      区别, 但万一将来换算改成"加偏置"就会出错, 放后面更稳;
 *   3. 钳位到合法区间。
 *
 * @param type  波形类别
 * @param duty  上层填的原始值
 * @return 可以直接交给 format3dec() 的值
 */
static float duty_to_output(wave_type_t type, float duty)
{
    float d = duty;

    /* ---- 步骤 1: 输入/输出单位换算 ----
     * 两个宏一致时下面两个分支都不编译, 零开销。 */
#if (REPORT_DUTY_INPUT_RATIO == 1) && (REPORT_DUTY_PERCENT == 1)
    d *= 100.0f;               /* 上层给 0~1, 要输出 0~100 */
#elif (REPORT_DUTY_INPUT_RATIO == 0) && (REPORT_DUTY_PERCENT == 0)
    d *= 0.01f;                /* 上层给 0~100, 要输出 0~1 */
#endif

    /* ---- 步骤 2: 非方波兜底 ----
     * 协议 2.4 节: "方波为实际值, 其余波形输出 0"。
     * 由通信层兜底, 上层算法即便在非方波分支留了脏值也不违反协议。 */
#if REPORT_FORCE_ZERO_DUTY
    if (type != WAVE_SQUARE) {
        d = 0.0f;
    }
#else
    (void)type;
#endif

    /* ---- 步骤 3: 钳位到合法区间 ----
     * 占空比的物理意义就是"一个周期里高电平占的比例", 必定在 [0, 1]。
     * 交越法在信号质量差时可能数出越界的结果(比如把噪声跳变当成边沿,
     * 数出 1.3 个周期的高电平), 送出 130.000 这种值反而比送 100.000
     * 误差更大。钳位不会让正确结果变差, 只会让错误结果少错一点。
     *
     * NaN 不在这里处理: NaN 与任何值比较都为假, 两个 if 都走不进去,
     * 最终由 format3dec() 的 NaN 分支输出 "0.000"。 */
    {
        const float lo = 0.0f;
#if REPORT_DUTY_PERCENT
        const float hi = 100.0f;
#else
        const float hi = 1.0f;
#endif
        if (d < lo) { d = lo; }
        if (d > hi) { d = hi; }
    }

    return d;
}

int report_format_result(char *dst, int cap, const result_t *r, int *out_len)
{
    char  num[REPORT_NUM_BUF_SIZE];        /* 单个数值的临时缓冲区 */
    int   pos = 0;
    int   ok  = 1;
    float duty_out;

    if (out_len != NULL) {
        *out_len = 0;
    }
    if ((dst == NULL) || (r == NULL) || (cap <= 0)) {
        return REPORT_ERR_NULL;
    }

    dst[0] = '\0';

    /* ---- 字段 0: 可选的 "RESULT" 前缀 ---- */
#if REPORT_FRAME_WITH_PREFIX
    ok = ok && str_append(dst, &pos, cap, "RESULT,");
#endif

    /* ---- 字段 1: 波形类别(英文小写, 纯 ASCII, 避免中文乱码) ---- */
    ok = ok && str_append(dst, &pos, cap, wave_to_str(r->type));

    /* ---- 字段 2: 频率 Hz, 三位小数 ---- */
    (void)format3dec(num, r->freq);
    ok = ok && str_append(dst, &pos, cap, ",");
    ok = ok && str_append(dst, &pos, cap, num);

    /* ---- 字段 3: 峰峰值 V, 三位小数 ---- */
    (void)format3dec(num, r->vpp);
    ok = ok && str_append(dst, &pos, cap, ",");
    ok = ok && str_append(dst, &pos, cap, num);

    /* ---- 字段 4: 占空比 ---- */
    duty_out = duty_to_output(r->type, r->duty);

#if REPORT_DUTY_BARE_ZERO
    /* 兜底成 0 的非方波输出裸 "0", 与《初赛评分规则》示例逐字符一致。
     * 判 duty_out == 0.0f 而不是判波形类别, 这样"方波实测占空比恰好
     * 为 0"也会输出 "0" —— 数值等价, 不影响评分。 */
    if (duty_out == 0.0f) {
        num[0] = '0';
        num[1] = '\0';
    } else {
        (void)format3dec(num, duty_out);
    }
#else
    (void)format3dec(num, duty_out);
#endif
    ok = ok && str_append(dst, &pos, cap, ",");
    ok = ok && str_append(dst, &pos, cap, num);

    /* ---- 帧结束符: \r\n (CR + LF, 0x0D 0x0A) ----
     * 协议 2.6 节规定。CCS 终端、串口助手、PC 接收端的行同步都以 '\n'
     * 为边界, '\r' 由接收端丢弃。 */
    ok = ok && str_append(dst, &pos, cap, "\r\n");

    if (!ok) {
        /* 容量不足 —— 宁可不发也不发半帧(协议 2.6 节)。
         * 把 dst 置空串, 调用方即便忽略返回值也不会发出残帧。 */
        dst[0] = '\0';
        return REPORT_ERR_CAPACITY;
    }

    if (out_len != NULL) {
        *out_len = pos;
    }
    return REPORT_OK;
}

int report_send_result_checked(const result_t *r)
{
    char buf[REPORT_FRAME_BUF_SIZE];
    int  len = 0;
    int  rc;

    rc = report_format_result(buf, (int)sizeof(buf), r, &len);
    if (rc != REPORT_OK) {
        return rc;
    }

    /* 整帧一次送出。用 bytes 版而非 str 版, 省掉一次求长。 */
    uart_send_bytes((const uint8_t *)buf, (uint32_t)len);
    return REPORT_OK;
}

void report_send_result(const result_t *r)
{
    (void)report_send_result_checked(r);
}

void report_send_comment(const char *text)
{
    uart_send_str("#");
    if (text != NULL) {
        uart_send_str(text);
    }
    uart_send_str("\r\n");
}

void report_send_values(wave_type_t type, float freq, float vpp, float duty)
{
    result_t r;

    r.type = type;
    r.freq = freq;
    r.vpp  = vpp;
    r.duty = duty;

    report_send_result(&r);
}

/* ===================================================================== *
 *  六、模式 B: 原始采样数据上传(核心算法放上位机时使用)
 * ===================================================================== *
 *  帧结构:
 *      #RAW,<n>,<fs>\r\n          帧头: 样点数、采样率
 *      <code>,<code>,...\r\n      数据体: ASCII 十进制, 逗号分隔,
 *                                 每 16 点换行(仅为可读性)
 *      #END,<checksum>\r\n        帧尾: 样点码值的 32 位累加和
 *
 *  上位机侧: 读到 "#RAW" 开始缓存, 读到 "#END" 比对校验和。
 *  "#" 开头与结果帧(字母或 "RESULT")天然区分, 两种模式可混在同一串口。
 * ===================================================================== */

/**
 * @brief 把无符号整数转成字符串并发出(内部辅助)。
 * @param v  待发送的值
 */
static void send_u32(uint32_t v)
{
    char num[REPORT_NUM_BUF_SIZE];
    int  n;

    n = u32_to_dec(num, v, 1);
    num[n] = '\0';
    uart_send_str(num);
}

void report_raw_begin(uint32_t n_samples, uint32_t fs_hz)
{
    /* 复位累加器 —— 必须在任何 put 之前, 否则校验和会带上一轮的残留。 */
    s_raw_checksum = 0u;
    s_raw_count    = 0u;

    uart_send_str("#RAW,");
    send_u32(n_samples);
    uart_send_str(",");
    send_u32(fs_hz);
    uart_send_str("\r\n");
}

void report_raw_put(uint16_t code)
{
    /* 累加校验和。故意让 uint32_t 自然溢出回绕 —— 上位机做同样的
       32 位无符号累加即可对齐, 无需关心溢出。 */
    s_raw_checksum += (uint32_t)code;

    /* 样点之间用逗号分隔, 但行首不加逗号。 */
    if ((s_raw_count % RAW_SAMPLES_PER_LINE) != 0u) {
        uart_send_str(",");
    }

    send_u32((uint32_t)code);

    ++s_raw_count;

    /* 每 16 点换行, 便于在串口终端里肉眼对齐查看。
       上位机解析时按逗号和换行一起当分隔符处理即可。 */
    if ((s_raw_count % RAW_SAMPLES_PER_LINE) == 0u) {
        uart_send_str("\r\n");
    }
}

void report_raw_put_block(const uint16_t *buf, uint32_t count)
{
    uint32_t i;

    if (buf == NULL) {
        return;
    }

    for (i = 0u; i < count; ++i) {
        report_raw_put(buf[i]);
    }
}

void report_raw_end(void)
{
    /* 若最后一行没凑满 16 点, 补一个换行, 保证 "#END" 独占一行 ——
       上位机的行同步靠这个。 */
    if ((s_raw_count % RAW_SAMPLES_PER_LINE) != 0u) {
        uart_send_str("\r\n");
    }

    uart_send_str("#END,");
    send_u32(s_raw_checksum);
    uart_send_str("\r\n");
}

void report_send_raw(const uint16_t *buf, uint32_t n_samples, uint32_t fs_hz)
{
    if (buf == NULL) {
        return;
    }

    report_raw_begin(n_samples, fs_hz);
    report_raw_put_block(buf, n_samples);
    report_raw_end();
}
