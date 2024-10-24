#include <string.h>

/* IDF */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/list.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* LLCC68 */
#include "llcc68_hal_init.h"

/* App */
#include "app/lora_server.h"

#define APP_LORA_SERVER_NVS_NAMESPACE "a_lora_server"
#define APP_LORA_SERVER_NVS_VERSION   1 /* DO NOT CHANGE THIS VALUE UNLESS THERE IS A STRUCTURE UPDATE */

#define APP_LORA_SERVER_SPI_HOST SPI2_HOST
#define APP_LORA_SERVER_SPI_FREQ (4 * 1000 * 1000)
#define APP_LORA_SERVER_PIN_SCK  12
#define APP_LORA_SERVER_PIN_MOSI 11
#define APP_LORA_SERVER_PIN_MISO 13
#define APP_LORA_SERVER_PIN_CS   10
#define APP_LORA_SERVER_PIN_RST  21
#define APP_LORA_SERVER_PIN_INT  9
#define APP_LORA_SERVER_PIN_BUSY 14

#define APP_LORA_SERVER_CMD_Q_LEN (16)

#define APP_LORA_SERVER_FREQUENCY_MIN     (868 * 1000 * 1000)     /* TODO: Use Kconfig */
#define APP_LORA_SERVER_FREQUENCY_MAX     (915 * 1000 * 1000 - 1) /* TODO: Use Kconfig */
#define APP_LORA_SERVER_FREQUENCY_DEFAULT (868400000UL)           /* 868.400 MHz */

#define APP_LORA_SERVER_POWER_DEFAULT (7) /* 7dBm */

typedef enum {
    APP_LORA_SERVER_CMD_TRANSMIT,
    APP_LORA_SERVER_CMD_UPDATE_PARAMS,
} app_lora_server_cmd_t;

typedef struct {
    app_lora_server_cmd_t cmd;
    void                 *data;
    size_t                data_len;
} app_lora_server_cmd_queue_item_t;

typedef struct {
    llcc68_hal_context_t hal_context;
    SemaphoreHandle_t    config_mutex;
    QueueHandle_t        cmd_queue;
} app_lora_server_state_t;

static const char *LOG_TAG = "asuna_lora";

static void                app_lora_server_gpio_init(void);
static int                 app_lora_server_spi_init(void);
static llcc68_hal_status_t app_llcc68_hal_spi_ops(void *handle, llcc68_hal_spi_transfer_t *xfer);
static llcc68_hal_status_t app_llcc68_hal_pin_ops(void *handle, llcc68_hal_pin_t pin, bool value);
static llcc68_hal_status_t app_llcc68_hal_wait_busy(void *handle);
static llcc68_hal_status_t app_llcc68_hal_delay(void *handle, uint32_t msec);
static void                app_lora_server_task(void *argument);

static app_lora_server_state_t s_lora_server_state = {
    .hal_context =
        {
            .handle = NULL,

            .spi_ops   = app_llcc68_hal_spi_ops,
            .pin_ops   = app_llcc68_hal_pin_ops,
            .wait_busy = app_llcc68_hal_wait_busy,
            .delay     = app_llcc68_hal_delay,
        },
    .config_mutex = NULL,
};

static const char *APP_LORA_SERVER_CFG_KEY_FLAG    = "cfg_valid"; /* Configuration key */
static const char *APP_LORA_SERVER_CFG_KEY_FREQ    = "freq";      /* Frequency */
static const char *APP_LORA_SERVER_CFG_KEY_POWER   = "power";     /* Transmit Power */
static const char *APP_LORA_SERVER_CFG_KEY_TYPE    = "type";      /* Network Type */
static const char *APP_LORA_SERVER_CFG_KEY_BW      = "bw";        /* Bandwidth */
static const char *APP_LORA_SERVER_CFG_KEY_SF      = "sf";        /* Spreading Factor */
static const char *APP_LORA_SERVER_CFG_KEY_CR      = "cr";        /* Coding Rate */
static const char *APP_LORA_SERVER_CFG_KEY_LDR_OPT = "ldr_opt";   /* Low Data-Rate Optimization */

