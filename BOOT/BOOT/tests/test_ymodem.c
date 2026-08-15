/* ================================================================
 * test_ymodem —— YMODEM 接收状态机主机单元测试（BOOT）
 *
 * 覆盖：完整传输/文件过大拒绝/写失败/坏帧重试/注入耗尽超时/
 *       文件 CRC 不匹配/错误码透传。
 * mock：ymodem_port 注入字节流 + write_fn 记录写入 + 手动时间轴。
 * ================================================================ */
#include <stdio.h>
#include <string.h>
#include "ymodem.h"
#include "ymodem_port.h"
#include "crc32.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

/* ---------------- mock：ymodem_port ---------------- */
static uint8_t  g_inject[8192];
static uint32_t g_inject_len;
static uint32_t g_inject_pos;
static uint32_t g_mock_tick;
static uint8_t  g_sent[1024];
static uint32_t g_sent_len;

void ymodem_port_init(void)            { g_mock_tick = 0; }
void ymodem_send_byte(uint8_t byte)    { if (g_sent_len < sizeof(g_sent)) g_sent[g_sent_len++] = byte; }
int32_t ymodem_read_byte(uint32_t timeout_ms)
{
    if (g_inject_pos < g_inject_len) {
        return g_inject[g_inject_pos++];
    }
    /* 注入耗尽：推进时间轴模拟等待，让 FSM 超时逻辑生效 */
    g_mock_tick += timeout_ms;
    return -1;
}
void ymodem_feed_watchdog(void)        { }
uint32_t ymodem_get_tick(void)         { return g_mock_tick; }

/* ---------------- mock：write_fn ---------------- */
static uint8_t  g_written[65536];
static uint32_t g_written_len;
static uint32_t g_write_calls;
static uint8_t  g_write_ok = 1;

static bool mock_write_fn(uint32_t off, const uint8_t *data, uint32_t len)
{
    g_write_calls++;
    if (!g_write_ok) {
        return false;
    }
    if (off + len > sizeof(g_written)) {
        return false;
    }
    memcpy(&g_written[off], data, len);
    if (off + len > g_written_len) {
        g_written_len = off + len;
    }
    return true;
}

/* ---------------- 帧构造 helper ---------------- */
static uint32_t calc_crc(const uint8_t *data, uint16_t len)
{
    uint32_t crc;
    crc32_init(&crc);
    crc32_update(&crc, data, len);
    return crc32_finalize(&crc);
}

/* 推入一帧：type + seq + ~seq + data + crc32 小端 */
static void push_frame(uint8_t type, uint8_t seq,
                       const uint8_t *data, uint16_t len)
{
    g_inject[g_inject_len++] = type;
    g_inject[g_inject_len++] = seq;
    g_inject[g_inject_len++] = (uint8_t)~seq;
    memcpy(&g_inject[g_inject_len], data, len);
    g_inject_len += len;
    uint32_t crc = calc_crc(data, len);
    g_inject[g_inject_len++] = (uint8_t)(crc & 0xFF);
    g_inject[g_inject_len++] = (uint8_t)((crc >> 8) & 0xFF);
    g_inject[g_inject_len++] = (uint8_t)((crc >> 16) & 0xFF);
    g_inject[g_inject_len++] = (uint8_t)((crc >> 24) & 0xFF);
}

/* file info 帧：文件名\0 + 大小(hex) + ' ' + CRC32(hex)，空格填充 128B。
 * 格式必须与 ymodem.c parse_file_info 一致：文件名以 NUL 结尾，
 * 其后紧跟 size hex、空格、crc hex。 */
static void push_file_info(const char *name, uint32_t size, uint32_t file_crc)
{
    uint8_t data[128];
    memset(data, 0, sizeof(data));
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s", name);   /* 文件名 */
    buf[n++] = '\0';                                   /* 显式 NUL 分隔 */
    /* 注意：size 字段协议约定为十六进制（parse_file_info 用 strtoul base 16，
     * 与上位机 ymodem_sender.py 的 %lX 一致） */
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%lX %08lX",
                  (unsigned long)size, (unsigned long)file_crc);
    memcpy(data, buf, (size_t)n);
    push_frame(YMODEM_SOH, 0, data, 128);
}

/* 数据帧（STX 1024 或 SOH 128） */
static void push_data_frame(uint8_t seq, const uint8_t *data, uint16_t len)
{
    uint8_t pad[1024];
    memset(pad, 0x1A, sizeof(pad));
    memcpy(pad, data, len);
    uint8_t type = (len > 128) ? YMODEM_STX : YMODEM_SOH;
    uint16_t flen = (type == YMODEM_STX) ? 1024 : 128;
    push_frame(type, seq, pad, flen);
}

/* 坏 CRC 数据帧（人为破坏帧尾） */
static void push_bad_crc_frame(uint8_t seq)
{
    uint8_t data[1024];
    memset(data, 0x42, sizeof(data));
    push_frame(YMODEM_STX, seq, data, 1024);
    g_inject[g_inject_len - 1] ^= 0xFF;   /* 破坏 CRC 尾字节 */
}

/* 结束帧（128B 全零） */
static void push_end_frame(void)
{
    uint8_t data[128];
    memset(data, 0, sizeof(data));
    push_frame(YMODEM_SOH, 0, data, 128);
}

