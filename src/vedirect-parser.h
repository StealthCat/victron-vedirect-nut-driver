#ifndef VEDIRECT_PARSER_H
#define VEDIRECT_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define VD_MAX_FIELDS 64
#define VD_MAX_KEY 32
#define VD_MAX_VALUE 128

typedef struct {
    char key[VD_MAX_KEY];
    char value[VD_MAX_VALUE];
} vd_field_t;

typedef enum {
    VD_PARSE_NONE = 0,
    VD_PARSE_FRAME_VALID = 1,
    VD_PARSE_FRAME_BAD_CHECKSUM = -1,
    VD_PARSE_OVERFLOW = -2
} vd_parse_result_t;

typedef enum {
    VD_STATE_SYNC = 0,
    VD_STATE_START_LF,
    VD_STATE_KEY,
    VD_STATE_VALUE,
    VD_STATE_FIELD_LF
} vd_parser_state_t;

typedef struct {
    vd_parser_state_t state;
    uint8_t checksum;
    vd_field_t fields[VD_MAX_FIELDS];
    size_t field_count;
    char key[VD_MAX_KEY];
    char value[VD_MAX_VALUE];
    size_t key_len;
    size_t value_len;
} vd_parser_t;

void vd_parser_init(vd_parser_t *p);
vd_parse_result_t vd_parser_feed(vd_parser_t *p, uint8_t byte);
const char *vd_frame_get(const vd_parser_t *p, const char *key);

#endif
