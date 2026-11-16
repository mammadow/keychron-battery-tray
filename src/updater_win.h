#ifndef KEYCHRON_UPDATER_WIN_H
#define KEYCHRON_UPDATER_WIN_H

#include "protocol.h"

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#define KC_UPDATE_MANIFEST_URL \
    L"https://github.com/mammadow/keychron-battery-tray/releases/latest/download/keychron-battery-tray.json"

typedef struct kc_update_check_result {
    bool ok;
    bool update_available;
    kc_release_info_t release;
    wchar_t error[192];
} kc_update_check_result_t;

typedef struct kc_update_install_result {
    bool ok;
    wchar_t script_path[MAX_PATH];
    wchar_t work_dir[MAX_PATH];
    wchar_t error[192];
} kc_update_install_result_t;

/** Compute lowercase SHA-256 hex digest of a local file using lazy-loaded bcrypt.dll. */
bool kc_updater_sha256_file(const wchar_t *path, char out_hex[65]);

/** Query latest release manifest over HTTPS and compare against current_ver_str. */
kc_update_check_result_t kc_updater_check_latest(const char *current_ver_str, HANDLE stop_event);

/**
 * Download the release zip over HTTPS, verify its SHA-256 digest, extract executables,
 * and write a self-cleaning swap script (apply_update.cmd) into a temp directory.
 */
kc_update_install_result_t kc_updater_prepare_install(const kc_release_info_t *release,
                                                      HANDLE stop_event);

/** Launch the prepared swap script hidden so it replaces binaries once this process exits. */
bool kc_updater_launch_script(const wchar_t *script_path);

/** Delete a staging directory created by kc_updater_prepare_install if launch fails/aborts. */
void kc_updater_cleanup_dir(const wchar_t *work_dir);

#endif /* KEYCHRON_UPDATER_WIN_H */
