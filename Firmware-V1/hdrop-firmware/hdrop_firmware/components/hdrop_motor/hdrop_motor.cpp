/**
 * @file hdrop_motor.cpp
 * @brief Controle dos motores ESC do H-DROP via periférico LEDC do ESP-IDF.
 * @details Implementa primitivos PWM para dois ESCs brushless em configuração
 *          diferencial, mistura cinemática ICR e watchdog de segurança por
 *          software (FreeRTOS timer one-shot de 2 s). Não depende da biblioteca
 *          ESP32Servo — usa o driver LEDC nativo do ESP-IDF v5.x.
 *
 *          Mapeamento de velocidade para pulso:
 *            pwm_us = 1520 + round(speed_norm × 480)
 *            speed_norm ∈ [-1.0, +1.0]
 *
 * @hardware  ESC Esquerdo: GPIO4  → LEDC canal 0
 *            ESC Direito:  GPIO15 → LEDC canal 1
 *            Timer LEDC:   LEDC_TIMER_0, 50 Hz, 16 bits
 * @depends   driver/ledc.h, freertos/semphr.h, freertos/timers.h, esp_log.h
 */

#include "hdrop_motor.h"

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_log.h"

#include <cmath>

/** Tag de log do módulo. */
static const char *TAG = "MOTOR";

/* ================================================================
 * Estado interno do módulo
 * ================================================================ */

/** Mutex que protege g_last_pwm_left e g_last_pwm_right contra
 *  acesso concorrente entre a task de controle e o watchdog. */
static SemaphoreHandle_t g_motor_mutex    = nullptr;

/** Timer FreeRTOS one-shot que dispara motor_stop() após MOTOR_WATCHDOG_MS
 *  sem chamada a motor_set_speeds(). */
static TimerHandle_t     g_watchdog_timer = nullptr;

/** Último pulso enviado ao ESC esquerdo, em microssegundos.
 *  Inicializado com MOTOR_PONTO_MORTO_US para refletir o estado de arming. */
static int g_last_pwm_left  = MOTOR_PONTO_MORTO_US;

/** Último pulso enviado ao ESC direito, em microssegundos. */
static int g_last_pwm_right = MOTOR_PONTO_MORTO_US;

/* ================================================================
 * Funções internas (static)
 * ================================================================ */

/**
 * @brief Converte largura de pulso em µs para duty cycle de 16 bits.
 * @details Período total a 50 Hz = 20 000 µs.
 *          duty = (us / 20000) × 65535
 *          Resolução resultante: ~0.3 µs por step.
 */
static uint32_t us_para_duty(int us)
{
    return (uint32_t)((float)us / (float)MOTOR_PERIODO_US * 65535.0f);
}

/**
 * @brief Aplica largura de pulso PWM diretamente a um canal LEDC.
 * @details Não adquire mutex — chamador é responsável pela sincronização
 *          quando necessário. As chamadas LEDC em si são thread-safe no driver
 *          para canais distintos; o mutex aqui protege apenas as variáveis C++.
 * @param canal Canal LEDC a configurar.
 * @param us    Largura de pulso desejada em microssegundos.
 */
static void set_esc_pulse_us(ledc_channel_t canal, int us)
{
    uint32_t duty = us_para_duty(us);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, canal, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, canal);
}

/**
 * @brief Limita valor float ao intervalo [min_val, max_val].
 */
static float clamp_f(float val, float min_val, float max_val)
{
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

/**
 * @brief Callback do watchdog FreeRTOS.
 * @details Executado no contexto da task do timer service após MOTOR_WATCHDOG_MS
 *          sem chamada a motor_set_speeds(). Para ambos os motores imediatamente.
 *          O timer é one-shot e não se reinicia automaticamente.
 * @param xTimer Handle do timer (não utilizado).
 */
static void watchdog_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGW(TAG, "Watchdog disparado: nenhum comando recebido em %d ms. Parando motores.",
             MOTOR_WATCHDOG_MS);
    motor_stop();
}

/* ================================================================
 * Implementação da API pública
 * ================================================================ */

