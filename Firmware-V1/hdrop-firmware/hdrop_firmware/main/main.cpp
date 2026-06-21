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
#include <cmath>

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
     * CALIBRAÇÃO — desabilitada para teste de bancada.
     * Requer rotação completa do veículo (≈ 2 s) para produzir offsets
     * válidos. Sem movimento, x_min ≈ x_max e y_min ≈ y_max, gerando
     * offsets errados que prejudicariam as leituras em campo.
     * Habilitar em ambiente de operação real antes do primeiro deploy.
     *
     * heading_calibrate(100);
     * ---------------------------------------------------------------- */

    /* ----------------------------------------------------------------
     * CONTROLE DE HEADING — desabilitado para teste de bancada.
     * Sem calibração e sem água, heading_hold() acionaria os motores com
     * setpoint inválido. Habilitar apenas após calibração em campo.
     *
     * heading_hold(0.0f);
     * ---------------------------------------------------------------- */

    /* ----------------------------------------------------------------
     * TESTE DE BANCADA: verifica comunicação I2C e estabilidade de leitura.
     * Critério de aceitação: heading estável com variação < 0.05 rad em repouso.
     * O watchdog do motor é alimentado com velocidade zero — motores ficam
     * parados mas o timer não dispara.
     * ---------------------------------------------------------------- */
    ESP_LOGI(TAG, "Modo bancada: lendo heading a cada 500 ms. Motores parados.");

    float theta_ant = 0.0f;
    bool  primeira  = true;

    while (true) {
        motor_set_speeds(0.0f, 0.0f);  /* alimenta o watchdog sem mover os ESCs */

        float theta = 0.0f;
        if (heading_read(&theta)) {
            float variacao = primeira ? 0.0f : fabsf(theta - theta_ant);
            ESP_LOGI(TAG, "Heading: %.3f rad (%.1f°) | var: %.4f rad",
                     theta, theta * 180.0f / 3.14159f, variacao);
            theta_ant = theta;
            primeira  = false;
        } else {
            ESP_LOGE(TAG, "Falha I2C — verificar cabeamento SDA/SCL.");
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
