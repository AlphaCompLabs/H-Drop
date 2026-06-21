/**
 * @file main.cpp
 * @brief Ponto de entrada do firmware H-DROP — Etapa 2: motor + heading.
 * @details Inicializa os componentes hdrop_motor e hdrop_heading em sequência,
 *          executa uma calibração rápida (100 amostras) e ativa o controle de
 *          heading para norte magnético (0.0 rad). Em ambiente de bancada, a
 *          calibração pode ser pulada se os offsets NVS já estiverem gravados.
 *
 * @depends hdrop_motor, hdrop_heading
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hdrop_motor.h"
#include "hdrop_heading.h"

/** Tag de log do módulo principal. */
static const char *TAG = "MAIN";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== H-DROP Firmware — Etapa 2 ===");

    /* ----------------------------------------------------------------
     * Etapa 1: inicialização dos motores com arming de 4 s.
     * Deve preceder heading_init() pois heading_hold() chama motor_mix().
     * ---------------------------------------------------------------- */
    esp_err_t ret = motor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicialização dos motores: %s", esp_err_to_name(ret));
        return;
    }

    /* ----------------------------------------------------------------
     * Etapa 2: inicialização do heading (I2C, QMC5883L, NVS, task 10 Hz).
     * ---------------------------------------------------------------- */
    ret = heading_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicialização do heading: %s", esp_err_to_name(ret));
        return;
    }

    /* ----------------------------------------------------------------
     * Calibração hard-iron: 100 amostras ≈ 5 s de rotação manual.
     * Em produção, chamar apenas quando o ambiente magnético muda
     * (novo local, alteração da estrutura do veículo).
     * Comentar as próximas 5 linhas para usar calibração gravada em NVS.
     * ---------------------------------------------------------------- */
    ESP_LOGI(TAG, "Iniciando calibração — gire o veículo em círculo completo.");
    ret = heading_calibrate(100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Calibração falhou — usando offsets da NVS ou zeros.");
    }

    /* ----------------------------------------------------------------
     * Verificação de leitura individual antes de ativar o controle.
     * ---------------------------------------------------------------- */
    float theta = 0.0f;
    if (heading_read(&theta)) {
        ESP_LOGI(TAG, "Heading inicial: %.3f rad (%.1f°)", theta, theta * 180.0f / 3.14159f);
    } else {
        ESP_LOGW(TAG, "Leitura de heading falhou (verificar cabeamento I2C).");
    }

    /* ----------------------------------------------------------------
     * Ativa controle proporcional de heading para norte magnético (0.0 rad).
     * A heading_task aplica motor_mix() a 10 Hz até |eθ| < 0.1 rad.
     * ---------------------------------------------------------------- */
    ESP_LOGI(TAG, "Ativando controle de heading para norte magnético (0.0 rad).");
    heading_hold(0.0f);

    /* ----------------------------------------------------------------
     * Loop principal: alimenta o watchdog do motor e loga heading a 1 Hz.
     * O controle efetivo ocorre na heading_task — esta task apenas monitora.
     * ---------------------------------------------------------------- */
    while (true) {
        motor_set_speeds(0.0f, 0.0f); /* alimenta o watchdog (será sobrescrito pela heading_task) */

        if (heading_read(&theta)) {
            ESP_LOGI(TAG, "Heading: %.3f rad (%.1f°) | PWM: esq=%d us dir=%d us",
                     theta, theta * 180.0f / 3.14159f,
                     motor_get_last_pwm_left(), motor_get_last_pwm_right());
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
