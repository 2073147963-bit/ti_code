/**
 * @file    wave_algo.c
 * @brief   波形识别与参数估计算法 —— 实现
 *
 * @details
 *  2026 年第十六届 TI 杯数字系统创新大赛 · 赛道一初赛
 *
 *  本文件是 ti_cup/main.c 那套已验证算法的移植版, 逻辑一字未改, 只做了
 *  两件事:
 *    1. 把结果输出从"自己拼 wave=... 字符串"改为填 result_t, 交给
 *       uart_report.c 按协议拼 RESULT,... 帧;
 *    2. 把峰峰值从 ADC 码值换算成物理电压 V, 并补齐去直流/估频两个
 *       中间步骤的对外接口。
 *
 *  算法链路(与 ti_cup/main.c 完全一致):
 *      去直流 -> 1024 点 FFT 粗估频率 + 抛物线插值
 *             -> Goertzel 在 f0±4 Hz 内 0.5 Hz 步长细化
 *             -> 周期折叠(同相位 bin 取均值, 深度去噪)
 *             -> 双电平比例判方波 / DFT 1-2-3 次谐波比判其余三类
 *             -> 分波形估 Vpp, 方波用交越法插值估占空比
 *
 *  为什么不对 Vpp 用分位数法: 周期折叠会把锯齿的尖峰磨平(400 Hz 时一个
 *  周期只有 25 个 bin), 分位数法低估可达 7%。改用基波幅度乘理论系数换算,
 *  对局部磨平免疫, 实测误差 ≤ 1.2%。
 *
 *  本文件不依赖任何 TI SDK 头文件, 只依赖 <math.h> 与 uart_report.h 的
 *  类型定义 —— 因此同一份源码也能在 PC 上用 gcc 编译做回归测试。
 *
 * @date    2026-10-10
 */

#include "wave_algo.h"
#include <math.h>

/* ===================================================================== *
 *  内部常量
 * ===================================================================== */

/** 圆周率。不用 M_PI —— 它在严格 C99 下不一定可见。 */
#define WAVE_PI             (3.14159265358979f)

/** 粗估用的 FFT 点数。1024 点 @ 10 kHz => 谱线间隔 9.766 Hz。 */
#define WAVE_FFT_N          (1024)

/** 折叠后单个周期的最大 bin 数。100 Hz 时一个周期 100 点, 取 128 够用。 */
#define WAVE_FOLD_MAX       (128)

/** Goertzel 细化扫描的点数: f0 ± 4 Hz, 步长 0.5 Hz => 17 点。 */
#define WAVE_NFINE          (17)
/** 细化扫描范围 Hz(单侧)。 */
#define WAVE_FINESPAN       (4.0f)
/** 细化扫描步长 Hz。 */
#define WAVE_FINESTEP       (0.5f)

/* ===================================================================== *
 *  内部工作区 —— 全部静态, 不占栈
 *
 *  内存账(MSPM0G3507 共 32 KB SRAM):
 *      主采样缓冲在 main.c, int16 x 10000        = 20000 B
 *      下面这些合计                               ≈ 9472 B
 *      ------------------------------------------------
 *      合计                                       ≈ 29.5 KB, 余量约 3 KB
 *  栈默认 512 B, 够用; 但不要再加大数组。
 * ===================================================================== */

static float   g_fft_re[WAVE_FFT_N];    /**< 粗估 FFT 实部                */
static float   g_fft_im[WAVE_FFT_N];    /**< 粗估 FFT 虚部                */
static int16_t g_folded[WAVE_FOLD_MAX]; /**< 折叠后的代表周期              */
static int16_t g_sort[WAVE_FOLD_MAX];   /**< 排序副本(求分位数)            */
static int16_t g_hi[WAVE_FOLD_MAX];     /**< 方波高电平平台样点            */
static int16_t g_lo[WAVE_FOLD_MAX];     /**< 方波低电平平台样点            */
static int32_t g_fsum[WAVE_FOLD_MAX];   /**< 折叠累加和                    */
static int16_t g_fcnt[WAVE_FOLD_MAX];   /**< 折叠每个 bin 的样点计数        */

/* ===================================================================== *
 *  一、去直流
 * ===================================================================== */

