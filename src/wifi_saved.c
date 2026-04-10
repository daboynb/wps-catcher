#include <string.h>
#include <esp_log.h>
#include "config.h"
#include "wifi_saved.h"

static const char *TAG = "WIFI_SAVED";

/*
 * NVS key scheme (all under 15-char limit):
 *   "wp_cnt"  — int32: number of saved entries
 *   "wpNs"    — string: SSID   for entry N (0..15)
 *   "wpNp"    — string: password for entry N
 *   "wpNb"    — string: BSSID   for entry N
 */

static void make_key(char *buf, int idx, char suffix)
{
    snprintf(buf, 16, "wp%d%c", idx, suffix);
}


static int load_count(void)
{
    int32_t cnt = 0;
    if (read_int_from_flash("wp_cnt", &cnt) != ESP_OK) return 0;
    if (cnt < 0) cnt = 0;
    if (cnt > MAX_SAVED_WIFI) cnt = MAX_SAVED_WIFI;
    return (int)cnt;
}


static void save_count(int cnt)
{
    save_int_to_flash("wp_cnt", cnt);
}


static void load_entry(int idx, saved_wifi_entry_t *e)
{
    char key[16];
    memset(e, 0, sizeof(*e));

    make_key(key, idx, 's');
    read_string_from_flash(key, e->ssid, sizeof(e->ssid));

    make_key(key, idx, 'p');
    read_string_from_flash(key, e->password, sizeof(e->password));

    make_key(key, idx, 'b');
    read_string_from_flash(key, e->bssid, sizeof(e->bssid));
}


static void save_entry(int idx, const saved_wifi_entry_t *e)
{
    char key[16];

    make_key(key, idx, 's');
    save_string_to_flash(key, e->ssid);

    make_key(key, idx, 'p');
    save_string_to_flash(key, e->password);

    make_key(key, idx, 'b');
    save_string_to_flash(key, e->bssid);
}


esp_err_t wifi_saved_add(const char *ssid, const char *password, const char *bssid)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    int cnt = load_count();

    /* Check for duplicate by BSSID — update password if exists */
    for (int i = 0; i < cnt; i++) {
        saved_wifi_entry_t e;
        load_entry(i, &e);
        if (bssid && bssid[0] && strcasecmp(e.bssid, bssid) == 0) {
            /* Update existing entry */
            strlcpy(e.ssid, ssid, sizeof(e.ssid));
            if (password) strlcpy(e.password, password, sizeof(e.password));
            save_entry(i, &e);
            ESP_LOGI(TAG, "Updated entry %d: %s [%s]", i, ssid, bssid);
            return ESP_OK;
        }
    }

    /* New entry */
    if (cnt >= MAX_SAVED_WIFI) {
        ESP_LOGW(TAG, "Storage full (%d entries), dropping oldest", MAX_SAVED_WIFI);
        /* Shift everything down by 1, dropping index 0 */
        for (int i = 0; i < cnt - 1; i++) {
            saved_wifi_entry_t e;
            load_entry(i + 1, &e);
            save_entry(i, &e);
        }
        cnt = MAX_SAVED_WIFI - 1;
    }

    saved_wifi_entry_t e = {0};
    strlcpy(e.ssid, ssid, sizeof(e.ssid));
    if (password) strlcpy(e.password, password, sizeof(e.password));
    if (bssid) strlcpy(e.bssid, bssid, sizeof(e.bssid));

    save_entry(cnt, &e);
    save_count(cnt + 1);

    ESP_LOGI(TAG, "Saved entry %d: %s [%s]", cnt, ssid, bssid ? bssid : "");
    return ESP_OK;
}


int wifi_saved_list(saved_wifi_entry_t *out, int max_entries)
{
    int cnt = load_count();
    if (cnt > max_entries) cnt = max_entries;

    for (int i = 0; i < cnt; i++) {
        load_entry(i, &out[i]);
    }
    return cnt;
}


esp_err_t wifi_saved_delete(int index)
{
    int cnt = load_count();
    if (index < 0 || index >= cnt) return ESP_ERR_INVALID_ARG;

    /* Compact: shift entries after index down by 1 */
    for (int i = index; i < cnt - 1; i++) {
        saved_wifi_entry_t e;
        load_entry(i + 1, &e);
        save_entry(i, &e);
    }

    cnt--;
    save_count(cnt);

    ESP_LOGI(TAG, "Deleted entry %d, %d remaining", index, cnt);
    return ESP_OK;
}


int wifi_saved_count(void)
{
    return load_count();
}
