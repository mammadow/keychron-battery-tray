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

size_t kc_build_status_report(uint8_t *buf, size_t buf_cap, uint16_t report_len) {
    size_t target = (report_len >= 2U) ? (size_t)report_len : KEYCHRON_REPORT_SIZE;
    if (!buf || buf_cap < target || target < 2U) {
        return 0;
    }
    memset(buf, 0, target);
    buf[0] = KEYCHRON_REPORT_ID_CMD;
    buf[1] = KEYCHRON_CMD_STATUS;
    return target;
}

bool kc_parse_battery_response(const uint8_t *buf, size_t len, uint8_t *out_pct) {
    if (!buf || !out_pct || len <= KEYCHRON_BATTERY_OFFSET) {
        return false;
    }
    if (buf[0] != KEYCHRON_REPORT_ID_RESP || buf[1] != KEYCHRON_CMD_STATUS) {
        return false;
    }
    uint8_t pct = buf[KEYCHRON_BATTERY_OFFSET];
    if (pct > 100U) {
        return false;
    }
    *out_pct = pct;
    return true;
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
    return score;
}

void kc_poll_state_init(kc_poll_state_t *st) {
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

void kc_poll_state_record_success(kc_poll_state_t *st, uint8_t pct, uint64_t now_ms) {
    if (!st) {
        return;
    }
    st->has_reading = true;
    st->last_pct = pct;
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
        return normal_interval_ms;
    }
    size_t idx = (size_t)(st->consecutive_failures - 1U);
    size_t max_idx = (sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0])) - 1U;
    if (idx > max_idx) {
        idx = max_idx;
    }
    return k_backoff_ms[idx];
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
    if (!st || threshold_pct == 0U || !st->has_reading || st->consecutive_failures > 0U) {
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
