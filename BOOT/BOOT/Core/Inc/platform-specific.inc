#ifndef PLATFORM_SPECIFIC_INC
#define PLATFORM_SPECIFIC_INC

/* 字大小定义 (Cortex-M4) */
#define uECC_WORD_SIZE          4
#define uECC_WORD_BITS          32
#define uECC_WORD_BITS_SHIFT    5
#define uECC_WORD_BITS_MASK     0x1F
#define HIGH_BIT_SET            0x80000000

/* 禁用汇编优化 */
#define uECC_NO_ASM

/* 函数可见性（Keil ARMCC 需要明确指定 static） */
#define uECC_VLI_API static

/* secp256r1 所需的最大字数量 */
//#define uECC_MAX_WORDS          8

/* 随机数尝试次数 */
#define uECC_RNG_MAX_TRIES      64

/* 类型定义 */
typedef uint32_t    uECC_word_t;
typedef uint64_t    uECC_dword_t;
typedef unsigned    wordcount_t;
typedef unsigned    bitcount_t;
typedef int         cmpresult_t;

#define UECC_WORD_1  0xFFFFFFFF
#define UECC_WORD_0  0x00000000

#endif