void wave_remove_dc(int16_t *x, uint32_t n)
{
    int32_t sum  = 0;
    int16_t mean;
    uint32_t i;

    if ((x == NULL) || (n == 0u)) {
        return;
    }

    for (i = 0u; i < n; ++i) {
        sum += x[i];
    }
    mean = (int16_t)(sum / (int32_t)n);

    for (i = 0u; i < n; ++i) {
        x[i] = (int16_t)(x[i] - mean);
    }
}

/* ===================================================================== *
 *  二、基频粗估: 1024 点 FFT + 抛物线插值
 * ===================================================================== */

/**
 * @brief 迭代式基 2 时间抽取 FFT(原地)。
 * @param re  实部数组, 长度 n, 原地修改
 * @param im  虚部数组, 长度 n, 原地修改
 * @param n   点数, 必须是 2 的幂
 */
static void wave_fft(float *re, float *im, int n)
{
    int i, len;

    /* ---- 位反转置换 ---- */
    {
        int j = 0;
        for (i = 1; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                float tr = re[i]; re[i] = re[j]; re[j] = tr;
                float ti = im[i]; im[i] = im[j]; im[j] = ti;
            }
        }
    }

    /* ---- 蝶形运算 ---- */
    for (len = 2; len <= n; len <<= 1) {
        float ang  = -2.0f * WAVE_PI / (float)len;
        float w_re = cosf(ang);
        float w_im = sinf(ang);
        for (i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            int   k;
            for (k = 0; k < len / 2; ++k) {
                int   a = i + k;
                int   b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                float nr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] = re[a] + tr; im[a] = im[a] + ti;
                nr = cr * w_re - ci * w_im;
                ci = cr * w_im + ci * w_re;
                cr = nr;
            }
        }
    }
}

/**
 * @brief FFT 谱峰法粗估基频, 失败返回 0。
 *
 * @details 只在 [WAVE_FMIN_HZ, WAVE_FMAX_HZ] 内找幅度最大谱线, 再用相邻
 *          三点的幅度做抛物线插值, 把分辨率从 9.766 Hz 提到约 1 Hz。
 *          这一步是整条链路的基础 —— 后面周期折叠对频率精度极敏感,
 *          频率差了 1 Hz, 折 1 秒就会漂移一整个周期。
 *
 * @param x  已去直流的样本
 * @return 粗估频率 Hz; 无效信号返回 0
 */
static float wave_estimate_freq_coarse(const int16_t *x)
{
    int   klo, khi, kmax, k;
    float mmax;
    float mag3[3];
    float kfrac, kpeak;
    float den;

    for (k = 0; k < WAVE_FFT_N; ++k) {
        g_fft_re[k] = (float)x[k];
        g_fft_im[k] = 0.0f;
    }
    wave_fft(g_fft_re, g_fft_im, WAVE_FFT_N);

    /* 搜索区间换算成谱线下标。谱线间隔 = fs / FFT_N。 */
    klo = (int)(WAVE_FMIN_HZ * (float)WAVE_FFT_N / WAVE_FS_HZ) + 1;
    khi = (int)(WAVE_FMAX_HZ * (float)WAVE_FFT_N / WAVE_FS_HZ);
    if (klo < 1) {
        klo = 1;
    }
    if (khi > WAVE_FFT_N / 2 - 1) {
        khi = WAVE_FFT_N / 2 - 1;
    }
    if (klo > khi) {
        return 0.0f;
    }

    /* 找幅度平方最大的谱线。比幅度省一次开方, 结果等价。 */
    kmax = klo;
    mmax = g_fft_re[klo] * g_fft_re[klo] + g_fft_im[klo] * g_fft_im[klo];
    for (k = klo + 1; k <= khi; ++k) {
        float m = g_fft_re[k] * g_fft_re[k] + g_fft_im[k] * g_fft_im[k];
        if (m > mmax) {
            mmax = m;
            kmax = k;
        }
    }

    /* 抛物线插值要开方(插值公式的参数是幅度不是功率), 只算 3 个点。 */
    mag3[0] = sqrtf(g_fft_re[kmax - 1] * g_fft_re[kmax - 1] +
                    g_fft_im[kmax - 1] * g_fft_im[kmax - 1]);
    mag3[1] = sqrtf(g_fft_re[kmax]     * g_fft_re[kmax]     +
                    g_fft_im[kmax]     * g_fft_im[kmax]);
    mag3[2] = sqrtf(g_fft_re[kmax + 1] * g_fft_re[kmax + 1] +
                    g_fft_im[kmax + 1] * g_fft_im[kmax + 1]);

    den = mag3[0] - 2.0f * mag3[1] + mag3[2];
    kfrac = (fabsf(den) < 1e-12f) ? 0.0f : (mag3[0] - mag3[2]) / (2.0f * den);
    kpeak = (float)kmax + kfrac;

    return kpeak * (WAVE_FS_HZ / (float)WAVE_FFT_N);
}

