#include "hid_win.h"
#include "protocol.h"

#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <string.h>

static bool kc_is_cancelled(HANDLE cancel_event) {
    return cancel_event && WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0;
}

static int kc_hex_digit(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') {
        return ch - L'0';
    }
    if (ch >= L'a' && ch <= L'f') {
        return 10 + (ch - L'a');
    }
    if (ch >= L'A' && ch <= L'F') {
        return 10 + (ch - L'A');
    }
    return -1;
}

/* Fast pre-filter: if a USB HID path carries VID_XXXX, skip non-matching VIDs
   before opening a metadata handle. Paths without VID_ (e.g. Bluetooth) pass through. */
static bool kc_path_might_match_vid(const wchar_t *path, uint16_t filter_vid) {
    if (!path || filter_vid == 0U) {
        return true;
    }
    for (size_t i = 0; path[i] != L'\0'; ++i) {
        wchar_t c0 = path[i];
        if ((c0 == L'v' || c0 == L'V') &&
            (path[i + 1] == L'i' || path[i + 1] == L'I') &&
            (path[i + 2] == L'd' || path[i + 2] == L'D') &&
            path[i + 3] == L'_') {
            int d0 = kc_hex_digit(path[i + 4]);
            int d1 = kc_hex_digit(path[i + 5]);
            int d2 = kc_hex_digit(path[i + 6]);
            int d3 = kc_hex_digit(path[i + 7]);
            if (d0 >= 0 && d1 >= 0 && d2 >= 0 && d3 >= 0) {
                uint16_t path_vid = (uint16_t)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
                return path_vid == filter_vid;
            }
        }
    }
    return true;
}

static bool kc_probe_hid_path(const wchar_t *path,
                              uint16_t filter_vid,
                              kc_hid_device_info_t *out_info) {
    if (!kc_path_might_match_vid(path, filter_vid)) {
        return false;
    }

    HANDLE h = CreateFileW(path,
                           0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL,
                           OPEN_EXISTING,
                           0,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    HIDD_ATTRIBUTES attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.Size = sizeof(attrs);

    bool matched = false;
    if (HidD_GetAttributes(h, &attrs) &&
        (filter_vid == 0U || attrs.VendorID == filter_vid)) {
        matched = true;
        memset(out_info, 0, sizeof(*out_info));
        wcsncpy(out_info->path, path, MAX_PATH - 1);
        out_info->vid = attrs.VendorID;
        out_info->pid = attrs.ProductID;
        out_info->interface_number = kc_extract_mi_from_path_w(path);

        PHIDP_PREPARSED_DATA ppd = NULL;
        if (HidD_GetPreparsedData(h, &ppd) && ppd) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                out_info->usage_page = caps.UsagePage;
                out_info->usage = caps.Usage;
                out_info->input_report_len = caps.InputReportByteLength;
                out_info->output_report_len = caps.OutputReportByteLength;
                out_info->feature_report_len = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(ppd);
        }

        if (!HidD_GetProductString(h, out_info->product, sizeof(out_info->product)) ||
            out_info->product[0] == L'\0') {
            wcsncpy(out_info->product, L"?", 127);
        } else {
            out_info->product[127] = L'\0';
        }
    }

    CloseHandle(h);
    return matched;
}

void kc_hid_session_init(kc_hid_session_t *session) {
    if (!session) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->cached_interface = -1;
}

void kc_hid_session_invalidate(kc_hid_session_t *session) {
    if (!session) {
        return;
    }
    session->cached_path[0] = L'\0';
    session->cached_vid = 0U;
    session->cached_pid = 0U;
    session->cached_interface = -1;
    session->preferred_transport = KC_XPORT_AUTO;
    session->has_info = false;
    memset(&session->cached_info, 0, sizeof(session->cached_info));
}

size_t kc_hid_list_devices(uint16_t filter_vid,
                           kc_hid_device_info_t *out,
                           size_t max_out) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO dev_info = SetupDiGetClassDevsW(
        &hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) {
        return 0;
    }

    size_t count = 0;
    SP_DEVICE_INTERFACE_DATA if_data;
    if_data.cbSize = sizeof(if_data);

    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, idx, &if_data);
         ++idx) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
            uint8_t storage[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + MAX_PATH * sizeof(wchar_t)];
        } buf;
        memset(&buf, 0, sizeof(buf));
        buf.detail.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        DWORD req_size = 0;
        if (!SetupDiGetDeviceInterfaceDetailW(
                dev_info, &if_data, &buf.detail, sizeof(buf.storage), &req_size, NULL)) {
            continue;
        }

        kc_hid_device_info_t info;
        if (kc_probe_hid_path(buf.detail.DevicePath, filter_vid, &info)) {
            if (out && count < max_out) {
                out[count] = info;
            }
            count++;
        }
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return count;
}

