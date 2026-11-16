#include "glyph_blit.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KC_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static void test_build_and_parse_report(void) {
    uint8_t buf[64];
    size_t n = kc_build_status_report(buf, sizeof(buf), 64U);
    KC_CHECK(n == 64U);
    KC_CHECK(buf[0] == KEYCHRON_REPORT_ID_CMD);
    KC_CHECK(buf[1] == KEYCHRON_CMD_STATUS);
    for (size_t i = 2; i < 64U; ++i) {
        KC_CHECK(buf[i] == 0U);
    }

    uint8_t resp[64] = {0};
    resp[0] = KEYCHRON_REPORT_ID_RESP;
    resp[1] = KEYCHRON_CMD_STATUS;
    resp[KEYCHRON_BATTERY_OFFSET] = 46U;

    uint8_t pct = 255U;
    bool charging = true;
    KC_CHECK(kc_parse_battery_status(resp, sizeof(resp), &pct, &charging));
    KC_CHECK(pct == 46U && !charging);

    /* Bit 7 (0x80) encodes charging state; level sits in bits 0..6. */
    resp[KEYCHRON_BATTERY_OFFSET] = (uint8_t)(0x80U | 46U);
    KC_CHECK(kc_parse_battery_status(resp, sizeof(resp), &pct, &charging));
    KC_CHECK(pct == 46U && charging);

    resp[KEYCHRON_BATTERY_OFFSET] = (uint8_t)(0x80U | 100U);
    KC_CHECK(kc_parse_battery_status(resp, sizeof(resp), &pct, &charging));
    KC_CHECK(pct == 100U && charging);

    /* Reject out-of-range level (>100 even with bit 7 masked), wrong cmd/id, short buffers. */
    resp[KEYCHRON_BATTERY_OFFSET] = (uint8_t)(0x80U | 101U);
    KC_CHECK(!kc_parse_battery_status(resp, sizeof(resp), &pct, &charging));
    resp[KEYCHRON_BATTERY_OFFSET] = 101U;
    KC_CHECK(!kc_parse_battery_response(resp, sizeof(resp), &pct));

    resp[KEYCHRON_BATTERY_OFFSET] = 50U;
    KC_CHECK(!kc_parse_battery_response(resp, KEYCHRON_BATTERY_OFFSET, &pct));
    KC_CHECK(kc_parse_battery_response(resp, KEYCHRON_BATTERY_OFFSET + 1U, &pct));
    KC_CHECK(pct == 50U);

    resp[1] = 0x05U;
    KC_CHECK(!kc_parse_battery_response(resp, sizeof(resp), &pct));

    resp[0] = 0xB3U;
    resp[1] = KEYCHRON_CMD_STATUS;
    KC_CHECK(!kc_parse_battery_response(resp, sizeof(resp), &pct));
}

static void test_path_helpers(void) {
    KC_CHECK(kc_extract_mi_from_path_w(
        L"\\\\?\\HID#VID_3434&PID_D028&MI_04#7&189d5f95&0&0000#{4d1e55b2}") == 4);
    KC_CHECK(kc_extract_mi_from_path_w(
        L"\\\\?\\hid#vid_3434&pid_d028&mi_0a&col02#7&0000") == 10);
    KC_CHECK(kc_extract_mi_from_path_w(
        L"\\\\?\\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev&VID&023434") == -1);
    KC_CHECK(kc_extract_mi_from_path_w(NULL) == -1);

    KC_CHECK(kc_path_might_match_vid_w(L"\\\\?\\HID#VID_3434&PID_D028&MI_04", 0x3434U));
    KC_CHECK(!kc_path_might_match_vid_w(L"\\\\?\\HID#VID_046D&PID_C548&MI_02", 0x3434U));
    KC_CHECK(kc_path_might_match_vid_w(L"\\\\?\\BTHENUM#Dev_11223344", 0x3434U));

    KC_CHECK(kc_raw_path_matches_target_w(
        L"\\\\?\\HID#VID_3434&PID_D028&MI_00#7&1c6adf5c&0&0000#{378de44c}", 0x3434U, 0U));
    KC_CHECK(kc_raw_path_matches_target_w(
        L"\\\\?\\HID#VID_3434&PID_D028&MI_00#7&1c6adf5c&0&0000#{378de44c}", 0x3434U, 0xD028U));
    KC_CHECK(!kc_raw_path_matches_target_w(
        L"\\\\?\\HID#VID_3434&PID_D028&MI_00#7&1c6adf5c&0&0000#{378de44c}", 0x3434U, 0xD048U));
    KC_CHECK(kc_raw_path_matches_target_w(
        L"\\\\?\\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev&VID&023434_PID&D048#8&0",
        0x3434U, 0xD048U));
    KC_CHECK(!kc_raw_path_matches_target_w(
        L"\\\\?\\HID#PIXA3848&Col01#5&183f45c0&0&0000#{378de44c}", 0x3434U, 0U));
    KC_CHECK(!kc_raw_path_matches_target_w(
        L"\\\\?\\ROOT#FEIZHI_VIRTUAL_MOUSE#0000#{378de44c}", 0x3434U, 0U));
    KC_CHECK(!kc_raw_path_matches_target_w(
        L"\\\\?\\HID#VID_046D&PID_C548&MI_00#7&0000#{378de44c}", 0x3434U, 0U));
    KC_CHECK(!kc_raw_path_matches_target_w(NULL, 0x3434U, 0U));

    KC_CHECK(kc_contains_ascii_ci_w(L"Keychron M5 Wireless", "keychron"));
    KC_CHECK(!kc_contains_ascii_ci_w(L"Logitech MX Master", "keychron"));
}

