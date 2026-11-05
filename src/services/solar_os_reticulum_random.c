#include "solar_os_reticulum_random.h"

#include <stdbool.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/ctr_drbg.h"
#include "solar_os_log.h"
#include "solar_os_wifi.h"

#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif
#if SOLAR_OS_BOARD_HAS_ADC
#include "adc_port.h"
#endif

static const char *TAG = "reticulum";

/* Mixed into every draw; the seed is what makes the output unpredictable. */
#define FRESH_INPUT_BYTES 16U

static mbedtls_ctr_drbg_context drbg;
static bool seeded;
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static portMUX_TYPE lock_init = portMUX_INITIALIZER_UNLOCKED;
/* How the last seed was gathered, for the log. */
static bool borrowed_adc;

static bool radio_running(void)
{
    bool running = solar_os_wifi_radio_started();
#if CONFIG_BT_ENABLED
    running = running ||
              esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
#endif
    return running;
}

/* mbedtls entropy callback: true random bytes, from the radio if it is
 * running, or else from the SAR ADC entropy source. That source must not run
 * alongside the radio or an ADC read; a radio started during these few
 * microseconds is the one case left uncovered. */
static int gather_entropy(void *context, unsigned char *out, size_t len)
{
    (void)context;
    if (radio_running()) {
        borrowed_adc = false;
        esp_fill_random(out, len);
        return 0;
    }
#if SOLAR_OS_BOARD_HAS_ADC
    if (adc_port_lock() != ESP_OK) {
        return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    }
#endif
    bootloader_random_enable();
    esp_fill_random(out, len);
    bootloader_random_disable();
#if SOLAR_OS_BOARD_HAS_ADC
    adc_port_unlock();
#endif
    borrowed_adc = true;
    return 0;
}

static bool take_lock(void)
{
    if (lock == NULL) {
        taskENTER_CRITICAL(&lock_init);
        if (lock == NULL) {
            lock = xSemaphoreCreateMutexStatic(&lock_storage);
        }
        taskEXIT_CRITICAL(&lock_init);
    }
    return lock != NULL && xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE;
}

static esp_err_t seed_locked(void)
{
    static const unsigned char personalization[] = "SolarOS Reticulum";
    int ret;
    if (seeded) {
        ret = mbedtls_ctr_drbg_reseed(&drbg, NULL, 0);
    } else {
        mbedtls_ctr_drbg_init(&drbg);
        ret = mbedtls_ctr_drbg_seed(&drbg, gather_entropy, NULL,
                                    personalization,
                                    sizeof(personalization) - 1U);
    }
    if (ret != 0) {
        if (!seeded) {
            mbedtls_ctr_drbg_free(&drbg);
        }
        SOLAR_OS_LOGE(TAG, "random generator not seeded (%d); no keys can be made",
                      ret);
        return ESP_FAIL;
    }
    seeded = true;
    SOLAR_OS_LOGI(TAG, "random generator seeded from %s",
                  borrowed_adc ? "the ADC entropy source (Wi-Fi and Bluetooth off)"
                               : "the radio");
    return ESP_OK;
}

esp_err_t solar_os_reticulum_random_reseed(void)
{
    if (!take_lock()) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t error = seed_locked();
    xSemaphoreGive(lock);
    return error;
}

esp_err_t solar_os_reticulum_random(uint8_t *out, size_t len)
{
    if (out == NULL && len > 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = seeded ? ESP_OK : seed_locked();
    while (error == ESP_OK && len > 0U) {
        const size_t chunk = len < MBEDTLS_CTR_DRBG_MAX_REQUEST ?
                             len : MBEDTLS_CTR_DRBG_MAX_REQUEST;
        unsigned char fresh[FRESH_INPUT_BYTES];
        esp_fill_random(fresh, sizeof(fresh));
        if (mbedtls_ctr_drbg_random_with_add(&drbg, out, chunk, fresh,
                                             sizeof(fresh)) != 0) {
            error = ESP_FAIL;
        }
        memset(fresh, 0, sizeof(fresh));
        out += chunk;
        len -= chunk;
    }
    xSemaphoreGive(lock);
    return error;
}
