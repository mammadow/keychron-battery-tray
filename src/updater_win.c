#include "updater_win.h"

#include <bcrypt.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef BCRYPT_SHA256_ALGORITHM
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
#endif

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#ifndef WINHTTP_OPTION_REDIRECT_POLICY
#define WINHTTP_OPTION_REDIRECT_POLICY 88
#endif

#ifndef WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
#define WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP 1
#endif

#define KC_UPD_MAX_MANIFEST_BYTES (64U * 1024U)
#define KC_UPD_MAX_ZIP_BYTES      (16U * 1024U * 1024U)

typedef HINTERNET (WINAPI *kc_wh_open_fn)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI *kc_wh_connect_fn)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI *kc_wh_open_req_fn)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL      (WINAPI *kc_wh_set_opt_fn)(HINTERNET, DWORD, LPVOID, DWORD);
typedef BOOL      (WINAPI *kc_wh_set_to_fn)(HINTERNET, int, int, int, int);
typedef BOOL      (WINAPI *kc_wh_send_req_fn)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL      (WINAPI *kc_wh_recv_resp_fn)(HINTERNET, LPVOID);
typedef BOOL      (WINAPI *kc_wh_query_hdr_fn)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL      (WINAPI *kc_wh_query_avail_fn)(HINTERNET, LPDWORD);
typedef BOOL      (WINAPI *kc_wh_read_fn)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL      (WINAPI *kc_wh_close_fn)(HINTERNET);
typedef BOOL      (WINAPI *kc_wh_crack_url_fn)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTSW);

typedef struct kc_wh_api {
    kc_wh_open_fn open;
    kc_wh_connect_fn connect;
    kc_wh_open_req_fn open_request;
    kc_wh_set_opt_fn set_option;
    kc_wh_set_to_fn set_timeouts;
    kc_wh_send_req_fn send_request;
    kc_wh_recv_resp_fn receive_response;
    kc_wh_query_hdr_fn query_headers;
    kc_wh_query_avail_fn query_data_available;
    kc_wh_read_fn read_data;
    kc_wh_close_fn close_handle;
    kc_wh_crack_url_fn crack_url;
} kc_wh_api_t;

typedef NTSTATUS (WINAPI *kc_bc_open_alg_fn)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS (WINAPI *kc_bc_close_alg_fn)(BCRYPT_ALG_HANDLE, ULONG);
typedef NTSTATUS (WINAPI *kc_bc_create_hash_fn)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS (WINAPI *kc_bc_hash_data_fn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS (WINAPI *kc_bc_finish_hash_fn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS (WINAPI *kc_bc_destroy_hash_fn)(BCRYPT_HASH_HANDLE);

typedef struct kc_bc_api {
    kc_bc_open_alg_fn open_algorithm_provider;
    kc_bc_close_alg_fn close_algorithm_provider;
    kc_bc_create_hash_fn create_hash;
    kc_bc_hash_data_fn hash_data;
    kc_bc_finish_hash_fn finish_hash;
    kc_bc_destroy_hash_fn destroy_hash;
} kc_bc_api_t;

static INIT_ONCE g_wh_init_once = INIT_ONCE_STATIC_INIT;
static kc_wh_api_t g_wh_api;
static INIT_ONCE g_bc_init_once = INIT_ONCE_STATIC_INIT;
static kc_bc_api_t g_bc_api;

static void kc_upd_load_proc(HMODULE mod, const char *name, void *dst_ptr, size_t dst_sz) {
    FARPROC proc = GetProcAddress(mod, name);
    if (proc && dst_sz == sizeof(proc)) {
        memcpy(dst_ptr, &proc, sizeof(proc));
    }
}

static BOOL CALLBACK kc_init_wh_api_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    HMODULE mod = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) {
        return TRUE;
    }
    kc_wh_api_t a;
    memset(&a, 0, sizeof(a));
    kc_upd_load_proc(mod, "WinHttpOpen", &a.open, sizeof(a.open));
    kc_upd_load_proc(mod, "WinHttpConnect", &a.connect, sizeof(a.connect));
    kc_upd_load_proc(mod, "WinHttpOpenRequest", &a.open_request, sizeof(a.open_request));
    kc_upd_load_proc(mod, "WinHttpSetOption", &a.set_option, sizeof(a.set_option));
    kc_upd_load_proc(mod, "WinHttpSetTimeouts", &a.set_timeouts, sizeof(a.set_timeouts));
    kc_upd_load_proc(mod, "WinHttpSendRequest", &a.send_request, sizeof(a.send_request));
    kc_upd_load_proc(mod, "WinHttpReceiveResponse", &a.receive_response, sizeof(a.receive_response));
    kc_upd_load_proc(mod, "WinHttpQueryHeaders", &a.query_headers, sizeof(a.query_headers));
    kc_upd_load_proc(mod, "WinHttpQueryDataAvailable", &a.query_data_available, sizeof(a.query_data_available));
    kc_upd_load_proc(mod, "WinHttpReadData", &a.read_data, sizeof(a.read_data));
    kc_upd_load_proc(mod, "WinHttpCloseHandle", &a.close_handle, sizeof(a.close_handle));
    kc_upd_load_proc(mod, "WinHttpCrackUrl", &a.crack_url, sizeof(a.crack_url));

    if (a.open && a.connect && a.open_request && a.set_option && a.set_timeouts &&
        a.send_request && a.receive_response && a.query_headers &&
        a.query_data_available && a.read_data && a.close_handle && a.crack_url) {
        g_wh_api = a;
    } else {
        FreeLibrary(mod);
    }
    return TRUE;
}