static void test_score_candidate_ranking(void) {
    kc_hid_candidate_t mouse = {0x3434, 0xD028, 0x0001, 0x0002, 0};
    kc_hid_candidate_t kbd = {0x3434, 0xD028, 0x0001, 0x0006, 1};
    kc_hid_candidate_t consumer = {0x3434, 0xD028, 0x000C, 0x0001, 1};
    kc_hid_candidate_t via = {0x3434, 0xD028, 0xFF60, 0x0061, 2};
    kc_hid_candidate_t dongle = {0x3434, 0xD028, 0xFFC1, 0x0001, 4};
    kc_hid_candidate_t wired = {0x3434, 0xD048, 0xFFC1, 0x0001, 4};
    kc_hid_candidate_t other_vid = {0x046D, 0xC548, 0xFFC1, 0x0001, 4};

    KC_CHECK(kc_score_candidate(&mouse, 0x3434, 0, -1) < 0);
    KC_CHECK(kc_score_candidate(&kbd, 0x3434, 0, -1) < 0);
    KC_CHECK(kc_score_candidate(&consumer, 0x3434, 0, -1) < 0);
    KC_CHECK(kc_score_candidate(&other_vid, 0x3434, 0, -1) < 0);

    int s_via = kc_score_candidate(&via, 0x3434, 0, -1);
    int s_dongle = kc_score_candidate(&dongle, 0x3434, 0, -1);
    int s_wired = kc_score_candidate(&wired, 0x3434, 0, -1);
    KC_CHECK(s_via > 0);
    KC_CHECK(s_dongle > s_via);
    KC_CHECK(s_wired > s_dongle);

    KC_CHECK(kc_score_candidate(&via, 0x3434, 0xD028, 2) > 0);
    KC_CHECK(kc_score_candidate(&dongle, 0x3434, 0xD028, 2) < 0);
}

