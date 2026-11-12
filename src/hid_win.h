#ifndef KEYCHRON_HID_WIN_H
#define KEYCHRON_HID_WIN_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define KC_HID_MAX_REPORT 256U

typedef enum kc_hid_status {
    KC_HID_OK = 0,
    KC_HID_NOT_FOUND,
    KC_HID_OPEN_FAILED,
    KC_HID_IO_ERROR,
    KC_HID_TIMEOUT,
    KC_HID_CANCELLED
} kc_hid_status_t;

typedef enum kc_hid_transport {
    KC_XPORT_AUTO = 0,
    KC_XPORT_WRITE = 1,
    KC_XPORT_FEATURE = 2
} kc_hid_transport_t;

typedef enum kc_hid_link {
    KC_LINK_UNKNOWN = 0,
    KC_LINK_24G = 1,
    KC_LINK_USB = 2,
    KC_LINK_BLE = 3
} kc_hid_link_t;

typedef struct kc_hid_device_info {
    wchar_t path[MAX_PATH];
    wchar_t product[128];
    uint16_t vid;
    uint16_t pid;
    uint16_t usage_page;
    uint16_t usage;
    int interface_number;
    uint16_t input_report_len;
    uint16_t output_report_len;
    uint16_t feature_report_len;
} kc_hid_device_info_t;

typedef struct kc_hid_session {
    wchar_t cached_path[MAX_PATH];
    uint16_t cached_vid;
    uint16_t cached_pid;
    int cached_interface;
    kc_hid_transport_t preferred_transport;
    kc_hid_device_info_t cached_info;
    bool has_info;
    kc_hid_link_t active_link;
    wchar_t cached_ble_path[MAX_PATH];
    bool cached_ble_is_svc;
    bool paired_probed;
    uint16_t paired_vid;
    uint16_t paired_pid;
} kc_hid_session_t;

void kc_hid_session_init(kc_hid_session_t *session);
void kc_hid_session_invalidate(kc_hid_session_t *session);

/**
 * Enumerate matching HID interfaces (vid == 0 enumerates all HID devices).
 * Returns total matching interfaces found (up to max_out written to out).
 */
size_t kc_hid_list_devices(uint16_t filter_vid,
                           kc_hid_device_info_t *out,
                           size_t max_out);

/**
 * Select best Keychron vendor interface for (vid, pid, interface_num).
 * Pass pid == 0 and/or interface_num < 0 to auto-detect.
 */
bool kc_hid_find_best_device(uint16_t vid,
                             uint16_t pid,
                             int interface_num,
                             kc_hid_device_info_t *out_info);

/**
 * Query battery percentage and charging state using cached device path when valid,
 * with OVERLAPPED I/O cancellation via cancel_event. Falls back to standard HID
 * Battery Strength (0x06/0x20) and BLE GATT Battery Service (0x180F/0x2A19) in
 * auto-detect mode when no vendor interface is found.
 */
kc_hid_status_t kc_hid_query_battery(kc_hid_session_t *session,
                                     uint16_t vid,
                                     uint16_t pid,
                                     int interface_num,
                                     uint32_t retries,
                                     uint32_t attempt_timeout_ms,
                                     HANDLE cancel_event,
                                     bool debug,
                                     FILE *debug_stream,
                                     uint8_t *out_pct,
                                     bool *out_charging);

const wchar_t *kc_hid_status_text_w(kc_hid_status_t status);
const wchar_t *kc_hid_link_label_w(kc_hid_link_t link);
const char *kc_hid_link_label_a(kc_hid_link_t link);

#endif /* KEYCHRON_HID_WIN_H */
