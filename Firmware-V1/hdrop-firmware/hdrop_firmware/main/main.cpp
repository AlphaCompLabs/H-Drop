/**
 * @file main.cpp
 * @brief Firmware de campo H-DROP — sequência segura de boot e teste de manobra.
 *
 * Sequência completa:
 *   [BOOT]        WiFi AP + ESC arming + magnetômetro (calibração da NVS)
 *   [AGUARDANDO]  Motores em neutro — espera MQTT + fix GNSS
 *   [PRONTO]      Heading hold ativo → aponta para norte magnético
 *   [TESTE]       Validação de heading e coordenadas GPS
 *   [MANOBRA]     Softstart → avança 5 s a 50% → para
 *   [OPERANDO]    Heading hold retoma, monitoramento contínuo
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "hdrop_motor.h"
#include "hdrop_heading.h"
#include "hdrop_pose.h"
#include "hdrop_ota.h"
#include <cmath>

static const char *TAG = "MAIN";

/* ── Parâmetros da manobra ──────────────────────────────────────── */
/** Potência de avanço: 50% de (2000 - 1520 µs) = 240 µs acima do ponto morto. */
#define MANOBRA_POTENCIA      0.5f
/** Duração do avanço em segundos. */
#define MANOBRA_DURACAO_S     5
/** Duração do softstart em passos de 100 ms (20 × 100 ms = 2 s). */
#define SOFTSTART_PASSOS      20
/** Timeout máximo aguardando MQTT + GNSS (5 minutos). */
#define TIMEOUT_PRONTO_MS     (5 * 60 * 1000)

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WATCHDOG";
        case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SW:        return "SW_RESET";
        default:                return "OUTRO";
    }
}