/* ===================================================================== *
 *  三、基频细化: Goertzel 扫描 + 抛物线插值
 * ===================================================================== */

/**
 * @brief Goertzel 求单频 f 的幅度。
 *
 * @param x  已去直流的样本
 * @param n  样点个数
 * @param f  待测频率 Hz
 * @return 该频率分量的幅度
 *
 * @note Goertzel 是"只算一个频点"的 DFT, 复杂度 O(n), 比在细化区间补零
 *       做 FFT 便宜得多 —— 这里只要 17 个频点。
 */
static float wave_goertzel_mag(const int16_t *x, uint32_t n, float f)
{
    float omega = 2.0f * WAVE_PI * f / WAVE_FS_HZ;
    float coeff = 2.0f * cosf(omega);
    float s1 = 0.0f, s2 = 0.0f;
    float m2;
    uint32_t i;

    for (i = 0u; i < n; ++i) {
        float s = (float)x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }

    m2 = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return (m2 > 0.0f) ? sqrtf(m2) : 0.0f;
}

float wave_estimate_freq(const int16_t *x, uint32_t n)
{
    float f0;
    float mag[WAVE_NFINE];
    float best_f, best_m;
    int   best_i = 0;
    int   i;

    if ((x == NULL) || (n == 0u)) {
        return 0.0f;
    }

    f0 = wave_estimate_freq_coarse(x);
    if (f0 <= 0.0f) {
        return 0.0f;
    }

    best_f = f0;
    best_m = -1.0f;
    for (i = 0; i < WAVE_NFINE; ++i) {
        float f = f0 - WAVE_FINESPAN + (float)i * WAVE_FINESTEP;
        mag[i] = wave_goertzel_mag(x, n, f);
        if (mag[i] > best_m) {
            best_m = mag[i];
            best_f = f;
            best_i = i;
        }
    }

    /* 在峰值附近三点再做一次抛物线插值, 把 0.5 Hz 的步长细化到 ~0.01 Hz。 */
    if ((best_i > 0) && (best_i < WAVE_NFINE - 1)) {
        float a   = mag[best_i - 1];
        float b   = mag[best_i];
        float c   = mag[best_i + 1];
        float den = a - 2.0f * b + c;
        if (fabsf(den) > 1e-12f) {
            float delta = 0.5f * (a - c) / den * WAVE_FINESTEP;
            best_f = (f0 - WAVE_FINESPAN + (float)best_i * WAVE_FINESTEP) + delta;
        }
    }

    return best_f;
}

/* ===================================================================== *
 *  四、周期折叠 —— 深度去噪
 * ===================================================================== */

/**
 * @brief 按估计频率把整段信号折成一个代表周期。
 *
 * @details 第 i 个样点的相位是 (i * f / fs) mod 1, 落在同一相位 bin 的
 *          所有样点取均值。N 个周期叠加相当于 N 倍平均, 白噪声按
 *          sqrt(N) 衰减 —— 1 秒 200 Hz 就是 200 次平均, 信噪比提升约 23 dB。
 *          这是全链路最有效的去噪手段, 且不像滑动平均那样磨平边沿。
 *
 * @param[in]  x      已去直流的样本
 * @param[in]  n      样点个数
 * @param[in]  f_est  估计频率 Hz
 * @param[out] out    折叠结果, 容量至少 WAVE_FOLD_MAX
 * @return 实际 bin 数(即一个周期被分成了几段)
 */