bool kc_hid_find_best_device(uint16_t vid,
                             uint16_t pid,
                             int interface_num,
                             kc_hid_device_info_t *out_info) {
    if (!out_info) {
        return false;
    }
    kc_hid_device_info_t list[32];
    size_t found = kc_hid_list_devices(vid, list, sizeof(list) / sizeof(list[0]));
    size_t limit = found < (sizeof(list) / sizeof(list[0])) ? found : (sizeof(list) / sizeof(list[0]));

    int best_score = -1;
    size_t best_idx = 0;
    for (size_t i = 0; i < limit; ++i) {
        kc_hid_candidate_t cand = {
            .vid = list[i].vid,
            .pid = list[i].pid,
            .usage_page = list[i].usage_page,
            .usage = list[i].usage,
            .interface_number = list[i].interface_number,
        };
        int score = kc_score_candidate(&cand, vid, pid, interface_num);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }
    if (best_score < 0) {
        return false;
    }
    *out_info = list[best_idx];
    return true;
}

static uint16_t kc_clamp_report_len(uint16_t reported) {
    if (reported < 2U) {
        return KEYCHRON_REPORT_SIZE;
    }
    if (reported > KC_HID_MAX_REPORT) {
        return KC_HID_MAX_REPORT;
    }
    return reported;
}

static bool kc_overlapped_write(HANDLE h,
                                const uint8_t *buf,
                                DWORD len,
                                HANDLE io_event,
                                HANDLE cancel_event,
                                bool *out_cancelled) {
    *out_cancelled = false;
    ResetEvent(io_event);

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = io_event;

    DWORD written = 0;
    if (WriteFile(h, buf, len, &written, &ov)) {
        return written > 0;
    }
    DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) {
        return false;
    }

    HANDLE handles[2] = {io_event, cancel_event};
    DWORD n_handles = cancel_event ? 2U : 1U;
    DWORD wait_res = WaitForMultipleObjects(n_handles, handles, FALSE, 500);
    if (wait_res == WAIT_OBJECT_0) {
        return GetOverlappedResult(h, &ov, &written, FALSE) && written > 0;
    }

    /* Always cancel and drain overlapped state before stack OVERLAPPED expires. */
    CancelIoEx(h, &ov);
    GetOverlappedResult(h, &ov, &written, TRUE);
    if (wait_res == WAIT_OBJECT_0 + 1) {
        *out_cancelled = true;
    }
    return false;
}

typedef enum kc_read_wait_result {
    KC_READ_PACKET = 0,
    KC_READ_TIMEOUT,
    KC_READ_ERROR,
    KC_READ_CANCELLED
} kc_read_wait_result_t;

static kc_read_wait_result_t kc_overlapped_read(HANDLE h,
                                                uint8_t *buf,
                                                DWORD len,
                                                DWORD timeout_ms,
                                                HANDLE io_event,
                                                HANDLE cancel_event,
                                                DWORD *out_bytes) {
    *out_bytes = 0;
    ResetEvent(io_event);

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = io_event;

    DWORD read_bytes = 0;
    if (ReadFile(h, buf, len, &read_bytes, &ov)) {
        *out_bytes = read_bytes;
        return KC_READ_PACKET;
    }
    DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) {
        return KC_READ_ERROR;
    }

    HANDLE handles[2] = {io_event, cancel_event};
    DWORD n_handles = cancel_event ? 2U : 1U;
    DWORD wait_res = WaitForMultipleObjects(n_handles, handles, FALSE, timeout_ms);
    if (wait_res == WAIT_OBJECT_0) {
        if (GetOverlappedResult(h, &ov, &read_bytes, FALSE)) {
            *out_bytes = read_bytes;
            return KC_READ_PACKET;
        }
        return KC_READ_ERROR;
    }

    CancelIoEx(h, &ov);
    GetOverlappedResult(h, &ov, &read_bytes, TRUE);
    if (wait_res == WAIT_OBJECT_0 + 1) {
        return KC_READ_CANCELLED;
    }
    return (wait_res == WAIT_TIMEOUT) ? KC_READ_TIMEOUT : KC_READ_ERROR;
}