static const kc_wh_api_t *kc_get_wh_api(void) {
    InitOnceExecuteOnce(&g_wh_init_once, kc_init_wh_api_cb, NULL, NULL);
    return g_wh_api.open ? &g_wh_api : NULL;
}

static BOOL CALLBACK kc_init_bc_api_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    HMODULE mod = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) {
        return TRUE;
    }
    kc_bc_api_t a;
    memset(&a, 0, sizeof(a));
    kc_upd_load_proc(mod, "BCryptOpenAlgorithmProvider", &a.open_algorithm_provider, sizeof(a.open_algorithm_provider));
    kc_upd_load_proc(mod, "BCryptCloseAlgorithmProvider", &a.close_algorithm_provider, sizeof(a.close_algorithm_provider));
    kc_upd_load_proc(mod, "BCryptCreateHash", &a.create_hash, sizeof(a.create_hash));
    kc_upd_load_proc(mod, "BCryptHashData", &a.hash_data, sizeof(a.hash_data));
    kc_upd_load_proc(mod, "BCryptFinishHash", &a.finish_hash, sizeof(a.finish_hash));
    kc_upd_load_proc(mod, "BCryptDestroyHash", &a.destroy_hash, sizeof(a.destroy_hash));

    if (a.open_algorithm_provider && a.close_algorithm_provider &&
        a.create_hash && a.hash_data && a.finish_hash && a.destroy_hash) {
        g_bc_api = a;
    } else {
        FreeLibrary(mod);
    }
    return TRUE;
}

static const kc_bc_api_t *kc_get_bc_api(void) {
    InitOnceExecuteOnce(&g_bc_init_once, kc_init_bc_api_cb, NULL, NULL);
    return g_bc_api.open_algorithm_provider ? &g_bc_api : NULL;
}

static bool kc_upd_cancelled(HANDLE stop_event) {
    return stop_event && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
}

static void kc_upd_set_err(wchar_t err[192], const wchar_t *msg) {
    if (!err) {
        return;
    }
    wcsncpy(err, msg ? msg : L"Update error", 191);
    err[191] = L'\0';
}

typedef struct kc_http_sink {
    char *mem_buf;
    size_t mem_len;
    size_t mem_cap;
    HANDLE file;
    const kc_bc_api_t *bc;
    BCRYPT_HASH_HANDLE hash_h;
    size_t max_bytes;
} kc_http_sink_t;

