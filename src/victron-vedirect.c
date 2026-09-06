/*
 * victron-vedirect.c
 *
 * Native NUT driver for Victron SmartShunt/BMV VE.Direct Text telemetry.
 * This is an independently written driver intended for NUT 2.8.x.
 *
 * It maps SmartShunt fields to standard NUT variables and can optionally
 * infer OL/OB from battery-current direction for DC-backed systems.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"
#include "main.h"
#include "serial.h"
#include "vedirect-parser.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>

#define DRIVER_NAME "Victron SmartShunt VE.Direct driver"
#define DRIVER_VERSION "0.4.0"
#define READ_CHUNK 512
#define READ_INITIAL_ATTEMPTS 12
#define READ_TIMEOUT_USEC 100000

upsdrv_info_t upsdrv_info = {
    DRIVER_NAME,
    DRIVER_VERSION,
    "OpenAI-generated community driver",
    DRV_EXPERIMENTAL,
    { NULL }
};

static vd_parser_t parser;
static double current_deadband_a = 0.5;
static double max_load_watts = 2000.0;
static double battery_capacity_ah = 400.0;
static double battery_actual_capacity_ah = 400.0;
static int status_inferred = 1;
static int publish_raw = 0;
static int have_last_current = 0;
static double last_current_a = 0.0;

static int parse_long_value(const char *s, long *out)
{
    char *end = NULL;
    long v;
    if (!s || !*s || strcmp(s, "---") == 0) return 0;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return 0;
    *out = v;
    return 1;
}

static double parse_nonnegative_double_or(const char *s, double fallback)
{
    char *end = NULL;
    double v;
    if (!s || !*s) return fallback;
    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(v) || v < 0.0) return fallback;
    return v;
}

static double parse_positive_double_or(const char *s, double fallback)
{
    double v = parse_nonnegative_double_or(s, fallback);
    return (v > 0.0) ? v : fallback;
}

static void raw_name(char *dst, size_t dstlen, const char *key)
{
    const char *prefix = "experimental.ve-direct.";
    size_t used = 0;
    if (dstlen == 0) return;
    while (*prefix && used + 1 < dstlen) dst[used++] = *prefix++;
    while (*key && used + 1 < dstlen) {
        unsigned char c = (unsigned char)*key++;
        dst[used++] = (char)((isalnum(c) || c == '_' || c == '-') ? c : '_');
    }
    dst[used] = '\0';
}

static void publish_raw_fields(const vd_parser_t *frame)
{
    size_t i;
    char name[96];
    if (!publish_raw) return;
    for (i = 0; i < frame->field_count; ++i) {
        raw_name(name, sizeof(name), frame->fields[i].key);
        dstate_setinfo(name, "%s", frame->fields[i].value);
    }
}

static void publish_identity(const vd_parser_t *frame)
{
    const char *model = vd_frame_get(frame, "BMV");
    const char *serial = vd_frame_get(frame, "SER#");
    const char *fw = vd_frame_get(frame, "FWE");
    const char *pid = vd_frame_get(frame, "PID");
    if (!fw) fw = vd_frame_get(frame, "FW");
    dstate_setinfo("device.mfr", "Victron Energy");
    dstate_setinfo("ups.mfr", "Victron Energy");
    dstate_setinfo("device.type", "ups");
    if (model && *model && strcmp(model, "---") != 0) {
        dstate_setinfo("device.model", "%s", model);
        dstate_setinfo("ups.model", "%s", model);
    } else {
        dstate_setinfo("device.model", "SmartShunt / VE.Direct battery monitor");
        dstate_setinfo("ups.model", "SmartShunt / VE.Direct battery monitor");
    }
    if (serial && *serial) {
        dstate_setinfo("device.serial", "%s", serial);
        dstate_setinfo("ups.serial", "%s", serial);
    }
    if (fw && *fw) dstate_setinfo("ups.firmware", "%s", fw);
    if (pid && *pid) dstate_setinfo("ups.productid", "%s", pid);
}

static void publish_load_values(const vd_parser_t *frame)
{
    const char *s_p = vd_frame_get(frame, "P");
    const char *s_v = vd_frame_get(frame, "V");
    const char *s_i = vd_frame_get(frame, "I");
    long p_w_raw = 0, v_mv = 0, i_ma = 0;
    int have_p = parse_long_value(s_p, &p_w_raw);
    int have_v = parse_long_value(s_v, &v_mv);
    int have_i = parse_long_value(s_i, &i_ma);
    double load_w = 0.0;
    int have_load = 0;
    if (have_i) {
        if (i_ma < 0) {
            if (have_p) load_w = fabs((double)p_w_raw);
            else if (have_v) load_w = ((double)v_mv / 1000.0) * ((double)(-i_ma) / 1000.0);
            else return;
        }
        have_load = 1;
    } else if (have_p) {
        if (p_w_raw < 0) load_w = -(double)p_w_raw;
        have_load = 1;
    }
    if (!have_load) return;
    dstate_setinfo("ups.realpower", "%.1f", load_w);
    dstate_setinfo("ups.realpower.nominal", "%.0f", max_load_watts);
    dstate_setinfo("ups.load", "%.1f", (load_w / max_load_watts) * 100.0);
}

static void publish_battery_values(const vd_parser_t *frame)
{
    const char *s;
    long v;
    s = vd_frame_get(frame, "V");
    if (parse_long_value(s, &v)) dstate_setinfo("battery.voltage", "%.3f", (double)v / 1000.0);
    s = vd_frame_get(frame, "I");
    if (parse_long_value(s, &v)) {
        last_current_a = (double)v / 1000.0;
        have_last_current = 1;
        dstate_setinfo("battery.current", "%.3f", last_current_a);
        if (last_current_a > current_deadband_a) dstate_setinfo("battery.charger.status", "charging");
        else if (last_current_a < -current_deadband_a) dstate_setinfo("battery.charger.status", "discharging");
        else dstate_setinfo("battery.charger.status", "resting");
    }
    s = vd_frame_get(frame, "SOC");
    if (parse_long_value(s, &v) && v >= 0 && v <= 1000) {
        double soc_pct = (double)v / 10.0;
        double remaining_ah = battery_actual_capacity_ah * soc_pct / 100.0;
        dstate_setinfo("battery.charge", "%.1f", soc_pct);
        dstate_setinfo("experimental.battery.capacity.remaining", "%.1f", remaining_ah);
    }
    s = vd_frame_get(frame, "CE");
    if (parse_long_value(s, &v)) dstate_setinfo("experimental.battery.consumed_ah", "%.1f", (double)v / 1000.0);
    s = vd_frame_get(frame, "TTG");
    if (parse_long_value(s, &v)) {
        if (v >= 0) dstate_setinfo("battery.runtime", "%ld", v * 60L);
        else dstate_delinfo("battery.runtime");
    }
    s = vd_frame_get(frame, "T");
    if (parse_long_value(s, &v)) dstate_setinfo("battery.temperature", "%ld", v);
}

static void publish_status(void)
{
    if (!status_inferred || !have_last_current) return;
    status_init();
    if (last_current_a < -current_deadband_a) status_set("OB");
    else status_set("OL");
    status_commit();
}

static void apply_frame(const vd_parser_t *frame)
{
    publish_identity(frame);
    publish_battery_values(frame);
    publish_load_values(frame);
    publish_raw_fields(frame);
    publish_status();
    dstate_dataok();
}

static int process_serial_bytes(const char *buf, int nread)
{
    int i;
    int valid_frames = 0;
    for (i = 0; i < nread; ++i) {
        vd_parse_result_t r = vd_parser_feed(&parser, (uint8_t)(unsigned char)buf[i]);
        if (r == VD_PARSE_FRAME_VALID) {
            apply_frame(&parser);
            valid_frames++;
        } else if (r == VD_PARSE_FRAME_BAD_CHECKSUM) {
            upsdebugx(2, "VE.Direct block rejected: checksum mismatch");
        } else if (r == VD_PARSE_OVERFLOW) {
            upsdebugx(2, "VE.Direct block rejected: parser field/buffer overflow");
        }
    }
    return valid_frames;
}

static int read_latest_valid_frame(void)
{
    char buf[READ_CHUNK];
    int attempt;
    int valid_frames = 0;
    int nread;
    for (attempt = 0; attempt < READ_INITIAL_ATTEMPTS; ++attempt) {
        nread = ser_get_buf(upsfd, buf, sizeof(buf), 0, READ_TIMEOUT_USEC);
        if (nread < 0) return valid_frames > 0 ? valid_frames : -1;
        if (nread == 0) continue;
        valid_frames += process_serial_bytes(buf, nread);
        if (valid_frames > 0) break;
    }
    if (valid_frames == 0) return 0;
    for (;;) {
        nread = ser_get_buf(upsfd, buf, sizeof(buf), 0, 0);
        if (nread < 0) {
            upsdebugx(2, "VE.Direct serial drain ended with a read error after %d valid frame(s)", valid_frames);
            break;
        }
        if (nread == 0) break;
        valid_frames += process_serial_bytes(buf, nread);
    }
    upsdebugx(3, "Processed %d valid VE.Direct frame(s) in this poll; newest frame published", valid_frames);
    return valid_frames;
}

static void set_rs232_power_lines(void)
{
#if defined(TIOCMBIS) && defined(TIOCM_DTR) && defined(TIOCM_RTS)
    int bits = TIOCM_DTR | TIOCM_RTS;
    if (ioctl(upsfd, TIOCMBIS, &bits) < 0)
        upslog_with_errno(LOG_WARNING, "Could not assert DTR/RTS for VE.Direct RS-232 adapter");
    else
        upsdebugx(1, "Asserted DTR and RTS for VE.Direct RS-232 adapter");
#else
    upslogx(LOG_WARNING, "rs232_power requested, but this platform lacks TIOCMBIS/TIOCM_DTR/TIOCM_RTS");
#endif
}

void upsdrv_initinfo(void)
{
    int r;
    dstate_setinfo("device.mfr", "Victron Energy");
    dstate_setinfo("ups.mfr", "Victron Energy");
    dstate_setinfo("device.model", "SmartShunt / VE.Direct battery monitor");
    dstate_setinfo("ups.model", "SmartShunt / VE.Direct battery monitor");
    dstate_setinfo("device.type", "ups");
    dstate_setinfo("battery.capacity.nominal", "%.1f", battery_capacity_ah);
    dstate_setinfo("battery.capacity", "%.1f", battery_actual_capacity_ah);
    r = read_latest_valid_frame();
    if (r <= 0) upslogx(LOG_WARNING, "No valid VE.Direct Text block received during initialization; will retry during polling");
}

void upsdrv_updateinfo(void)
{
    int r = read_latest_valid_frame();
    if (r <= 0) {
        if (r < 0) upslogx(LOG_WARNING, "VE.Direct serial read failed");
        else upsdebugx(1, "No complete valid VE.Direct block received in polling window");
        dstate_datastale();
    }
}

void upsdrv_shutdown(void)
{
    upslogx(LOG_ERR, "SmartShunt is a monitor and cannot switch inverter/load power; shutdown command is unsupported");
    if (handling_upsdrv_shutdown > 0) set_exit_flag(EF_EXIT_FAILURE);
}

void upsdrv_help(void)
{
    printf("Driver-specific options:\n");
    printf("  status_mode=inferred|none   infer OL/OB from battery current, or publish no OL/OB\n");
    printf("  current_deadband=<amps>     threshold for charging/discharging and inferred OB (default 0.5)\n");
    printf("  max_load_watts=<watts>      inverter full-load rating used for ups.load (default 2000)\n");
    printf("  battery_capacity_ah=<Ah>    design/rated bank capacity (default 400)\n");
    printf("  battery_actual_capacity_ah=<Ah> actual full-charge capacity; defaults to design capacity\n");
    printf("  publish_raw                 publish experimental.ve-direct.* telemetry\n");
    printf("  rs232_power                 assert DTR+RTS to power Victron isolated RS-232 interface\n");
}

void upsdrv_tweak_prognames(void) {}

void upsdrv_makevartable(void)
{
    addvar(VAR_VALUE, "status_mode", "OL/OB handling: inferred (default) or none");
    addvar(VAR_VALUE, "current_deadband", "Current threshold in amperes (default 0.5)");
    addvar(VAR_VALUE, "max_load_watts", "Inverter full-load rating in watts for ups.load (default 2000)");
    addvar(VAR_VALUE, "battery_capacity_ah", "Design/rated battery-bank capacity in Ah (default 400)");
    addvar(VAR_VALUE, "battery_actual_capacity_ah", "Actual full-charge battery-bank capacity in Ah; defaults to design capacity");
    addvar(VAR_FLAG, "publish_raw", "Publish experimental.ve-direct.* raw fields");
    addvar(VAR_FLAG, "rs232_power", "Assert DTR and RTS for Victron VE.Direct-to-RS232 adapter");
}

void upsdrv_initups(void)
{
    const char *s;
    vd_parser_init(&parser);
    poll_interval = 1;
    s = getval("status_mode");
    if (s) {
        if (!strcasecmp(s, "none")) status_inferred = 0;
        else if (!strcasecmp(s, "inferred")) status_inferred = 1;
        else fatalx(EXIT_FAILURE, "status_mode must be 'inferred' or 'none'");
    }
    current_deadband_a = parse_nonnegative_double_or(getval("current_deadband"), 0.5);
    max_load_watts = parse_positive_double_or(getval("max_load_watts"), 2000.0);
    battery_capacity_ah = parse_positive_double_or(getval("battery_capacity_ah"), 400.0);
    battery_actual_capacity_ah = parse_positive_double_or(getval("battery_actual_capacity_ah"), battery_capacity_ah);
    publish_raw = testvar("publish_raw") ? 1 : 0;
    upsfd = ser_open(device_path);
    ser_set_speed(upsfd, device_path, B19200);
    if (testvar("rs232_power")) set_rs232_power_lines();
}

void upsdrv_cleanup(void)
{
    ser_close(upsfd, device_path);
}
