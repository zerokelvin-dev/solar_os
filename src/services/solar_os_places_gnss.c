#include <stdio.h>

#include "solar_os_gnss.h"
#include "solar_os_places.h"
#include "solar_os_time.h"

#define PLACES_GNSS_READ_TIMEOUT_MS 20U

esp_err_t solar_os_places_update_self(void)
{
    esp_err_t last = ESP_ERR_NOT_FOUND;
    for (size_t index = 0U; index < solar_os_gnss_count(); index++) {
        solar_os_gnss_info_t info;
        if (!solar_os_gnss_get(index, &info) || !info.powered) {
            continue;
        }
        solar_os_gnss_fix_t fix;
        last = solar_os_gnss_read_fix(info.name, PLACES_GNSS_READ_TIMEOUT_MS, &fix);
        if (last != ESP_OK) {
            continue;
        }
        if (!fix.valid) {
            last = ESP_ERR_NOT_FOUND;
            continue;
        }

        char detail[SOLAR_OS_PLACES_DETAIL_MAX];
        snprintf(detail,
                 sizeof(detail),
                 "%s fix, %u sats",
                 info.name,
                 fix.satellites_valid ? (unsigned)fix.satellites : 0U);
        uint64_t epoch_ms = 0U;
        if (solar_os_time_get_utc_epoch_ms(&epoch_ms) != ESP_OK) {
            epoch_ms = 0U;
        }
        const solar_os_places_publish_t point = {
            .source = "gnss",
            .key = "self",
            .label = "me",
            .detail = detail,
            .kind = SOLAR_OS_PLACES_KIND_SELF,
            .latitude_e7 = fix.latitude_deg_e7,
            .longitude_e7 = fix.longitude_deg_e7,
            .timestamp_ms = epoch_ms,
        };
        return solar_os_places_publish(&point, NULL);
    }
    return last;
}
