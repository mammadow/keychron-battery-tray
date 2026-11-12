#include "protocol.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wchar_t kc_towupper_ascii(wchar_t ch) {
    if (ch >= L'a' && ch <= L'z') {
        return (wchar_t)(ch - (L'a' - L'A'));
    }
    return ch;
}

static int kc_hex_val(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') {
        return ch - L'0';
    }
    ch = kc_towupper_ascii(ch);
    if (ch >= L'A' && ch <= L'F') {
        return 10 + (ch - L'A');
    }
    return -1;
}

static size_t kc_build_cmd_report(uint8_t *buf,
                                  size_t buf_cap,
                                  uint16_t report_len,
                                  uint8_t report_id,
                                  uint8_t cmd) {
    size_t target = (report_len >= 2U) ? (size_t)report_len : KEYCHRON_REPORT_SIZE;
    if (!buf || buf_cap < target || target < 2U) {
        return 0;
    }
    memset(buf, 0, target);
    buf[0] = report_id;
    buf[1] = cmd;
    return target;
}

size_t kc_build_status_report(uint8_t *buf, size_t buf_cap, uint16_t report_len) {
    return kc_build_cmd_report(buf, buf_cap, report_len,
                               KEYCHRON_REPORT_ID_CMD, KEYCHRON_CMD_STATUS);
}

size_t kc_build_recv_state_report(uint8_t *buf, size_t buf_cap, uint16_t report_len) {
    return kc_build_cmd_report(buf, buf_cap, report_len,
                               KEYCHRON_REPORT_ID_INFO, KEYCHRON_CMD_RECV_STATE);
}

bool kc_parse_recv_state_response(const uint8_t *buf,
                                  size_t len,
                                  uint16_t *out_vid,
                                  uint16_t *out_pid,
                                  bool *out_connected) {
    if (!buf || len < KEYCHRON_RECV_SLOTS_OFFSET + KEYCHRON_RECV_SLOT_STRIDE) {
        return false;
    }
    if ((buf[0] != KEYCHRON_REPORT_ID_INFO && buf[0] != KEYCHRON_REPORT_ID_INFO_RESP) ||
        buf[1] != KEYCHRON_CMD_RECV_STATE) {
        return false;
    }

    bool have_any = false;
    uint16_t first_vid = 0U;
    uint16_t first_pid = 0U;

    for (size_t slot = 0U; slot < KEYCHRON_RECV_MAX_SLOTS; ++slot) {
        size_t off = KEYCHRON_RECV_SLOTS_OFFSET + slot * KEYCHRON_RECV_SLOT_STRIDE;
        if (off + KEYCHRON_RECV_SLOT_STRIDE > len) {
            break;
        }
        uint16_t svid = (uint16_t)((uint16_t)buf[off] | ((uint16_t)buf[off + 1U] << 8));
        uint16_t spid = (uint16_t)((uint16_t)buf[off + 2U] | ((uint16_t)buf[off + 3U] << 8));
        bool conn = (buf[off + 4U] != 0U);
        if (svid == 0U || spid == 0U) {
            continue;
        }
        if (!have_any) {
            have_any = true;
            first_vid = svid;
            first_pid = spid;
        }
        if (conn) {
            if (out_vid) *out_vid = svid;
            if (out_pid) *out_pid = spid;
            if (out_connected) *out_connected = true;
            return true;
        }
    }

    if (!have_any) {
        return false;
    }
    if (out_vid) *out_vid = first_vid;
    if (out_pid) *out_pid = first_pid;
    if (out_connected) *out_connected = false;
    return true;
}

const char *kc_model_name_for_pid(uint16_t pid) {
    switch (pid) {
        case 0xD048U: return "Keychron M5";
        case 0xD049U: return "Keychron M6 8K";
        case 0xD056U: return "Keychron M7 8K";
        default:      return NULL;
    }
}