int app_lora_server_init(void) {
    int ret = 0;

    ESP_LOGI(LOG_TAG, "Initializing LoRa server...");

    app_lora_server_gpio_init();

    if (app_lora_server_spi_init() != 0) {
        ESP_LOGE(LOG_TAG, "Failed to initialize SPI interface.");
        return -2;
    }

    s_lora_server_state.config_mutex = xSemaphoreCreateMutex();
    if (s_lora_server_state.config_mutex == NULL) {
        ESP_LOGE(LOG_TAG, "Failed to create mutex.");
        return -3;
    }

    s_lora_server_state.cmd_queue = xQueueCreate(APP_LORA_SERVER_CMD_Q_LEN, sizeof(app_lora_server_cmd_queue_item_t));
    if (s_lora_server_state.cmd_queue == NULL) {
        ESP_LOGE(LOG_TAG, "Failed to create command queue.");

        ret = -4;
        goto del_mutex_exit;
    }

    lora_modem_config_t modem_cfg;
    if (app_lora_server_config_get(&modem_cfg) != 0) {
        ESP_LOGW(LOG_TAG, "Configuration invalid, restore to default...");
        app_lora_server_config_init(&modem_cfg);

        if (app_lora_server_config_set(&modem_cfg) != 0) {
            ESP_LOGE(LOG_TAG, "Configuration validation failed...");
            goto del_queue_exit;
        }
    }

    if (xTaskCreate(app_lora_server_task, "asuna_lora", 4096, &s_lora_server_state, 3, NULL) != pdPASS) {
        ESP_LOGE(LOG_TAG, "Task creation failed...");
        goto del_queue_exit;
    }

    ESP_LOGI(LOG_TAG, "Initialization completed.");

    return 0;

del_queue_exit:
    vQueueDelete(s_lora_server_state.cmd_queue);

del_mutex_exit:
    vSemaphoreDelete(s_lora_server_state.config_mutex);

    return ret;
}

void app_lora_server_config_init(lora_modem_config_t *config) {
    config->frequency        = APP_LORA_SERVER_FREQUENCY_DEFAULT;
    config->power            = APP_LORA_SERVER_POWER_DEFAULT;
    config->network_type     = LORA_MODEM_NETWORK_PRIVATE;
    config->bandwidth        = LORA_MODEM_BW_125;
    config->coding_rate      = LORA_MODEM_CR_1;
    config->spreading_factor = LORA_MODEM_SF_11;
    config->ldr_optimization = false;
}

