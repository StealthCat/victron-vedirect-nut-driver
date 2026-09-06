#include "vedirect-parser.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static size_t append_text(uint8_t *out, size_t pos, const char *s)
{
    size_t n = strlen(s);
    memcpy(out + pos, s, n);
    return pos + n;
}

static size_t build_frame(uint8_t *out, size_t cap,
                          const char *voltage_mv,
                          const char *current_ma,
                          const char *soc_permille,
                          const char *ttg_minutes)
{
    size_t p = 0, i;
    uint8_t sum = 0;

    (void)cap;
    p = append_text(out, p, "\r\nV\t");
    p = append_text(out, p, voltage_mv);
    p = append_text(out, p, "\r\nI\t");
    p = append_text(out, p, current_ma);
    p = append_text(out, p, "\r\nSOC\t");
    p = append_text(out, p, soc_permille);
    p = append_text(out, p, "\r\nTTG\t");
    p = append_text(out, p, ttg_minutes);
    p = append_text(out, p, "\r\nAlarm\tOFF\r\nChecksum\t");

    for (i = 0; i < p; ++i)
        sum = (uint8_t)(sum + out[i]);
    out[p++] = (uint8_t)(0u - sum);
    return p;
}

static void test_single_and_bad_checksum(void)
{
    uint8_t frame[512];
    size_t len = build_frame(frame, sizeof(frame), "13210", "-17430", "842", "906");
    size_t i;
    vd_parser_t parser;
    int valid = 0;

    vd_parser_init(&parser);
    for (i = 0; i < len; ++i) {
        if (vd_parser_feed(&parser, frame[i]) == VD_PARSE_FRAME_VALID)
            valid++;
    }

    assert(valid == 1);
    assert(strcmp(vd_frame_get(&parser, "V"), "13210") == 0);
    assert(strcmp(vd_frame_get(&parser, "I"), "-17430") == 0);
    assert(strcmp(vd_frame_get(&parser, "SOC"), "842") == 0);
    assert(strcmp(vd_frame_get(&parser, "TTG"), "906") == 0);

    frame[5] ^= 1u;
    vd_parser_init(&parser);
    valid = 0;
    for (i = 0; i < len; ++i) {
        if (vd_parser_feed(&parser, frame[i]) == VD_PARSE_FRAME_VALID)
            valid++;
    }
    assert(valid == 0);
}

static void test_back_to_back_frames_newest_wins(void)
{
    uint8_t stream[1024];
    size_t len1, len2, total, i;
    vd_parser_t parser;
    int valid = 0;
    char newest_i[VD_MAX_VALUE] = "";
    char newest_soc[VD_MAX_VALUE] = "";

    len1 = build_frame(stream, sizeof(stream), "13600", "12000", "900", "---");
    len2 = build_frame(stream + len1, sizeof(stream) - len1, "13120", "-18500", "898", "1164");
    total = len1 + len2;

    vd_parser_init(&parser);
    for (i = 0; i < total; ++i) {
        if (vd_parser_feed(&parser, stream[i]) == VD_PARSE_FRAME_VALID) {
            const char *cur = vd_frame_get(&parser, "I");
            const char *soc = vd_frame_get(&parser, "SOC");
            valid++;
            assert(cur != NULL);
            assert(soc != NULL);
            snprintf(newest_i, sizeof(newest_i), "%s", cur);
            snprintf(newest_soc, sizeof(newest_soc), "%s", soc);
        }
    }

    assert(valid == 2);
    assert(strcmp(newest_i, "-18500") == 0);
    assert(strcmp(newest_soc, "898") == 0);
}

static void test_partial_frame_survives_poll_boundary(void)
{
    uint8_t frame[512];
    size_t len = build_frame(frame, sizeof(frame), "13050", "-25000", "700", "600");
    size_t split = len / 2;
    size_t i;
    vd_parser_t parser;
    int valid = 0;

    vd_parser_init(&parser);

    for (i = 0; i < split; ++i)
        assert(vd_parser_feed(&parser, frame[i]) != VD_PARSE_FRAME_VALID);

    for (i = split; i < len; ++i) {
        if (vd_parser_feed(&parser, frame[i]) == VD_PARSE_FRAME_VALID)
            valid++;
    }

    assert(valid == 1);
    assert(strcmp(vd_frame_get(&parser, "I"), "-25000") == 0);
    assert(strcmp(vd_frame_get(&parser, "SOC"), "700") == 0);
}

int main(void)
{
    test_single_and_bad_checksum();
    test_back_to_back_frames_newest_wins();
    test_partial_frame_survives_poll_boundary();

    puts("VE.Direct parser/stream regression tests passed");
    return 0;
}