bool kc_parse_battery_status(const uint8_t *buf,
                             size_t len,
                             uint8_t *out_pct,
                             bool *out_charging) {
    if (!buf || !out_pct || len <= KEYCHRON_BATTERY_OFFSET) {
        return false;
    }
    if (buf[0] != KEYCHRON_REPORT_ID_RESP || buf[1] != KEYCHRON_CMD_STATUS) {
        return false;
    }
    uint8_t raw = buf[KEYCHRON_BATTERY_OFFSET];
    uint8_t pct = (uint8_t)(raw & KEYCHRON_BATTERY_LEVEL);
    if (pct > 100U) {
        return false;
    }
    *out_pct = pct;
    if (out_charging) {
        *out_charging = (raw & KEYCHRON_BATTERY_CHARGING) != 0U;
    }
    return true;
}

bool kc_parse_battery_response(const uint8_t *buf, size_t len, uint8_t *out_pct) {
    return kc_parse_battery_status(buf, len, out_pct, NULL);
}

int kc_extract_mi_from_path_w(const wchar_t *path) {
    if (!path) {
        return -1;
    }
    for (size_t i = 0; path[i] != L'\0'; ++i) {
        if ((path[i] == L'&' || path[i] == L'\\' || path[i] == L'#') &&
            kc_towupper_ascii(path[i + 1]) == L'M' &&
            kc_towupper_ascii(path[i + 2]) == L'I' &&
            path[i + 3] == L'_') {
            int hi = kc_hex_val(path[i + 4]);
            int lo = kc_hex_val(path[i + 5]);
            if (hi >= 0 && lo >= 0) {
                wchar_t next = path[i + 6];
                if (next == L'\0' || next == L'&' || next == L'#' || next == L'\\') {
                    return (hi << 4) | lo;
                }
            }
        }
    }
    return -1;
}

static bool kc_find_hex_tag_u16_w(const wchar_t *path, const char *tag3, uint16_t *out_val) {
    if (!path || !tag3) {
        return false;
    }
    wchar_t t0 = (wchar_t)(unsigned char)tag3[0];
    wchar_t t1 = (wchar_t)(unsigned char)tag3[1];
    wchar_t t2 = (wchar_t)(unsigned char)tag3[2];

    for (size_t i = 0; path[i] != L'\0'; ++i) {
        if (kc_towupper_ascii(path[i]) == t0 &&
            kc_towupper_ascii(path[i + 1]) == t1 &&
            kc_towupper_ascii(path[i + 2]) == t2 &&
            (path[i + 3] == L'_' || path[i + 3] == L'&')) {
            uint32_t acc = 0U;
            size_t digits = 0U;
            size_t k = i + 4U;
            while (digits < 8U) {
                int hv = kc_hex_val(path[k]);
                if (hv < 0) {
                    break;
                }
                acc = (acc << 4) | (uint32_t)hv;
                digits++;
                k++;
            }
            wchar_t term = path[k];
            if (digits >= 4U &&
                (term == L'\0' || term == L'&' || term == L'_' || term == L'#' || term == L'\\')) {
                *out_val = (uint16_t)(acc & 0xFFFFU);
                return true;
            }
        }
    }
    return false;
}

bool kc_path_might_match_vid_w(const wchar_t *path, uint16_t filter_vid) {
    if (!path || filter_vid == 0U) {
        return true;
    }
    uint16_t found_vid = 0U;
    if (!kc_find_hex_tag_u16_w(path, "VID", &found_vid)) {
        return true;
    }
    return found_vid == filter_vid;
}

bool kc_raw_path_matches_target_w(const wchar_t *path,
                                  uint16_t target_vid,
                                  uint16_t target_pid) {
    if (!path || !path[0]) {
        return false;
    }
    uint16_t found_vid = 0U;
    if (!kc_find_hex_tag_u16_w(path, "VID", &found_vid)) {
        return false;
    }
    if (target_vid != 0U && found_vid != target_vid) {
        return false;
    }
    if (target_pid != 0U) {
        uint16_t found_pid = 0U;
        if (!kc_find_hex_tag_u16_w(path, "PID", &found_pid) || found_pid != target_pid) {
            return false;
        }
    }
    return true;
}