esp_err_t motor_init(void)
{
    /* ----------------------------------------------------------------
     * Mutex criado antes de qualquer acesso a hardware, pois motor_stop()
     * pode ser chamada pelo watchdog logo após sua criação.
     * ---------------------------------------------------------------- */
    g_motor_mutex = xSemaphoreCreateMutex();
    if (g_motor_mutex == nullptr) {
        ESP_LOGE(TAG, "Falha ao criar mutex do motor");
        return ESP_FAIL;
    }

    /* ----------------------------------------------------------------
     * Configuração do timer LEDC a 50 Hz com resolução de 16 bits.
     * A resolução de 16 bits (65535 steps) é necessária para representar
     * larguras de pulso ESC de ~1 µs com precisão suficiente a 50 Hz.
     * ---------------------------------------------------------------- */
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode      = LEDC_LOW_SPEED_MODE;
    timer_cfg.duty_resolution = LEDC_TIMER_16_BIT;
    timer_cfg.timer_num       = LEDC_TIMER_0;
    timer_cfg.freq_hz         = MOTOR_FREQ_HZ;
    timer_cfg.clk_cfg         = LEDC_AUTO_CLK;

    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar timer LEDC: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ----------------------------------------------------------------
     * Ambos os canais iniciam com duty de ponto morto (1520 µs) para
     * evitar transitórios no momento da configuração do hardware.
     * ---------------------------------------------------------------- */
    uint32_t duty_neutro = us_para_duty(MOTOR_PONTO_MORTO_US);

    ledc_channel_config_t ch_esq = {};
    ch_esq.gpio_num   = MOTOR_PINO_ESC_ESQUERDO;
    ch_esq.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_esq.channel    = LEDC_CHANNEL_0;
    ch_esq.intr_type  = LEDC_INTR_DISABLE;
    ch_esq.timer_sel  = LEDC_TIMER_0;
    ch_esq.duty       = duty_neutro;
    ch_esq.hpoint     = 0;

    ret = ledc_channel_config(&ch_esq);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar canal LEDC esquerdo: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t ch_dir = {};
    ch_dir.gpio_num   = MOTOR_PINO_ESC_DIREITO;
    ch_dir.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_dir.channel    = LEDC_CHANNEL_1;
    ch_dir.intr_type  = LEDC_INTR_DISABLE;
    ch_dir.timer_sel  = LEDC_TIMER_0;
    ch_dir.duty       = duty_neutro;
    ch_dir.hpoint     = 0;

    ret = ledc_channel_config(&ch_dir);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar canal LEDC direito: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ----------------------------------------------------------------
     * Sequência de arming: sinal neutro contínuo por MOTOR_ARMING_MS.
     * ESCs ignoram qualquer comando de velocidade antes deste período.
     * O vTaskDelay bloqueante aqui é intencional — motor_init() deve ser
     * a primeira chamada em app_main(), antes de criar tasks de controle.
     * ---------------------------------------------------------------- */
    ESP_LOGI(TAG, "Arming dos ESCs iniciado (%d ms)...", MOTOR_ARMING_MS);
    vTaskDelay(pdMS_TO_TICKS(MOTOR_ARMING_MS));
    ESP_LOGI(TAG, "Arming concluído. Motores prontos para receber comandos.");

    /* ----------------------------------------------------------------
     * Watchdog por software: timer one-shot de MOTOR_WATCHDOG_MS.
     * Reiniciado a cada chamada de motor_set_speeds(). Se não reiniciado
     * dentro do intervalo, chama motor_stop() via watchdog_callback().
     * ---------------------------------------------------------------- */
    g_watchdog_timer = xTimerCreate(
        "motor_wdg",
        pdMS_TO_TICKS(MOTOR_WATCHDOG_MS),
        pdFALSE,       /* one-shot: não auto-reinicia após disparo */
        nullptr,
        watchdog_callback
    );

    if (g_watchdog_timer == nullptr) {
        ESP_LOGE(TAG, "Falha ao criar timer watchdog do motor");
        return ESP_FAIL;
    }

    xTimerStart(g_watchdog_timer, 0);
    ESP_LOGI(TAG, "Watchdog iniciado (%d ms).", MOTOR_WATCHDOG_MS);

    return ESP_OK;
}

