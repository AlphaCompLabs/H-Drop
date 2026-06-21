/**
 * @file main.cpp
 * @brief Ponto de entrada do firmware H-DROP — Etapa 3 (modo bancada).
 * @details Inicializa motor, heading e pose. Em bancada, executa validação
 *          das funções de parsing GNSS com strings sintéticas calculadas
 *          (ver simulations.md). A pose_task roda em background tentando
 *          conectar ao A7670SA — inofensivo sem o modem, apenas logará
 *          timeouts AT até que o hardware seja conectado.
 *
 * @depends hdrop_motor, hdrop_heading, hdrop_pose
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hdrop_motor.h"
#include "hdrop_heading.h"
#include "hdrop_pose.h"
#include <cmath>

/** Tag de log do módulo principal. */
static const char *TAG = "MAIN";

/* ================================================================
 * [SIMULAÇÃO] Strings GNSS sintéticas para teste de bancada.
 * Calculadas para referência (15°47'S, 47°20'W) — Brasília/DF.
 * Ver simulations.md — Simulação 3 para reverter em campo.
 * ================================================================ */

/** Fix de referência (mode=2, lat=15°47'S, lon=47°20'W). */
static const char *GNSS_FIX_REF =
    "2,09,05,00,1547.000000,S,04720.000000,W,200626,120000.0,1050.0,0.0,0.0,1.2,0.9,0.8";

/** Fix 100 m ao norte da referência (mode=3). y esperado = 100.0000 m. */
static const char *GNSS_FIX_100M_N =
    "3,09,05,00,1546.946041,S,04720.000000,W,200626,120001.0,1050.0,0.0,0.0,1.2,0.9,0.8";

/** Fix 100 m ao leste da referência (mode=3). x esperado = 100.0000 m. */
static const char *GNSS_FIX_100M_E =
    "3,09,05,00,1547.000000,S,04719.943927,W,200626,120002.0,1050.0,0.0,0.0,1.2,0.9,0.8";

/**
 * @brief Executa teste de bancada dos critérios de aceitação da Etapa 3.
 * @details Valida sem A7670SA:
 *          1. pose_update_gnss() retorna true para mode="2" e mode="3"
 *          2. Distância euclidiana entre dois fixes conhecidos (erro < 5 m)
 *          3. String sem fix retorna false
 */
