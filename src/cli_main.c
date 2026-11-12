#include "hid_win.h"
#include "protocol.h"
#include "version.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static HANDLE g_cli_stop_event = NULL;

static BOOL WINAPI kc_cli_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            if (g_cli_stop_event) {
                SetEvent(g_cli_stop_event);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

static void print_usage(void) {
    wprintf(L"Usage: keychron-battery [OPTIONS]\n\n"
            L"Options:\n"
            L"  --list             List Keychron HID interfaces and exit\n"
            L"  --watch [seconds]  Poll continuously (1-3600s, default: 5) until Ctrl+C\n"
            L"  --vid <hex|dec>    Vendor ID (default: 0x3434)\n"
            L"  --pid <hex|dec>    Product ID (auto-detects vendor interface if omitted)\n"
            L"  --interface <n>    USB interface number (auto-detects if omitted)\n"
            L"  --timeout <ms>     Per-attempt response timeout (100-10000, default: 1500)\n"
            L"  --raw              Print bare percentage integer (e.g. 46)\n"
            L"  --json             Output structured JSON (works with query, --watch, or --list)\n"
            L"  --debug            Print raw HID caps, send method, and response bytes\n"
            L"  --version, -v      Print version and exit\n"
            L"  --help, -h         Show this help message\n");
}

static void wide_to_utf8(const wchar_t *wstr, char *out, size_t out_cap) {
    if (!out || out_cap == 0U) {
        return;
    }
    if (!wstr || !wstr[0]) {
        out[0] = '\0';
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out, (int)out_cap, NULL, NULL);
    if (n <= 0) {
        out[0] = '\0';
    }
}

static void list_interfaces(uint16_t vid, bool json_mode) {
    kc_hid_device_info_t list[32];
    size_t found = kc_hid_list_devices(vid, list, sizeof(list) / sizeof(list[0]));
    size_t limit = found < (sizeof(list) / sizeof(list[0])) ? found : (sizeof(list) / sizeof(list[0]));

    if (json_mode) {
        printf("[");
        for (size_t i = 0; i < limit; ++i) {
            char prod_utf8[256];
            char prod_esc[512];
            wide_to_utf8(list[i].product, prod_utf8, sizeof(prod_utf8));
            kc_json_escape(prod_utf8, prod_esc, sizeof(prod_esc));
            printf("%s{\"vid\":\"0x%04x\",\"pid\":\"0x%04x\",\"interface\":%d,"
                   "\"usage_page\":\"0x%04x\",\"usage\":\"0x%04x\",\"product\":\"%s\"}",
                   (i > 0U) ? "," : "",
                   list[i].vid,
                   list[i].pid,
                   list[i].interface_number,
                   list[i].usage_page,
                   list[i].usage,
                   prod_esc);
        }
        printf("]\n");
        return;
    }

    if (found == 0U) {
        wprintf(L"No Keychron HID devices found for VID 0x%04x.\n", vid);
        return;
    }

    wprintf(L"%-8ls%-11ls%-12ls%-8ls%ls\n",
            L"PID", L"Interface", L"UsagePage", L"Usage", L"Product");
    for (size_t i = 0; i < limit; ++i) {
        wprintf(L"0x%-6x%-11d0x%-10x0x%-6x%ls\n",
                list[i].pid,
                list[i].interface_number,
                list[i].usage_page,
                list[i].usage,
                list[i].product);
    }
}

static int kc_cli_run_once(kc_hid_session_t *session,
                           uint16_t vid,
                           uint16_t pid,
                           int interface_num,
                           int timeout_ms,
                           HANDLE cancel_event,
                           bool debug,
                           bool raw_mode,
                           bool json_mode,
                           bool watch_mode) {
    uint8_t pct = 0;
    bool charging = false;
    kc_hid_status_t st = kc_hid_query_battery(
        session, vid, pid, interface_num, watch_mode ? 2U : 3U, (uint32_t)timeout_ms,
        cancel_event, debug, stderr, &pct, &charging);
    if (st == KC_HID_CANCELLED) {
        return -1;
    }
    if (st != KC_HID_OK) {
        if (json_mode) {
            char err_utf8[128];
            char err_esc[256];
            wide_to_utf8(kc_hid_status_text_w(st), err_utf8, sizeof(err_utf8));
            kc_json_escape(err_utf8, err_esc, sizeof(err_esc));
            printf("{\"ok\":false,\"error\":\"%s\"}\n", err_esc);
            fflush(stdout);
        } else if (watch_mode) {
            SYSTEMTIME lt;
            GetLocalTime(&lt);
            fwprintf(stderr, L"[%02u:%02u:%02u] Error: %ls\n",
                     (unsigned)lt.wHour, (unsigned)lt.wMinute, (unsigned)lt.wSecond,
                     kc_hid_status_text_w(st));
            fflush(stderr);
        } else {
            fwprintf(stderr, L"Error: %ls\n", kc_hid_status_text_w(st));
        }
        return 1;
    }

    if (json_mode) {
        char prod_utf8[256] = "";
        char prod_esc[512] = "";
        uint16_t out_vid = session->has_info ? session->cached_info.vid : vid;
        uint16_t out_pid = session->has_info ? session->cached_info.pid : pid;
        int out_iface = session->has_info ? session->cached_info.interface_number : interface_num;
        if (session->has_info) {
            wide_to_utf8(session->cached_info.product, prod_utf8, sizeof(prod_utf8));
            kc_json_escape(prod_utf8, prod_esc, sizeof(prod_esc));
        }
        char paired_field[48] = "";
        if (session->paired_pid != 0U) {
            snprintf(paired_field, sizeof(paired_field), ",\"paired_pid\":\"0x%04x\"",
                     (unsigned)session->paired_pid);
        }
        char link_field[32] = "";
        const char *link_a = kc_hid_link_label_a(session->active_link);
        if (link_a[0] != '\0') {
            snprintf(link_field, sizeof(link_field), ",\"link\":\"%s\"", link_a);
        }
        printf("{\"ok\":true,\"vid\":\"0x%04x\",\"pid\":\"0x%04x\",\"interface\":%d,"
               "\"product\":\"%s\"%s%s,\"battery\":%u,\"charging\":%s}\n",
               out_vid, out_pid, out_iface, prod_esc, paired_field, link_field,
               (unsigned)pct, charging ? "true" : "false");
        fflush(stdout);
    } else if (raw_mode) {
        wprintf(L"%u\n", (unsigned)pct);
        fflush(stdout);
    } else {
        const wchar_t *link_w = kc_hid_link_label_w(session->active_link);
        wchar_t suffix[48] = L"";
        if (link_w[0] != L'\0' && charging) {
            _snwprintf(suffix, 48, L" (%ls, charging)", link_w);
        } else if (link_w[0] != L'\0') {
            _snwprintf(suffix, 48, L" (%ls)", link_w);
        } else if (charging) {
            _snwprintf(suffix, 48, L" (charging)");
        }
        suffix[47] = L'\0';
        if (watch_mode) {
            SYSTEMTIME lt;
            GetLocalTime(&lt);
            wprintf(L"[%02u:%02u:%02u] Battery: %u%%%ls\n",
                    (unsigned)lt.wHour, (unsigned)lt.wMinute, (unsigned)lt.wSecond,
                    (unsigned)pct, suffix);
            fflush(stdout);
        } else {
            wprintf(L"Battery: %u%%%ls\n", (unsigned)pct, suffix);
        }
    }
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    bool do_list = false;
    bool watch_mode = false;
    int watch_sec = 5;
    bool debug = false;
    bool raw_mode = false;
    bool json_mode = false;
    uint16_t vid = KEYCHRON_VID;
    uint16_t pid = 0U;
    int interface_num = -1;
    int timeout_ms = 1500;

    for (int i = 1; i < argc; ++i) {
        const wchar_t *arg = argv[i];
        if (wcscmp(arg, L"--list") == 0) {
            do_list = true;
        } else if (wcscmp(arg, L"--watch") == 0) {
            watch_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                if (!kc_parse_int_range_w(argv[++i], 1, 3600, &watch_sec)) {
                    fwprintf(stderr, L"Invalid --watch interval (expected 1-3600 seconds): %ls\n", argv[i]);
                    return 2;
                }
            }
        } else if (wcscmp(arg, L"--debug") == 0) {
            debug = true;
        } else if (wcscmp(arg, L"--raw") == 0) {
            raw_mode = true;
        } else if (wcscmp(arg, L"--json") == 0) {
            json_mode = true;
        } else if (wcscmp(arg, L"--version") == 0 || wcscmp(arg, L"-v") == 0) {
            printf("keychron-battery %s\n", KC_APP_VERSION);
            return 0;
        } else if (wcscmp(arg, L"--help") == 0 || wcscmp(arg, L"-h") == 0) {
            print_usage();
            return 0;
        } else if (wcscmp(arg, L"--vid") == 0 && i + 1 < argc) {
            if (!kc_parse_u16_w(argv[++i], &vid)) {
                fwprintf(stderr, L"Invalid --vid value: %ls\n", argv[i]);
                return 2;
            }
        } else if (wcscmp(arg, L"--pid") == 0 && i + 1 < argc) {
            if (!kc_parse_u16_w(argv[++i], &pid)) {
                fwprintf(stderr, L"Invalid --pid value: %ls\n", argv[i]);
                return 2;
            }
        } else if (wcscmp(arg, L"--interface") == 0 && i + 1 < argc) {
            if (!kc_parse_int_range_w(argv[++i], 0, 255, &interface_num)) {
                fwprintf(stderr, L"Invalid --interface value: %ls\n", argv[i]);
                return 2;
            }
        } else if (wcscmp(arg, L"--timeout") == 0 && i + 1 < argc) {
            if (!kc_parse_int_range_w(argv[++i], 100, 10000, &timeout_ms)) {
                fwprintf(stderr, L"Invalid --timeout value (expected 100-10000): %ls\n", argv[i]);
                return 2;
            }
        } else {
            fwprintf(stderr, L"Unknown or incomplete argument: %ls\n", arg);
            print_usage();
            return 2;
        }
    }

    if (do_list && watch_mode) {
        fwprintf(stderr, L"--list and --watch cannot be combined.\n");
        return 2;
    }

    if (do_list) {
        list_interfaces(vid, json_mode);
        return 0;
    }

    kc_hid_session_t session;
    kc_hid_session_init(&session);

    if (!watch_mode) {
        int rc = kc_cli_run_once(
            &session, vid, pid, interface_num, timeout_ms, NULL,
            debug, raw_mode, json_mode, false);
        return (rc == 0) ? 0 : 1;
    }

    HANDLE stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stop_ev) {
        return 1;
    }
    g_cli_stop_event = stop_ev;
    SetConsoleCtrlHandler(kc_cli_ctrl_handler, TRUE);

    for (;;) {
        int rc = kc_cli_run_once(
            &session, vid, pid, interface_num, timeout_ms, stop_ev,
            debug, raw_mode, json_mode, true);
        if (rc < 0) {
            break;
        }
        if (WaitForSingleObject(stop_ev, (DWORD)watch_sec * 1000U) == WAIT_OBJECT_0) {
            break;
        }
    }

    SetConsoleCtrlHandler(kc_cli_ctrl_handler, FALSE);
    g_cli_stop_event = NULL;
    CloseHandle(stop_ev);
    return 0;
}