static uint32_t wave_fold_periods(const int16_t *x, uint32_t n,
                                  float f_est, int16_t *out)
{
    uint32_t nbins;
    float    dphase, phase;
    uint32_t i, b;

    if (f_est <= 0.0f) {
        return 0u;
    }

    /* 一个周期有几个采样点 —— 就是折叠后的 bin 数。 */
    nbins = (uint32_t)(WAVE_FS_HZ / f_est + 0.5f);
    if (nbins < 4u) {
        nbins = 4u;
    }
    if (nbins > WAVE_FOLD_MAX) {
        nbins = WAVE_FOLD_MAX;    /* 频率太低时截断, 防止越界 */
    }

    for (b = 0u; b < nbins; ++b) {
        g_fsum[b] = 0;
        g_fcnt[b] = 0;
    }

    /* 相位用累加而不是每次 fmod —— 1 万次 fmod 在 M0+ 上是可观的软浮点开销。 */
    dphase = f_est / WAVE_FS_HZ;
    phase  = 0.0f;
    for (i = 0u; i < n; ++i) {
        uint32_t bin = (uint32_t)(phase * (float)nbins);
        if (bin >= nbins) {
            bin = nbins - 1u;
        }
        g_fsum[bin] += x[i];
        g_fcnt[bin]++;
        phase += dphase;
        if (phase >= 1.0f) {
            phase -= 1.0f;
        }
    }

    for (b = 0u; b < nbins; ++b) {
        out[b] = (g_fcnt[b] > 0) ? (int16_t)(g_fsum[b] / g_fcnt[b]) : 0;
    }

    return nbins;
}

/* ===================================================================== *
 *  五、排序与分位数
 * ===================================================================== */

/** @brief 插入排序。nbins ≤ 128, 简单实现足够。 */
static void wave_sort_i16(int16_t *a, uint32_t n)
{
    uint32_t i;
    for (i = 1u; i < n; ++i) {
        int16_t v = a[i];
        int32_t j = (int32_t)i - 1;
        while ((j >= 0) && (a[j] > v)) {
            a[j + 1] = a[j];
            --j;
        }
        a[j + 1] = v;
    }
}

/**
 * @brief 从已排序数组取 p 分位数(0~1), 线性插值。
 * @param sorted  已升序排序的数组
 * @param n       长度
 * @param p       分位点, 如 0.05 表示 5%
 */
static int16_t wave_percentile(const int16_t *sorted, uint32_t n, float p)
{
    float    idx;
    uint32_t lo, hi;
    float    frac;

    if (n == 0u) {
        return 0;
    }

    idx  = p * (float)(n - 1u);
    lo   = (uint32_t)idx;
    hi   = ((lo + 1u) < n) ? (lo + 1u) : lo;
    frac = idx - (float)lo;

    return (int16_t)((1.0f - frac) * (float)sorted[lo] + frac * (float)sorted[hi]);
}

/* ===================================================================== *
 *  六、分类与参数估计
 * ===================================================================== */

/**
 * @brief 双电平比例 —— 判方波的核心指标。
 *
 * @details 数一数有多少样点落在 [p5, p5+0.2*range] 或 [p95-0.2*range, p95]
 *          这两个"平台带"里。方波绝大部分时间停在两个电平上, 比例接近 1;
 *          三角/锯齿是斜坡, 扫过这两个带的时间很短, 比例明显低。
 *
 * @return 落在平台带内的样点占比 0~1
 */
static float wave_two_level_ratio(const int16_t *x, uint32_t n,
                                  int16_t p5, int16_t p95)
{
    float    range = (float)(p95 - p5);
    float    band;
    uint32_t cnt = 0u;
    uint32_t i;

    if (range < 1.0f) {
        range = 1.0f;
    }
    band = 0.2f * range;

    for (i = 0u; i < n; ++i) {
        if (((float)x[i] >= ((float)p95 - band)) ||
            ((float)x[i] <= ((float)p5  + band))) {
            ++cnt;
        }
    }

    return (float)cnt / (float)n;
}

/**
 * @brief 基波幅度 —— 最小二乘拟合 x ≈ a·sin(θ) + b·cos(θ) + c, 返回 √(a²+b²)。
 *
 * @param x  折叠后的波形
 * @param n  bin 数(即一个周期的点数)
 * @return 基波幅度(单位同 x, 即 ADC 码值)
 */