static void test_backoff_and_clamps(void) {
    KC_CHECK(kc_clamp_poll_minutes(5U) == 5U);
    KC_CHECK(kc_clamp_poll_minutes(15U) == 15U);
    KC_CHECK(kc_clamp_poll_minutes(30U) == 30U);
    KC_CHECK(kc_clamp_poll_minutes(60U) == 60U);
    KC_CHECK(kc_clamp_poll_minutes(42U) == 30U);

    KC_CHECK(kc_clamp_low_alert(0U) == 0U);
    KC_CHECK(kc_clamp_low_alert(10U) == 10U);
    KC_CHECK(kc_clamp_low_alert(15U) == 15U);
    KC_CHECK(kc_clamp_low_alert(20U) == 20U);
    KC_CHECK(kc_clamp_low_alert(99U) == 15U);

    KC_CHECK(kc_clamp_report_len(0U) == KEYCHRON_REPORT_SIZE);
    KC_CHECK(kc_clamp_report_len(1U) == KEYCHRON_REPORT_SIZE);
    KC_CHECK(kc_clamp_report_len(64U) == 64U);
    KC_CHECK(kc_clamp_report_len(512U) == KEYCHRON_MAX_REPORT_SIZE);

    kc_poll_state_t st;
    kc_poll_state_init(&st);

    KC_CHECK(kc_next_wait_ms(&st) == KEYCHRON_POLL_INTERVAL_MS);
    KC_CHECK(kc_next_wait_ms_for(&st, 5U * 60U * 1000U) == 5U * 60U * 1000U);
    KC_CHECK(kc_next_wait_ms_for(&st, 500U) == 10000U);
    KC_CHECK(!kc_should_show_error_glyph(&st));

    kc_poll_state_record_failure(&st);
    KC_CHECK(kc_should_show_error_glyph(&st));
    KC_CHECK(kc_next_wait_ms(&st) == 30U * 1000U);

    kc_poll_state_record_success(&st, 82U, false, 1000ULL);
    KC_CHECK(!kc_should_show_error_glyph(&st));
    KC_CHECK(kc_next_wait_ms(&st) == KEYCHRON_POLL_INTERVAL_MS);

    kc_poll_state_record_failure(&st);
    KC_CHECK(!kc_should_show_error_glyph(&st));
    KC_CHECK(kc_next_wait_ms(&st) == 30U * 1000U);

    kc_poll_state_record_failure(&st);
    KC_CHECK(!kc_should_show_error_glyph(&st));
    KC_CHECK(kc_next_wait_ms(&st) == 60U * 1000U);

    kc_poll_state_record_failure(&st);
    KC_CHECK(kc_should_show_error_glyph(&st));
    KC_CHECK(kc_next_wait_ms(&st) == 120U * 1000U);

    kc_poll_state_record_failure(&st);
    KC_CHECK(kc_next_wait_ms(&st) == 300U * 1000U);
    kc_poll_state_record_failure(&st);
    KC_CHECK(kc_next_wait_ms(&st) == 300U * 1000U);
    KC_CHECK(kc_next_wait_ms_for_wake(&st, 30U * 60U * 1000U, false) == 300U * 1000U);
    KC_CHECK(kc_next_wait_ms_for_wake(&st, 30U * 60U * 1000U, true) == 30U * 60U * 1000U);
}

static void test_low_battery_hysteresis_and_charging(void) {
    kc_poll_state_t st;
    kc_poll_state_init(&st);

    /* Never alert while charging, even if <= threshold. */
    kc_poll_state_record_success(&st, 10U, true, 25ULL);
    KC_CHECK(!kc_check_low_battery_alert(&st, 15U));

    kc_poll_state_record_success(&st, 10U, false, 50ULL);
    KC_CHECK(!kc_check_low_battery_alert(&st, 0U)); /* 0 disables alerts */

    kc_poll_state_record_success(&st, 15U, false, 100ULL);
    KC_CHECK(kc_check_low_battery_alert(&st, 15U));
    KC_CHECK(!kc_check_low_battery_alert(&st, 15U));

    kc_poll_state_record_success(&st, 18U, false, 200ULL);
    KC_CHECK(!kc_check_low_battery_alert(&st, 15U));

    kc_poll_state_record_success(&st, 20U, false, 300ULL);
    KC_CHECK(!kc_check_low_battery_alert(&st, 15U));

    kc_poll_state_record_success(&st, 14U, false, 400ULL);
    KC_CHECK(kc_check_low_battery_alert(&st, 15U));
}

static void test_cli_parsers_and_json_escape(void) {
    uint16_t u16 = 0U;
    KC_CHECK(kc_parse_u16_w(L"0xd028", &u16) && u16 == 0xD028U);
    KC_CHECK(kc_parse_u16_w(L"13364", &u16) && u16 == 13364U);
    KC_CHECK(!kc_parse_u16_w(L"0x10000", &u16));
    KC_CHECK(!kc_parse_u16_w(L"-1", &u16));
    KC_CHECK(!kc_parse_u16_w(L"12abc", &u16));
    KC_CHECK(!kc_parse_u16_w(L"", &u16));

    int iv = -1;
    KC_CHECK(kc_parse_int_range_w(L"4", 0, 255, &iv) && iv == 4);
    KC_CHECK(kc_parse_int_range_w(L"1500", 100, 10000, &iv) && iv == 1500);
    KC_CHECK(!kc_parse_int_range_w(L"99", 100, 10000, &iv));
    KC_CHECK(!kc_parse_int_range_w(L"256", 0, 255, &iv));

    char out[64];
    size_t len = kc_json_escape("Keychron \"8K\"\\v2\n\x01", out, sizeof(out));
    KC_CHECK(strcmp(out, "Keychron \\\"8K\\\"\\\\v2\\n\\u0001") == 0);
    KC_CHECK(len == strlen(out));
}

