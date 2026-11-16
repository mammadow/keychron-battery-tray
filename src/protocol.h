#ifndef KEYCHRON_PROTOCOL_H
#define KEYCHRON_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define KEYCHRON_VID                 0x3434U
#define KEYCHRON_RECEIVER_PID        0xD028U
#define KEYCHRON_REPORT_ID_CMD       0xB3U
#define KEYCHRON_REPORT_ID_RESP      0xB4U
#define KEYCHRON_REPORT_ID_INFO      0xB5U
#define KEYCHRON_REPORT_ID_INFO_RESP 0xB6U
#define KEYCHRON_CMD_RECV_STATE      0x03U
#define KEYCHRON_CMD_STATUS          0x06U
#define KEYCHRON_RECV_SLOTS_OFFSET   3U
#define KEYCHRON_RECV_SLOT_STRIDE    5U
#define KEYCHRON_RECV_MAX_SLOTS      3U
#define KEYCHRON_BATTERY_OFFSET      20U
#define KEYCHRON_BATTERY_CHARGING    0x80U
#define KEYCHRON_BATTERY_LEVEL       0x7FU
#define KEYCHRON_REPORT_SIZE         64U
#define KEYCHRON_MAX_REPORT_SIZE     256U
#define KEYCHRON_VENDOR_USAGE_PAGE   0xFFC1U
#define KEYCHRON_VIA_USAGE_PAGE      0xFF60U
#define KEYCHRON_DEFAULT_INTERFACE   4

#define KEYCHRON_POLL_INTERVAL_MS  (30U * 60U * 1000U)
#define KEYCHRON_CHARGING_POLL_MS  (2U * 60U * 1000U)
#define KEYCHRON_LOW_BATTERY_PCT   15U
#define KEYCHRON_LOW_BATTERY_HYST  5U
#define KEYCHRON_STALE_GRACE_MAX   2U
#define KEYCHRON_DRAIN_MAX_GAP_MS  (90ULL * 60ULL * 1000ULL)
#define KEYCHRON_DRAIN_MIN_SPAN_MS (30ULL * 60ULL * 1000ULL)

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
    bool last_charging;
    uint64_t last_success_ms;
    uint32_t consecutive_failures;
    bool low_battery_notified;
    bool full_battery_notified;
    bool has_drain_anchor;
    uint8_t drain_anchor_pct;
    uint64_t active_drain_ms;
    bool drain_gap_tainted;
} kc_poll_state_t;

/**
 * Build a zero-padded Keychron battery status command report.
 * Returns the report byte count written, or 0 if buf_cap is insufficient.
 */
size_t kc_build_status_report(uint8_t *buf, size_t buf_cap, uint16_t report_len);

/**
 * Validate and extract battery percentage (0..100) and charging bit (0x80)
 * from a Keychron 0xB4 0x06 packet.
 */
bool kc_parse_battery_status(const uint8_t *buf,
                             size_t len,
                             uint8_t *out_pct,
                             bool *out_charging);

/** Convenience wrapper extracting only percentage (0..100). */
bool kc_parse_battery_response(const uint8_t *buf, size_t len, uint8_t *out_pct);

/**
 * Build a zero-padded Keychron receiver paired-device query report (0xB5 0x03).
 * Returns report byte count written, or 0 if buf_cap is insufficient.
 */
size_t kc_build_recv_state_report(uint8_t *buf, size_t buf_cap, uint16_t report_len);

/**
 * Parse a Keychron receiver state response (0xB5/0xB6 0x03) for the paired mouse's
 * VID/PID and active link status. Prefers a connected slot, falling back to the
 * first non-zero paired slot if all slots are idle/asleep.
 */
bool kc_parse_recv_state_response(const uint8_t *buf,
                                  size_t len,
                                  uint16_t *out_vid,
                                  uint16_t *out_pid,
                                  bool *out_connected);

/**
 * Return a known Keychron marketing model name for a mouse USB PID,
 * or NULL if the PID is not in the built-in verified lookup table.
 */
const char *kc_model_name_for_pid(uint16_t pid);

/**
 * Extract USB composite interface number (e.g. "&MI_04" -> 4) from a Windows HID path.
 * Returns -1 when no MI_XX segment is present.
 */
int kc_extract_mi_from_path_w(const wchar_t *path);

/**
 * Return true unless a Windows HID path explicitly encodes a non-matching VID_XXXX.
 * Paths without VID_ (e.g. Bluetooth) return true so attributes can be probed.
 */
bool kc_path_might_match_vid_w(const wchar_t *path, uint16_t filter_vid);

/**
 * Strictly match a Windows Raw Input device name against target VID (and optional
 * non-zero PID). Returns false for touchpads, virtual mice, and other vendors.
 */
bool kc_raw_path_matches_target_w(const wchar_t *path,
                                  uint16_t target_vid,
                                  uint16_t target_pid);