static float wave_fundamental_amp(const int16_t *x, uint32_t n)
{
    int32_t sum = 0;
    float   c;
    float   a_num = 0.0f, a_den = 0.0f;
    float   b_num = 0.0f, b_den = 0.0f;
    float   a, b;
    uint32_t i;

    for (i = 0u; i < n; ++i) {
        sum += x[i];
    }
    c = (float)sum / (float)n;

    for (i = 0u; i < n; ++i) {
        float xc = (float)x[i] - c;
        float th = 2.0f * WAVE_PI * (float)i / (float)n;
        float s  = sinf(th);
        float cc = cosf(th);
        a_num += xc * s;  a_den += s * s;
        b_num += xc * cc; b_den += cc * cc;
    }

    if (a_den < 1e-9f) {
        a_den = 1e-9f;
    }
    if (b_den < 1e-9f) {
        b_den = 1e-9f;
    }
    a = a_num / a_den;
    b = b_num / b_den;

    return sqrtf(a * a + b * b);
}

/**
 * @brief DFT 求折叠波形的 1/2/3 次谐波幅度。
 *
 * @param[in]  x   折叠后的波形
 * @param[in]  n   bin 数
 * @param[out] a1  基波幅度
 * @param[out] a2  2 次谐波幅度
 * @param[out] a3  3 次谐波幅度
 *
 * @note 2、3 次谐波的 cos/sin 用倍角公式从基波的 cos/sin 递推得到,
 *       避免在 128 点的循环里调用 6 次三角函数。
 */
static void wave_dft_harmonics(const int16_t *x, uint32_t n,
                               float *a1, float *a2, float *a3)
{
    int32_t sum = 0;
    float   mean;
    float   re1 = 0.0f, im1 = 0.0f;
    float   re2 = 0.0f, im2 = 0.0f;
    float   re3 = 0.0f, im3 = 0.0f;
    uint32_t i;

    for (i = 0u; i < n; ++i) {
        sum += x[i];
    }
    mean = (float)sum / (float)n;

    for (i = 0u; i < n; ++i) {
        float xc = (float)x[i] - mean;
        float th = 2.0f * WAVE_PI * (float)i / (float)n;
        float c1 = cosf(th);
        float s1 = sinf(th);
        /* 倍角公式 */
        float c2 = c1 * c1 - s1 * s1;
        float s2 = 2.0f * s1 * c1;
        float c3 = 4.0f * c1 * c1 * c1 - 3.0f * c1;
        float s3 = s1 * (3.0f - 4.0f * s1 * s1);

        re1 += xc * c1; im1 -= xc * s1;
        re2 += xc * c2; im2 -= xc * s2;
        re3 += xc * c3; im3 -= xc * s3;
    }

    *a1 = sqrtf(re1 * re1 + im1 * im1);
    *a2 = sqrtf(re2 * re2 + im2 * im2);
    *a3 = sqrtf(re3 * re3 + im3 * im3);
}

/**
 * @brief 方波峰峰值: 高低两个平台各自取中位数再相减。
 *
 * @note 用中位数而不是均值/极值 —— 均值会被 50% 以外的占空比带偏,
 *       极值会被噪声毛刺抬高。中位数对两者都免疫。
 *
 * @return 峰峰值, 单位 ADC 码值
 */
static float wave_square_vpp(const int16_t *x, uint32_t n, float mid)
{
    uint32_t hi_cnt = 0u, lo_cnt = 0u;
    uint32_t i;
    float    hi_med, lo_med;

    for (i = 0u; i < n; ++i) {
        if ((float)x[i] > mid) {
            g_hi[hi_cnt++] = x[i];
        } else if ((float)x[i] < mid) {
            g_lo[lo_cnt++] = x[i];
        }
    }
    if ((hi_cnt == 0u) || (lo_cnt == 0u)) {
        return 0.0f;
    }

    wave_sort_i16(g_hi, hi_cnt);
    wave_sort_i16(g_lo, lo_cnt);
    hi_med = (float)g_hi[hi_cnt / 2u];
    lo_med = (float)g_lo[lo_cnt / 2u];

    return hi_med - lo_med;
}