static void kc_debug_hex(FILE *stream, const char *label, const uint8_t *buf, size_t len) {
    if (!stream) {
        return;
    }
    fprintf(stream, "[debug] %s (%zu bytes):", label, len);
    for (size_t i = 0; i < len; ++i) {
        fprintf(stream, " %02x", buf[i]);
    }
    fputc('\n', stream);
}

static HANDLE kc_open_cached_or_discover(kc_hid_session_t *session,
                                         uint16_t vid,
                                         uint16_t pid,
                                         int interface_num,
                                         bool *out_used_cache,
                                         kc_hid_status_t *out_err) {
    *out_used_cache = false;
    *out_err = KC_HID_OK;

    if (session &&
        session->cached_path[0] != L'\0' &&
        session->cached_vid == vid &&
        session->cached_pid == pid &&
        session->cached_interface == interface_num) {
        HANDLE h = CreateFileW(session->cached_path,
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL,
                               OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED,
                               NULL);
        if (h != INVALID_HANDLE_VALUE) {
            *out_used_cache = true;
            return h;
        }
        kc_hid_session_invalidate(session);
    }

    kc_hid_device_info_t info;
    if (!kc_hid_find_best_device(vid, pid, interface_num, &info)) {
        *out_err = KC_HID_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }

    HANDLE h = CreateFileW(info.path,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL,
                           OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        *out_err = KC_HID_OPEN_FAILED;
        return INVALID_HANDLE_VALUE;
    }

    if (session) {
        wcsncpy(session->cached_path, info.path, MAX_PATH - 1);
        session->cached_path[MAX_PATH - 1] = L'\0';
        session->cached_vid = vid;
        session->cached_pid = pid;
        session->cached_interface = interface_num;
        session->cached_info = info;
        session->has_info = true;
    }
    return h;
}

static kc_hid_status_t kc_query_handle(kc_hid_session_t *session,
                                       HANDLE h,
                                       uint32_t retries,
                                       uint32_t attempt_timeout_ms,
                                       HANDLE cancel_event,
                                       bool debug,
                                       FILE *debug_stream,
                                       uint8_t *out_pct) {
    uint16_t in_len = KEYCHRON_REPORT_SIZE;
    uint16_t out_len = KEYCHRON_REPORT_SIZE;
    uint16_t feat_len = KEYCHRON_REPORT_SIZE;
    PHIDP_PREPARSED_DATA ppd = NULL;
    if (HidD_GetPreparsedData(h, &ppd) && ppd) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
            in_len = kc_clamp_report_len(caps.InputReportByteLength);
            out_len = kc_clamp_report_len(caps.OutputReportByteLength);
            feat_len = kc_clamp_report_len(caps.FeatureReportByteLength);
        }
        HidD_FreePreparsedData(ppd);
    }

    if (debug && debug_stream) {
        fprintf(debug_stream, "[debug] caps in=%u out=%u feat=%u\n", in_len, out_len, feat_len);
    }

    HANDLE io_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!io_event) {
        return KC_HID_IO_ERROR;
    }

    kc_hid_status_t final_status = KC_HID_TIMEOUT;
    uint8_t out_buf[KC_HID_MAX_REPORT];
    uint8_t in_buf[KC_HID_MAX_REPORT];

    for (uint32_t attempt = 0; attempt < retries; ++attempt) {
        if (kc_is_cancelled(cancel_event)) {
            final_status = KC_HID_CANCELLED;
            break;
        }
        if (debug && debug_stream) {
            fprintf(debug_stream, "[debug] attempt %u/%u\n", attempt + 1U, retries);
        }

        HidD_FlushQueue(h);

        bool use_feature = (session->preferred_transport == KC_XPORT_FEATURE) ||
                           (session->preferred_transport == KC_XPORT_AUTO && attempt > 0U);
        bool sent_ok = false;

        if (!use_feature) {
            size_t plen = kc_build_status_report(out_buf, sizeof(out_buf), out_len);
            bool cancelled = false;
            sent_ok = kc_overlapped_write(h, out_buf, (DWORD)plen, io_event, cancel_event, &cancelled);
            if (debug && debug_stream) {
                fprintf(debug_stream, "[debug] WriteFile(%zu) -> %s\n", plen, sent_ok ? "ok" : "fail");
            }
            if (cancelled) {
                final_status = KC_HID_CANCELLED;
                break;
            }
            if (sent_ok && session->preferred_transport == KC_XPORT_AUTO) {
                session->preferred_transport = KC_XPORT_WRITE;
            }
        }

        if (!sent_ok) {
            size_t flen = kc_build_status_report(out_buf, sizeof(out_buf), feat_len);
            sent_ok = HidD_SetFeature(h, out_buf, (ULONG)flen) != FALSE;
            if (debug && debug_stream) {
                fprintf(debug_stream, "[debug] HidD_SetFeature(%zu) -> %s\n", flen, sent_ok ? "ok" : "fail");
            }
            if (sent_ok) {
                session->preferred_transport = KC_XPORT_FEATURE;
            }
        }

        /* Never stall waiting on ReadFile if neither write nor feature succeeded. */
        if (!sent_ok) {
            final_status = KC_HID_IO_ERROR;
            break;
        }

        ULONGLONG deadline = GetTickCount64() + attempt_timeout_ms;
        bool got_valid = false;
        while (!kc_is_cancelled(cancel_event)) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                break;
            }
            ULONGLONG rem = deadline - now;
            DWORD slice = (rem > 250ULL) ? 250U : (DWORD)rem;
            DWORD bytes_read = 0;

            kc_read_wait_result_t rres = kc_overlapped_read(
                h, in_buf, in_len, slice, io_event, cancel_event, &bytes_read);
            if (rres == KC_READ_CANCELLED) {
                final_status = KC_HID_CANCELLED;
                goto done;
            }
            if (rres == KC_READ_ERROR) {
                final_status = KC_HID_IO_ERROR;
                goto done;
            }
            if (rres == KC_READ_TIMEOUT) {
                continue;
            }

            if (debug && debug_stream) {
                kc_debug_hex(debug_stream, "read", in_buf, bytes_read);
            }
            uint8_t pct = 0;
            if (kc_parse_battery_response(in_buf, bytes_read, &pct)) {
                *out_pct = pct;
                final_status = KC_HID_OK;
                got_valid = true;
                break;
            }
        }

        if (got_valid) {
            break;
        }

        if (attempt == 0U && session->preferred_transport == KC_XPORT_WRITE) {
            session->preferred_transport = KC_XPORT_AUTO;
        }

        if (attempt + 1U < retries) {
            if (cancel_event) {
                if (WaitForSingleObject(cancel_event, 150) == WAIT_OBJECT_0) {
                    final_status = KC_HID_CANCELLED;
                    break;
                }
            } else {
                Sleep(150);
            }
        }
    }