/** Case-insensitive substring check for ASCII needle in wide haystack. */
bool kc_contains_ascii_ci_w(const wchar_t *haystack, const char *needle);

/** Clamp HID report size to [KEYCHRON_REPORT_SIZE fallback, KEYCHRON_MAX_REPORT_SIZE]. */
uint16_t kc_clamp_report_len(uint16_t reported);

/** Clamp poll interval to supported menu values (5, 15, 30, 60; default 30). */
uint32_t kc_clamp_poll_minutes(uint32_t minutes);

/** Clamp low-battery threshold to supported values (0, 10, 15, 20; default 15). */
uint8_t kc_clamp_low_alert(uint32_t pct);

/**
 * Score a HID interface candidate for Keychron battery queries.
 * Returns < 0 if excluded; higher positive scores indicate better vendor matches.
 */
int kc_score_candidate(const kc_hid_candidate_t *cand,
                       uint16_t preferred_vid,
                       uint16_t preferred_pid,
                       int preferred_interface);

void kc_poll_state_init(kc_poll_state_t *st);
void kc_poll_state_record_success(kc_poll_state_t *st,
                                  uint8_t pct,
                                  bool charging,
                                  uint64_t now_ms);
void kc_poll_state_record_failure(kc_poll_state_t *st);

/** Return milliseconds until next poll using default 30m steady interval. */
uint32_t kc_next_wait_ms(const kc_poll_state_t *st);

/** Return milliseconds until next poll using configurable steady interval (clamped >= 10s). */
uint32_t kc_next_wait_ms_for(const kc_poll_state_t *st, uint32_t normal_interval_ms);

/**
 * Return milliseconds until next poll, scaling prolonged sleep retries up to
 * normal_interval_ms once Raw Input mouse wake is actively armed (>= 5 misses).
 */
uint32_t kc_next_wait_ms_for_wake(const kc_poll_state_t *st,
                                   uint32_t normal_interval_ms,
                                   bool raw_wake_armed);

/** Return true if tray icon should display error ("!") rather than last_pct or "--". */
bool kc_should_show_error_glyph(const kc_poll_state_t *st);

/**
 * Return true once when discharging battery drops to or below threshold_pct (0 disables alerts);
 * re-arms once battery rises at least KEYCHRON_LOW_BATTERY_HYST points above threshold.
 */
bool kc_check_low_battery_alert(kc_poll_state_t *st, uint8_t threshold_pct);

/**
 * Return true once when charging reaches 100% (if enabled);
 * re-arms once battery drops to or below (100 - KEYCHRON_LOW_BATTERY_HYST).
 */
bool kc_check_full_battery_alert(kc_poll_state_t *st, bool enabled);

/**
 * Estimate active-session battery drain rate (tenths of % per hour, e.g. 18 = 1.8%/h)
 * and remaining active runtime (tenths of an hour). Returns false until >= 2% drop
 * over >= 30 active discharging minutes is observed.
 */
bool kc_estimate_drain(const kc_poll_state_t *st,
                       uint32_t *out_rate_pct_x10,
                       uint32_t *out_hours_x10);

/** Parse unsigned 16-bit integer (decimal or 0x hex) from wide string. */
bool kc_parse_u16_w(const wchar_t *arg, uint16_t *out);

/** Parse integer within [min_val, max_val] (decimal or 0x hex) from wide string. */
bool kc_parse_int_range_w(const wchar_t *arg, int min_val, int max_val, int *out);

/** Escape UTF-8 text as JSON string contents (without outer quotes). Returns chars needed. */
size_t kc_json_escape(const char *src, char *dst, size_t dst_cap);

#define KC_PROT_LONGEVITY_MIN_PCT  25U
#define KC_PROT_LONGEVITY_MAX_PCT  80U
#define KC_PROT_BALANCED_MIN_PCT   20U
#define KC_PROT_BALANCED_MAX_PCT   90U
#define KC_PROT_HYST_PCT           5U
#define KC_CAL_CYCLE_TARGET_PCT    1000U
#define KC_CAL_MIN_DAYS_CYCLES_PCT 100U
#define KC_CAL_INTERVAL_DAYS       90U
#define KC_CAL_MAX_IDLE_DAYS       180U
#define KC_CAL_SNOOZE_DAYS         14U
#define KC_LAPTOP_GUARD_PCT        25U
#define KC_LAPTOP_GUARD_MOUSE_MIN  50U

typedef enum kc_protector_mode {
    KC_PROT_OFF = 0,
    KC_PROT_LONGEVITY = 1,
    KC_PROT_BALANCED = 2
} kc_protector_mode_t;