static void executar_teste_bancada_pose(void)
{
    ESP_LOGI(TAG, "--- [BANCADA] Início dos testes de parsing GNSS ---");

    /* ── Critério 1a: mode="2" retorna true; define ponto de referência ── */
    bool ok_ref = pose_update_gnss(GNSS_FIX_REF);
    hdrop_pose_t p_ref = pose_get();
    ESP_LOGI(TAG, "[T1] Fix referência (mode=2): retornou=%s | x=%.4f m y=%.4f m",
             ok_ref ? "true ✓" : "false ✗", p_ref.x, p_ref.y);

    /* ── Critério 1b: mode="3" retorna true; y ≈ 100 m ── */
    bool ok_n = pose_update_gnss(GNSS_FIX_100M_N);
    hdrop_pose_t p_n = pose_get();
    float dist_n = sqrtf(p_n.x * p_n.x + p_n.y * p_n.y);
    ESP_LOGI(TAG, "[T2] Fix 100m Norte (mode=3): retornou=%s | x=%.4f m y=%.4f m",
             ok_n ? "true ✓" : "false ✗", p_n.x, p_n.y);
    ESP_LOGI(TAG, "     Distância euclidiana: %.4f m | erro: %.4f m | %s",
             dist_n, fabsf(dist_n - 100.0f),
             fabsf(dist_n - 100.0f) < 5.0f ? "PASSOU ✓" : "FALHOU ✗");

    /* ── Critério 1c: fix leste — verifica conversão do eixo X ──
       Aplica a partir da referência, comparando x vs referência (não vs fix N) */
    pose_update_gnss(GNSS_FIX_REF);  /* reseta para referência */
    bool ok_e = pose_update_gnss(GNSS_FIX_100M_E);
    hdrop_pose_t p_e = pose_get();
    float dist_e = sqrtf(p_e.x * p_e.x + p_e.y * p_e.y);
    ESP_LOGI(TAG, "[T3] Fix 100m Leste (mode=3): retornou=%s | x=%.4f m y=%.4f m",
             ok_e ? "true ✓" : "false ✗", p_e.x, p_e.y);
    ESP_LOGI(TAG, "     Distância euclidiana: %.4f m | erro: %.4f m | %s",
             dist_e, fabsf(dist_e - 100.0f),
             fabsf(dist_e - 100.0f) < 5.0f ? "PASSOU ✓" : "FALHOU ✗");

    /* ── Critério negativo: string sem fix deve retornar false ── */
    bool ok_nofix = pose_update_gnss(",,,,,,,,,,,,,,,");
    ESP_LOGI(TAG, "[T4] Sem fix (mode vazio): retornou=%s | esperado: false %s",
             ok_nofix ? "true" : "false", !ok_nofix ? "✓" : "✗");

    ESP_LOGI(TAG, "--- [BANCADA] Fim dos testes de parsing GNSS ---");
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== H-DROP Firmware — Etapa 3 (bancada) ===");

    /* ----------------------------------------------------------------
     * 1. Motores — arming de 4 s.
     * ---------------------------------------------------------------- */
    esp_err_t ret = motor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicialização dos motores: %s", esp_err_to_name(ret));
        return;
    }

    /* ----------------------------------------------------------------
     * 2. Heading — I2C, QMC5883L e heading_task 10 Hz.
     * ---------------------------------------------------------------- */
    ret = heading_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicialização do heading: %s", esp_err_to_name(ret));
        return;
    }

    /* ----------------------------------------------------------------
     * 3. Pose — UART2 e pose_task com FSM MQTT.
     *    Em bancada: UART2 inicializa; pose_task fará loop de timeout AT
     *    até o A7670SA ser conectado. O teste abaixo não depende da task.
     * ---------------------------------------------------------------- */
    ret = pose_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicialização da pose: %s", esp_err_to_name(ret));
        return;
    }

    /* ----------------------------------------------------------------
     * [SIMULAÇÃO] Teste de bancada — parsing GNSS com strings sintéticas.
     * Valida critérios de aceitação 1, 2 e 3 da Etapa 3 sem modem.
     * Ver simulations.md — Simulação 3 para reverter em campo.
     * ---------------------------------------------------------------- */
    executar_teste_bancada_pose();

    /* ----------------------------------------------------------------
     * [SIMULAÇÃO] Calibração e heading_hold desabilitados para bancada.
     * Ver simulations.md — Simulações 1 e 2 para reverter em campo.
     *
     * heading_calibrate(100);
     * heading_hold(0.0f);
     * ---------------------------------------------------------------- */

    /* ----------------------------------------------------------------
     * Loop de monitoramento: heading a cada 500 ms, pose a cada 2 s.
     * ---------------------------------------------------------------- */
    ESP_LOGI(TAG, "Entrando em loop de monitoramento (bancada).");

    float    theta_ant = 0.0f;
    bool     primeira  = true;
    uint32_t ciclo     = 0;

    while (true) {
        motor_set_speeds(0.0f, 0.0f);  /* alimenta watchdog sem acionar ESCs */

        float theta = 0.0f;
        if (heading_read(&theta)) {
            float variacao = primeira ? 0.0f : fabsf(theta - theta_ant);
            theta_ant = theta;
            primeira  = false;
            ESP_LOGI(TAG, "Heading: %.3f rad (%.1f°) | var: %.4f rad",
                     theta, theta * 180.0f / 3.14159f, variacao);
        } else {
            ESP_LOGE(TAG, "Falha I2C — verificar cabeamento SDA/SCL.");
        }

        /* Loga pose a cada ~2 s (4 ciclos de 500 ms) */
        if (++ciclo % 4 == 0) {
            hdrop_pose_t p = pose_get();
            ESP_LOGI(TAG, "Pose: x=%.2f m y=%.2f m theta=%.3f rad fix=%d",
                     p.x, p.y, p.theta, p.gnss_valid ? 1 : 0);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
