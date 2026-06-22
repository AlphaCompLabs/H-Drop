/**
 * @file hdrop_ota.cpp
 * @brief OTA via WiFi AP com servidor HTTP embutido.
 *        Padrão idêntico ao firm_diogo.ino: ESP32 cria a rede,
 *        operador acessa http://192.168.4.1/ e envia o .bin.
 */

#include "hdrop_ota.h"
#include "hdrop_motor.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

static const char *TAG = "OTA";

static httpd_handle_t s_server = NULL;

/* ================================================================
 * HTML — Página de upload (visual dark do firm_diogo.ino)
 * ================================================================ */

static const char *HTML_UPDATE =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='UTF-8'>"
    "<title>H-DROP - OTA via Web</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;text-align:center;margin:0;"
    "padding:20px;background-color:#222;color:white}"
    ".card{background-color:#333;padding:30px;border-radius:10px;"
    "margin:40px auto;max-width:400px;box-shadow:0 4px 10px rgba(0,0,0,.5)}"
    "h2{color:#ffeb3b;margin-top:0}"
    "input[type=file]{margin:20px 0;background:#555;padding:10px;"
    "border-radius:5px;width:80%;color:white;cursor:pointer}"
    "input[type=submit]{background-color:#4CAF50;color:white;width:100%;"
    "height:50px;font-size:18px;font-weight:bold;border-radius:10px;"
    "border:none;cursor:pointer}"
    "input[type=submit]:hover{background-color:#45a049}"
    "p{color:#ccc;font-size:14px}"
    "code{background:#444;padding:2px 6px;border-radius:4px;font-size:12px}"
    "</style></head>"
    "<body><div class='card'>"
    "<h2>Atualiza&ccedil;&atilde;o de Firmware</h2>"
    "<p>Selecione o arquivo <code>hdrop_firmware.bin</code>"
    "<br>gerado por <code>idf.py build</code></p>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='update' accept='.bin' required>"
    "<input type='submit' value='Enviar Novo C&oacute;digo'>"
    "</form>"
    "</div></body></html>";

/* ================================================================
 * Handler GET / — serve a página de upload
 * ================================================================ */

static esp_err_t handler_get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_UPDATE, (ssize_t)strlen(HTML_UPDATE));
    return ESP_OK;
}

/* ================================================================
 * Handler POST /update — recebe e grava o firmware
 * ================================================================ */

static esp_err_t handler_post_update(httpd_req_t *req)
{
    static char buf[1024];
    int  remaining = req->content_len;
    bool iniciado  = false;

    const esp_partition_t *part_alvo = esp_ota_get_next_update_partition(NULL);
    if (!part_alvo) {
        ESP_LOGE(TAG, "Nenhuma particao OTA — verificar partitions_ota.csv");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "FALHA: sem particao OTA disponivel.");
        return ESP_FAIL;
    }

    /* Para os motores imediatamente por segurança — mesmo comportamento
     * do firm_diogo.ino que chamava writeMicroseconds(PONTO_MORTO). */
    motor_stop();
    ESP_LOGI(TAG, "Motores parados. Recebendo firmware: %d bytes → '%s'",
             remaining, part_alvo->label);

    esp_ota_handle_t ota_handle = 0;

    while (remaining > 0) {
        int a_receber = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int recebido  = httpd_req_recv(req, buf, a_receber);

        if (recebido <= 0) {
            if (recebido == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Erro ao receber dados: %d", recebido);
            if (iniciado) esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "FALHA NA ATUALIZACAO");
            return ESP_FAIL;
        }

        if (!iniciado) {
            /* Localiza o magic byte 0xE9 do ESP32 para descartar
             * os cabeçalhos multipart enviados pelo navegador. */
            int offset = 0;
            for (int i = 0; i < recebido; i++) {
                if ((uint8_t)buf[i] == 0xE9) { offset = i; break; }
            }

            if (esp_ota_begin(part_alvo, OTA_WITH_SEQUENTIAL_WRITES,
                              &ota_handle) != ESP_OK) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                    "FALHA NA ATUALIZACAO");
                return ESP_FAIL;
            }
            esp_ota_write(ota_handle, buf + offset, recebido - offset);
            iniciado = true;
        } else {
            esp_ota_write(ota_handle, buf, recebido);
        }

        remaining -= recebido;
    }

    if (!iniciado) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "FALHA NA ATUALIZACAO");
        return ESP_FAIL;
    }

    if (esp_ota_end(ota_handle) != ESP_OK ||
        esp_ota_set_boot_partition(part_alvo) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "FALHA NA ATUALIZACAO");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Firmware gravado em '%s'. Reiniciando...", part_alvo->label);

    /* Resposta idêntica ao firm_diogo.ino antes do ESP.restart(). */
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "SUCESSO! REINICIANDO...",
                    (ssize_t)strlen("SUCESSO! REINICIANDO..."));

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/* ================================================================
 * Servidor HTTP
 * ================================================================ */

static void http_server_start(void)
{
    if (s_server) return;

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.server_port       = OTA_HTTP_PORT;
    config.max_uri_handlers  = 4;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar servidor HTTP");
        return;
    }

    static const httpd_uri_t uri_get = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handler_get_index,
        .user_ctx = NULL,
    };
    static const httpd_uri_t uri_post = {
        .uri      = "/update",
        .method   = HTTP_POST,
        .handler  = handler_post_update,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_server, &uri_get);
    httpd_register_uri_handler(s_server, &uri_post);
}

/* ================================================================
 * Handler de evento WiFi AP
 * ================================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        http_server_start();
        ESP_LOGI(TAG, "Rede '%s' criada. Acesse: http://192.168.4.1/",
                 OTA_AP_SSID);

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev =
            (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "Dispositivo conectado — MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 ev->mac[0], ev->mac[1], ev->mac[2],
                 ev->mac[3], ev->mac[4], ev->mac[5]);

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Dispositivo desconectado da rede OTA.");
    }
}

/* ================================================================
 * API pública
 * ================================================================ */

esp_err_t ota_init(void)
{
    /* NVS exigido internamente pelo driver WiFi. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrompida — apagando e reinicializando");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    ESP_ERROR_CHECK(esp_netif_init());

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.ap.ssid,     OTA_AP_SSID,     sizeof(wifi_cfg.ap.ssid) - 1);
    strncpy((char *)wifi_cfg.ap.password, OTA_AP_PASSWORD, sizeof(wifi_cfg.ap.password) - 1);
    wifi_cfg.ap.ssid_len       = (uint8_t)strlen(OTA_AP_SSID);
    wifi_cfg.ap.max_connection = 4;
    wifi_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* O servidor HTTP e o log de confirmação sobem no handler WIFI_EVENT_AP_START. */
    return ESP_OK;
}