bool kc_contains_ascii_ci_w(const wchar_t *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) {
        return false;
    }
    size_t nlen = strlen(needle);
    for (size_t i = 0; haystack[i] != L'\0'; ++i) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] != L'\0') {
            wchar_t hc = kc_towupper_ascii(haystack[i + j]);
            wchar_t nc = kc_towupper_ascii((wchar_t)(unsigned char)needle[j]);
            if (hc != nc) {
                break;
            }
            ++j;
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

uint16_t kc_clamp_report_len(uint16_t reported) {
    if (reported < 2U) {
        return KEYCHRON_REPORT_SIZE;
    }
    if (reported > KEYCHRON_MAX_REPORT_SIZE) {
        return KEYCHRON_MAX_REPORT_SIZE;
    }
    return reported;
}

uint32_t kc_clamp_poll_minutes(uint32_t minutes) {
    switch (minutes) {
        case 5U:
        case 15U:
        case 30U:
        case 60U:
            return minutes;
        default:
            return 30U;
    }
}

uint8_t kc_clamp_low_alert(uint32_t pct) {
    switch (pct) {
        case 0U:
        case 10U:
        case 15U:
        case 20U:
            return (uint8_t)pct;
        default:
            return (uint8_t)KEYCHRON_LOW_BATTERY_PCT;
    }
}

int kc_score_candidate(const kc_hid_candidate_t *cand,
                       uint16_t preferred_vid,
                       uint16_t preferred_pid,
                       int preferred_interface) {
    if (!cand) {
        return -1;
    }
    if (preferred_vid != 0U && cand->vid != preferred_vid) {
        return -1;
    }
    if (preferred_pid != 0U && cand->pid != preferred_pid) {
        return -1;
    }
    if (preferred_interface >= 0 && cand->interface_number != preferred_interface) {
        return -1;
    }

    /* When caller pinned exact interface and PID, allow it directly. */
    if (preferred_pid != 0U && preferred_interface >= 0) {
        return (cand->usage_page >= 0xFF00U) ? 1000 : 500;
    }

    /* Auto-detection must never touch standard mouse/keyboard/consumer collections. */
    if (cand->usage_page == 0x0001U || cand->usage_page == 0x000CU) {
        return -1;
    }

    int score = 10;
    if (cand->usage_page == KEYCHRON_VENDOR_USAGE_PAGE) {
        score += 1000;
    } else if (cand->usage_page >= 0xFF00U) {
        score += 200;
        if (cand->usage_page != KEYCHRON_VIA_USAGE_PAGE) {
            score += 100;
        }
    }
    if (cand->interface_number == KEYCHRON_DEFAULT_INTERFACE) {
        score += 300;
    }
    /* Prefer direct wired mouse over receiver dongle when both are connected. */
    if (preferred_pid == 0U && cand->pid != 0U && cand->pid != KEYCHRON_RECEIVER_PID) {
        score += 50;
    }
    return score;
}

void kc_poll_state_init(kc_poll_state_t *st) {
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

void kc_poll_state_record_success(kc_poll_state_t *st,
                                  uint8_t pct,
                                  bool charging,
                                  uint64_t now_ms) {
    if (!st) {
        return;
    }
    if (charging) {
        st->has_drain_anchor = false;
        st->drain_anchor_pct = pct;
        st->active_drain_ms = 0ULL;
        st->drain_gap_tainted = false;
    } else if (!st->has_drain_anchor || st->last_charging || pct > st->drain_anchor_pct) {
        st->has_drain_anchor = true;
        st->drain_anchor_pct = pct;
        st->active_drain_ms = 0ULL;
        st->drain_gap_tainted = false;
    } else if (st->has_reading) {
        uint64_t dt = (now_ms > st->last_success_ms) ? (now_ms - st->last_success_ms) : 0ULL;
        if (!st->drain_gap_tainted && dt > 0ULL && dt <= KEYCHRON_DRAIN_MAX_GAP_MS) {
            st->active_drain_ms += dt;
        } else if (pct < st->last_pct) {
            /* Exclude drops that happened while disconnected or asleep > max gap. */
            uint8_t lost_offline = (uint8_t)(st->last_pct - pct);
            st->drain_anchor_pct = (st->drain_anchor_pct >= lost_offline)
                ? (uint8_t)(st->drain_anchor_pct - lost_offline)
                : pct;
        }
        st->drain_gap_tainted = false;
    }

    st->has_reading = true;
    st->last_pct = pct;
    st->last_charging = charging;
    st->last_success_ms = now_ms;
    st->consecutive_failures = 0;
}

void kc_poll_state_record_failure(kc_poll_state_t *st) {
    if (!st) {
        return;
    }
    if (st->consecutive_failures < UINT32_MAX) {
        st->consecutive_failures++;
    }
    /* Exclude sleep/disconnect gaps from active drain-rate accumulation. */
    st->drain_gap_tainted = true;
}

uint32_t kc_next_wait_ms_for(const kc_poll_state_t *st, uint32_t normal_interval_ms) {
    static const uint32_t k_backoff_ms[] = {
        30U * 1000U,
        60U * 1000U,
        120U * 1000U,
        300U * 1000U,
    };
    if (normal_interval_ms < 10000U) {
        normal_interval_ms = 10000U;
    }
    if (!st || st->consecutive_failures == 0U) {
        if (st && st->has_reading && st->last_charging &&
            normal_interval_ms > KEYCHRON_CHARGING_POLL_MS) {
            return KEYCHRON_CHARGING_POLL_MS;
        }
        return normal_interval_ms;
    }
    size_t idx = (size_t)(st->consecutive_failures - 1U);
    size_t max_idx = (sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0])) - 1U;
    if (idx > max_idx) {
        idx = max_idx;
    }
    return k_backoff_ms[idx];
}

