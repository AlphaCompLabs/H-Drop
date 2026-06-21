/**
 * @file hdrop_pose.h
 * @brief Interface pública do componente de estimativa de pose do H-DROP.
 * @details Define a representação q = (x, y, θ) ∈ SE(2) do veículo e as assinaturas
 *          das funções de inicialização, atualização e leitura da pose. A posição x,y
 *          é obtida via GNSS (A7670SA em UART2) com conversão de coordenadas geodésicas
 *          para um referencial local plano. O ângulo θ é lido do magnetômetro pelo
 *          componente hdrop_heading.
 *
 *          A FSM de conectividade (REDE → MQTT → OPERANDO) é gerenciada internamente
 *          pela pose_task, que publica telemetria JSON em "hdrop/raw" a 2 Hz.
 *
 * @hardware  A7670SA: UART2 | TX=GPIO17 | RX=GPIO16 | 115200 baud
 * @depends   driver/uart.h, freertos, hdrop_heading
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Hardware — UART2 / A7670SA
 * ================================================================ */

/** Pino GPIO de transmissão UART2 (ESP32 TX → modem RX). */
#define POSE_PINO_TX              17

/** Pino GPIO de recepção UART2 (modem TX → ESP32 RX). */
#define POSE_PINO_RX              16

/** Baud rate do A7670SA. Fixo em hardware — alterar requer reconfiguração do modem. */
#define POSE_UART_BAUD            115200

/* ================================================================
 * Tipos — representação de pose q = (x, y, θ)
 * ================================================================ */

/**
 * @brief Representa a pose completa do veículo no plano de navegação.
 * @details Segue a representação q = (x, y, θ) ∈ SE(2) da cinemática
 *          diferencial planar. x e y são expressos em metros relativos
 *          ao ponto de referência (primeiro fix GNSS válido após boot).
 *          θ vem do magnetômetro (hdrop_heading), sem deriva posicional.
 */
typedef struct {
    float    x;             /**< Posição leste-oeste em metros (eixo X local) */
    float    y;             /**< Posição norte-sul em metros (eixo Y local) */
    float    theta;         /**< Orientação em radianos, intervalo [0, 2π) */
    bool     gnss_valid;    /**< true se o último fix GNSS foi aceito como válido */
    uint32_t last_gnss_ms;  /**< Timestamp do último fix em ms (xTaskGetTickCount) */
} hdrop_pose_t;

/* ================================================================
 * API pública
 * ================================================================ */

/**
 * @brief Inicializa o UART2, cria mutex de pose e inicia a pose_task.
 * @details Configura UART_NUM_2 a 115200 baud 8N1 sem controle de fluxo.
 *          A pose_task executa a FSM de conectividade (CONECTANDO_REDE →
 *          CONECTANDO_MQTT → OPERANDO) e publica telemetria JSON a 2 Hz.
 *          Deve ser chamada após heading_init().
 * @return ESP_OK em sucesso; ESP_FAIL em erro de UART ou alocação de recursos.
 */
esp_err_t pose_init(void);

/**
 * @brief Atualiza x e y da pose a partir de uma string +CGNSSINFO.
 * @details Valida o campo mode (aceita "2" ou "3"), converte DDMM.MMMMMM para
 *          graus decimais, define lat₀/lon₀ no primeiro fix válido e calcula
 *          deslocamento XY usando aproximação de Terra plana (erro < 0.5% até
 *          ~50 km do ponto de referência).
 *          Thread-safe — adquire mutex interno antes de escrever na pose.
 * @param cgnssinfo_str String após "+CGNSSINFO:" (sem prefixo), ex:
 *                      "2,09,05,00,1547.123456,S,04720.654321,W,130625,..."
 * @return true  se mode = 2 ou 3 e parsing bem-sucedido;
 *         false se sem fix (mode vazio ou ',' inicial) ou string malformada.
 */
bool pose_update_gnss(const char *cgnssinfo_str);

/**
 * @brief Atualiza o ângulo θ da pose com leitura do magnetômetro.
 * @details Thread-safe — adquire mutex antes de escrever na pose.
 * @param theta_rad Heading em radianos [0, 2π), tipicamente de heading_read().
 */
void pose_update_heading(float theta_rad);

/**
 * @brief Retorna uma cópia thread-safe da pose atual.
 * @details Adquire mutex, copia g_pose para variável local e libera.
 *          Segura para chamar de qualquer task a qualquer momento.
 * @return Cópia da hdrop_pose_t atual. Se o mutex não puder ser adquirido
 *         em 10 ms, retorna a última cópia válida sem garantia de atomicidade.
 */
hdrop_pose_t pose_get(void);

#ifdef __cplusplus
}
#endif
