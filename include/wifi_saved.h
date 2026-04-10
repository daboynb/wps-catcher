#ifndef _WIFI_SAVED_H_
#define _WIFI_SAVED_H_

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_SAVED_WIFI 16

typedef struct {
    char    ssid[33];
    char    password[65];
    char    bssid[18];   /* "AA:BB:CC:DD:EE:FF" */
} saved_wifi_entry_t;

/* Add a new entry (deduplicates by BSSID; updates password if exists) */
esp_err_t wifi_saved_add(const char *ssid, const char *password, const char *bssid);

/* Get all saved entries. Returns actual count. */
int wifi_saved_list(saved_wifi_entry_t *out, int max_entries);

/* Delete entry by index (0-based). Compacts remaining entries. */
esp_err_t wifi_saved_delete(int index);

/* Get current count of saved entries */
int wifi_saved_count(void);

#endif