static void test_glyph_bbox_and_premul_blit(void) {
    uint32_t src[36] = {0};
    src[1 * 6 + 2] = 0x00FFFFFFU;
    src[1 * 6 + 3] = 0x00808080U;
    src[2 * 6 + 2] = 0x00FFFFFFU;
    src[2 * 6 + 3] = 0x00000000U;

    int min_x = -1, min_y = -1, max_x = -1, max_y = -1;
    KC_CHECK(kc_measure_ink_bbox(src, 6, 6, &min_x, &min_y, &max_x, &max_y));
    KC_CHECK(min_x == 2 && min_y == 1 && max_x == 3 && max_y == 2);

    uint32_t empty[4] = {0};
    KC_CHECK(!kc_measure_ink_bbox(empty, 2, 2, &min_x, &min_y, &max_x, &max_y));

    uint32_t dst[16];
    kc_blit_glyph_premul(src, 6, 2, 1, 2, 2, dst, 4, 1, 1, 24U, 24U, 24U);
    KC_CHECK(dst[0] == 0U);
    KC_CHECK(dst[1 * 4 + 1] == 0xFF181818U);
    KC_CHECK(dst[1 * 4 + 2] == 0x800C0C0CU);
    KC_CHECK(dst[2 * 4 + 1] == 0xFF181818U);
    KC_CHECK(dst[2 * 4 + 2] == 0U);
}

static void test_recv_state_and_model_lookup(void) {
    uint8_t cmd[64];
    size_t n = kc_build_recv_state_report(cmd, sizeof(cmd), 64U);
    KC_CHECK(n == 64U);
    KC_CHECK(cmd[0] == KEYCHRON_REPORT_ID_INFO);
    KC_CHECK(cmd[1] == KEYCHRON_CMD_RECV_STATE);
    for (size_t i = 2; i < 64U; ++i) {
        KC_CHECK(cmd[i] == 0U);
    }

    /* Captured response from Ultra-Link 8K: b6 03 01 34 34 56 d0 01 ... */
    uint8_t resp[64] = {0};
    resp[0] = KEYCHRON_REPORT_ID_INFO_RESP;
    resp[1] = KEYCHRON_CMD_RECV_STATE;
    resp[2] = 0x01U;
    resp[3] = 0x34U;
    resp[4] = 0x34U;
    resp[5] = 0x56U;
    resp[6] = 0xD0U;
    resp[7] = 0x01U;

    uint16_t vid = 0U, pid = 0U;
    bool connected = false;
    KC_CHECK(kc_parse_recv_state_response(resp, sizeof(resp), &vid, &pid, &connected));
    KC_CHECK(vid == 0x3434U && pid == 0xD056U && connected);

    /* Prefer connected slot 1 over idle slot 0; handle 0xB5 response header too. */
    resp[0] = KEYCHRON_REPORT_ID_INFO;
    resp[7] = 0x00U;
    resp[8] = 0x34U;
    resp[9] = 0x34U;
    resp[10] = 0x49U;
    resp[11] = 0xD0U;
    resp[12] = 0x01U;
    KC_CHECK(kc_parse_recv_state_response(resp, sizeof(resp), &vid, &pid, &connected));
    KC_CHECK(vid == 0x3434U && pid == 0xD049U && connected);

    /* Paired-but-asleep slot 0 still resolves VID/PID with connected=false. */
    memset(resp + 8, 0, 5);
    resp[5] = 0x48U;
    resp[6] = 0xD0U;
    KC_CHECK(kc_parse_recv_state_response(resp, sizeof(resp), &vid, &pid, &connected));
    KC_CHECK(vid == 0x3434U && pid == 0xD048U && !connected);

    /* Reject empty slots, wrong cmd/header, or truncated packets. */
    memset(resp + 3, 0, 5);
    KC_CHECK(!kc_parse_recv_state_response(resp, sizeof(resp), &vid, &pid, &connected));
    resp[3] = 0x34U;
    resp[4] = 0x34U;
    resp[5] = 0x56U;
    resp[6] = 0xD0U;
    resp[1] = 0x06U;
    KC_CHECK(!kc_parse_recv_state_response(resp, sizeof(resp), &vid, &pid, &connected));
    resp[1] = KEYCHRON_CMD_RECV_STATE;
    KC_CHECK(!kc_parse_recv_state_response(resp, 7U, &vid, &pid, &connected));

    KC_CHECK(strcmp(kc_model_name_for_pid(0xD048U), "Keychron M5") == 0);
    KC_CHECK(strcmp(kc_model_name_for_pid(0xD049U), "Keychron M6 8K") == 0);
    KC_CHECK(strcmp(kc_model_name_for_pid(0xD056U), "Keychron M7 8K") == 0);
    KC_CHECK(kc_model_name_for_pid(0xD099U) == NULL);
}

