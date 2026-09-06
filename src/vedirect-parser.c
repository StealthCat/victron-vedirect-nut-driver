#include "vedirect-parser.h"

#include <string.h>

static void clear_frame(vd_parser_t *p)
{
    p->field_count = 0;
    p->key_len = 0;
    p->value_len = 0;
    p->key[0] = '\0';
    p->value[0] = '\0';
}

static void start_candidate(vd_parser_t *p)
{
    clear_frame(p);
    p->checksum = (uint8_t)'\r';
    p->state = VD_STATE_START_LF;
}

static void reset_to_sync(vd_parser_t *p)
{
    p->state = VD_STATE_SYNC;
    p->checksum = 0;
    p->key_len = 0;
    p->value_len = 0;
    p->key[0] = '\0';
    p->value[0] = '\0';
}

static int store_field(vd_parser_t *p)
{
    vd_field_t *f;

    if (p->field_count >= VD_MAX_FIELDS)
        return -1;

    f = &p->fields[p->field_count++];
    memcpy(f->key, p->key, p->key_len + 1);
    memcpy(f->value, p->value, p->value_len + 1);
    return 0;
}

void vd_parser_init(vd_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = VD_STATE_SYNC;
}

vd_parse_result_t vd_parser_feed(vd_parser_t *p, uint8_t byte)
{
    switch (p->state) {
    case VD_STATE_SYNC:
        if (byte == (uint8_t)'\r')
            start_candidate(p);
        return VD_PARSE_NONE;

    case VD_STATE_START_LF:
        if (byte != (uint8_t)'\n') {
            if (byte == (uint8_t)'\r')
                start_candidate(p);
            else
                reset_to_sync(p);
            return VD_PARSE_NONE;
        }
        p->checksum = (uint8_t)(p->checksum + byte);
        p->key_len = 0;
        p->value_len = 0;
        p->key[0] = '\0';
        p->value[0] = '\0';
        p->state = VD_STATE_KEY;
        return VD_PARSE_NONE;

    case VD_STATE_KEY:
        p->checksum = (uint8_t)(p->checksum + byte);
        if (byte == (uint8_t)'\t') {
            if (p->key_len == 0) {
                reset_to_sync(p);
                return VD_PARSE_NONE;
            }
            p->key[p->key_len] = '\0';
            p->value_len = 0;
            p->value[0] = '\0';
            p->state = VD_STATE_VALUE;
            return VD_PARSE_NONE;
        }
        if (byte == (uint8_t)'\r' || byte == (uint8_t)'\n' || p->key_len + 1 >= VD_MAX_KEY) {
            reset_to_sync(p);
            return VD_PARSE_OVERFLOW;
        }
        p->key[p->key_len++] = (char)byte;
        p->key[p->key_len] = '\0';
        return VD_PARSE_NONE;

    case VD_STATE_VALUE:
        if (strcmp(p->key, "Checksum") == 0) {
            vd_parse_result_t result;
            p->checksum = (uint8_t)(p->checksum + byte);
            result = (p->checksum == 0) ? VD_PARSE_FRAME_VALID : VD_PARSE_FRAME_BAD_CHECKSUM;
            reset_to_sync(p);
            return result;
        }

        p->checksum = (uint8_t)(p->checksum + byte);
        if (byte == (uint8_t)'\r') {
            p->value[p->value_len] = '\0';
            if (store_field(p) < 0) {
                reset_to_sync(p);
                return VD_PARSE_OVERFLOW;
            }
            p->state = VD_STATE_FIELD_LF;
            return VD_PARSE_NONE;
        }
        if (byte == (uint8_t)'\n' || p->value_len + 1 >= VD_MAX_VALUE) {
            reset_to_sync(p);
            return VD_PARSE_OVERFLOW;
        }
        p->value[p->value_len++] = (char)byte;
        p->value[p->value_len] = '\0';
        return VD_PARSE_NONE;

    case VD_STATE_FIELD_LF:
        if (byte != (uint8_t)'\n') {
            if (byte == (uint8_t)'\r')
                start_candidate(p);
            else
                reset_to_sync(p);
            return VD_PARSE_NONE;
        }
        p->checksum = (uint8_t)(p->checksum + byte);
        p->key_len = 0;
        p->value_len = 0;
        p->key[0] = '\0';
        p->value[0] = '\0';
        p->state = VD_STATE_KEY;
        return VD_PARSE_NONE;
    }

    reset_to_sync(p);
    return VD_PARSE_NONE;
}

const char *vd_frame_get(const vd_parser_t *p, const char *key)
{
    size_t i;
    for (i = 0; i < p->field_count; ++i) {
        if (strcmp(p->fields[i].key, key) == 0)
            return p->fields[i].value;
    }
    return NULL;
}