/* Softstart: rampa de 0 até `v_alvo` em SOFTSTART_PASSOS × 100 ms. */
static void softstart(float v_alvo)
{
    ESP_LOGI(TAG, "[MANOBRA] Softstart → %.0f%% em %d s...",
             v_alvo * 100.0f, SOFTSTART_PASSOS / 10);
    for (int i = 1; i <= SOFTSTART_PASSOS; i++) {
        float v = v_alvo * ((float)i / (float)SOFTSTART_PASSOS);
        motor_set_speeds(v, v);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGW(TAG, ">>> Reset anterior: %s <<<", reset_reason_str(esp_reset_reason()));
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   H-DROP ASV — Firmware de campo     ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");

    /* ── [BOOT 0] WiFi OTA — sobe em background ───────────────────── */
    esp_err_t ret = ota_init();
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "OTA sem WiFi (%s).", esp_err_to_name(ret));

    /* ── [BOOT 1] Motores — arming 4 s, neutro garantido ─────────── */
    ret = motor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: motor_init falhou. Abortando.");
        return;
    }

    /* ── [BOOT 2] Magnetômetro — carrega calibração da NVS ─────────
     *    Calibração já está gravada. heading_calibrate() não é chamado.  */
    ret = heading_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: heading_init falhou. Abortando.");
        return;
    }

    /* ── [BOOT 3] Pose/Modem — FSM aguarda 12 s internamente ───────
     *    Deixa o A7670SA passar o pico de corrente de boot antes de AT. */
    ret = pose_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: pose_init falhou. Abortando.");
        return;
    }

    ESP_LOGI(TAG, "Boot completo. Motores em neutro. Aguardando MQTT + GNSS...");

    /* ════════════════════════════════════════════════════════════════
     * [AGUARDANDO] — motores em neutro, espera MQTT e fix GNSS
     * ════════════════════════════════════════════════════════════════ */
    uint32_t t_inicio = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool mqtt_ok  = false;
    bool gnss_ok  = false;

    while (!(mqtt_ok && gnss_ok)) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        mqtt_ok = pose_mqtt_connected();
        hdrop_pose_t p = pose_get();
        gnss_ok = p.gnss_valid;

        float theta = 0.0f;
        heading_read(&theta);

        ESP_LOGI(TAG, "[AGUARDANDO] MQTT:%s  GNSS:%s  heading=%.1f°  heap=%lu",
                 mqtt_ok ? "OK" : "...",
                 gnss_ok ? "OK" : "...",
                 theta * 180.0f / (float)M_PI,
                 (unsigned long)esp_get_free_heap_size());

        if (gnss_ok)
            ESP_LOGI(TAG, "[AGUARDANDO] Pos: x=%.2f m  y=%.2f m", p.x, p.y);

        /* Timeout de segurança */
        uint32_t decorrido = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - t_inicio;
        if (decorrido > TIMEOUT_PRONTO_MS) {
            ESP_LOGW(TAG, "Timeout aguardando prontidão (%s %s). Prosseguindo assim mesmo.",
                     mqtt_ok ? "MQTT:OK" : "MQTT:FAIL",
                     gnss_ok ? "GNSS:OK" : "GNSS:FAIL");
            break;
        }
    }

    /* ════════════════════════════════════════════════════════════════
     * [PRONTO] — ASV pronto para operação
     * ════════════════════════════════════════════════════════════════ */
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   ASV PRONTO PARA OPERAÇÃO           ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");

    /* Ativa heading hold — aponta para norte magnético */
    heading_hold(0.0f);
    ESP_LOGI(TAG, "[PRONTO] Heading hold ativo → norte magnético (0°).");
    ESP_LOGI(TAG, "[PRONTO] Aguardando 5 s para estabilizar orientação...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* ════════════════════════════════════════════════════════════════
     * [TESTE] — validação de heading e GPS
     * ════════════════════════════════════════════════════════════════ */
    {
        float theta = 0.0f;
        heading_read(&theta);
        hdrop_pose_t p = pose_get();

        ESP_LOGI(TAG, "── TESTE DE HEADING ─────────────────────");
        ESP_LOGI(TAG, "   Heading atual:  %.3f rad  (%.1f°)", theta, theta * 180.0f / (float)M_PI);
        ESP_LOGI(TAG, "   Alvo:           0.000 rad  (0.0° — norte)");
        ESP_LOGI(TAG, "   Erro:           %.1f°", (theta * 180.0f / (float)M_PI));

        ESP_LOGI(TAG, "── TESTE DE LOCALIZAÇÃO GPS ─────────────");
        if (p.gnss_valid) {
            ESP_LOGI(TAG, "   Fix GNSS: VÁLIDO");
            ESP_LOGI(TAG, "   x = %.4f m  (leste/oeste do ponto de origem)", p.x);
            ESP_LOGI(TAG, "   y = %.4f m  (norte/sul do ponto de origem)", p.y);
        } else {
            ESP_LOGW(TAG, "   Fix GNSS: AGUARDANDO SATÉLITES");
        }
        ESP_LOGI(TAG, "─────────────────────────────────────────");
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    /* ════════════════════════════════════════════════════════════════
     * [MANOBRA] — avança 5 s a 50% de potência
     * ════════════════════════════════════════════════════════════════ */
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   MANOBRA: AVANÇO 5 s @ 50%%          ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");
    ESP_LOGI(TAG, "[MANOBRA] PWM alvo: %d µs  (ponto morto=%d + %d µs)",
             (int)(MOTOR_PONTO_MORTO_US + MANOBRA_POTENCIA * MOTOR_DELTA_MAX_US),
             MOTOR_PONTO_MORTO_US,
             (int)(MANOBRA_POTENCIA * MOTOR_DELTA_MAX_US));

    /* Desativa heading hold — motores ficam livres para avançar reto */
    heading_hold(-1.0f);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Softstart: 2 s de rampa até 50% */
    softstart(MANOBRA_POTENCIA);

    /* Avanço a 50% por 5 s */
    ESP_LOGI(TAG, "[MANOBRA] Avançando %d s @ %.0f%%...", MANOBRA_DURACAO_S, MANOBRA_POTENCIA * 100.0f);
    for (int i = 0; i < MANOBRA_DURACAO_S * 10; i++) {
        motor_set_speeds(MANOBRA_POTENCIA, MANOBRA_POTENCIA);
        if (i % 10 == 0) {
            float theta = 0.0f;
            heading_read(&theta);
            ESP_LOGI(TAG, "[MANOBRA] t=%ds | PWM=%dµs | heading=%.1f°",
                     i / 10,
                     motor_get_last_pwm_left(),
                     theta * 180.0f / (float)M_PI);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Para os motores */
    motor_stop();
    ESP_LOGI(TAG, "[MANOBRA] PARADO.");
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* ════════════════════════════════════════════════════════════════
     * [OPERANDO] — retoma heading hold e monitoramento contínuo
     * ════════════════════════════════════════════════════════════════ */
    heading_hold(0.0f);
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   MANOBRA CONCLUÍDA — EM OPERAÇÃO    ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");

    while (true) {
        float theta = 0.0f;
        heading_read(&theta);
        hdrop_pose_t p = pose_get();

        ESP_LOGI(TAG, "heading=%.1f° | x=%.2fm y=%.2fm | fix=%s | mqtt=%s | heap=%lu",
                 theta * 180.0f / (float)M_PI,
                 p.x, p.y,
                 p.gnss_valid        ? "OK" : "--",
                 pose_mqtt_connected() ? "OK" : "--",
                 (unsigned long)esp_get_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