static void test_charging_cadence_full_alert_and_drain(void) {
    kc_poll_state_t st;
    kc_poll_state_init(&st);

    /* Charging caps normal steady interval (e.g. 30m) down to KEYCHRON_CHARGING_POLL_MS (2m). */
    kc_poll_state_record_success(&st, 55U, true, 1000ULL);
    KC_CHECK(kc_next_wait_ms(&st) == KEYCHRON_CHARGING_POLL_MS);
    KC_CHECK(kc_next_wait_ms_for(&st, 60U * 1000U) == 60U * 1000U);

    /* Full battery alert fires once at 100% charging with 95% re-arm hysteresis. */
    KC_CHECK(!kc_check_full_battery_alert(&st, true));
    kc_poll_state_record_success(&st, 100U, false, 2000ULL);
    KC_CHECK(!kc_check_full_battery_alert(&st, true));
    kc_poll_state_record_success(&st, 100U, true, 3000ULL);
    KC_CHECK(!kc_check_full_battery_alert(&st, false)); /* disabled */
    KC_CHECK(kc_check_full_battery_alert(&st, true));
    KC_CHECK(!kc_check_full_battery_alert(&st, true));

    kc_poll_state_record_success(&st, 97U, true, 4000ULL);
    kc_poll_state_record_success(&st, 100U, true, 5000ULL);
    KC_CHECK(!kc_check_full_battery_alert(&st, true)); /* inside 96..100 hysteresis */

    kc_poll_state_record_success(&st, 95U, true, 6000ULL);
    KC_CHECK(!kc_check_full_battery_alert(&st, true));
    kc_poll_state_record_success(&st, 100U, true, 7000ULL);
    KC_CHECK(kc_check_full_battery_alert(&st, true));

    /* Active drain estimation: anchor at 90%, exclude sleep/failure gaps, measure 90m active drop -> 87%. */
    uint32_t rate_x10 = 0U, hours_x10 = 0U;
    kc_poll_state_init(&st);
    kc_poll_state_record_success(&st, 90U, false, 1000ULL);
    KC_CHECK(!kc_estimate_drain(&st, &rate_x10, &hours_x10));

    kc_poll_state_record_success(&st, 89U, false, 1000ULL + 30ULL * 60ULL * 1000ULL);
    KC_CHECK(!kc_estimate_drain(&st, &rate_x10, &hours_x10)); /* only 1% drop */

    /* Simulate overnight sleep failure gap (4h) -> must NOT count toward active_drain_ms. */
    kc_poll_state_record_failure(&st);
    kc_poll_state_record_success(&st, 89U, false, 1000ULL + 270ULL * 60ULL * 1000ULL);
    KC_CHECK(!kc_estimate_drain(&st, &rate_x10, &hours_x10));

    /* Add 60 active minutes -> 90m active total, 3% drop (2.0%/h), 87% -> 43.5h left. */
    kc_poll_state_record_success(&st, 87U, false, 1000ULL + 330ULL * 60ULL * 1000ULL);
    KC_CHECK(kc_estimate_drain(&st, &rate_x10, &hours_x10));
    KC_CHECK(rate_x10 == 20U && hours_x10 == 435U);
}

