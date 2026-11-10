#ifndef KEYCHRON_PROTOCOL_H
#define KEYCHRON_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define KEYCHRON_VID               0x3434U
#define KEYCHRON_REPORT_ID_CMD     0xB3U
#define KEYCHRON_REPORT_ID_RESP    0xB4U
#define KEYCHRON_CMD_STATUS        0x06U
#define KEYCHRON_BATTERY_OFFSET    20U
#define KEYCHRON_REPORT_SIZE       64U
#define KEYCHRON_VENDOR_USAGE_PAGE 0xFFC1U
#define KEYCHRON_VIA_USAGE_PAGE    0xFF60U
#define KEYCHRON_DEFAULT_INTERFACE 4

#define KEYCHRON_POLL_INTERVAL_MS  (30U * 60U * 1000U)
#define KEYCHRON_LOW_BATTERY_PCT   15U
#define KEYCHRON_LOW_BATTERY_HYST  5U
#define KEYCHRON_STALE_GRACE_MAX   2U

typedef struct kc_hid_candidate {
    uint16_t vid;
    uint16_t pid;
    uint16_t usage_page;
    uint16_t usage;
    int interface_number;
} kc_hid_candidate_t;

typedef struct kc_poll_state {
    bool has_reading;
    uint8_t last_pct;
    uint64_t last_success_ms;
    uint32_t consecutive_failures;
    bool low_battery_notified;
} kc_poll_state_t;

/**
 * Build a zero-padded Keychron battery status command report.
 * Returns the report byte count written, or 0 if buf_cap is insufficient.
 */
size_t kc_build_status_report(uint8_t *buf, size_t buf_cap, uint16_t report_len);

/**
 * Validate and extract battery percentage (0..100) from a Keychron 0xB4 0x06 packet.
 */
bool kc_parse_battery_response(const uint8_t *buf, size_t len, uint8_t *out_pct);

/**
 * Extract USB composite interface number (e.g. "&MI_04" -> 4) from a Windows HID path.
 * Returns -1 when no MI_XX segment is present.
 */
int kc_extract_mi_from_path_w(const wchar_t *path);

/**
 * Score a HID interface candidate for Keychron battery queries.
 * Returns < 0 if excluded; higher positive scores indicate better vendor matches.
 */
int kc_score_candidate(const kc_hid_candidate_t *cand,
                       uint16_t preferred_vid,
                       uint16_t preferred_pid,
                       int preferred_interface);

void kc_poll_state_init(kc_poll_state_t *st);
void kc_poll_state_record_success(kc_poll_state_t *st, uint8_t pct, uint64_t now_ms);
void kc_poll_state_record_failure(kc_poll_state_t *st);

/** Return milliseconds until next poll using default 30m steady interval. */
uint32_t kc_next_wait_ms(const kc_poll_state_t *st);

/** Return milliseconds until next poll using configurable steady interval (clamped >= 10s). */
uint32_t kc_next_wait_ms_for(const kc_poll_state_t *st, uint32_t normal_interval_ms);

/** Return true if tray icon should display error ("!") rather than last_pct or "--". */
bool kc_should_show_error_glyph(const kc_poll_state_t *st);

/**
 * Return true once when battery drops to or below threshold_pct (0 disables alerts);
 * re-arms once battery rises at least KEYCHRON_LOW_BATTERY_HYST points above threshold.
 */
bool kc_check_low_battery_alert(kc_poll_state_t *st, uint8_t threshold_pct);

/** Parse unsigned 16-bit integer (decimal or 0x hex) from wide string. */
bool kc_parse_u16_w(const wchar_t *arg, uint16_t *out);

/** Parse integer within [min_val, max_val] (decimal or 0x hex) from wide string. */
bool kc_parse_int_range_w(const wchar_t *arg, int min_val, int max_val, int *out);

/** Escape UTF-8 text as JSON string contents (without outer quotes). Returns chars needed. */
size_t kc_json_escape(const char *src, char *dst, size_t dst_cap);

#endif /* KEYCHRON_PROTOCOL_H */