static bool kc_http_fetch(const wchar_t *url,
                          kc_http_sink_t *sink,
                          HANDLE stop_event,
                          wchar_t err[192]) {
    const kc_wh_api_t *wh = kc_get_wh_api();
    if (!wh) {
        kc_upd_set_err(err, L"Windows HTTP library (winhttp.dll) unavailable.");
        return false;
    }

    wchar_t host[256] = L"";
    wchar_t path[1024] = L"";
    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0]));

    if (!wh->crack_url(url, 0U, 0U, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS || host[0] == L'\0') {
        kc_upd_set_err(err, L"Invalid HTTPS update URL.");
        return false;
    }

    HINTERNET h_sess = wh->open(L"KeychronBatteryTray/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS,
                                0U);
    if (!h_sess) {
        h_sess = wh->open(L"KeychronBatteryTray/1.0",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS,
                          0U);
    }
    if (!h_sess) {
        kc_upd_set_err(err, L"Failed to open WinHTTP session.");
        return false;
    }

    wh->set_timeouts(h_sess, 5000, 8000, 8000, 15000);

    HINTERNET h_conn = wh->connect(h_sess, host, uc.nPort, 0U);
    if (!h_conn) {
        wh->close_handle(h_sess);
        kc_upd_set_err(err, L"Could not connect to GitHub.");
        return false;
    }

    HINTERNET h_req = wh->open_request(
        h_conn, L"GET", path[0] ? path : L"/", NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!h_req) {
        wh->close_handle(h_conn);
        wh->close_handle(h_sess);
        kc_upd_set_err(err, L"Could not create HTTPS request.");
        return false;
    }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    wh->set_option(h_req, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    bool ok = false;
    if (!wh->send_request(h_req, WINHTTP_NO_ADDITIONAL_HEADERS, 0U, WINHTTP_NO_REQUEST_DATA, 0U, 0U, 0U) ||
        !wh->receive_response(h_req, NULL)) {
        kc_upd_set_err(err, L"Network error contacting GitHub Releases.");
        goto cleanup;
    }

    DWORD status_code = 0U;
    DWORD sc_sz = sizeof(status_code);
    if (!wh->query_headers(h_req,
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX,
                           &status_code, &sc_sz, WINHTTP_NO_HEADER_INDEX) ||
        status_code < 200U || status_code >= 300U) {
        _snwprintf(err, 192, L"GitHub returned HTTP %lu.", (unsigned long)status_code);
        err[191] = L'\0';
        goto cleanup;
    }

    size_t total = 0U;
    uint8_t chunk[16384];
    for (;;) {
        if (kc_upd_cancelled(stop_event)) {
            kc_upd_set_err(err, L"Cancelled.");
            goto cleanup;
        }
        DWORD avail = 0U;
        if (!wh->query_data_available(h_req, &avail)) {
            kc_upd_set_err(err, L"Download interrupted.");
            goto cleanup;
        }
        if (avail == 0U) {
            ok = true;
            break;
        }
        DWORD to_read = (avail > (DWORD)sizeof(chunk)) ? (DWORD)sizeof(chunk) : avail;
        DWORD read_n = 0U;
        if (!wh->read_data(h_req, chunk, to_read, &read_n)) {
            kc_upd_set_err(err, L"Failed reading response body.");
            goto cleanup;
        }
        if (read_n == 0U) {
            ok = true;
            break;
        }
        if (total + (size_t)read_n > sink->max_bytes) {
            kc_upd_set_err(err, L"Downloaded file exceeds size limit.");
            goto cleanup;
        }
        total += (size_t)read_n;

        if (sink->mem_buf) {
            if (sink->mem_len + (size_t)read_n >= sink->mem_cap) {
                kc_upd_set_err(err, L"Manifest exceeds buffer limit.");
                goto cleanup;
            }
            memcpy(sink->mem_buf + sink->mem_len, chunk, (size_t)read_n);
            sink->mem_len += (size_t)read_n;
            sink->mem_buf[sink->mem_len] = '\0';
        }
        if (sink->file != INVALID_HANDLE_VALUE) {
            DWORD written = 0U;
            if (!WriteFile(sink->file, chunk, read_n, &written, NULL) || written != read_n) {
                kc_upd_set_err(err, L"Failed writing update file to temp storage.");
                goto cleanup;
            }
        }
        if (sink->bc && sink->hash_h) {
            if (!BCRYPT_SUCCESS(sink->bc->hash_data(sink->hash_h, chunk, read_n, 0U))) {
                kc_upd_set_err(err, L"SHA-256 hashing failed.");
                goto cleanup;
            }
        }
    }

cleanup:
    wh->close_handle(h_req);
    wh->close_handle(h_conn);
    wh->close_handle(h_sess);
    return ok;
}

static void kc_format_sha256_hex(const uint8_t digest[32], char out_hex[65]) {
    static const char k_hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32U; ++i) {
        out_hex[i * 2U] = k_hex[(digest[i] >> 4) & 0x0FU];
        out_hex[i * 2U + 1U] = k_hex[digest[i] & 0x0FU];
    }
    out_hex[64] = '\0';
}

