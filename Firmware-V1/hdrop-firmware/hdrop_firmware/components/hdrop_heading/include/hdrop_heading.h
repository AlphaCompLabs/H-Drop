/**
 * @file hdrop_heading.h
 * @brief Interface pública do componente de estimação e controle de heading do H-DROP.
 * @details Define constantes de hardware (I2C, QMC5883L), parâmetros de controle P e
 *          as assinaturas das funções de leitura, calibração e controle de heading.
 *          O heading é estimado a partir do magnetômetro QMC5883L via atan2f(y_cal, x_cal),
 *          normalizado para [0, 2π). Calibração hard-iron (offsets X/Y) é persistida em NVS.
 *
 * @hardware  QMC5883L: I2C_NUM_0 | SDA=GPIO21 | SCL=GPIO22 | Endereço=0x0D
 * @depends   driver/i2c.h, nvs_flash.h, nvs.h, freertos, hdrop_motor
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Pinos e barramento I2C
 * ================================================================ */

/** Pino GPIO de dados I2C (SDA) do magnetômetro. */
#define HEADING_PINO_SDA          21

/** Pino GPIO de clock I2C (SCL) do magnetômetro. */
#define HEADING_PINO_SCL          22

/** Frequência do barramento I2C em Hz. 400 kHz (modo fast) para minimizar
 *  latência de leitura e suportar a taxa de saída de 200 Hz do QMC5883L. */
#define HEADING_I2C_FREQ_HZ       100000

/* ================================================================
 * QMC5883L — endereço e registradores
 * ================================================================ */

/** Endereço I2C do QMC5883L (fixo em hardware, não configurável). */
#define QMC5883L_ADDR             0x0D

/** Registrador de dados: X_LSB (0x00) até Z_MSB (0x05) — leitura de 6 bytes. */
#define QMC5883L_REG_DATA         0x00

/** Registrador de controle 1 (CTRL1): configura modo, taxa, range e OSR. */
#define QMC5883L_REG_CTRL1        0x09

/** Registrador SET/RESET Period: deve receber 0x01 antes do CTRL1 (soft reset). */
#define QMC5883L_REG_SET          0x0B

/** Valor de CTRL1: OSR=512, RNG=±8G, ODR=200 Hz, Modo Contínuo.
 *  Bits [7:6]=00 (OSR 512), [5:4]=01 (±8G), [3:2]=11 (200 Hz), [1:0]=01 (Contínuo). */
#define QMC5883L_CTRL1_VAL        0b00011101

/* ================================================================
 * Parâmetros da task e do controlador
 * ================================================================ */

/** Taxa de execução da heading_task em Hz (período = 100 ms). */
#define HEADING_TASK_RATE_HZ      10

/** Ganho proporcional do controlador de heading.
 *  ω_cmd = HEADING_KP × eθ, com eθ em radianos → ω_cmd em rad/s. */
#define HEADING_KP                2.0f

/** Distância entre propulsores (bitola) usada pela mistura ICR do controlador de heading.
 *  Deve ser consistente com o valor usado nas chamadas externas a motor_mix(). */
#define HEADING_L_BITOLA_M        0.35f

/** Velocidade linear mantida em zero durante o controle puro de heading. */
#define HEADING_V_HOLD_MS         0.0f

/* ================================================================
 * NVS — persistência de calibração
 * ================================================================ */

/** Namespace NVS para armazenamento dos coeficientes de calibração hard-iron. */
#define HEADING_NVS_NAMESPACE     "hdrop_hdg"

/* ================================================================
 * Tipos auxiliares
 * ================================================================ */

/**
 * @brief Coeficientes de calibração hard-iron do QMC5883L.
 * @details Offsets removidos dos eixos X e Y antes do cálculo do ângulo.
 *          x_cal = x_raw - (x_max + x_min) / 2.0
 *          y_cal = y_raw - (y_max + y_min) / 2.0
 *          Valores em unidades ADC brutas do sensor (int16_t).
 */
typedef struct {
    int16_t x_min; /**< Mínimo observado do eixo X durante calibração */
    int16_t x_max; /**< Máximo observado do eixo X durante calibração */
    int16_t y_min; /**< Mínimo observado do eixo Y durante calibração */
    int16_t y_max; /**< Máximo observado do eixo Y durante calibração */
} hdrop_cal_t;

/* ================================================================
 * API pública
 * ================================================================ */

/**
 * @brief Inicializa o barramento I2C, o QMC5883L e cria a heading_task.
 * @details Sequência:
 *            1. Inicializa NVS flash (apaga se corrompida).
 *            2. Configura I2C_NUM_0 a 400 kHz com pull-ups internos.
 *            3. Executa soft reset do QMC5883L (reg 0x0B = 0x01).
 *            4. Configura CTRL1 (reg 0x09 = 0b00011101).
 *            5. Carrega calibração hard-iron da NVS (usa zero se ausente).
 *            6. Cria heading_task a 10 Hz com prioridade 5.
 * @return  ESP_OK em sucesso; ESP_FAIL em qualquer erro de configuração.
 * @note    Deve ser chamada após motor_init() e antes de qualquer uso de
 *          heading_read() ou heading_hold().
 */
esp_err_t heading_init(void);

/**
 * @brief Lê o heading atual do QMC5883L e retorna em radianos.
 * @details Lê 6 bytes do registrador 0x00, aplica calibração hard-iron e
 *          calcula θ = atan2f(y_cal, x_cal), normalizado para [0, 2π).
 *          Thread-safe: pode ser chamada de qualquer task.
 * @param[out] theta_rad Ponteiro onde o heading será escrito (em radianos, [0, 2π)).
 *                       Não modificado em caso de falha I2C.
 * @return true  se a leitura I2C foi bem-sucedida;
 *         false em falha de comunicação (bus error, NAK, timeout).
 */
bool heading_read(float *theta_rad);

/**
 * @brief Executa calibração hard-iron coletando n_samples leituras do sensor.
 * @details O operador deve girar o veículo em um círculo completo durante a
 *          coleta. A função bloqueia por n_samples × 50 ms. Ao término,
 *          calcula x_min/max e y_min/max e persiste em NVS.
 *          A heading_task é pausada durante a calibração para evitar conflito
 *          de acesso I2C, e retomada ao final.
 * @param n_samples Número de leituras a coletar (recomendado: 200 para 10 s de rotação).
 * @return ESP_OK se ao menos 1 leitura foi bem-sucedida e a NVS foi gravada;
 *         ESP_FAIL em falha total de I2C ou escrita NVS.
 */
esp_err_t heading_calibrate(uint16_t n_samples);

/**
 * @brief Ativa o controle proporcional de heading com o ângulo alvo fornecido.
 * @details Define o setpoint e habilita o controlador interno da heading_task.
 *          A cada ciclo de 100 ms, a task calcula:
 *            eθ = atan2f(sinf(target − θ), cosf(target − θ))
 *            ω_cmd = HEADING_KP × eθ
 *          e chama motor_mix(HEADING_V_HOLD_MS, ω_cmd, HEADING_L_BITOLA_M).
 *          Chamar com target_rad = -1.0f desativa o controle e para os motores.
 * @param target_rad Heading desejado em radianos, intervalo [0, 2π).
 *                   Valores fora do intervalo são normalizados internamente.
 */
void heading_hold(float target_rad);

#ifdef __cplusplus
}
#endif