done:
    CloseHandle(io_event);
    return final_status;
}

kc_hid_status_t kc_hid_query_battery(kc_hid_session_t *session,
                                     uint16_t vid,
                                     uint16_t pid,
                                     int interface_num,
                                     uint32_t retries,
                                     uint32_t attempt_timeout_ms,
                                     HANDLE cancel_event,
                                     bool debug,
                                     FILE *debug_stream,
                                     uint8_t *out_pct) {
    if (!out_pct) {
        return KC_HID_IO_ERROR;
    }
    if (kc_is_cancelled(cancel_event)) {
        return KC_HID_CANCELLED;
    }
    if (retries == 0U) {
        retries = 1U;
    }
    if (attempt_timeout_ms == 0U) {
        attempt_timeout_ms = 1500U;
    }

    kc_hid_session_t local_session;
    if (!session) {
        kc_hid_session_init(&local_session);
        session = &local_session;
    }

    for (int pass = 0; pass < 2; ++pass) {
        bool used_cache = false;
        kc_hid_status_t open_err = KC_HID_OK;
        HANDLE h = kc_open_cached_or_discover(
            session, vid, pid, interface_num, &used_cache, &open_err);
        if (h == INVALID_HANDLE_VALUE) {
            return open_err;
        }

        kc_hid_status_t st = kc_query_handle(
            session, h, retries, attempt_timeout_ms, cancel_event, debug, debug_stream, out_pct);
        CloseHandle(h);

        if (st == KC_HID_IO_ERROR && used_cache && pass == 0 && !kc_is_cancelled(cancel_event)) {
            kc_hid_session_invalidate(session);
            continue;
        }
        if (st == KC_HID_IO_ERROR && used_cache) {
            kc_hid_session_invalidate(session);
        }
        return st;
    }

    return KC_HID_IO_ERROR;
}

const wchar_t *kc_hid_status_text_w(kc_hid_status_t status) {
    switch (status) {
        case KC_HID_OK:          return L"ok";
        case KC_HID_NOT_FOUND:   return L"device not found";
        case KC_HID_OPEN_FAILED: return L"cannot open device";
        case KC_HID_IO_ERROR:    return L"HID I/O error";
        case KC_HID_TIMEOUT:     return L"no response (mouse asleep/off)";
        case KC_HID_CANCELLED:   return L"cancelled";
        default:                 return L"unknown error";
    }
}