bool kc_updater_sha256_file(const wchar_t *path, char out_hex[65]) {
    if (!path || !out_hex) {
        return false;
    }
    out_hex[0] = '\0';
    const kc_bc_api_t *bc = kc_get_bc_api();
    if (!bc) {
        return false;
    }
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash_h = NULL;
    bool ok = false;
    if (!BCRYPT_SUCCESS(bc->open_algorithm_provider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0U))) {
        CloseHandle(f);
        return false;
    }
    if (!BCRYPT_SUCCESS(bc->create_hash(alg, &hash_h, NULL, 0U, NULL, 0U, 0U))) {
        bc->close_algorithm_provider(alg, 0U);
        CloseHandle(f);
        return false;
    }

    uint8_t buf[16384];
    DWORD read_n = 0U;
    bool read_ok = true;
    while (ReadFile(f, buf, (DWORD)sizeof(buf), &read_n, NULL)) {
        if (read_n == 0U) {
            break;
        }
        if (!BCRYPT_SUCCESS(bc->hash_data(hash_h, buf, read_n, 0U))) {
            read_ok = false;
            break;
        }
    }
    if (read_ok) {
        uint8_t digest[32];
        if (BCRYPT_SUCCESS(bc->finish_hash(hash_h, digest, (ULONG)sizeof(digest), 0U))) {
            kc_format_sha256_hex(digest, out_hex);
            ok = true;
        }
    }

    bc->destroy_hash(hash_h);
    bc->close_algorithm_provider(alg, 0U);
    CloseHandle(f);
    return ok;
}

kc_update_check_result_t kc_updater_check_latest(const char *current_ver_str, HANDLE stop_event) {
    kc_update_check_result_t res;
    memset(&res, 0, sizeof(res));

    kc_semver_t local_ver;
    if (!kc_parse_semver(current_ver_str, &local_ver)) {
        kc_upd_set_err(res.error, L"Invalid local version.");
        return res;
    }

    char *json = (char *)calloc(1, KC_UPD_MAX_MANIFEST_BYTES + 1U);
    if (!json) {
        kc_upd_set_err(res.error, L"Out of memory.");
        return res;
    }

    kc_http_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.mem_buf = json;
    sink.mem_cap = KC_UPD_MAX_MANIFEST_BYTES + 1U;
    sink.file = INVALID_HANDLE_VALUE;
    sink.max_bytes = KC_UPD_MAX_MANIFEST_BYTES;

    if (!kc_http_fetch(KC_UPDATE_MANIFEST_URL, &sink, stop_event, res.error)) {
        free(json);
        return res;
    }

    if (!kc_parse_scoop_manifest(json, sink.mem_len, &res.release)) {
        free(json);
        kc_upd_set_err(res.error, L"Release manifest format not recognized.");
        return res;
    }
    free(json);

    res.ok = true;
    res.update_available = (kc_compare_semver(res.release.version, local_ver) > 0);
    return res;
}

