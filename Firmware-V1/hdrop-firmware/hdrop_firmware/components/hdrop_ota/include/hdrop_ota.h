/**
 * @file hdrop_ota.h
 * @brief Interface pública do componente de atualização OTA via WiFi do H-DROP.
 * @details O ESP32 cria sua própria rede WiFi (modo AP). O operador conecta o
 *          computador ou celular diretamente a essa rede e acessa o painel em
 *          http://192.168.4.1/ para enviar um novo firmware (.bin).
 *
 *          Não é necessário roteador externo — o veículo é a própria rede.
 *          O IP de acesso é sempre 192.168.4.1 (padrão do softAP do ESP32).
 *
 *          Ao iniciar o upload o componente chama motor_stop() por segurança,
 *          replicando o comportamento do firm_diogo.ino.
 *
 * @hardware WiFi interno do ESP32 (sem hardware externo)
 * @depends  esp_wifi, esp_netif, esp_http_server, app_update, nvs_flash, hdrop_motor
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * CONFIGURAÇÃO DA REDE AP — alterar antes do primeiro flash
 * ================================================================ */

/** Nome da rede WiFi criada pelo ESP32 (SSID do Access Point). */
#define OTA_AP_SSID        "H-DROP-Control"

/** Senha da rede. Mínimo 8 caracteres (exigência WPA2). */
#define OTA_AP_PASSWORD    "senha_segura_123"

/** Porta HTTP do servidor OTA. 80 permite acesso direto via navegador. */
#define OTA_HTTP_PORT      80

/* ================================================================
 * API pública
 * ================================================================ */

/**
 * @brief Inicializa o WiFi em modo AP e sobe o servidor HTTP de OTA.
 * @details O ESP32 cria a rede OTA_AP_SSID imediatamente, sem depender
 *          de roteador externo. O servidor HTTP sobe junto após o AP
 *          iniciar. A função retorna sem bloquear app_main.
 *          IP de acesso fixo: http://192.168.4.1/
 * @return ESP_OK em sucesso; ESP_FAIL em erro de NVS, netif ou WiFi.
 */
esp_err_t ota_init(void);

#ifdef __cplusplus
}
#endif