static void test_battery_protector_and_calibration(void) {
    uint64_t t0 = 1700000000ULL;
    kc_protector_persist_t p;
    kc_protector_runtime_t rt;
    kc_protector_persist_init(&p, t0);
    kc_protector_runtime_init(&rt);

    KC_CHECK(p.mode == KC_PROT_LONGEVITY && p.laptop_guard && !p.calibrating);
    KC_CHECK(kc_clamp_protector_mode(0U) == KC_PROT_OFF);
    KC_CHECK(kc_clamp_protector_mode(2U) == KC_PROT_BALANCED);
    KC_CHECK(kc_clamp_protector_mode(99U) == KC_PROT_LONGEVITY);

    uint8_t min_p = 0U, max_p = 0U;
    kc_protector_bounds(KC_PROT_LONGEVITY, &min_p, &max_p);
    KC_CHECK(min_p == 25U && max_p == 80U);
    kc_protector_bounds(KC_PROT_BALANCED, &min_p, &max_p);
    KC_CHECK(min_p == 20U && max_p == 90U);
    kc_protector_bounds(KC_PROT_OFF, &min_p, &max_p);
    KC_CHECK(min_p == 0U && max_p == 100U);

    /* Visual priority: Normal > Yellow (<=25%) > Red (<=15%), Green charging < 80%, Cyan >= 80%, Gray stale. */
    kc_poll_state_t st;
    kc_poll_state_init(&st);
    kc_poll_state_record_success(&st, 50U, false, 1000ULL);
    KC_CHECK(kc_compute_battery_visual(&st, &p, 15U) == KC_VIS_NORMAL);

    kc_poll_state_record_success(&st, 25U, false, 2000ULL);
    KC_CHECK(kc_compute_battery_visual(&st, &p, 15U) == KC_VIS_CHARGE_SOON);
    KC_CHECK(kc_check_charge_soon_alert(&st, &p, &rt, 15U));
    KC_CHECK(!kc_check_charge_soon_alert(&st, &p, &rt, 15U));

    /* At <=15%, critical Red overrides Yellow and suppresses duplicate Charge Soon alert. */
    kc_poll_state_record_success(&st, 15U, false, 3000ULL);
    KC_CHECK(kc_compute_battery_visual(&st, &p, 15U) == KC_VIS_LOW);
    rt.charge_soon_notified = false;
    KC_CHECK(!kc_check_charge_soon_alert(&st, &p, &rt, 15U));

    /* Discharging below hysteresis band stays armed; >=30% re-arms Charge Soon. */
    kc_poll_state_record_success(&st, 28U, false, 4000ULL);
    kc_check_charge_soon_alert(&st, &p, &rt, 15U);
    kc_poll_state_record_success(&st, 25U, false, 5000ULL);
    KC_CHECK(!rt.charge_soon_notified || kc_check_charge_soon_alert(&st, &p, &rt, 15U) == false);
    rt.charge_soon_notified = true;
    kc_poll_state_record_success(&st, 29U, false, 6000ULL);
    kc_check_charge_soon_alert(&st, &p, &rt, 15U);
    KC_CHECK(rt.charge_soon_notified);
    kc_poll_state_record_success(&st, 30U, false, 7000ULL);
    kc_check_charge_soon_alert(&st, &p, &rt, 15U);
    KC_CHECK(!rt.charge_soon_notified);

    /* Charge ceiling visual & 5% hysteresis, plus trickle 1% wobble immunity. */
    bool cal_done = false;
    KC_CHECK(!kc_protector_record_sample(&p, &rt, 50U, true, t0 + 10ULL, &cal_done));
    KC_CHECK(kc_protector_record_sample(&p, &rt, 79U, true, t0 + 20ULL, &cal_done));
    KC_CHECK(p.cum_charged_pct == 29U && !cal_done);
    /* 1% drop while charging then back to 79% must NOT add phantom cycle%. */
    KC_CHECK(!kc_protector_record_sample(&p, &rt, 78U, true, t0 + 30ULL, &cal_done));
    KC_CHECK(!kc_protector_record_sample(&p, &rt, 79U, true, t0 + 40ULL, &cal_done));
    KC_CHECK(p.cum_charged_pct == 29U);

    KC_CHECK(kc_protector_record_sample(&p, &rt, 80U, true, t0 + 50ULL, &cal_done));
    KC_CHECK(p.cum_charged_pct == 30U);
    kc_poll_state_record_success(&st, 79U, true, 8000ULL);
    KC_CHECK(kc_compute_battery_visual(&st, &p, 15U) == KC_VIS_CHARGING);
    kc_poll_state_record_success(&st, 80U, true, 9000ULL);
    KC_CHECK(kc_compute_battery_visual(&st, &p, 15U) == KC_VIS_CHARGE_LIMIT);
    KC_CHECK(kc_check_charge_limit_alert(&st, &p, &rt));
    KC_CHECK(!kc_check_charge_limit_alert(&st, &p, &rt));

    /* Laptop battery guard triggers once when laptop <=25% on battery and mouse >=50%. */
    KC_CHECK(!kc_check_laptop_guard_alert(&st, &p, &rt, true, false, 20U));
    KC_CHECK(kc_check_laptop_guard_alert(&st, &p, &rt, true, true, 25U));
    KC_CHECK(!kc_check_laptop_guard_alert(&st, &p, &rt, true, true, 22U));

    /* Failures mark visual Stale regardless of battery percentage. */
    kc_poll_state_record_failure(&st);
    KC_CHECK(kc_compute_battery_visual(&st, &p, 15U) == KC_VIS_STALE);

    /* Calibration due at >=1000% (10 cycles) OR 90 days (>=1 cycle) OR 180d idle; snooze defers 14d. */
    uint32_t days = 0U, cyc_x10 = 0U;
    KC_CHECK(p.cum_charged_pct == 30U);
    KC_CHECK(!kc_protector_is_cal_due(&p, t0 + 89ULL * 86400ULL, &days, &cyc_x10));
    KC_CHECK(days == 89U && cyc_x10 == 3U);
    /* Day 90 with only 30% (<1 cycle) waits until >=100% charged or 180-day fallback. */
    KC_CHECK(!kc_protector_is_cal_due(&p, t0 + 90ULL * 86400ULL, &days, &cyc_x10));
    KC_CHECK(kc_protector_is_cal_due(&p, t0 + 180ULL * 86400ULL, &days, &cyc_x10));
    KC_CHECK(days == 180U);

    /* Reaching 100% (1.0 cycle) makes Day 90 trigger cleanly, then snooze defers 14 days. */
    KC_CHECK(!kc_protector_record_sample(&p, &rt, 20U, false, t0 + 55ULL, &cal_done));
    KC_CHECK(kc_protector_record_sample(&p, &rt, 90U, true, t0 + 60ULL, &cal_done));
    KC_CHECK(p.cum_charged_pct == 100U);
    KC_CHECK(!kc_protector_is_cal_due(&p, t0 + 89ULL * 86400ULL, &days, &cyc_x10));
    KC_CHECK(kc_protector_is_cal_due(&p, t0 + 90ULL * 86400ULL, &days, &cyc_x10));

    kc_protector_snooze_calibration(&p, &rt, t0 + 90ULL * 86400ULL);
    KC_CHECK(!kc_protector_is_cal_due(&p, t0 + 103ULL * 86400ULL, &days, &cyc_x10));
    KC_CHECK(kc_protector_is_cal_due(&p, t0 + 104ULL * 86400ULL, &days, &cyc_x10));

    /* Heavy usage (e.g. 8K polling hitting 1000% = 10.0 cycles early on Day 5) triggers due. */
    p.cal_snooze_until = 0ULL;
    p.cum_charged_pct = 1000U;
    KC_CHECK(kc_protector_is_cal_due(&p, t0 + 5ULL * 86400ULL, &days, &cyc_x10));
    KC_CHECK(cyc_x10 == 100U);

    /* Enable calibration: lifts 80% cap -> Green while charging >80%, auto-completes at 100%. */
    KC_CHECK(kc_protector_set_calibrating(&p, &rt, true));
    kc_poll_state_record_success(&st, 85U, true, 10000ULL);
    KC_CHECK(kc_compute_battery_visual(&st, &p, 15U) == KC_VIS_CHARGING);
    KC_CHECK(!kc_check_charge_limit_alert(&st, &p, &rt));

    uint64_t t_cal = t0 + 104ULL * 86400ULL + 100ULL;
    KC_CHECK(kc_protector_record_sample(&p, &rt, 100U, true, t_cal, &cal_done));
    KC_CHECK(cal_done);
    KC_CHECK(!p.calibrating && p.cum_charged_pct == 0U && p.last_cal_unix == t_cal);
    KC_CHECK(rt.pending_cal_complete && rt.charge_limit_notified);
    kc_poll_state_record_success(&st, 100U, true, 11000ULL);
    KC_CHECK(kc_compute_battery_visual(&st, &p, 15U) == KC_VIS_CHARGE_LIMIT);

    /* Clock rewind safety: now_unix < last_cal_unix never underflows. */
    KC_CHECK(!kc_protector_is_cal_due(&p, t0, &days, &cyc_x10));
    KC_CHECK(days == 0U && cyc_x10 == 0U);
}