static bool kc_dir_is_writable(const wchar_t *dir) {
    wchar_t probe[MAX_PATH];
    if (_snwprintf(probe, MAX_PATH, L"%ls\\.kc_upd_test_%lu.tmp",
                   dir, (unsigned long)GetCurrentProcessId()) <= 0) {
        return false;
    }
    probe[MAX_PATH - 1] = L'\0';
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0U, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

static bool kc_file_exists_nonempty(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        return false;
    }
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        return false;
    }
    return fad.nFileSizeHigh > 0U || fad.nFileSizeLow > 0U;
}

static bool kc_run_hidden_cmd(wchar_t *cmd_line, DWORD timeout_ms, HANDLE stop_event) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(NULL, cmd_line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }
    HANDLE waits[2] = {pi.hProcess, stop_event};
    DWORD n_waits = stop_event ? 2U : 1U;
    DWORD wres = WaitForMultipleObjects(n_waits, waits, FALSE, timeout_ms);
    DWORD exit_code = 1U;
    bool ok = (wres == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code == 0U);
    if (wres != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1U);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

static bool kc_escape_bat_percent(const wchar_t *src, char *out_utf8, size_t cap) {
    char utf8[MAX_PATH * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, utf8, (int)sizeof(utf8), NULL, NULL);
    if (n <= 0) {
        return false;
    }
    size_t j = 0U;
    for (size_t i = 0; utf8[i] != '\0'; ++i) {
        if (utf8[i] == '%') {
            if (j + 2U >= cap) return false;
            out_utf8[j++] = '%';
            out_utf8[j++] = '%';
        } else {
            if (j + 1U >= cap) return false;
            out_utf8[j++] = utf8[i];
        }
    }
    out_utf8[j] = '\0';
    return true;
}

