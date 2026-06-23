/**
 * @file main.cpp
 * @brief Teste standalone INA226 — varredura I2C + leitura de ID e tensão.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "TESTE_INA226";

#define I2C_PORT    I2C_NUM_0
#define PIN_SDA     21
#define PIN_SCL     22
#define I2C_FREQ    100000

/* INA226 registers */
#define REG_CONFIG   0x00
#define REG_BUS_V    0x02
#define REG_MFR_ID   0xFE   /* deve retornar 0x5449 ("TI") */
#define REG_DIE_ID   0xFF   /* deve retornar 0x2260 */

static void i2c_init(void)
{
    i2c_config_t cfg = {};
    cfg.mode             = I2C_MODE_MASTER;
    cfg.sda_io_num       = PIN_SDA;
    cfg.scl_io_num       = PIN_SCL;
    cfg.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = I2C_FREQ;
    i2c_param_config(I2C_PORT, &cfg);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "════ SCAN I2C (0x08–0x77) ════");
    int n = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "  Encontrado: 0x%02X", addr);
            n++;
        }
    }
    ESP_LOGI(TAG, "Total: %d dispositivo(s)", n);
}

/* Lê registrador de 16 bits de qualquer endereço I2C */
static esp_err_t read_reg16(uint8_t addr, uint8_t reg, uint16_t *out)
{
    uint8_t buf[2];
    esp_err_t r = i2c_master_write_read_device(
        I2C_PORT, addr, &reg, 1, buf, 2, pdMS_TO_TICKS(100));
    if (r == ESP_OK)
        *out = ((uint16_t)buf[0] << 8) | buf[1];
    return r;
}

static void testar_ina226(uint8_t addr)
{
    ESP_LOGI(TAG, "── Testando 0x%02X ──────────────────────", addr);

    uint16_t mfr = 0, die = 0, cfg = 0, vbus = 0;

    if (read_reg16(addr, REG_MFR_ID, &mfr) == ESP_OK)
        ESP_LOGI(TAG, "  Mfr ID : 0x%04X %s", mfr,
                 mfr == 0x5449 ? "(TI — correto!)" : "(inesperado)");
    else
        ESP_LOGE(TAG, "  Mfr ID : FALHA DE LEITURA");

    if (read_reg16(addr, REG_DIE_ID, &die) == ESP_OK)
        ESP_LOGI(TAG, "  Die ID : 0x%04X %s", die,
                 die == 0x2260 ? "(INA226 confirmado!)" : "(inesperado)");
    else
        ESP_LOGE(TAG, "  Die ID : FALHA DE LEITURA");

    if (read_reg16(addr, REG_CONFIG, &cfg) == ESP_OK)
        ESP_LOGI(TAG, "  Config : 0x%04X", cfg);

    /* Tensão de barramento: LSB = 1.25 mV */
    if (read_reg16(addr, REG_BUS_V, &vbus) == ESP_OK) {
        int32_t mv = (int32_t)vbus * 5 / 4;
        ESP_LOGI(TAG, "  Tensão : %ldmV (%.2fV)", mv, mv / 1000.0f);
    } else {
        ESP_LOGE(TAG, "  Tensão : FALHA DE LEITURA");
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   TESTE INA226                       ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");

    i2c_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    int ciclo = 0;
    while (true) {
        ciclo++;
        ESP_LOGI(TAG, "\n══ CICLO %d ══════════════════════════════", ciclo);

        i2c_scan();

        /* Testa os 4 endereços possíveis do INA226 (A0/A1 em GND, VS, SDA, SCL) */
        uint8_t candidatos[] = { 0x40, 0x41, 0x44, 0x45 };
        for (int i = 0; i < 4; i++)
            testar_ina226(candidatos[i]);

        ESP_LOGI(TAG, "══ FIM CICLO %d — aguardando 5 s ══════════", ciclo);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