static void test_semver_and_manifest_parser(void) {
    kc_semver_t a, b;
    KC_CHECK(kc_parse_semver("1.5.0", &a) && a.major == 1U && a.minor == 5U && a.patch == 0U);
    KC_CHECK(kc_parse_semver("v1.10.2", &b) && b.major == 1U && b.minor == 10U && b.patch == 2U);
    KC_CHECK(kc_compare_semver(a, b) < 0);
    KC_CHECK(kc_compare_semver(b, a) > 0);
    KC_CHECK(kc_compare_semver(a, a) == 0);
    KC_CHECK(!kc_parse_semver("1.5", &a));
    KC_CHECK(!kc_parse_semver("1.5.0-beta", &a));
    KC_CHECK(!kc_parse_semver("-1.0.0", &a));

    const char *sample_json =
        "{\n"
        "  \"version\": \"1.6.0\",\n"
        "  \"architecture\": {\n"
        "    \"64bit\": {\n"
        "      \"url\": \"https://github.com/mammadow/keychron-battery-tray/releases/download/v1.6.0/keychron-battery-win64-v1.6.0.zip\",\n"
        "      \"hash\": \"6AF099449FC90802299A1A1DC1E9F35281B1C21E96CB9BFDD7CB7238BA738C83\"\n"
        "    }\n"
        "  },\n"
        "  \"autoupdate\": {\n"
        "    \"architecture\": {\n"
        "      \"64bit\": {\n"
        "        \"url\": \"https://github.com/mammadow/keychron-battery-tray/releases/download/v$version/keychron-battery-win64-v$version.zip\",\n"
        "        \"hash\": { \"url\": \"$baseurl/SHA256SUMS.txt\" }\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "}\n";
    kc_release_info_t rel;
    KC_CHECK(kc_parse_scoop_manifest(sample_json, strlen(sample_json), &rel));
    KC_CHECK(rel.version.major == 1U && rel.version.minor == 6U && rel.version.patch == 0U);
    KC_CHECK(strcmp(rel.version_str, "1.6.0") == 0);
    KC_CHECK(strcmp(rel.download_url,
                    "https://github.com/mammadow/keychron-battery-tray/releases/download/v1.6.0/keychron-battery-win64-v1.6.0.zip") == 0);
    KC_CHECK(strcmp(rel.sha256_hex,
                    "6af099449fc90802299a1a1dc1e9f35281b1c21e96cb9bfdd7cb7238ba738c83") == 0);

    /* Reject untrusted download host or placeholder template URLs or non-64-hex hashes. */
    const char *evil_host =
        "{\"version\":\"9.9.9\",\"url\":\"https://evil.example/payload.zip\","
        "\"hash\":\"6af099449fc90802299a1a1dc1e9f35281b1c21e96cb9bfdd7cb7238ba738c83\"}";
    KC_CHECK(!kc_parse_scoop_manifest(evil_host, strlen(evil_host), &rel));

    const char *bad_hash =
        "{\"version\":\"9.9.9\","
        "\"url\":\"https://github.com/mammadow/keychron-battery-tray/releases/download/v9.9.9/a.zip\","
        "\"hash\":\"REPLACE_WITH_RELEASE_ZIP_SHA256\"}";
    KC_CHECK(!kc_parse_scoop_manifest(bad_hash, strlen(bad_hash), &rel));
}

int main(void) {
    test_build_and_parse_report();
    test_recv_state_and_model_lookup();
    test_charging_cadence_full_alert_and_drain();
    test_battery_protector_and_calibration();
    test_semver_and_manifest_parser();
    test_path_helpers();
    test_score_candidate_ranking();
    test_backoff_and_clamps();
    test_low_battery_hysteresis_and_charging();
    test_cli_parsers_and_json_escape();
    test_glyph_bbox_and_premul_blit();
    printf("All keychron core tests passed.\n");
    return 0;
}