static void kc_delete_tree_dir(const wchar_t *dir) {
    if (!dir || !dir[0]) {
        return;
    }
    wchar_t pat[MAX_PATH];
    if (_snwprintf(pat, MAX_PATH, L"%ls\\*", dir) <= 0) {
        return;
    }
    pat[MAX_PATH - 1] = L'\0';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            wchar_t child[MAX_PATH];
            if (_snwprintf(child, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) > 0) {
                child[MAX_PATH - 1] = L'\0';
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                    kc_delete_tree_dir(child);
                } else {
                    SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(child);
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

void kc_updater_cleanup_dir(const wchar_t *work_dir) {
    kc_delete_tree_dir(work_dir);
}

kc_update_install_result_t kc_updater_prepare_install(const kc_release_info_t *release,
                                                      HANDLE stop_event) {
    kc_update_install_result_t res;
    memset(&res, 0, sizeof(res));
    if (!release || !release->download_url[0] || strlen(release->sha256_hex) != 64U) {
        kc_upd_set_err(res.error, L"Invalid release metadata.");
        return res;
    }

    wchar_t cur_exe[MAX_PATH];
    DWORD cur_len = GetModuleFileNameW(NULL, cur_exe, MAX_PATH);
    if (cur_len == 0U || cur_len >= MAX_PATH) {
        kc_upd_set_err(res.error, L"Could not locate current executable.");
        return res;
    }
    wchar_t install_dir[MAX_PATH];
    wcsncpy(install_dir, cur_exe, MAX_PATH - 1);
    install_dir[MAX_PATH - 1] = L'\0';
    wchar_t *slash = wcsrchr(install_dir, L'\\');
    if (!slash) {
        kc_upd_set_err(res.error, L"Invalid executable directory.");
        return res;
    }
    *slash = L'\0';

    if (!kc_dir_is_writable(install_dir)) {
        kc_upd_set_err(res.error, L"Install folder is read-only. Run as Administrator or update manually.");
        return res;
    }

    const kc_bc_api_t *bc = kc_get_bc_api();
    if (!bc) {
        kc_upd_set_err(res.error, L"Windows CNG library (bcrypt.dll) unavailable.");
        return res;
    }

    wchar_t temp_root[MAX_PATH];
    DWORD tlen = GetTempPathW(MAX_PATH, temp_root);
    if (tlen == 0U || tlen >= MAX_PATH - 48U) {
        kc_upd_set_err(res.error, L"Temp directory unavailable.");
        return res;
    }
    _snwprintf(res.work_dir, MAX_PATH, L"%ls\\kc_upd_%lu_%llu",
               temp_root[tlen - 1U] == L'\\' ? (temp_root[tlen - 1U] = L'\0', temp_root) : temp_root,
               (unsigned long)GetCurrentProcessId(),
               (unsigned long long)GetTickCount64());
    res.work_dir[MAX_PATH - 1] = L'\0';

    wchar_t stage_dir[MAX_PATH];
    wchar_t zip_path[MAX_PATH];
    _snwprintf(stage_dir, MAX_PATH, L"%ls\\stage", res.work_dir);
    stage_dir[MAX_PATH - 1] = L'\0';
    _snwprintf(zip_path, MAX_PATH, L"%ls\\update.zip", res.work_dir);
    zip_path[MAX_PATH - 1] = L'\0';

    if (!CreateDirectoryW(res.work_dir, NULL) || !CreateDirectoryW(stage_dir, NULL)) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Could not create temporary staging directory.");
        return res;
    }

    wchar_t url_w[512];
    if (MultiByteToWideChar(CP_UTF8, 0, release->download_url, -1, url_w, 512) <= 0) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Invalid release download URL.");
        return res;
    }

    HANDLE zf = CreateFileW(zip_path, GENERIC_WRITE, 0U, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (zf == INVALID_HANDLE_VALUE) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Could not create temporary zip file.");
        return res;
    }

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash_h = NULL;
    if (!BCRYPT_SUCCESS(bc->open_algorithm_provider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0U))) {
        CloseHandle(zf);
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Failed to initialize SHA-256.");
        return res;
    }
    if (!BCRYPT_SUCCESS(bc->create_hash(alg, &hash_h, NULL, 0U, NULL, 0U, 0U))) {
        bc->close_algorithm_provider(alg, 0U);
        CloseHandle(zf);
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Failed to allocate SHA-256 hash.");
        return res;
    }

    kc_http_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.file = zf;
    sink.bc = bc;
    sink.hash_h = hash_h;
    sink.max_bytes = KC_UPD_MAX_ZIP_BYTES;

    bool fetched = kc_http_fetch(url_w, &sink, stop_event, res.error);
    uint8_t digest[32];
    bool hashed = fetched && BCRYPT_SUCCESS(bc->finish_hash(hash_h, digest, (ULONG)sizeof(digest), 0U));
    bc->destroy_hash(hash_h);
    bc->close_algorithm_provider(alg, 0U);
    CloseHandle(zf);

    if (!fetched || !hashed) {
        kc_updater_cleanup_dir(res.work_dir);
        if (fetched) kc_upd_set_err(res.error, L"SHA-256 finalization failed.");
        return res;
    }

    char got_hex[65];
    kc_format_sha256_hex(digest, got_hex);
    if (strcmp(got_hex, release->sha256_hex) != 0) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"SHA-256 verification failed! Update aborted.");
        return res;
    }

    wchar_t sys_dir[MAX_PATH];
    UINT slen = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (slen == 0U || slen >= MAX_PATH - 16U) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"System directory unavailable.");
        return res;
    }
    wchar_t tar_cmd[MAX_PATH * 3 + 64];
    _snwprintf(tar_cmd, sizeof(tar_cmd) / sizeof(tar_cmd[0]),
               L"\"%ls\\tar.exe\" -xf \"%ls\" -C \"%ls\"",
               sys_dir, zip_path, stage_dir);
    tar_cmd[(sizeof(tar_cmd) / sizeof(tar_cmd[0])) - 1U] = L'\0';
    if (!kc_run_hidden_cmd(tar_cmd, 30000U, stop_event)) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Failed to extract update archive.");
        return res;
    }

    wchar_t stage_tray[MAX_PATH];
    wchar_t stage_cli[MAX_PATH];
    wchar_t target_cli[MAX_PATH];
    _snwprintf(stage_tray, MAX_PATH, L"%ls\\keychron-battery-tray.exe", stage_dir);
    stage_tray[MAX_PATH - 1] = L'\0';
    _snwprintf(stage_cli, MAX_PATH, L"%ls\\keychron-battery.exe", stage_dir);
    stage_cli[MAX_PATH - 1] = L'\0';
    _snwprintf(target_cli, MAX_PATH, L"%ls\\keychron-battery.exe", install_dir);
    target_cli[MAX_PATH - 1] = L'\0';

    if (!kc_file_exists_nonempty(stage_tray)) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Extracted archive missing keychron-battery-tray.exe.");
        return res;
    }

    _snwprintf(res.script_path, MAX_PATH, L"%ls\\apply_update.cmd", res.work_dir);
    res.script_path[MAX_PATH - 1] = L'\0';

    char b_st_tray[MAX_PATH * 4];
    char b_tg_tray[MAX_PATH * 4];
    char b_st_cli[MAX_PATH * 4];
    char b_tg_cli[MAX_PATH * 4];
    char b_work[MAX_PATH * 4];
    if (!kc_escape_bat_percent(stage_tray, b_st_tray, sizeof(b_st_tray)) ||
        !kc_escape_bat_percent(cur_exe, b_tg_tray, sizeof(b_tg_tray)) ||
        !kc_escape_bat_percent(stage_cli, b_st_cli, sizeof(b_st_cli)) ||
        !kc_escape_bat_percent(target_cli, b_tg_cli, sizeof(b_tg_cli)) ||
        !kc_escape_bat_percent(res.work_dir, b_work, sizeof(b_work))) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Path encoding overflow.");
        return res;
    }

    char script_body[MAX_PATH * 24];
    int slen_b = snprintf(script_body, sizeof(script_body),
                          "@echo off\r\n"
                          "chcp 65001 >nul\r\n"
                          "setlocal DisableDelayedExpansion\r\n"
                          "for /L %%%%i in (1,1,30) do (\r\n"
                          "  copy /Y \"%s\" \"%s\" >nul 2>&1\r\n"
                          "  if not errorlevel 1 goto done_tray\r\n"
                          "  ping -n 1 -w 250 127.0.0.1 >nul\r\n"
                          ")\r\n"
                          "rd /s /q \"%s\" >nul 2>&1\r\n"
                          "exit /b 1\r\n"
                          ":done_tray\r\n"
                          "if exist \"%s\" copy /Y \"%s\" \"%s\" >nul 2>&1\r\n"
                          "start \"\" \"%s\"\r\n"
                          "ping -n 2 127.0.0.1 >nul\r\n"
                          "rd /s /q \"%s\" >nul 2>&1\r\n"
                          "exit /b 0\r\n",
                          b_st_tray, b_tg_tray, b_work,
                          b_st_cli, b_st_cli, b_tg_cli,
                          b_tg_tray, b_work);
    if (slen_b <= 0 || (size_t)slen_b >= sizeof(script_body)) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Updater script buffer overflow.");
        return res;
    }

    HANDLE sf = CreateFileW(res.script_path, GENERIC_WRITE, 0U, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (sf == INVALID_HANDLE_VALUE) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Failed to write swap helper script.");
        return res;
    }
    DWORD written = 0U;
    BOOL w_ok = WriteFile(sf, script_body, (DWORD)slen_b, &written, NULL);
    CloseHandle(sf);
    if (!w_ok || written != (DWORD)slen_b) {
        kc_updater_cleanup_dir(res.work_dir);
        kc_upd_set_err(res.error, L"Failed writing swap helper script.");
        return res;
    }

    res.ok = true;
    return res;
}

bool kc_updater_launch_script(const wchar_t *script_path) {
    if (!script_path || !script_path[0]) {
        return false;
    }
    wchar_t sys_dir[MAX_PATH];
    UINT slen = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (slen == 0U || slen >= MAX_PATH - 16U) {
        return false;
    }
    wchar_t cmd_line[MAX_PATH * 2 + 32];
    _snwprintf(cmd_line, sizeof(cmd_line) / sizeof(cmd_line[0]),
               L"\"%ls\\cmd.exe\" /c \"%ls\"", sys_dir, script_path);
    cmd_line[(sizeof(cmd_line) / sizeof(cmd_line[0])) - 1U] = L'\0';

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(NULL, cmd_line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