int app_lora_server_config_set(const lora_modem_config_t *config) {
    /* ---- Sanity checks ---- */
    if (config->bandwidth >= LORA_MODEM_BW_INVALID) return -1;
    if (config->coding_rate >= LORA_MODEM_CR_INVALID) return -1;
    if (config->spreading_factor >= LORA_MODEM_SF_INVALID) return -1;
    if (config->frequency > APP_LORA_SERVER_FREQUENCY_MAX) return -1;
    if (config->frequency < APP_LORA_SERVER_FREQUENCY_MIN) return -1;

    if (xSemaphoreTake(s_lora_server_state.config_mutex, portMAX_DELAY) != pdPASS) {
        return -2;
    }

    /* ---- Create NVS handle ---- */
    nvs_handle handle;
    ESP_ERROR_CHECK(nvs_open(APP_LORA_SERVER_NVS_NAMESPACE, NVS_READWRITE, &handle));

    /* ---- Store configuration ---- */
    ESP_ERROR_CHECK(nvs_set_u8(handle, APP_LORA_SERVER_CFG_KEY_FLAG, APP_LORA_SERVER_NVS_VERSION));

    ESP_ERROR_CHECK(nvs_set_u32(handle, APP_LORA_SERVER_CFG_KEY_FREQ, config->frequency));
    ESP_ERROR_CHECK(nvs_set_u8(handle, APP_LORA_SERVER_CFG_KEY_POWER, config->power));
    ESP_ERROR_CHECK(nvs_set_u8(handle, APP_LORA_SERVER_CFG_KEY_TYPE, config->network_type));
    ESP_ERROR_CHECK(nvs_set_u8(handle, APP_LORA_SERVER_CFG_KEY_BW, config->bandwidth));
    ESP_ERROR_CHECK(nvs_set_u8(handle, APP_LORA_SERVER_CFG_KEY_CR, config->coding_rate));
    ESP_ERROR_CHECK(nvs_set_u8(handle, APP_LORA_SERVER_CFG_KEY_SF, config->spreading_factor));
    ESP_ERROR_CHECK(nvs_set_u8(handle, APP_LORA_SERVER_CFG_KEY_LDR_OPT, config->ldr_optimization));

    ESP_ERROR_CHECK(nvs_commit(handle));

    nvs_close(handle);

    xSemaphoreGive(s_lora_server_state.config_mutex);

    const app_lora_server_cmd_queue_item_t cmd = {
        .cmd      = APP_LORA_SERVER_CMD_UPDATE_PARAMS,
        .data     = NULL,
        .data_len = 0,
    };

    if (xQueueSend(s_lora_server_state.cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(LOG_TAG, "Failed to issue configuration update command.");
        return -1;
    }

    return 0;
}

int app_lora_server_config_get(lora_modem_config_t *config) {
    int       ret = 0;
    esp_err_t err;

    if (xSemaphoreTake(s_lora_server_state.config_mutex, portMAX_DELAY) != pdPASS) {
        return -1;
    }

    /* ---- Create NVS handle ---- */
    nvs_handle handle;
    err = nvs_open(APP_LORA_SERVER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ret = -1;
        goto release_lock_exit;
    }

    /* Check NVS data flag */

    uint8_t cfg_flag;
    if (nvs_get_u8(handle, APP_LORA_SERVER_CFG_KEY_FLAG, &cfg_flag) != ESP_OK) {
        ret = -1;
        goto release_lock_exit;
    }

    /* ?? Downgrade is not allowed ?? */
    if (cfg_flag > APP_LORA_SERVER_NVS_VERSION) {
        ret = -2;
        goto release_lock_exit;
    }

    /* TODO: Handle structure update. */

    /* ---- Load configuration: frequency ---- */
    ESP_ERROR_CHECK(nvs_get_u32(handle, APP_LORA_SERVER_CFG_KEY_FREQ, &config->frequency));

    /* ---- Load configuration: power ---- */
    ESP_ERROR_CHECK(nvs_get_u8(handle, APP_LORA_SERVER_CFG_KEY_POWER, &config->power));

    /* ---- Load configuration: network_type ---- */
    uint8_t network_type;
    ESP_ERROR_CHECK(nvs_get_u8(handle, APP_LORA_SERVER_CFG_KEY_TYPE, &network_type));
    config->network_type = network_type;

    /* ---- Load configuration: bandwidth ---- */
    uint8_t bandwidth;
    ESP_ERROR_CHECK(nvs_get_u8(handle, APP_LORA_SERVER_CFG_KEY_BW, &bandwidth));
    config->bandwidth = bandwidth;

    /* ---- Load configuration: coding_rate ---- */
    uint8_t coding_rate;
    ESP_ERROR_CHECK(nvs_get_u8(handle, APP_LORA_SERVER_CFG_KEY_CR, &coding_rate));
    config->coding_rate = coding_rate;

    /* ---- Load configuration: spreading_factor ---- */
    uint8_t spreading_factor;
    ESP_ERROR_CHECK(nvs_get_u8(handle, APP_LORA_SERVER_CFG_KEY_SF, &spreading_factor));
    config->spreading_factor = spreading_factor;

    /* ---- Load configuration: ldr_optimization ---- */
    uint8_t ldr_optimization;
    ESP_ERROR_CHECK(nvs_get_u8(handle, APP_LORA_SERVER_CFG_KEY_LDR_OPT, &ldr_optimization));
    config->ldr_optimization = ldr_optimization;

    /* ---- Close NVS handle ---- */
    nvs_close(handle);

release_lock_exit:
    xSemaphoreGive(s_lora_server_state.config_mutex);

    return ret;
}

int app_lora_server_broadcast(const uint8_t *data, size_t length) {
    void *payload = malloc(length);
    if (payload == NULL) {
        ESP_LOGE(LOG_TAG, "Failed to allocate packet buffer");

        return -1;
    }

    memcpy(payload, data, length);

    app_lora_server_cmd_queue_item_t cmd = {
        .cmd      = APP_LORA_SERVER_CMD_TRANSMIT,
        .data     = payload,
        .data_len = length,
    };

    if (xQueueSend(s_lora_server_state.cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(LOG_TAG, "Failed to enqueue packet.");
        goto free_buf_exit;
    }

    return 0;

free_buf_exit:
    free(payload);

    return -1;
}

static void app_lora_server_gpio_init(void) {
    gpio_config_t pin_cfg = {
        .pin_bit_mask = BIT64(APP_LORA_SERVER_PIN_CS) | BIT64(APP_LORA_SERVER_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };

    gpio_config(&pin_cfg);

    pin_cfg.pin_bit_mask = BIT64(APP_LORA_SERVER_PIN_BUSY);
    pin_cfg.mode         = GPIO_MODE_INPUT;
    pin_cfg.pull_up_en   = GPIO_PULLUP_ENABLE;

    gpio_config(&pin_cfg);

    gpio_set_level(APP_LORA_SERVER_PIN_CS, 1U);
    gpio_set_level(APP_LORA_SERVER_PIN_RST, 1U);
}

static int app_lora_server_spi_init(void) {
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num     = APP_LORA_SERVER_PIN_MOSI,
        .miso_io_num     = APP_LORA_SERVER_PIN_MISO,
        .sclk_io_num     = APP_LORA_SERVER_PIN_SCK,
        .quadhd_io_num   = -1,
        .quadwp_io_num   = -1,
        .max_transfer_sz = 256,
    };

    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = APP_LORA_SERVER_SPI_FREQ,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 2,
    };

    if (spi_bus_initialize(APP_LORA_SERVER_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        return -1;
    }

    spi_device_handle_t spi_device;

    if (spi_bus_add_device(APP_LORA_SERVER_SPI_HOST, &dev_cfg, &spi_device) != ESP_OK) {
        return -2;
    }

    s_lora_server_state.hal_context.handle = spi_device;

    return 0;
}

static llcc68_hal_status_t app_llcc68_hal_spi_ops(void *handle, llcc68_hal_spi_transfer_t *xfer) {
    spi_device_handle_t spi_device = handle;

    spi_transaction_t txn = {
        .tx_buffer = xfer->tx_data,
        .rx_buffer = xfer->rx_data,
        .length    = xfer->length * 8,
    };

    if (spi_device_polling_transmit(spi_device, &txn) != ESP_OK) {
        ESP_LOGE(LOG_TAG, "SPI transaction failed, len: %d", txn.length);

        return LLCC68_HAL_STATUS_ERROR;
    }

    return LLCC68_HAL_STATUS_OK;
}

static llcc68_hal_status_t app_llcc68_hal_pin_ops(void *handle, llcc68_hal_pin_t pin, bool value) {
    gpio_num_t pin_num;

    switch (pin) {
        case LLCC68_HAL_PIN_CS:
            pin_num = APP_LORA_SERVER_PIN_CS;
            break;
        case LLCC68_HAL_PIN_RESET:
            pin_num = APP_LORA_SERVER_PIN_RST;
            break;

        default:
            return LLCC68_HAL_STATUS_ERROR;
    }

    gpio_set_level(pin_num, value);

    return LLCC68_HAL_STATUS_OK;
}

static llcc68_hal_status_t app_llcc68_hal_wait_busy(void *handle) {
    while (true) {
        const int busy = gpio_get_level(APP_LORA_SERVER_PIN_BUSY);
        if (!busy) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return LLCC68_HAL_STATUS_OK;
}

static llcc68_hal_status_t app_llcc68_hal_delay(void *handle, uint32_t msec) {
    vTaskDelay(pdMS_TO_TICKS(msec));
    return LLCC68_HAL_STATUS_OK;
}

static void app_lora_server_task(void *argument) {
    void *context = &((app_lora_server_state_t *)argument)->hal_context;

    bool                             is_initialized = false;
    app_lora_server_cmd_queue_item_t cmd;
    for (;;) {
        if (!is_initialized) {
            int ret = lora_modem_init(context);
            if (ret != 0) {
                ESP_LOGE(LOG_TAG, "Failed to initialize LoRa modem: %d", ret);

                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            /* Initial synchronize LoRa modem configuration */
            cmd.cmd = APP_LORA_SERVER_CMD_UPDATE_PARAMS;
            xQueueSend(s_lora_server_state.cmd_queue, &cmd, portMAX_DELAY);

            is_initialized = true;
            ESP_LOGI(LOG_TAG, "LoRa modem initialized.");
        }

        if (xQueueReceive(s_lora_server_state.cmd_queue, &cmd, portMAX_DELAY) != pdPASS) {
            ESP_LOGW(LOG_TAG, "Failed to receive from queue.");
            continue;
        }

        switch (cmd.cmd) {
            case APP_LORA_SERVER_CMD_UPDATE_PARAMS: {
                lora_modem_config_t cfg;
                if (app_lora_server_config_get(&cfg) != 0) {
                    ESP_LOGW(LOG_TAG, "Failed to get LoRa modem configuration.");
                    continue;
                }

                if (lora_modem_set_config(context, &cfg) != 0) {
                    ESP_LOGW(LOG_TAG, "Failed to set LoRa modem configuration.");
                    continue;
                }

                ESP_LOGI(LOG_TAG, "LoRa modem configuration updated.");
                break;
            }

            case APP_LORA_SERVER_CMD_TRANSMIT: {
                const int ret = lora_modem_transmit(context, cmd.data, cmd.data_len);

                free(cmd.data);
                if (ret != 0) {
                    ESP_LOGW(LOG_TAG, "Failed to transmit data.");
                }
                break;
            }

            default:
                break;
        }
    }
}