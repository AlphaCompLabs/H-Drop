/**
 * @file main.cpp
 * @brief Ponto de entrada do firmware H-DROP — Etapa 1: verificação do componente motor.
 * @details Inicializa o componente hdrop_motor, executa sequência de arming e realiza
 *          testes de mistura cinemática ICR e parada de emergência para validar os
 *          critérios de aceitação da Etapa 1. Em operação normal, a task principal
 *          alimenta o watchdog periodicamente enviando velocidade zero.
 *
 * @depends hdrop_motor
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hdrop_motor.h"

/** Tag de log do módulo principal. */
static const char *TAG = "MAIN";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== H-DROP Firmware — Etapa 1 ===");

    /* ----------------------------------------------------------------
     * Inicialização do componente motor: configura LEDC, executa arming
     * de 4 s e inicia o watchdog de 2 s. Nenhuma task de movimento deve
     * ser criada antes que esta função retorne ESP_OK.
     * ---------------------------------------------------------------- */
    esp_err_t ret = motor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicialização dos motores: %s", esp_err_to_name(ret));
        return;
    }

    /* ----------------------------------------------------------------
     * Critério 1: motor_mix(0.5f, 0.0f, 0.35f)
     * Esperado: vL = vR = 0.5 → PWM = 1520 + round(0.5 × 480) = 1760 µs
     * Ambos os ESCs devem estar acima de 1520 µs.
     * ---------------------------------------------------------------- */
    ESP_LOGI(TAG, "Teste 1: avanço linear — motor_mix(0.5, 0.0, 0.35)");
    motor_mix(0.5f, 0.0f, 0.35f);
    ESP_LOGI(TAG, "  PWM esquerdo: %d us | PWM direito: %d us",
             motor_get_last_pwm_left(), motor_get_last_pwm_right());

    vTaskDelay(pdMS_TO_TICKS(1000));

    /* ----------------------------------------------------------------
     * Critério 2: motor_mix(0.0f, 1.0f, 0.35f)
     * Esperado: vL = -0.175, vR = +0.175 → PWM_esq < 1520, PWM_dir > 1520
     * Rotação no lugar: ESC direito avança, ESC esquerdo recua.
     * ---------------------------------------------------------------- */
    ESP_LOGI(TAG, "Teste 2: rotação no lugar — motor_mix(0.0, 1.0, 0.35)");
    motor_mix(0.0f, 1.0f, 0.35f);
    ESP_LOGI(TAG, "  PWM esquerdo: %d us | PWM direito: %d us",
             motor_get_last_pwm_left(), motor_get_last_pwm_right());

    vTaskDelay(pdMS_TO_TICKS(1000));

    /* ----------------------------------------------------------------
     * Critério 3: motor_stop() envia 1520 µs em menos de 5 ms.
     * ---------------------------------------------------------------- */
    ESP_LOGI(TAG, "Teste 3: parada de emergência — motor_stop()");
    motor_stop();
    ESP_LOGI(TAG, "  PWM esquerdo: %d us | PWM direito: %d us",
             motor_get_last_pwm_left(), motor_get_last_pwm_right());

    /* ----------------------------------------------------------------
     * Loop de operação: alimenta o watchdog a cada 1 s com velocidade zero.
     * Sem este loop, o watchdog dispararia após 2 s e já chamaria motor_stop()
     * novamente (o que seria inofensivo, mas geraria log desnecessário).
     * ---------------------------------------------------------------- */
    ESP_LOGI(TAG, "Entrando em loop de manutenção (watchdog feed a cada 1 s).");
    while (true) {
        motor_set_speeds(0.0f, 0.0f);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
