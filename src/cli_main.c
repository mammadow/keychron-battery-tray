#include "hid_win.h"
#include "protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static void print_usage(void) {
    wprintf(L"Usage: keychron-battery [OPTIONS]\n\n"
            L"Options:\n"
            L"  --list             List Keychron HID interfaces and exit\n"
            L"  --vid <hex|dec>    Vendor ID (default: 0x3434)\n"
            L"  --pid <hex|dec>    Product ID (auto-detects vendor interface if omitted)\n"
            L"  --interface <n>    USB interface number (auto-detects if omitted)\n"
            L"  --timeout <ms>     Per-attempt response timeout (100-10000, default: 1500)\n"
            L"  --raw              Print bare percentage integer (e.g. 46)\n"
            L"  --json             Output structured JSON (works with query or --list)\n"
            L"  --debug            Print raw HID caps, send method, and response bytes\n"
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

int wmain(int argc, wchar_t **argv) {
    bool do_list = false;
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
        } else if (wcscmp(arg, L"--debug") == 0) {
            debug = true;
        } else if (wcscmp(arg, L"--raw") == 0) {
            raw_mode = true;
        } else if (wcscmp(arg, L"--json") == 0) {
            json_mode = true;
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

    if (do_list) {
        list_interfaces(vid, json_mode);
        return 0;
    }

    kc_hid_session_t session;
    kc_hid_session_init(&session);

    uint8_t pct = 0;
    kc_hid_status_t st = kc_hid_query_battery(
        &session, vid, pid, interface_num, 3U, (uint32_t)timeout_ms, NULL, debug, stderr, &pct);
    if (st != KC_HID_OK) {
        if (json_mode) {
            char err_utf8[128];
            char err_esc[256];
            wide_to_utf8(kc_hid_status_text_w(st), err_utf8, sizeof(err_utf8));
            kc_json_escape(err_utf8, err_esc, sizeof(err_esc));
            printf("{\"ok\":false,\"error\":\"%s\"}\n", err_esc);
        } else {
            fwprintf(stderr, L"Error: %ls\n", kc_hid_status_text_w(st));
        }
        return 1;
    }

    if (json_mode) {
        char prod_utf8[256] = "";
        char prod_esc[512] = "";
        uint16_t out_vid = session.has_info ? session.cached_info.vid : vid;
        uint16_t out_pid = session.has_info ? session.cached_info.pid : pid;
        int out_iface = session.has_info ? session.cached_info.interface_number : interface_num;
        if (session.has_info) {
            wide_to_utf8(session.cached_info.product, prod_utf8, sizeof(prod_utf8));
            kc_json_escape(prod_utf8, prod_esc, sizeof(prod_esc));
        }
        printf("{\"ok\":true,\"vid\":\"0x%04x\",\"pid\":\"0x%04x\",\"interface\":%d,"
               "\"product\":\"%s\",\"battery\":%u}\n",
               out_vid, out_pid, out_iface, prod_esc, (unsigned)pct);
    } else if (raw_mode) {
        wprintf(L"%u\n", (unsigned)pct);
    } else {
        wprintf(L"Battery: %u%%\n", (unsigned)pct);
    }
    return 0;
}