void motor_set_speeds(float left, float right)
{
    /* Satura velocidade normalizada antes da conversão para µs */
    left  = clamp_f(left,  -1.0f, 1.0f);
    right = clamp_f(right, -1.0f, 1.0f);

    int pwm_left  = MOTOR_PONTO_MORTO_US + (int)roundf(left  * (float)MOTOR_DELTA_MAX_US);
    int pwm_right = MOTOR_PONTO_MORTO_US + (int)roundf(right * (float)MOTOR_DELTA_MAX_US);

    /* Aplica pulso via LEDC antes de adquirir mutex para minimizar latência */
    set_esc_pulse_us(LEDC_CHANNEL_0, pwm_left);
    set_esc_pulse_us(LEDC_CHANNEL_1, pwm_right);

    /* Atualiza variáveis compartilhadas sob mutex */
    if (xSemaphoreTake(g_motor_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_last_pwm_left  = pwm_left;
        g_last_pwm_right = pwm_right;
        xSemaphoreGive(g_motor_mutex);
    } else {
        ESP_LOGW(TAG, "motor_set_speeds: timeout ao adquirir mutex");
    }

    /* Reinicia o watchdog a cada comando recebido (alimenta o "cão de guarda") */
    if (g_watchdog_timer != nullptr) {
        xTimerReset(g_watchdog_timer, 0);
    }
}

void motor_stop(void)
{
    /* ----------------------------------------------------------------
     * Aplica ponto morto diretamente, sem aguardar mutex para as chamadas
     * LEDC, garantindo tempo de resposta mínimo em situações de emergência
     * e no callback do watchdog (contexto de timer service).
     * ---------------------------------------------------------------- */
    set_esc_pulse_us(LEDC_CHANNEL_0, MOTOR_PONTO_MORTO_US);
    set_esc_pulse_us(LEDC_CHANNEL_1, MOTOR_PONTO_MORTO_US);

    if (g_motor_mutex != nullptr &&
        xSemaphoreTake(g_motor_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_last_pwm_left  = MOTOR_PONTO_MORTO_US;
        g_last_pwm_right = MOTOR_PONTO_MORTO_US;
        xSemaphoreGive(g_motor_mutex);
    }
}

void motor_mix(float v, float omega, float L_bitola)
{
    /* ----------------------------------------------------------------
     * Cinemática diferencial ICR (Centro Instantâneo de Rotação):
     *   vL = v - omega * (L / 2)
     *   vR = v + omega * (L / 2)
     *
     * Resultado em m/s. Normalização proporcional é aplicada quando
     * |vL| ou |vR| excede 1.0, preservando a relação diferencial
     * (razão vL/vR) que define a curvatura do trajeto.
     * ---------------------------------------------------------------- */
    float half_L = L_bitola * 0.5f;
    float vL = v - omega * half_L;
    float vR = v + omega * half_L;

    /* Normalização proporcional: divide pelo maior valor absoluto quando > 1.0 */
    float max_abs = fmaxf(fabsf(vL), fabsf(vR));
    if (max_abs > 1.0f) {
        vL /= max_abs;
        vR /= max_abs;
    }

    motor_set_speeds(vL, vR);
}

int motor_get_last_pwm_left(void)
{
    int val = MOTOR_PONTO_MORTO_US;
    if (g_motor_mutex != nullptr &&
        xSemaphoreTake(g_motor_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        val = g_last_pwm_left;
        xSemaphoreGive(g_motor_mutex);
    }
    return val;
}

int motor_get_last_pwm_right(void)
{
    int val = MOTOR_PONTO_MORTO_US;
    if (g_motor_mutex != nullptr &&
        xSemaphoreTake(g_motor_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        val = g_last_pwm_right;
        xSemaphoreGive(g_motor_mutex);
    }
    return val;
}