typedef enum kc_battery_visual {
    KC_VIS_NORMAL = 0,
    KC_VIS_LOW = 1,
    KC_VIS_CHARGING = 2,
    KC_VIS_STALE = 3,
    KC_VIS_CHARGE_SOON = 4,
    KC_VIS_CHARGE_LIMIT = 5
} kc_battery_visual_t;

typedef struct kc_protector_persist {
    uint8_t mode;
    bool laptop_guard;
    bool calibrating;
    uint32_t cum_charged_pct;
    uint64_t last_cal_unix;
    uint64_t cal_snooze_until;
} kc_protector_persist_t;

typedef struct kc_protector_runtime {
    bool has_prev_pct;
    uint8_t prev_pct;
    bool charge_soon_notified;
    bool charge_limit_notified;
    bool laptop_guard_notified;
    bool cal_due_notified;
    bool pending_cal_complete;
} kc_protector_runtime_t;

/** Initialize persisted Battery Protector configuration to recommended Longevity defaults. */
void kc_protector_persist_init(kc_protector_persist_t *p, uint64_t now_unix);

/** Clamp raw numeric mode value into a valid kc_protector_mode_t value. */
uint8_t kc_clamp_protector_mode(uint32_t raw_mode);

/** Return active [min_pct, max_pct] bounds for the given protector mode (0..100 when Off). */
void kc_protector_bounds(uint8_t mode, uint8_t *out_min_pct, uint8_t *out_max_pct);

/** Initialize runtime Battery Protector session alert flags. */
void kc_protector_runtime_init(kc_protector_runtime_t *rt);

/** Reset per-device session flags (preserving persistent calibration & cycle counters). */
void kc_protector_runtime_reset_session(kc_protector_runtime_t *rt);

/**
 * Record a valid battery poll sample for cycle accumulation & automatic 100% calibration
 * completion. Sets *out_dirty if persisted state changed, and *out_cal_completed once when
 * an active calibration reaches 100% while charging.
 */
bool kc_protector_record_sample(kc_protector_persist_t *p,
                                kc_protector_runtime_t *rt,
                                uint8_t pct,
                                bool charging,
                                uint64_t now_unix,
                                bool *out_cal_completed);

/**
 * Compute days elapsed and full-equivalent cycles (tenths, e.g. 250 = 25.0) since last
 * calibration, returning true when calibration is currently recommended (due & not snoozed).
 */
bool kc_protector_is_cal_due(const kc_protector_persist_t *p,
                             uint64_t now_unix,
                             uint32_t *out_days,
                             uint32_t *out_cycles_x10);

/** Enable or cancel a 100% calibration run until the next full charge. */
bool kc_protector_set_calibrating(kc_protector_persist_t *p,
                                  kc_protector_runtime_t *rt,
                                  bool enable);

/** Snooze the calibration reminder for KC_CAL_SNOOZE_DAYS from now_unix. */
void kc_protector_snooze_calibration(kc_protector_persist_t *p,
                                     kc_protector_runtime_t *rt,
                                     uint64_t now_unix);

/** Reset cycle counters and mark battery calibrated as of now_unix. */
void kc_protector_reset_calibration(kc_protector_persist_t *p,
                                    kc_protector_runtime_t *rt,
                                    uint64_t now_unix);

/** Compute prioritized tray visual state (Red Low < Yellow Charge Soon < Green Charging < Cyan Limit). */
kc_battery_visual_t kc_compute_battery_visual(const kc_poll_state_t *poll,
                                              const kc_protector_persist_t *p,
                                              uint8_t low_alert_pct);

/** Return true once when discharging enters recommended plug-in floor (above urgent low_alert_pct). */
bool kc_check_charge_soon_alert(const kc_poll_state_t *poll,
                                const kc_protector_persist_t *p,
                                kc_protector_runtime_t *rt,
                                uint8_t low_alert_pct);

/** Return true once when charging reaches recommended ceiling max_pct (disabled while calibrating). */
bool kc_check_charge_limit_alert(const kc_poll_state_t *poll,
                                 const kc_protector_persist_t *p,
                                 kc_protector_runtime_t *rt);

/**
 * Return true once when charging from an unplugged laptop battery that dropped <= KC_LAPTOP_GUARD_PCT
 * while mouse already has >= KC_LAPTOP_GUARD_MOUSE_MIN (skipped while calibrating).
 */
bool kc_check_laptop_guard_alert(const kc_poll_state_t *poll,
                                 const kc_protector_persist_t *p,
                                 kc_protector_runtime_t *rt,
                                 bool has_sys_battery,
                                 bool on_laptop_battery,
                                 uint8_t laptop_pct);

/** Return true once when calibration is recommended and hasn't been notified this session. */
bool kc_check_cal_due_alert(const kc_poll_state_t *poll,
                            const kc_protector_persist_t *p,
                            kc_protector_runtime_t *rt,
                            uint64_t now_unix);

#endif /* KEYCHRON_PROTOCOL_H */
