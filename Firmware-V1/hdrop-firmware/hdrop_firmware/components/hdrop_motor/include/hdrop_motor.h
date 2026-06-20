/**
 * @file hdrop_motor.h
 * @brief Interface pública do componente de controle dos ESCs do H-DROP.
 * @details Define constantes de configuração PWM, assinaturas das funções de
 *          controle dos propulsores e os pinos de hardware associados. O
 *          mapeamento de velocidade normalizada para largura de pulso segue
 *          a convenção do firmware de referência (firm_diogo.ino), mantendo
 *          PONTO_MORTO = 1520 µs. A implementação usa o periférico LEDC nativo
 *          do ESP-IDF — sem dependência de ESP32Servo ou Arduino.h.
 *
 * @hardware  ESC Esquerdo: GPIO4 (LEDC canal 0) | ESC Direito: GPIO15 (LEDC canal 1)
 * @depends   driver/ledc.h, freertos/semphr.h, freertos/timers.h
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Pinos de hardware
 * ================================================================ */

/** Pino GPIO do ESC esquerdo do catamarã. */
#define MOTOR_PINO_ESC_ESQUERDO   4

/** Pino GPIO do ESC direito do catamarã. */
#define MOTOR_PINO_ESC_DIREITO    15

/* ================================================================
 * Parâmetros do sinal PWM
 * ================================================================ */

/** Frequência PWM dos ESCs em Hz. ESCs padrão operam a 50 Hz (período de 20 ms). */
#define MOTOR_FREQ_HZ             50

/** Período total do sinal PWM em microssegundos (1 / 50 Hz = 20 000 µs). */
#define MOTOR_PERIODO_US          20000

/** Largura de pulso neutra dos ESCs em microssegundos.
 *  Valor calibrado para os ESCs do H-DROP (não é o padrão de 1500 µs). */
#define MOTOR_PONTO_MORTO_US      1520

/** Delta máximo de pulso a partir do ponto morto.
 *  Range completo: 1520 ± 480 µs = [1040, 2000] µs efetivos.
 *  Formula: pwm_us = MOTOR_PONTO_MORTO_US + round(speed_norm × MOTOR_DELTA_MAX_US) */
#define MOTOR_DELTA_MAX_US        480

/* ================================================================
 * Parâmetros de temporização
 * ================================================================ */

/** Duração da sequência de arming dos ESCs em milissegundos.
 *  ESCs requerem sinal neutro contínuo por este período no boot antes
 *  de aceitar qualquer comando de movimento. */
#define MOTOR_ARMING_MS           4000

/** Período do watchdog de segurança em milissegundos.
 *  Ambos os motores são parados automaticamente se motor_set_speeds()
 *  não for chamada dentro deste intervalo. */
#define MOTOR_WATCHDOG_MS         2000

/* ================================================================
 * API pública
 * ================================================================ */

/**
 * @brief Inicializa os dois canais LEDC e executa a sequência de arming dos ESCs.
 * @details Configura o timer LEDC 0 a 50 Hz com resolução de 16 bits e dois canais
 *          (canal 0 → GPIO4, canal 1 → GPIO15). Mantém sinal neutro (1520 µs) por
 *          4 segundos antes de liberar comandos. Ao final, cria e inicia o timer
 *          watchdog FreeRTOS de 2 s. Deve ser chamada uma única vez no início de
 *          app_main(), antes de criar tasks que utilizem os motores.
 * @return  ESP_OK em sucesso; ESP_FAIL se a configuração LEDC ou o mutex falhar.
 * @note    O sistema não aceita comandos de movimento antes que esta função retorne
 *          ESP_OK. A sequência de arming não pode ser pulada.
 */
esp_err_t motor_init(void);

/**
 * @brief Define a velocidade normalizada de cada propulsor. Thread-safe via mutex.
 * @details Converte velocidade normalizada para largura de pulso µs e aplica via LEDC.
 *          Reinicia o watchdog interno a cada chamada. Valores fora de [-1.0, +1.0]
 *          são saturados antes da conversão.
 * @param left  Velocidade normalizada do propulsor esquerdo ∈ [-1.0, +1.0].
 *              +1.0 = avanço máximo, -1.0 = ré máximo, 0.0 = neutro.
 * @param right Velocidade normalizada do propulsor direito ∈ [-1.0, +1.0].
 */
void motor_set_speeds(float left, float right);

/**
 * @brief Envia 1520 µs para ambos os ESCs imediatamente. Chamada de emergência.
 * @details Aplica o pulso neutro diretamente via LEDC sem aguardar mutex para as
 *          chamadas de hardware, garantindo latência mínima. Segura para chamar
 *          do callback do watchdog (contexto da task de timer FreeRTOS).
 * @note    Não reinicia o watchdog — o sistema continuará em parada até que
 *          motor_set_speeds() seja chamada novamente.
 */
void motor_stop(void);

/**
 * @brief Realiza mistura cinemática ICR e aciona os propulsores.
 * @details Aplica o modelo diferencial de catamarã:
 *            vL = v - omega * (L_bitola / 2)
 *            vR = v + omega * (L_bitola / 2)
 *          Normaliza proporcionalmente caso |vL| ou |vR| exceda 1.0, preservando
 *          a relação diferencial. Chama motor_set_speeds() com os valores resultantes.
 * @param v        Velocidade linear desejada em m/s.
 * @param omega    Velocidade angular desejada em rad/s (positivo = giro anti-horário).
 * @param L_bitola Distância entre os propulsores (bitola) em metros.
 */
void motor_mix(float v, float omega, float L_bitola);

/**
 * @brief Retorna o último valor PWM enviado ao ESC esquerdo em microssegundos.
 * @details Leitura thread-safe via mutex. Retorna MOTOR_PONTO_MORTO_US antes
 *          de qualquer chamada a motor_set_speeds().
 * @return Largura de pulso em µs do último comando enviado ao ESC esquerdo.
 */
int motor_get_last_pwm_left(void);

/**
 * @brief Retorna o último valor PWM enviado ao ESC direito em microssegundos.
 * @details Leitura thread-safe via mutex. Retorna MOTOR_PONTO_MORTO_US antes
 *          de qualquer chamada a motor_set_speeds().
 * @return Largura de pulso em µs do último comando enviado ao ESC direito.
 */
int motor_get_last_pwm_right(void);

#ifdef __cplusplus
}
#endif
