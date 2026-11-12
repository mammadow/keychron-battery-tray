#include "hid_win.h"
#include "protocol.h"

#include <bluetoothleapis.h>
#include <devpropdef.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdlib.h>
#include <string.h>

#define KC_BT_UUID_BATTERY_SERVICE 0x180FU
#define KC_BT_UUID_BATTERY_LEVEL   0x2A19U
#define KC_HID_PAGE_GENERIC_DEV    0x0006U
#define KC_HID_USAGE_BATTERY       0x0020U
#define KC_FALLBACK_COOLDOWN_MS    (5ULL * 60ULL * 1000ULL)

static const GUID KC_GUID_BTHLE_DEV = {
    0x781aee18U, 0x7733U, 0x4ce4U, {0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92}
};

static const GUID KC_GUID_BTH_GATT_SVC = {
    0x6e3bb679U, 0x4372U, 0x40c8U, {0x9e, 0xaa, 0x45, 0x09, 0xdf, 0x26, 0x0c, 0xd8}
};

static const DEVPROPKEY KC_DEVPKEY_DEVICE_DESC = {
    {0xa45c254eU, 0xdf1cU, 0x4efdU, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2U
};
static const DEVPROPKEY KC_DEVPKEY_HARDWARE_IDS = {
    {0xa45c254eU, 0xdf1cU, 0x4efdU, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 3U
};
static const DEVPROPKEY KC_DEVPKEY_FRIENDLY_NAME = {
    {0xa45c254eU, 0xdf1cU, 0x4efdU, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14U
};
static const DEVPROPKEY KC_DEVPKEY_INSTANCE_ID = {
    {0x78c34fc8U, 0x104aU, 0x4acaU, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256U
};

static bool kc_is_cancelled(HANDLE cancel_event) {
    return cancel_event && WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0;
}

static bool kc_probe_hid_path(const wchar_t *path,
                              uint16_t filter_vid,
                              kc_hid_device_info_t *out_info) {
    if (!kc_path_might_match_vid_w(path, filter_vid)) {
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
    session->transport_verified = false;
    session->has_info = false;
    memset(&session->cached_info, 0, sizeof(session->cached_info));
    session->active_link = KC_LINK_UNKNOWN;
    session->cached_ble_path[0] = L'\0';
    session->cached_ble_is_svc = false;
    session->paired_probed = false;
    session->paired_vid = 0U;
    session->paired_pid = 0U;
    session->next_fallback_probe_ms = 0ULL;
}

static wchar_t *kc_cm_get_interface_list(const GUID *iface_guid) {
    if (!iface_guid) {
        return NULL;
    }
    GUID guid_copy = *iface_guid;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG chars = 0U;
        if (CM_Get_Device_Interface_List_SizeW(
                &chars, &guid_copy, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
            chars <= 1U) {
            return NULL;
        }
        wchar_t *list = (wchar_t *)calloc((size_t)chars + 1U, sizeof(wchar_t));
        if (!list) {
            return NULL;
        }
        CONFIGRET cr = CM_Get_Device_Interface_ListW(
            &guid_copy, NULL, list, chars, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_SUCCESS) {
            list[chars] = L'\0';
            return list;
        }
        free(list);
        if (cr != CR_BUFFER_SMALL) {
            return NULL;
        }
    }
    return NULL;
}

static kc_hid_link_t kc_infer_hid_link(const kc_hid_device_info_t *info) {
    if (!info) {
        return KC_LINK_UNKNOWN;
    }
    if (info->pid == KEYCHRON_RECEIVER_PID) {
        return KC_LINK_24G;
    }
    if (info->interface_number >= 0) {
        return KC_LINK_USB;
    }
    return KC_LINK_BLE;
}

size_t kc_hid_list_devices(uint16_t filter_vid,
                           kc_hid_device_info_t *out,
                           size_t max_out) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    wchar_t *paths = kc_cm_get_interface_list(&hid_guid);
    if (!paths) {
        return 0;
    }

    size_t count = 0;
    for (const wchar_t *p = paths; *p != L'\0'; p += wcslen(p) + 1U) {
        kc_hid_device_info_t info;
        if (kc_probe_hid_path(p, filter_vid, &info)) {
            if (out && count < max_out) {
                out[count] = info;
            }
            count++;
        }
    }

    free(paths);
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

static const GUID KC_BTH_BASE_GUID = {
    0x00000000U, 0x0000U, 0x1000U, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}
};

static bool kc_uuid_matches_short(const BTH_LE_UUID *u, USHORT expected) {
    if (!u) {
        return false;
    }
    if (u->IsShortUuid) {
        return u->Value.ShortUuid == expected;
    }
    GUID expected_long = KC_BTH_BASE_GUID;
    expected_long.Data1 = (ULONG)expected;
    return memcmp(&u->Value.LongUuid, &expected_long, sizeof(GUID)) == 0;
}

typedef HRESULT (WINAPI *kc_bt_get_svcs_fn)(
    HANDLE, USHORT, PBTH_LE_GATT_SERVICE, USHORT *, ULONG);
typedef HRESULT (WINAPI *kc_bt_get_chars_fn)(
    HANDLE, PBTH_LE_GATT_SERVICE, USHORT, PBTH_LE_GATT_CHARACTERISTIC, USHORT *, ULONG);
typedef HRESULT (WINAPI *kc_bt_get_val_fn)(
    HANDLE, PBTH_LE_GATT_CHARACTERISTIC, ULONG, PBTH_LE_GATT_CHARACTERISTIC_VALUE, USHORT *, ULONG);

typedef struct kc_bt_api {
    kc_bt_get_svcs_fn get_services;
    kc_bt_get_chars_fn get_characteristics;
    kc_bt_get_val_fn get_characteristic_value;
} kc_bt_api_t;

static INIT_ONCE g_bt_init_once = INIT_ONCE_STATIC_INIT;
static kc_bt_api_t g_bt_api = {NULL, NULL, NULL};

static void kc_load_proc(HMODULE mod, const char *name, void *dst_fn_ptr, size_t dst_sz) {
    FARPROC proc = GetProcAddress(mod, name);
    if (proc && dst_sz == sizeof(proc)) {
        memcpy(dst_fn_ptr, &proc, sizeof(proc));
    }
}

static BOOL CALLBACK kc_init_bt_api_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    HMODULE mod = LoadLibraryExW(L"BluetoothApis.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) {
        return TRUE;
    }

    kc_bt_api_t loaded = {NULL, NULL, NULL};
    kc_load_proc(mod, "BluetoothGATTGetServices", &loaded.get_services, sizeof(loaded.get_services));
    kc_load_proc(mod, "BluetoothGATTGetCharacteristics", &loaded.get_characteristics, sizeof(loaded.get_characteristics));
    kc_load_proc(mod, "BluetoothGATTGetCharacteristicValue", &loaded.get_characteristic_value, sizeof(loaded.get_characteristic_value));

    if (loaded.get_services && loaded.get_characteristics && loaded.get_characteristic_value) {
        g_bt_api = loaded;
    } else {
        FreeLibrary(mod);
    }
    return TRUE;
}

static const kc_bt_api_t *kc_get_bt_api(void) {
    InitOnceExecuteOnce(&g_bt_init_once, kc_init_bt_api_cb, NULL, NULL);
    if (!g_bt_api.get_services || !g_bt_api.get_characteristics || !g_bt_api.get_characteristic_value) {
        return NULL;
    }
    return &g_bt_api;
}

static bool kc_ble_read_char_from_service(const kc_bt_api_t *bt,
                                          HANDLE h,
                                          PBTH_LE_GATT_SERVICE svc,
                                          uint8_t *out_pct) {
    USHORT n_chars = 0;
    HRESULT hr = bt->get_characteristics(
        h, svc, 0, NULL, &n_chars, BLUETOOTH_GATT_FLAG_NONE);
    if ((hr != S_OK && hr != HRESULT_FROM_WIN32(ERROR_MORE_DATA)) || n_chars == 0U) {
        return false;
    }

    PBTH_LE_GATT_CHARACTERISTIC chars = (PBTH_LE_GATT_CHARACTERISTIC)calloc(
        n_chars, sizeof(BTH_LE_GATT_CHARACTERISTIC));
    if (!chars) {
        return false;
    }

    bool ok = false;
    USHORT actual_chars = 0;
    hr = bt->get_characteristics(
        h, svc, n_chars, chars, &actual_chars, BLUETOOTH_GATT_FLAG_NONE);
    if (SUCCEEDED(hr)) {
        for (USHORT i = 0; i < actual_chars; ++i) {
            if (!chars[i].IsReadable ||
                !kc_uuid_matches_short(&chars[i].CharacteristicUuid, KC_BT_UUID_BATTERY_LEVEL)) {
                continue;
            }
            USHORT val_sz = 0;
            hr = bt->get_characteristic_value(
                h, &chars[i], 0, NULL, &val_sz, BLUETOOTH_GATT_FLAG_NONE);
            if ((hr != S_OK && hr != HRESULT_FROM_WIN32(ERROR_MORE_DATA)) || val_sz == 0U) {
                continue;
            }
            PBTH_LE_GATT_CHARACTERISTIC_VALUE val = (PBTH_LE_GATT_CHARACTERISTIC_VALUE)calloc(1, val_sz);
            if (!val) {
                continue;
            }
            USHORT actual_sz = 0;
            hr = bt->get_characteristic_value(
                h, &chars[i], val_sz, val, &actual_sz, BLUETOOTH_GATT_FLAG_NONE);
            if (SUCCEEDED(hr) && val->DataSize >= 1U && val->Data[0] <= 100U) {
                *out_pct = val->Data[0];
                ok = true;
            }
            free(val);
            if (ok) {
                break;
            }
        }
    }
    free(chars);
    return ok;
}

static bool kc_cm_read_str_prop(DEVINST dn,
                                const DEVPROPKEY *key,
                                wchar_t *out,
                                size_t out_chars) {
    DEVPROPTYPE ptype = 0;
    ULONG sz = (ULONG)(out_chars * sizeof(wchar_t));
    memset(out, 0, sz);
    if (CM_Get_DevNode_PropertyW(dn, key, &ptype, (PBYTE)out, &sz, 0) != CR_SUCCESS) {
        return false;
    }
    out[out_chars - 1U] = L'\0';
    return out[0] != L'\0';
}

static bool kc_cm_iface_matches_keychron(const wchar_t *path,
                                         wchar_t *out_name,
                                         size_t out_name_cap) {
    if (out_name && out_name_cap > 0U) {
        out_name[0] = L'\0';
    }
    bool matched = kc_contains_ascii_ci_w(path, "keychron") ||
                   kc_contains_ascii_ci_w(path, "3434");

    wchar_t inst_id[MAX_DEVICE_ID_LEN];
    DEVPROPTYPE ptype = 0;
    ULONG inst_sz = sizeof(inst_id);
    memset(inst_id, 0, sizeof(inst_id));
    if (CM_Get_Device_Interface_PropertyW(
            path, &KC_DEVPKEY_INSTANCE_ID, &ptype, (PBYTE)inst_id, &inst_sz, 0) != CR_SUCCESS) {
        return matched;
    }
    inst_id[MAX_DEVICE_ID_LEN - 1U] = L'\0';

    DEVINST dn = 0;
    if (CM_Locate_DevNodeW(&dn, inst_id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return matched;
    }

    const DEVPROPKEY *str_keys[2] = {&KC_DEVPKEY_FRIENDLY_NAME, &KC_DEVPKEY_DEVICE_DESC};
    for (size_t k = 0; k < 2U; ++k) {
        wchar_t buf[256];
        if (kc_cm_read_str_prop(dn, str_keys[k], buf, 256U)) {
            if (out_name && out_name[0] == L'\0') {
                wcsncpy(out_name, buf, out_name_cap - 1U);
                out_name[out_name_cap - 1U] = L'\0';
            }
            if (kc_contains_ascii_ci_w(buf, "keychron") ||
                kc_contains_ascii_ci_w(buf, "3434")) {
                matched = true;
            }
        }
    }

    wchar_t hwid[512];
    ULONG hwid_sz = (ULONG)(sizeof(hwid) - 2U * sizeof(wchar_t));
    memset(hwid, 0, sizeof(hwid));
    if (CM_Get_DevNode_PropertyW(
            dn, &KC_DEVPKEY_HARDWARE_IDS, &ptype, (PBYTE)hwid, &hwid_sz, 0) == CR_SUCCESS) {
        for (const wchar_t *entry = hwid; *entry != L'\0'; entry += wcslen(entry) + 1U) {
            if (kc_contains_ascii_ci_w(entry, "keychron") ||
                kc_contains_ascii_ci_w(entry, "3434")) {
                matched = true;
                break;
            }
        }
    }
    return matched;
}

static bool kc_ble_read_device_path(const wchar_t *path,
                                    bool is_svc_if,
                                    uint8_t *out_pct) {
    const kc_bt_api_t *bt = kc_get_bt_api();
    if (!bt) {
        return false;
    }

    HANDLE h = CreateFileW(path,
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL,
                           OPEN_EXISTING,
                           0,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool found = false;
    if (is_svc_if) {
        found = kc_ble_read_char_from_service(bt, h, NULL, out_pct);
    } else {
        USHORT n_svcs = 0;
        HRESULT hr = bt->get_services(
            h, 0, NULL, &n_svcs, BLUETOOTH_GATT_FLAG_NONE);
        if ((hr == S_OK || hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) && n_svcs > 0U) {
            PBTH_LE_GATT_SERVICE svcs = (PBTH_LE_GATT_SERVICE)calloc(
                n_svcs, sizeof(BTH_LE_GATT_SERVICE));
            if (svcs) {
                USHORT actual_svcs = 0;
                if (SUCCEEDED(bt->get_services(
                        h, n_svcs, svcs, &actual_svcs, BLUETOOTH_GATT_FLAG_NONE))) {
                    for (USHORT s = 0; s < actual_svcs; ++s) {
                        if (kc_uuid_matches_short(&svcs[s].ServiceUuid,
                                                  KC_BT_UUID_BATTERY_SERVICE) &&
                            kc_ble_read_char_from_service(bt, h, &svcs[s], out_pct)) {
                            found = true;
                            break;
                        }
                    }
                }
                free(svcs);
            }
        }
    }
    CloseHandle(h);
    return found;
}

static bool kc_try_ble_gatt_fallback(kc_hid_session_t *session,
                                     bool debug,
                                     FILE *debug_stream,
                                     uint8_t *out_pct) {
    if (session && session->cached_ble_path[0] != L'\0') {
        if (kc_ble_read_device_path(session->cached_ble_path,
                                    session->cached_ble_is_svc, out_pct)) {
            if (debug && debug_stream) {
                fprintf(debug_stream, "[debug] BLE GATT Battery Service 0x180F (cached) -> %u%%\n",
                        (unsigned)*out_pct);
            }
            session->active_link = KC_LINK_BLE;
            session->paired_probed = false;
            session->paired_vid = 0U;
            session->paired_pid = 0U;
            return true;
        }
        session->cached_ble_path[0] = L'\0';
        session->cached_ble_is_svc = false;
        if (session->cached_path[0] == L'\0') {
            session->has_info = false;
        }
    }

    /* First try per-service GATT interface (Battery Service 0x180F), then device interface. */
    const GUID *guids[2] = {&KC_GUID_BTH_GATT_SVC, &KC_GUID_BTHLE_DEV};
    for (int g = 0; g < 2; ++g) {
        wchar_t *paths = kc_cm_get_interface_list(guids[g]);
        if (!paths) {
            continue;
        }

        for (const wchar_t *p = paths; *p != L'\0'; p += wcslen(p) + 1U) {
            if (g == 0 && !kc_contains_ascii_ci_w(p, "0000180f")) {
                continue;
            }

            wchar_t dev_name[128];
            if (!kc_cm_iface_matches_keychron(p, dev_name, 128)) {
                continue;
            }

            if (kc_ble_read_device_path(p, (g == 0), out_pct)) {
                if (debug && debug_stream) {
                    fprintf(debug_stream, "[debug] BLE GATT Battery Service 0x180F -> %u%%\n",
                            (unsigned)*out_pct);
                }
                if (session) {
                    wcsncpy(session->cached_ble_path, p, MAX_PATH - 1);
                    session->cached_ble_path[MAX_PATH - 1] = L'\0';
                    session->cached_ble_is_svc = (g == 0);
                    memset(&session->cached_info, 0, sizeof(session->cached_info));
                    wcsncpy(session->cached_info.path, p, MAX_PATH - 1);
                    wcsncpy(session->cached_info.product,
                            dev_name[0] ? dev_name : L"Keychron Bluetooth", 127);
                    session->cached_info.vid = KEYCHRON_VID;
                    session->cached_info.interface_number = -1;
                    session->has_info = true;
                    session->active_link = KC_LINK_BLE;
                    session->paired_probed = false;
                    session->paired_vid = 0U;
                    session->paired_pid = 0U;
                }
                free(paths);
                return true;
            }
        }
        free(paths);
    }
    return false;
}

static bool kc_try_std_hid_battery_fallback(kc_hid_session_t *session,
                                            uint16_t vid,
                                            bool debug,
                                            FILE *debug_stream,
                                            uint8_t *out_pct) {
    kc_hid_device_info_t list[32];
    size_t found = kc_hid_list_devices(vid, list, sizeof(list) / sizeof(list[0]));
    size_t limit = found < (sizeof(list) / sizeof(list[0])) ? found : (sizeof(list) / sizeof(list[0]));

    for (size_t i = 0; i < limit; ++i) {
        if (list[i].input_report_len < 2U || list[i].input_report_len > KC_HID_MAX_REPORT) {
            continue;
        }
        HANDLE h = CreateFileW(list[i].path,
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL,
                               OPEN_EXISTING,
                               0,
                               NULL);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }

        bool ok = false;
        PHIDP_PREPARSED_DATA ppd = NULL;
        if (HidD_GetPreparsedData(h, &ppd) && ppd) {
            HIDP_VALUE_CAPS vcaps[4];
            USHORT n_caps = 4U;
            if (HidP_GetSpecificValueCaps(
                    HidP_Input, KC_HID_PAGE_GENERIC_DEV, 0, KC_HID_USAGE_BATTERY,
                    vcaps, &n_caps, ppd) == HIDP_STATUS_SUCCESS && n_caps > 0U) {
                uint8_t rep[KC_HID_MAX_REPORT];
                memset(rep, 0, sizeof(rep));
                rep[0] = vcaps[0].ReportID;
                if (HidD_GetInputReport(h, rep, list[i].input_report_len)) {
                    ULONG raw = 0;
                    if (HidP_GetUsageValue(
                            HidP_Input, KC_HID_PAGE_GENERIC_DEV, 0, KC_HID_USAGE_BATTERY,
                            &raw, ppd, (PCHAR)rep, list[i].input_report_len) == HIDP_STATUS_SUCCESS) {
                        LONG lmin = vcaps[0].LogicalMin;
                        LONG lmax = vcaps[0].LogicalMax;
                        LONG sraw = (LONG)raw;
                        if (lmax > lmin && sraw >= lmin && sraw <= lmax) {
                            *out_pct = (uint8_t)(((sraw - lmin) * 100L + (lmax - lmin) / 2L) / (lmax - lmin));
                            ok = true;
                        } else if (raw <= 100UL) {
                            *out_pct = (uint8_t)raw;
                            ok = true;
                        }
                    }
                }
            }
            HidD_FreePreparsedData(ppd);
        }
        CloseHandle(h);

        if (ok) {
            if (debug && debug_stream) {
                fprintf(debug_stream, "[debug] standard HID Battery Strength (0x06/0x20) -> %u%%\n",
                        (unsigned)*out_pct);
            }
            if (session) {
                session->cached_info = list[i];
                session->has_info = true;
                session->active_link = kc_infer_hid_link(&list[i]);
                session->paired_probed = false;
                session->paired_vid = 0U;
                session->paired_pid = 0U;
            }
            return true;
        }
    }
    return false;
}

static bool kc_try_any_fallback(kc_hid_session_t *session,
                                uint16_t vid,
                                bool debug,
                                FILE *debug_stream,
                                uint8_t *out_pct) {
    return kc_try_std_hid_battery_fallback(session, vid, debug, debug_stream, out_pct) ||
           kc_try_ble_gatt_fallback(session, debug, debug_stream, out_pct);
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

typedef size_t (*kc_report_builder_fn)(uint8_t *buf, size_t cap, uint16_t report_len);
typedef bool (*kc_packet_parser_fn)(const uint8_t *buf, size_t len, void *ctx);

typedef struct kc_battery_parse_ctx {
    uint8_t pct;
    bool charging;
} kc_battery_parse_ctx_t;

typedef struct kc_recv_parse_ctx {
    uint16_t vid;
    uint16_t pid;
    bool connected;
} kc_recv_parse_ctx_t;

static bool kc_parse_battery_pkt(const uint8_t *buf, size_t len, void *ctx) {
    kc_battery_parse_ctx_t *bctx = (kc_battery_parse_ctx_t *)ctx;
    return kc_parse_battery_status(buf, len, &bctx->pct, &bctx->charging);
}

static bool kc_parse_recv_pkt(const uint8_t *buf, size_t len, void *ctx) {
    kc_recv_parse_ctx_t *rctx = (kc_recv_parse_ctx_t *)ctx;
    return kc_parse_recv_state_response(buf, len, &rctx->vid, &rctx->pid, &rctx->connected);
}

static bool kc_try_write_report(HANDLE h,
                                uint16_t out_len,
                                HANDLE io_event,
                                HANDLE cancel_event,
                                kc_report_builder_fn builder,
                                uint8_t *out_buf,
                                size_t out_cap,
                                bool debug,
                                FILE *debug_stream,
                                bool *out_cancelled) {
    *out_cancelled = false;
    size_t plen = builder(out_buf, out_cap, out_len);
    bool ok = kc_overlapped_write(h, out_buf, (DWORD)plen, io_event, cancel_event, out_cancelled);
    if (debug && debug_stream) {
        fprintf(debug_stream, "[debug] WriteFile(%zu) -> %s\n", plen, ok ? "ok" : "fail");
    }
    return ok;
}

static kc_hid_status_t kc_hid_transact(HANDLE h,
                                       uint16_t in_len,
                                       uint16_t out_len,
                                       uint16_t feat_len,
                                       HANDLE io_event,
                                       HANDLE cancel_event,
                                       kc_report_builder_fn builder,
                                       kc_packet_parser_fn parser,
                                       void *parser_ctx,
                                       kc_hid_transport_t *xport,
                                       bool allow_feature_fallback,
                                       uint32_t timeout_ms,
                                       bool debug,
                                       FILE *debug_stream) {
    uint8_t out_buf[KC_HID_MAX_REPORT];
    uint8_t in_buf[KC_HID_MAX_REPORT];

    HidD_FlushQueue(h);

    bool try_feature_first = (*xport == KC_XPORT_FEATURE) ||
                             (*xport == KC_XPORT_AUTO && allow_feature_fallback);
    bool sent_ok = false;

    if (!try_feature_first) {
        bool cancelled = false;
        sent_ok = kc_try_write_report(
            h, out_len, io_event, cancel_event, builder,
            out_buf, sizeof(out_buf), debug, debug_stream, &cancelled);
        if (cancelled) {
            return KC_HID_CANCELLED;
        }
        if (sent_ok && *xport == KC_XPORT_AUTO) {
            *xport = KC_XPORT_WRITE;
        }
    }

    if (!sent_ok && *xport != KC_XPORT_WRITE) {
        size_t flen = builder(out_buf, sizeof(out_buf), feat_len);
        sent_ok = HidD_SetFeature(h, out_buf, (ULONG)flen) != FALSE;
        if (debug && debug_stream) {
            fprintf(debug_stream, "[debug] HidD_SetFeature(%zu) -> %s\n", flen, sent_ok ? "ok" : "fail");
        }
        if (sent_ok) {
            *xport = KC_XPORT_FEATURE;
        } else if (try_feature_first && *xport == KC_XPORT_AUTO) {
            bool cancelled = false;
            sent_ok = kc_try_write_report(
                h, out_len, io_event, cancel_event, builder,
                out_buf, sizeof(out_buf), debug, debug_stream, &cancelled);
            if (cancelled) {
                return KC_HID_CANCELLED;
            }
            if (sent_ok) {
                *xport = KC_XPORT_WRITE;
            }
        }
    }

    if (!sent_ok) {
        return KC_HID_IO_ERROR;
    }

    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (!kc_is_cancelled(cancel_event)) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            return KC_HID_TIMEOUT;
        }
        ULONGLONG rem = deadline - now;
        DWORD slice = (rem > 250ULL) ? 250U : (DWORD)rem;
        DWORD bytes_read = 0;

        kc_read_wait_result_t rres = kc_overlapped_read(
            h, in_buf, in_len, slice, io_event, cancel_event, &bytes_read);
        if (rres == KC_READ_CANCELLED) {
            return KC_HID_CANCELLED;
        }
        if (rres == KC_READ_ERROR) {
            return KC_HID_IO_ERROR;
        }
        if (rres == KC_READ_TIMEOUT) {
            continue;
        }

        if (debug && debug_stream) {
            kc_debug_hex(debug_stream, "read", in_buf, bytes_read);
        }
        if (parser(in_buf, bytes_read, parser_ctx)) {
            return KC_HID_OK;
        }
    }
    return KC_HID_CANCELLED;
}

static void kc_probe_receiver_paired_model(kc_hid_session_t *session,
                                           HANDLE h,
                                           uint16_t in_len,
                                           uint16_t out_len,
                                           uint16_t feat_len,
                                           HANDLE io_event,
                                           HANDLE cancel_event,
                                           bool debug,
                                           FILE *debug_stream) {
    if (!session || !session->has_info ||
        session->cached_info.pid != KEYCHRON_RECEIVER_PID ||
        session->paired_probed || kc_is_cancelled(cancel_event)) {
        return;
    }
    session->paired_probed = true;

    kc_hid_transport_t probe_xport = session->preferred_transport;
    kc_recv_parse_ctx_t rctx;
    memset(&rctx, 0, sizeof(rctx));

    kc_hid_status_t st = kc_hid_transact(
        h, in_len, out_len, feat_len, io_event, cancel_event,
        kc_build_recv_state_report, kc_parse_recv_pkt, &rctx,
        &probe_xport, false, 300U, debug, debug_stream);
    if (st != KC_HID_OK || rctx.pid == 0U) {
        return;
    }

    session->paired_vid = rctx.vid;
    session->paired_pid = rctx.pid;
    const char *model = (rctx.vid == KEYCHRON_VID) ? kc_model_name_for_pid(rctx.pid) : NULL;
    if (model) {
        MultiByteToWideChar(CP_UTF8, 0, model, -1, session->cached_info.product, 128);
    } else if (rctx.vid == KEYCHRON_VID) {
        _snwprintf(session->cached_info.product, 128, L"Keychron Mouse (0x%04X)", (unsigned)rctx.pid);
    } else {
        _snwprintf(session->cached_info.product, 128, L"Mouse (0x%04X:0x%04X)",
                   (unsigned)rctx.vid, (unsigned)rctx.pid);
    }
    session->cached_info.product[127] = L'\0';
}

static kc_hid_status_t kc_query_handle(kc_hid_session_t *session,
                                       HANDLE h,
                                       uint32_t retries,
                                       uint32_t attempt_timeout_ms,
                                       HANDLE cancel_event,
                                       bool debug,
                                       FILE *debug_stream,
                                       uint8_t *out_pct,
                                       bool *out_charging) {
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
    for (uint32_t attempt = 0; attempt < retries; ++attempt) {
        if (kc_is_cancelled(cancel_event)) {
            final_status = KC_HID_CANCELLED;
            break;
        }
        if (debug && debug_stream) {
            fprintf(debug_stream, "[debug] attempt %u/%u\n", attempt + 1U, retries);
        }

        kc_battery_parse_ctx_t bctx;
        memset(&bctx, 0, sizeof(bctx));
        final_status = kc_hid_transact(
            h, in_len, out_len, feat_len, io_event, cancel_event,
            kc_build_status_report, kc_parse_battery_pkt, &bctx,
            &session->preferred_transport,
            (!session->transport_verified && attempt > 0U),
            attempt_timeout_ms, debug, debug_stream);

        if (final_status == KC_HID_OK) {
            session->transport_verified = true;
            *out_pct = bctx.pct;
            if (out_charging) {
                *out_charging = bctx.charging;
            }
            kc_probe_receiver_paired_model(
                session, h, in_len, out_len, feat_len, io_event,
                cancel_event, debug, debug_stream);
            break;
        }
        if (final_status == KC_HID_CANCELLED || final_status == KC_HID_IO_ERROR) {
            break;
        }

        if (!session->transport_verified &&
            attempt == 0U &&
            session->preferred_transport == KC_XPORT_WRITE) {
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

    CloseHandle(io_event);
    return final_status;
}

static bool kc_try_any_fallback_throttled(kc_hid_session_t *session,
                                          uint16_t vid,
                                          bool debug,
                                          FILE *debug_stream,
                                          uint8_t *out_pct) {
    ULONGLONG now_ms = GetTickCount64();
    if (session && session->next_fallback_probe_ms != 0ULL &&
        now_ms < session->next_fallback_probe_ms) {
        return false;
    }
    if (kc_try_any_fallback(session, vid, debug, debug_stream, out_pct)) {
        if (session) {
            session->next_fallback_probe_ms = 0ULL;
        }
        return true;
    }
    if (session) {
        session->next_fallback_probe_ms = now_ms + KC_FALLBACK_COOLDOWN_MS;
    }
    return false;
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
                                     uint8_t *out_pct,
                                     bool *out_charging) {
    if (!out_pct) {
        return KC_HID_IO_ERROR;
    }
    if (out_charging) {
        *out_charging = false;
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

    /* Fast path when already operating in Bluetooth LE mode with a cached GATT path. */
    if (pid == 0U && interface_num < 0 &&
        session->active_link == KC_LINK_BLE &&
        session->cached_ble_path[0] != L'\0') {
        if (kc_try_ble_gatt_fallback(session, debug, debug_stream, out_pct)) {
            session->next_fallback_probe_ms = 0ULL;
            return KC_HID_OK;
        }
        kc_hid_session_invalidate(session);
    }

    for (int pass = 0; pass < 2; ++pass) {
        bool used_cache = false;
        kc_hid_status_t open_err = KC_HID_OK;
        HANDLE h = kc_open_cached_or_discover(
            session, vid, pid, interface_num, &used_cache, &open_err);
        if (h == INVALID_HANDLE_VALUE) {
            if (open_err == KC_HID_NOT_FOUND && pid == 0U && interface_num < 0 &&
                kc_try_any_fallback_throttled(session, vid, debug, debug_stream, out_pct)) {
                return KC_HID_OK;
            }
            return open_err;
        }

        kc_hid_status_t st = kc_query_handle(
            session, h, retries, attempt_timeout_ms, cancel_event,
            debug, debug_stream, out_pct, out_charging);
        CloseHandle(h);

        if (st == KC_HID_OK) {
            session->next_fallback_probe_ms = 0ULL;
            if (session->has_info) {
                session->active_link = kc_infer_hid_link(&session->cached_info);
            }
            return KC_HID_OK;
        }
        if (st == KC_HID_IO_ERROR && used_cache && pass == 0 && !kc_is_cancelled(cancel_event)) {
            kc_hid_session_invalidate(session);
            continue;
        }
        if (st == KC_HID_IO_ERROR && used_cache) {
            kc_hid_session_invalidate(session);
        }
        /* Allow Bluetooth / std HID fallback even when the 2.4GHz receiver stays docked. */
        if (st == KC_HID_TIMEOUT && pid == 0U && interface_num < 0 &&
            session->has_info && session->cached_info.pid == KEYCHRON_RECEIVER_PID &&
            !kc_is_cancelled(cancel_event) &&
            kc_try_any_fallback_throttled(session, vid, debug, debug_stream, out_pct)) {
            return KC_HID_OK;
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

const wchar_t *kc_hid_link_label_w(kc_hid_link_t link) {
    switch (link) {
        case KC_LINK_24G: return L"2.4GHz";
        case KC_LINK_USB: return L"USB";
        case KC_LINK_BLE: return L"BLE";
        default:          return L"";
    }
}

const char *kc_hid_link_label_a(kc_hid_link_t link) {
    switch (link) {
        case KC_LINK_24G: return "2.4GHz";
        case KC_LINK_USB: return "USB";
        case KC_LINK_BLE: return "BLE";
        default:          return "";
    }
}
