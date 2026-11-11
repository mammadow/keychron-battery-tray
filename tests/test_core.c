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

int main(void) {
    test_build_and_parse_report();
    test_path_helpers();
    test_score_candidate_ranking();
    test_backoff_and_clamps();
    test_low_battery_hysteresis_and_charging();
    test_cli_parsers_and_json_escape();
    test_glyph_bbox_and_premul_blit();
    printf("All keychron core tests passed.\n");
    return 0;
}