static void reset_mock(void)
{
    g_inject_len = 0;
    g_inject_pos = 0;
    g_mock_tick = 0;
    g_sent_len = 0;
    g_written_len = 0;
    g_write_calls = 0;
    g_write_ok = 1;
    memset(g_written, 0, sizeof(g_written));
}

static ymodem_status_t run_receive(uint32_t flash_end)
{
    ymodem_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.flash_end = flash_end;
    ctx.write_fn  = mock_write_fn;
    return ymodem_receive(&ctx, 0u);
}

/* ---------------- 测试用例 ---------------- */

static void test_complete_transfer(void)
{
    printf("[test] 完整传输成功\n");
    reset_mock();

    uint8_t payload[2000];
    for (int i = 0; i < 2000; i++) {
        payload[i] = (uint8_t)(i * 7 + 3);
    }
    uint32_t file_crc = calc_crc(payload, 2000);

    push_file_info("APP.bin", 2000, file_crc);
    push_data_frame(1, &payload[0], 1024);
    push_data_frame(2, &payload[1024], 976);
    g_inject[g_inject_len++] = YMODEM_EOT;
    g_inject[g_inject_len++] = YMODEM_EOT;
    push_end_frame();

    ymodem_status_t st = run_receive(1024 * 1024);
    CHECK(st == YMODEM_OK, "返回 YMODEM_OK");
    CHECK(g_written_len == 2000, "写入 2000 字节");
    CHECK(memcmp(g_written, payload, 2000) == 0, "写入内容一致");
}

static void test_file_too_large(void)
{
    printf("[test] 文件过大拒绝（ERR_FILE 透传）\n");
    reset_mock();
    push_file_info("big.bin", 2 * 1024 * 1024, 0x12345678);
    ymodem_status_t st = run_receive(1024 * 1024);
    CHECK(st == YMODEM_ERR_FILE, "返回 ERR_FILE（非一律 TIMEOUT）");
    CHECK(g_written_len == 0, "未写入任何数据");
}

static void test_write_failure(void)
{
    printf("[test] 写目标失败（ERR_FLASH 透传）\n");
    reset_mock();
    g_write_ok = 0;
    uint8_t payload[200];
    memset(payload, 0x55, sizeof(payload));
    push_file_info("APP.bin", 200, calc_crc(payload, 200));
    push_data_frame(1, payload, 200);
    ymodem_status_t st = run_receive(1024 * 1024);
    CHECK(st == YMODEM_ERR_FLASH, "返回 ERR_FLASH");
}

static void test_bad_frame_retry(void)
{
    printf("[test] 坏 CRC 帧 -> NAK 重发 -> 成功\n");
    reset_mock();
    uint8_t payload[300];
    memset(payload, 0xAB, sizeof(payload));
    push_file_info("APP.bin", 300, calc_crc(payload, 300));
    push_bad_crc_frame(1);            /* 第一帧 CRC 坏 */
    push_data_frame(1, &payload[0], 300);   /* 重发好帧 */
    g_inject[g_inject_len++] = YMODEM_EOT;
    g_inject[g_inject_len++] = YMODEM_EOT;
    push_end_frame();

    ymodem_status_t st = run_receive(1024 * 1024);
    CHECK(st == YMODEM_OK, "坏帧后重发成功");
    CHECK(g_written_len == 300, "内容写入");
    /* 检查发送过 NAK（0x15） */
    uint8_t saw_nak = 0;
    for (uint32_t i = 0; i < g_sent_len; i++) {
        if (g_sent[i] == YMODEM_NAK) { saw_nak = 1; break; }
    }
    CHECK(saw_nak, "曾发送 NAK 请求重发");
}

static void test_injection_exhausted_timeout(void)
{
    printf("[test] 注入耗尽 -> 重试后 ERR_TIMEOUT\n");
    reset_mock();
    uint8_t payload[100];
    memset(payload, 0x33, sizeof(payload));
    push_file_info("APP.bin", 100, calc_crc(payload, 100));
    /* 只注入 file info，随后数据注入耗尽 → 超时重试 → CAN 取消 */
    ymodem_status_t st = run_receive(1024 * 1024);
    CHECK(st == YMODEM_ERR_TIMEOUT, "返回 ERR_TIMEOUT");
    uint8_t saw_can = 0;
    for (uint32_t i = 0; i < g_sent_len; i++) {
        if (g_sent[i] == YMODEM_CAN) { saw_can = 1; break; }
    }
    CHECK(saw_can, "取消时发送 CAN 序列");
}

static void test_file_crc_mismatch(void)
{
    printf("[test] 文件级 CRC 不匹配（ERR_CRC）\n");
    reset_mock();
    uint8_t payload[200];
    memset(payload, 0x77, sizeof(payload));
    /* file info 里登记的 CRC 是错的 */
    push_file_info("APP.bin", 200, 0xDEADBEEF);
    push_data_frame(1, payload, 200);
    g_inject[g_inject_len++] = YMODEM_EOT;
    g_inject[g_inject_len++] = YMODEM_EOT;
    push_end_frame();

    ymodem_status_t st = run_receive(1024 * 1024);
    CHECK(st == YMODEM_ERR_CRC, "返回 ERR_CRC");
}

int ymodem_test_main(void)
{
    test_complete_transfer();
    test_file_too_large();
    test_write_failure();
    test_bad_frame_retry();
    test_injection_exhausted_timeout();
    test_file_crc_mismatch();
    if (failures) {
        printf("\nRESULT: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nRESULT: all passed\n");
    return 0;
}