/**
 * @brief 方波占空比: 交越法, 阈值取高低电平中点, 边沿做线性插值。
 *
 * @details 在折叠后的一个周期上找第一个上升沿和随后的下降沿, 用线性插值
 *          算出阈值穿越的精确位置, 两沿距离除以周期即为占空比。
 *          边沿插值很要紧 —— 一个周期只有 25~100 个 bin 时, 不插值会让
 *          占空比有 1/bin 的量化误差, 即 1%~4%。
 *
 * @return 占空比 0~1; 找不到边沿返回 0
 */
static float wave_duty_from_folded(const int16_t *x, uint32_t n, float mid)
{
    float    rise = -1.0f, fall = -1.0f;
    uint32_t k;

    if (n == 0u) {
        return 0.0f;
    }

    /* 展开两倍遍历, 处理下降沿落在周期起点之前的跨周期情形。 */
    for (k = 1u; k <= 2u * n; ++k) {
        uint32_t i = k % n;
        uint32_t j = (k - 1u) % n;
        float    a = (float)x[j];
        float    b = (float)x[i];

        if ((rise < 0.0f) && (a < mid) && (b >= mid)) {
            float d = b - a;
            float frac = (fabsf(d) > 1e-6f) ? (mid - a) / d : 0.0f;
            if (frac < 0.0f) { frac = 0.0f; }
            if (frac > 1.0f) { frac = 1.0f; }
            rise = (float)(k - 1u) + frac;
        } else if ((rise >= 0.0f) && (a > mid) && (b <= mid)) {
            float d = b - a;
            float frac = (fabsf(d) > 1e-6f) ? (mid - a) / d : 0.0f;
            if (frac < 0.0f) { frac = 0.0f; }
            if (frac > 1.0f) { frac = 1.0f; }
            fall = (float)(k - 1u) + frac;
            break;
        }
    }

    if ((rise < 0.0f) || (fall < 0.0f)) {
        return 0.0f;
    }

    {
        float duty = (fall - rise) / (float)n;
        if (duty < 0.0f) { duty += 1.0f; }
        if (duty > 1.0f) { duty -= 1.0f; }
        if (duty < 0.0f) { duty = 0.0f; }
        if (duty > 1.0f) { duty = 1.0f; }
        return duty;
    }
}

/**
 * @brief 分类 + 分波形估参数。
 *
 * @param[in]  folded  折叠后的代表周期
 * @param[in]  nbins   bin 数
 * @param[out] type    波形类别
 * @param[out] vpp_code 峰峰值(ADC 码值)
 * @param[out] duty    占空比 0~1(仅方波有意义)
 *
 * @details 分类分两步走, 先判方波再判其余的:
 *    第一步 双电平比例 > 0.7 -> 方波;
 *    第二步 对非方波求 1/2/3 次谐波比:
 *           理论谐波特征 —— 正弦: 只有基波; 三角: 奇次谐波按 1/n² 衰减;
 *           锯齿: 全谐波按 1/n 衰减, 偶次(尤其 2 次)明显。
 *           所以 A2/A1 > 0.3 判锯齿, A3/A1 > 0.08 判三角。
 *
 * @note 谐波比的阈值是实测标定的经验值, 不是理论值 —— 理论值假设理想
 *       波形, 实测受采样和折叠影响会偏离, 现场如果偶发误判, 优先调这两个数。
 */