uint32_t kc_next_wait_ms_for_wake(const kc_poll_state_t *st,
                                   uint32_t normal_interval_ms,
                                   bool raw_wake_armed) {
    uint32_t base = kc_next_wait_ms_for(st, normal_interval_ms);
    if (!raw_wake_armed || !st || st->consecutive_failures < 5U) {
        return base;
    }
    if (normal_interval_ms < 10000U) {
        normal_interval_ms = 10000U;
    }
    return (normal_interval_ms > base) ? normal_interval_ms : base;
}

uint32_t kc_next_wait_ms(const kc_poll_state_t *st) {
    return kc_next_wait_ms_for(st, KEYCHRON_POLL_INTERVAL_MS);
}

bool kc_should_show_error_glyph(const kc_poll_state_t *st) {
    if (!st) {
        return true;
    }
    if (!st->has_reading) {
        return st->consecutive_failures > 0U;
    }
    return st->consecutive_failures > KEYCHRON_STALE_GRACE_MAX;
}

bool kc_check_low_battery_alert(kc_poll_state_t *st, uint8_t threshold_pct) {
    if (!st || threshold_pct == 0U || !st->has_reading ||
        st->last_charging || st->consecutive_failures > 0U) {
        return false;
    }
    if (st->last_pct <= threshold_pct) {
        if (!st->low_battery_notified) {
            st->low_battery_notified = true;
            return true;
        }
        return false;
    }
    if (st->last_pct >= (uint8_t)(threshold_pct + KEYCHRON_LOW_BATTERY_HYST)) {
        st->low_battery_notified = false;
    }
    return false;
}

bool kc_check_full_battery_alert(kc_poll_state_t *st, bool enabled) {
    if (!st || !enabled || !st->has_reading || st->consecutive_failures > 0U) {
        return false;
    }
    if (st->last_pct <= (uint8_t)(100U - KEYCHRON_LOW_BATTERY_HYST)) {
        st->full_battery_notified = false;
        return false;
    }
    if (st->last_charging && st->last_pct == 100U && !st->full_battery_notified) {
        st->full_battery_notified = true;
        return true;
    }
    return false;
}

