/**
 * @file    e2e_sender_shim.c
 * @brief   把单片机发送端的拼帧能力暴露给端到端测试, 绕开类型重名
 *
 * @details
 *  端到端测试要做的事是: 让【真正的发送端代码】拼出一帧, 再喂给
 *  【真正的接收端代码】解析, 验证两边对协议的理解完全一致。
 *
 *  但 uart_report.h 和 pc_receiver.c 各自定义了同名的 wave_type_t 与
 *  result_t(这本身是对的 —— 两端是独立工程, 各自自洽), 放进同一个
 *  翻译单元就会重定义冲突。
 *
 *  解决办法是加这一层薄壳: 本文件单独编译, 只 include 发送端的头,
 *  对外只暴露 int / float / char* 这类基础类型的接口。测试文件声明
 *  这几个原型即可, 完全不碰发送端的结构体, 冲突自然消失。
 *
 *  这样 test_e2e.c 里的期望帧就不再是手写的字符串字面量, 而是
 *  uart_report.c 当场算出来的真实输出 —— 发送端一旦改坏格式,
 *  端到端测试会立刻失败, 而不是因为两边都照着同一份手写常量而
 *  一起"通过"。
 *
 *  编译: 与 uart_report.c 一起作为独立 TU 加入, 需要
 *        -DREPORT_SIM_STDOUT=1(避免依赖 TI SDK 的头文件)。
 *
 * @date    2026-10-08
 */

#include "uart_report.h"

/**
 * @brief 用真实的发送端代码拼一帧, 写进调用方的字符数组。
 *
 * @param[out] dst   目标缓冲区
 * @param[in]  cap   dst 容量(字节, 含结尾 '\0')
 * @param[in]  type  波形类别, 取 0=sine 1=square 2=triangle 3=sawtooth,
 *                   与 wave_type_t 的枚举值一致; 传 99 可测非法类别
 * @param[in]  freq  频率 Hz
 * @param[in]  vpp   峰峰值 V
 * @param[in]  duty  占空比, 单位按 REPORT_DUTY_INPUT_RATIO(默认 0~1)
 *
 * @return 帧长度(不含 '\0'); 失败返回 0 且 dst 为空串
 */
int sender_make_frame(char *dst, int cap, int type,
                      float freq, float vpp, float duty)
{
    result_t r;
    int      len = 0;

    r.type = (wave_type_t)type;
    r.freq = freq;
    r.vpp  = vpp;
    r.duty = duty;

    if (report_format_result(dst, cap, &r, &len) != REPORT_OK) {
        return 0;
    }
    return len;
}

/**
 * @brief 暴露发送端的类别->字符串映射, 供测试核对两端用词一致。
 *
 * @param type  同 sender_make_frame() 的 type
 * @return 协议规定的英文小写单词, 或 "unknown"
 */
const char *sender_wave_word(int type)
{
    return wave_to_str((wave_type_t)type);
}