static void wave_classify_and_params(const int16_t *folded, uint32_t nbins,
                                     wave_type_t *type,
                                     float *vpp_code, float *duty)
{
    int16_t p5, p95;
    uint32_t i;

    *type     = WAVE_SINE;
    *vpp_code = 0.0f;
    *duty     = 0.0f;

    if (nbins == 0u) {
        return;
    }

    /* 复制并排序, 用来求分位数。 */
    for (i = 0u; i < nbins; ++i) {
        g_sort[i] = folded[i];
    }
    wave_sort_i16(g_sort, nbins);
    p5  = wave_percentile(g_sort, nbins, 0.05f);
    p95 = wave_percentile(g_sort, nbins, 0.95f);

    /* ---- 第一步: 双电平比例判方波 ---- */
    {
        float tlv = wave_two_level_ratio(folded, nbins, p5, p95);

        if (tlv > 0.7f) {
            *type = WAVE_SQUARE;
        } else {
            /* ---- 第二步: DFT 谐波比判其余三类 ---- */
            float a1, a2, a3;
            wave_dft_harmonics(folded, nbins, &a1, &a2, &a3);

            if (a1 > 1e-6f) {
                float r1 = a2 / a1;    /* 2 次/基波 */
                float r2 = a3 / a1;    /* 3 次/基波 */

                if (r1 > 0.3f) {
                    *type = WAVE_SAWTOOTH;            /* 锯齿: 偶次谐波丰富 */
                } else if (r2 > 0.08f) {
                    *type = (r2 < 0.25f) ? WAVE_TRIANGLE : WAVE_SQUARE;
                } else {
                    *type = WAVE_SINE;
                }
            }
        }
    }

    /* ---- 第三步: 分波形估峰峰值 ----
     * 共同点是都用"基波幅度 × 理论系数"定标, 而不是直接取折叠波形的
     * 极差 —— 折叠会把尖峰磨平, 极差会系统性低估。 */
    {
        float A;

        switch (*type) {
            case WAVE_SQUARE: {
                float mid = 0.5f * (float)(p5 + p95);
                *vpp_code = wave_square_vpp(folded, nbins, mid);
                *duty     = wave_duty_from_folded(folded, nbins, mid);
                break;
            }
            case WAVE_SINE:
                A = wave_fundamental_amp(folded, nbins);
                *vpp_code = 2.0f * A;                    /* 正弦: Vpp = 2·A1 */
                break;

            case WAVE_TRIANGLE:
                A = wave_fundamental_amp(folded, nbins);
                *vpp_code = (WAVE_PI * WAVE_PI / 4.0f) * A;  /* 三角基波系数 8/π² */
                break;

            case WAVE_SAWTOOTH:
                A = wave_fundamental_amp(folded, nbins);
                *vpp_code = WAVE_PI * A;                 /* 锯齿基波系数 2/π */
                break;

            default:
                *vpp_code = 0.0f;
                break;
        }
    }
}

/* ===================================================================== *
 *  七、对外总入口
 * ===================================================================== */

/** @brief 无效频率判定的容差: 落在搜索区间外这么多就认为没信号。 */
#define WAVE_FREQ_GUARD_HZ  (20.0f)

int wave_analyze(int16_t *x, uint32_t n, result_t *out)
{
    float      f;
    uint32_t   nbins;
    wave_type_t type;
    float      vpp_code, duty;

    if ((x == NULL) || (out == NULL)) {
        return WAVE_ERR_NULL;
    }

    /* ---- 1. 去直流(原地修改 x, 头文件已声明这一副作用) ---- */
    wave_remove_dc(x, n);

    /* ---- 2. 基频估计 ---- */
    f = wave_estimate_freq(x, n);
    if ((f < (WAVE_FMIN_HZ - WAVE_FREQ_GUARD_HZ)) ||
        (f > (WAVE_FMAX_HZ + WAVE_FREQ_GUARD_HZ))) {
        /* 没接信号、幅度太小、或信号频率远超赛题范围。 */
        out->type = WAVE_SINE;
        out->freq = 0.0f;
        out->vpp  = 0.0f;
        out->duty = 0.0f;
        return WAVE_ERR_NOSIG;
    }

    /* ---- 3. 周期折叠 ---- */
    nbins = wave_fold_periods(x, n, f, g_folded);
    if (nbins == 0u) {
        return WAVE_ERR_NOSIG;
    }

    /* ---- 4. 分类 + 参数估计 ---- */
    wave_classify_and_params(g_folded, nbins, &type, &vpp_code, &duty);

    /* ---- 5. 填结果, 同时把量纲换成通信层要的 ----
     *   freq: Hz, 已经是物理量
     *   vpp : 码值 -> 伏特。ADC 12 位满量程 4095 对应 VREF(3.3 V)
     *   duty: 保持归一化 0~1, 与 REPORT_DUTY_INPUT_RATIO=1 一致,
     *         乘 100 由 uart_report 做, 这一层不重复换算。 */
    out->type = type;
    out->freq = f;
    out->vpp  = vpp_code * (WAVE_ADC_VREF / WAVE_ADC_FS_CODE);
    out->duty = duty;

    return WAVE_OK;
}