bool kc_estimate_drain(const kc_poll_state_t *st,
                       uint32_t *out_rate_pct_x10,
                       uint32_t *out_hours_x10) {
    if (!st || !st->has_reading || st->last_charging || !st->has_drain_anchor ||
        st->active_drain_ms < KEYCHRON_DRAIN_MIN_SPAN_MS) {
        return false;
    }
    if (st->drain_anchor_pct <= st->last_pct) {
        return false;
    }
    uint32_t drop = (uint32_t)(st->drain_anchor_pct - st->last_pct);
    if (drop < 2U) {
        return false;
    }

    uint64_t num_rate = (uint64_t)drop * 36000000ULL + (st->active_drain_ms / 2ULL);
    uint64_t rate_x10 = num_rate / st->active_drain_ms;
    if (rate_x10 < 1ULL || rate_x10 > 500ULL) {
        return false;
    }

    uint64_t denom_ms = (uint64_t)drop * 360000ULL;
    uint64_t rem_hours_x10 = ((uint64_t)st->last_pct * st->active_drain_ms + (denom_ms / 2ULL)) / denom_ms;

    if (out_rate_pct_x10) {
        *out_rate_pct_x10 = (uint32_t)rate_x10;
    }
    if (out_hours_x10) {
        *out_hours_x10 = (uint32_t)rem_hours_x10;
    }
    return true;
}

bool kc_parse_u16_w(const wchar_t *arg, uint16_t *out) {
    if (!arg || !out || !arg[0] || arg[0] == L'-') {
        return false;
    }
    wchar_t *end = NULL;
    unsigned long val = wcstoul(arg, &end, 0);
    if (!end || *end != L'\0' || val > 0xFFFFUL) {
        return false;
    }
    *out = (uint16_t)val;
    return true;
}

bool kc_parse_int_range_w(const wchar_t *arg, int min_val, int max_val, int *out) {
    if (!arg || !out || !arg[0]) {
        return false;
    }
    wchar_t *end = NULL;
    long val = wcstol(arg, &end, 0);
    if (!end || *end != L'\0' || val < (long)min_val || val > (long)max_val) {
        return false;
    }
    *out = (int)val;
    return true;
}

size_t kc_json_escape(const char *src, char *dst, size_t dst_cap) {
    if (!src) {
        src = "";
    }
    size_t out_idx = 0U;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; ++p) {
        unsigned char c = *p;
        char seq[7];
        size_t seq_len = 0U;
        if (c == '"' || c == '\\') {
            seq[0] = '\\';
            seq[1] = (char)c;
            seq_len = 2U;
        } else if (c == '\b') {
            memcpy(seq, "\\b", 2);
            seq_len = 2U;
        } else if (c == '\f') {
            memcpy(seq, "\\f", 2);
            seq_len = 2U;
        } else if (c == '\n') {
            memcpy(seq, "\\n", 2);
            seq_len = 2U;
        } else if (c == '\r') {
            memcpy(seq, "\\r", 2);
            seq_len = 2U;
        } else if (c == '\t') {
            memcpy(seq, "\\t", 2);
            seq_len = 2U;
        } else if (c < 0x20U) {
            snprintf(seq, sizeof(seq), "\\u%04x", (unsigned)c);
            seq_len = 6U;
        } else {
            seq[0] = (char)c;
            seq_len = 1U;
        }

        if (dst && dst_cap > 0U && out_idx < dst_cap - 1U) {
            size_t avail = (dst_cap - 1U) - out_idx;
            size_t copy_n = seq_len < avail ? seq_len : avail;
            memcpy(dst + out_idx, seq, copy_n);
        }
        out_idx += seq_len;
    }
    if (dst && dst_cap > 0U) {
        size_t term = out_idx < dst_cap - 1U ? out_idx : dst_cap - 1U;
        dst[term] = '\0';
    }
    return out_idx;
}
