/**
 * @file hdrop_pose.cpp
 * @brief Estimativa de pose via GNSS (A7670SA/UART2) e controle de conectividade MQTT.
 * @details Porta a FSM de hdrop_barco_autonomo.ino para ESP-IDF, substituindo:
 *            Serial2  → UART_NUM_2 via driver/uart.h
 *            Serial   → ESP_LOGI/LOGW/LOGE
 *            delay()  → vTaskDelay(pdMS_TO_TICKS())
 *            millis() → xTaskGetTickCount() × portTICK_PERIOD_MS
 *            String   → buffers char[] estáticos
 *
 *          FSM interna gerenciada pela pose_task (prioridade 4, 8 KB de stack):
 *            CONECTANDO_REDE → CONECTANDO_MQTT → OPERANDO
 *
 *          No estado OPERANDO (2 Hz):
 *            1. Lê AT+CGNSSINFO → pose_update_gnss()
 *            2. Lê magnetômetro → pose_update_heading()
 *            3. Publica JSON em hdrop/raw com campo "pose"
 *            4. Escuta URCs de hdrop/comando (prefixo "ROTA:")
 *
 * @hardware  A7670SA: UART2 | TX=GPIO17 | RX=GPIO16 | 115200 baud
 * @depends   driver/uart.h, freertos/semphr.h, hdrop_heading
 */

#include "hdrop_pose.h"
#include "hdrop_heading.h"
#include "hdrop_motor.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>

/** Tag de log do módulo. */
static const char *TAG = "POSE";

/* ================================================================
 * Configuração MQTT e AT — constantes internas
 * ================================================================ */

/** Endereço do broker MQTT conforme configuração do firmware de referência. */
static const char *POSE_MQTT_BROKER      = "tcp://broker.hivemq.com:1883";

/** Client ID MQTT — único por embarcação em operação multi-veículo. */
static const char *POSE_MQTT_CLIENT_ID   = "hdrop_boat_001";

/** Tópico de publicação de telemetria. */
static const char *POSE_MQTT_TOPIC_PUB   = "hdrop/raw";

/** Tópico de subscrição de comandos de rota. */
static const char *POSE_MQTT_TOPIC_SUB   = "hdrop/comando";

/** APN da operadora para ativação do contexto PDP. */
static const char *POSE_APN              = "zap.vivo.com.br";

/** Keepalive MQTT em segundos (manual A7670SA §18). */
static const int   POSE_MQTT_KEEPALIVE   = 60;

/** Comprimentos de tópico em bytes (strlen). */
static const int   POSE_TOPIC_PUB_LEN    = 9;   /* strlen("hdrop/raw")     */
static const int   POSE_TOPIC_SUB_LEN    = 13;  /* strlen("hdrop/comando") */

/** Timeout padrão de comandos AT em ms. */
static const uint32_t POSE_AT_TMO_MS     = 5000;

/** Timeout para operações de rede lentas (registro, CGACT, CMQTTCONNECT). */
static const uint32_t POSE_AT_TMO_LONGO  = 15000;

/** Timeout específico para AT+CGNSSPWR=1 (manual: módulo leva ~9 s). */
static const uint32_t POSE_AT_TMO_GNSS   = 9000;

/** Timeout para AT+CMQTTPUB (QoS 0 é rápido; manual indica mínimo de 60 s). */
static const uint32_t POSE_AT_TMO_PUB    = 10000;

/** Intervalo de telemetria em ms (2 Hz). */
static const uint32_t POSE_TELEMETRIA_MS = 500;

/** Falhas MQTT consecutivas antes de verificar e reconectar. */
static const int POSE_MAX_ERROS_MQTT     = 3;

/** Velocidade linear máxima estimada em m/s — calibrar em campo com GPS de referência.
 *  Usada para converter PWM normalizado em m/s no dead reckoning. */
static const float V_MAX_MS              = 1.0f;

/* ================================================================
 * Buffers estáticos — declarados em nível de módulo para não usar
 * stack da pose_task (evita stack overflow com buffers grandes).
 * ================================================================ */

/** Buffer para resposta de comandos AT. */
static char s_buf_at[512];

/** Buffer para string bruta do +CGNSSINFO. */
static char s_buf_gnss[160];

/** Buffer para payload JSON de telemetria. */
static char s_buf_json[384];

/** Acumulador de URCs assíncronas do modem (mensagens não solicitadas). */
static char s_buf_urc[2048];

/* ================================================================
 * Estado da pose e do módulo
 * ================================================================ */

/** Pose atual — protegida por g_pose_mutex. */
static hdrop_pose_t g_pose = {0.0f, 0.0f, 0.0f, false, 0};

/** Mutex que serializa leituras e escritas em g_pose. */
static SemaphoreHandle_t g_pose_mutex = nullptr;

/** Ponto de referência geográfico: definido no primeiro fix GNSS válido. */
static float g_lat0_deg = 0.0f;
static float g_lon0_deg = 0.0f;
static bool  g_ref_set  = false;

/** Contador de erros MQTT consecutivos — compartilhado entre task e processamento de URCs. */
static int s_cont_erros_mqtt = 0;

/** Indica se o FSM está em estado OPERANDO (MQTT conectado e publicando). */
static volatile bool g_mqtt_operando = false;

/** Handle do timer periódico de dead reckoning (10 Hz = 100 ms). */
static esp_timer_handle_t g_dr_timer = nullptr;

/* ================================================================
 * Estados da FSM de conectividade
 * ================================================================ */

/**
 * @brief Estados da máquina de estados de conectividade do A7670SA.
 */
typedef enum {
    FSM_CONECTANDO_REDE,  /**< Configura APN, ativa PDP e inicializa GNSS */
    FSM_CONECTANDO_MQTT,  /**< Inicia sessão MQTT e assina hdrop/comando */
    FSM_OPERANDO          /**< Loop de telemetria 2 Hz + leitura de URCs */
} fsm_estado_t;

/* ================================================================
 * Helpers internos — temporização
 * ================================================================ */

/**
 * @brief Retorna timestamp atual em milissegundos desde o boot.
 * @details Baseado em xTaskGetTickCount para não depender do componente
 *          esp_timer, mantendo REQUIRES mínimo no CMakeLists.
 */
static inline uint32_t agora_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ================================================================
 * AT command helpers — portados de hdrop_barco_autonomo.ino
 * ================================================================ */

/**
 * @brief Drena o buffer UART por ~30 ms para eliminar respostas residuais.
 * @details Preserva URCs críticas (+CMQTTRXSTART, +CMQTTCONNLOST) no
 *          s_buf_urc em vez de descartá-las.
 */
static void drenar_uart(void)
{
    char   drain[512] = {0};
    size_t total      = 0;
    uint32_t t0       = agora_ms();

    while (agora_ms() - t0 < 30) {
        uint8_t byte;
        /* pdMS_TO_TICKS(2) = 0 a 100 Hz → busy-loop. Usar 1 tick (~10 ms) para
         * garantir yield ao idle task e evitar starvation do watchdog. */
        if (uart_read_bytes(UART_NUM_2, &byte, 1, pdMS_TO_TICKS(10)) == 1) {
            if (total < sizeof(drain) - 1) {
                drain[total++] = (char)byte;
                drain[total]   = '\0';
            }
            t0 = agora_ms();  /* reinicia janela ao receber dado */
        }
    }

    if (total == 0) return;

    /* Preserva URCs críticas que chegaram durante operações anteriores */
    if (strstr(drain, "+CMQTTRXSTART:") || strstr(drain, "+CMQTTCONNLOST:")) {
        size_t urc_len = strlen(s_buf_urc);
        strncat(s_buf_urc, drain, sizeof(s_buf_urc) - urc_len - 1);
    }
}

/**
 * @brief Envia comando AT e aguarda substring esperada dentro do timeout.
 * @details Drena o UART antes de enviar para evitar contaminação por respostas
 *          anteriores. Retorna imediatamente ao detectar "ERROR" no buffer.
 * @param cmd        Comando AT sem \\r\\n.
 * @param esperado   Substring de sucesso na resposta (ex: "+CMQTTSTART: 0").
 * @param timeout_ms Tempo máximo de espera em ms.
 * @param out        Buffer opcional para capturar resposta completa (pode ser nullptr).
 * @param out_len    Tamanho de out.
 * @return true  se esperado encontrado antes do timeout;
 *         false em timeout ou "ERROR" recebido.
 */
static bool enviar_at(const char *cmd, const char *esperado,
                      uint32_t timeout_ms, char *out, size_t out_len)
{
    drenar_uart();

    /* Envia cmd + terminador AT */
    uart_write_bytes(UART_NUM_2, cmd, strlen(cmd));
    uart_write_bytes(UART_NUM_2, "\r\n", 2);

    ESP_LOGI(TAG, "[AT>] %s", cmd);

    char     resp[512] = {0};
    size_t   resp_len  = 0;
    uint32_t t0        = agora_ms();

    while (agora_ms() - t0 < timeout_ms) {
        uint8_t byte;
        if (uart_read_bytes(UART_NUM_2, &byte, 1, pdMS_TO_TICKS(10)) == 1) {
            if (resp_len < sizeof(resp) - 1) {
                resp[resp_len++] = (char)byte;
                resp[resp_len]   = '\0';
            }
        }

        if (strstr(resp, esperado)) {
            ESP_LOGI(TAG, "[AT<] %.200s", resp);
            if (out && out_len > 0) {
                strncpy(out, resp, out_len - 1);
                out[out_len - 1] = '\0';
            }
            return true;
        }

        /* Retorno antecipado em caso de ERROR para não desperdiçar o timeout */
        if (strstr(resp, "ERROR")) {
            ESP_LOGE(TAG, "[AT<ERR] %.200s", resp);
            if (out && out_len > 0) {
                strncpy(out, resp, out_len - 1);
                out[out_len - 1] = '\0';
            }
            return false;
        }
    }

    ESP_LOGW(TAG, "[AT<TMO] %.200s", resp);
    return false;
}

/**
 * @brief Envia dados brutos após o modem exibir o prompt ">".
 * @details Não adiciona \\r\\n — o modem aceita dado puro no modo prompt
 *          (AT+CMQTTTOPIC, AT+CMQTTPAYLOAD, AT+CMQTTSUBTOPIC).
 * @param dado       String de dados a enviar.
 * @param esperado   Substring de confirmação esperada na resposta.
 * @param timeout_ms Tempo máximo de espera em ms.
 * @return true se confirmação recebida; false em timeout ou ERROR.
 */
static bool enviar_dado_prompt(const char *dado, const char *esperado, uint32_t timeout_ms)
{
    uart_write_bytes(UART_NUM_2, dado, strlen(dado));
    ESP_LOGI(TAG, "[AT>>] %s", dado);

    char     resp[256] = {0};
    size_t   resp_len  = 0;
    uint32_t t0        = agora_ms();

    while (agora_ms() - t0 < timeout_ms) {
        uint8_t byte;
        if (uart_read_bytes(UART_NUM_2, &byte, 1, pdMS_TO_TICKS(10)) == 1) {
            if (resp_len < sizeof(resp) - 1) {
                resp[resp_len++] = (char)byte;
                resp[resp_len]   = '\0';
            }
        }
        if (strstr(resp, esperado)) {
            ESP_LOGI(TAG, "[AT<<] %s", resp);
            return true;
        }
        if (strstr(resp, "ERROR")) {
            ESP_LOGE(TAG, "[AT<<ERR] %s", resp);
            return false;
        }
    }

    ESP_LOGW(TAG, "[AT<<TMO]");
    return false;
}

/* ================================================================
 * MQTT — gerenciamento de sessão
 * ================================================================ */

/**
 * @brief Executa sequência de limpeza MQTT: DISC → REL → STOP.
 * @details Chamada antes de toda tentativa de reconexão para garantir
 *          estado limpo do subsistema MQTT interno do modem (manual §18).
 */
static void liberar_sessao_mqtt(void)
{
    ESP_LOGI(TAG, "[MQTT] Liberando sessão anterior...");
    enviar_at("AT+CMQTTDISC=0,120", "+CMQTTDISC: 0,0", POSE_AT_TMO_LONGO, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    enviar_at("AT+CMQTTREL=0", "OK", POSE_AT_TMO_MS, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    enviar_at("AT+CMQTTSTOP", "+CMQTTSTOP: 0", 12000, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
}

/**
 * @brief Verifica se a sessão MQTT está ativa via AT+CMQTTCONNECT?.
 * @return true se a resposta contém o endereço do broker configurado.
 */
static bool mqtt_conectado(void)
{
    if (!enviar_at("AT+CMQTTCONNECT?", "+CMQTTCONNECT:", POSE_AT_TMO_MS,
                   s_buf_at, sizeof(s_buf_at))) {
        return false;
    }
    return strstr(s_buf_at, "broker.hivemq.com") != nullptr;
}

/* ================================================================
 * FSM — estados de conectividade
 * ================================================================ */

/**
 * @brief Aguarda o boot completo do A7670SA após um reset inesperado.
 * @details Lê o UART até encontrar "+CPIN: READY" (SIM pronto) ou timeout.
 *          Chamada quando echo retorna no meio de uma sessão — indica que o
 *          módulo reiniciou por brownout durante a busca de torre 4G.
 * @param timeout_ms Tempo máximo de espera em ms.
 */
static void aguardar_boot_modem(uint32_t timeout_ms)
{
    ESP_LOGW(TAG, "[MODEM] Reset detectado. Aguardando +CPIN: READY (max %lu s)...",
             (unsigned long)(timeout_ms / 1000));

    char   buf[128] = {0};
    size_t len      = 0;
    uint32_t t0     = agora_ms();

    while (agora_ms() - t0 < timeout_ms) {
        uint8_t byte;
        if (uart_read_bytes(UART_NUM_2, &byte, 1, pdMS_TO_TICKS(10)) == 1) {
            if (len < sizeof(buf) - 1) {
                buf[len++] = (char)byte;
                buf[len]   = '\0';
            } else {
                /* Janela deslizante: descarta metade ao encher (64 bytes restantes) */
                memmove(buf, buf + 64, 64);
                buf[64] = '\0';
                len = 64;
            }
            if (strstr(buf, "+CPIN: READY")) {
                ESP_LOGI(TAG, "[MODEM] +CPIN: READY recebido. Aguardando 3 s...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                return;
            }
        }
    }
    /* Timeout — continua mesmo assim; ATE0 vai confirmar se está pronto */
    ESP_LOGW(TAG, "[MODEM] Timeout aguardando +CPIN: READY. Continuando.");
    vTaskDelay(pdMS_TO_TICKS(2000));
}

/**
 * @brief Executa o estado CONECTANDO_REDE: verifica modem, registra na rede
 *        celular, configura APN, ativa contexto PDP e inicializa GNSS.
 * @return true se rede e GNSS prontos; false em falha irrecuperável.
 */
static bool conectar_rede(void)
{
    ESP_LOGI(TAG, "[REDE] Conectando à rede celular...");

    /* ── Verificar comunicação com o modem (até 10 tentativas × 1 s) ── */
    bool modem_ok = false;
    for (int i = 0; i < 10; i++) {
        if (enviar_at("AT", "OK", 2000, nullptr, 0)) { modem_ok = true; break; }
        ESP_LOGW(TAG, "[REDE] Aguardando modem (%d/10)...", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!modem_ok) { ESP_LOGE(TAG, "[REDE] Modem não responde."); return false; }

    /* Desabilitar eco para não poluir respostas de comandos posteriores */
    enviar_at("ATE0", "OK", 2000, nullptr, 0);

    /* ── Diagnóstico de boot do modem ────────────────────────────────────
     * Roda uma vez por conexão para mostrar o estado inicial no serial.   */
    ESP_LOGI(TAG, "[DIAG] ──────────────────────────────────────");
    ESP_LOGI(TAG, "[DIAG]  DIAGNÓSTICO MODEM A7670SA");
    ESP_LOGI(TAG, "[DIAG] ──────────────────────────────────────");

    /* SIM card */
    if (enviar_at("AT+CPIN?", "OK", 3000, s_buf_at, sizeof(s_buf_at))) {
        if (strstr(s_buf_at, "READY"))
            ESP_LOGI(TAG,  "[DIAG] SIM: PRONTO");
        else if (strstr(s_buf_at, "SIM PIN"))
            ESP_LOGE(TAG,  "[DIAG] SIM: AGUARDANDO PIN — desbloqueie o SIM");
        else
            ESP_LOGW(TAG,  "[DIAG] SIM: %s", s_buf_at);
    } else {
        ESP_LOGE(TAG, "[DIAG] SIM: sem resposta");
    }

    /* Qualidade do sinal (0-31 = válido, 99 = sem sinal) */
    if (enviar_at("AT+CSQ", "OK", 3000, s_buf_at, sizeof(s_buf_at))) {
        int csq = -1;
        sscanf(s_buf_at, "%*[^+]+CSQ: %d", &csq);
        if (csq == 99 || csq < 0)
            ESP_LOGE(TAG,  "[DIAG] Sinal: SEM SINAL (CSQ=99) — verifique antena 4G");
        else if (csq < 10)
            ESP_LOGW(TAG,  "[DIAG] Sinal: FRACO (CSQ=%d)", csq);
        else
            ESP_LOGI(TAG,  "[DIAG] Sinal: BOM (CSQ=%d)", csq);
    }

    /* Modo de rede */
    if (enviar_at("AT+CNMP?", "OK", 3000, s_buf_at, sizeof(s_buf_at)))
        ESP_LOGI(TAG, "[DIAG] Modo rede: %s", s_buf_at);

    /* Registro LTE */
    if (enviar_at("AT+CEREG?", "OK", 3000, s_buf_at, sizeof(s_buf_at)))
        ESP_LOGI(TAG, "[DIAG] CEREG(LTE): %s", s_buf_at);

    ESP_LOGI(TAG, "[DIAG] ──────────────────────────────────────");

    /* ── Aguardar registro na rede (até ~3 min, com recuperação de resets) ── */
    bool registrado = false;
    for (int i = 0; i < 45; i++) {
        /* Espera "OK" completo — garante que o status após "+CREG:" está no buffer. */
        enviar_at("AT+CREG?", "OK", POSE_AT_TMO_MS, s_buf_at, sizeof(s_buf_at));
        ESP_LOGI(TAG, "[REDE] CREG(%d/45): %s", i + 1, s_buf_at);

        if (strstr(s_buf_at, "+CREG: 0,1") || strstr(s_buf_at, "+CREG: 0,5")) {
            registrado = true;
            ESP_LOGI(TAG, "[REDE] Registrado na rede.");
            break;
        }

        /* Detecta reset do modem: eco retorna quando o módulo reinicia por brownout.
         * Também detecta a sequência de boot "*ATREADY:" no buffer capturado. */
        bool modem_resetou = strstr(s_buf_at, "AT+CREG?") != nullptr ||
                             strstr(s_buf_at, "*ATREADY:")  != nullptr;
        if (modem_resetou) {
            /* Verifica se +CPIN: READY já está no buffer atual (boot já concluiu) */
            if (!strstr(s_buf_at, "+CPIN: READY")) {
                aguardar_boot_modem(20000);  /* 20 s para o boot completar */
            } else {
                ESP_LOGI(TAG, "[MODEM] Boot ja concluido (CPIN no buffer). Aguardando 3 s...");
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            /* Reinicia comunicação AT após o boot */
            enviar_at("ATE0", "OK", 5000, nullptr, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;  /* Não conta espera extra — próxima iteração imediatamente */
        }

        /* Status 0,2 = buscando rede — aguarda mais antes de re-tentar */
        if (strstr(s_buf_at, "+CREG: 0,2")) {
            ESP_LOGI(TAG, "[REDE] Buscando rede... aguardando 5 s.");
            vTaskDelay(pdMS_TO_TICKS(5000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    if (!registrado) { ESP_LOGE(TAG, "[REDE] Sem registro após ~3 min."); return false; }

    /* ── Configurar APN ── */
    {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", POSE_APN);
        if (!enviar_at(cmd, "OK", POSE_AT_TMO_MS, nullptr, 0)) {
            ESP_LOGW(TAG, "[REDE] Aviso: falha ao configurar APN (pode já estar OK).");
        }
    }

    /* ── Ativar contexto PDP ── */
    if (!enviar_at("AT+CGACT=1,1", "OK", POSE_AT_TMO_LONGO, nullptr, 0)) {
        if (!enviar_at("AT+CGACT?", "+CGACT: 1,1", POSE_AT_TMO_MS, nullptr, 0)) {
            ESP_LOGE(TAG, "[REDE] Contexto PDP não pôde ser ativado.");
            return false;
        }
        ESP_LOGI(TAG, "[REDE] Contexto PDP já estava ativo.");
    }

    /* ── Inicializar módulo GNSS ──
     * Manual A7670SA §24: aguardar "+CGNSSPWR: READY!" antes de operar o GNSS.
     * Reset limpo (CGNSSPWR=0) garantido antes de ligar para evitar estado residual. */
    ESP_LOGI(TAG, "[GNSS] Inicializando módulo GNSS...");
    enviar_at("AT+CGNSSPWR=0", "OK", 3000, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    if (enviar_at("AT+CGNSSPWR=1", "+CGNSSPWR: READY!", POSE_AT_TMO_GNSS, nullptr, 0)) {
        ESP_LOGI(TAG, "[GNSS] Módulo GNSS pronto.");
    } else {
        ESP_LOGW(TAG, "[GNSS] '+CGNSSPWR: READY!' não detectado. Continuando.");
    }

    /* Confirma modo GNSS ativo — útil para diagnóstico em campo.
     * Módulo SA (foreign): modo 3 = GPS+GLONASS+GALILEO+SBAS+QZSS (ideal para Brasil). */
    if (enviar_at("AT+CGNSSMODE?", "+CGNSSMODE:", 3000, s_buf_at, sizeof(s_buf_at))) {
        ESP_LOGI(TAG, "[GNSS] Modo ativo: %s", s_buf_at);
    } else {
        ESP_LOGW(TAG, "[GNSS] Nao foi possivel consultar modo GNSS.");
    }

    return true;
}

/**
 * @brief Executa o estado CONECTANDO_MQTT: inicia serviço, adquire cliente,
 *        conecta ao broker e assina hdrop/comando.
 * @return true se MQTT pronto para publicar; false em falha.
 */
static bool conectar_mqtt(void)
{
    ESP_LOGI(TAG, "[MQTT] Conectando ao broker MQTT...");

    /* AT+CMQTTSTART — deve preceder qualquer operação MQTT (manual §18.2.1) */
    if (!enviar_at("AT+CMQTTSTART", "+CMQTTSTART: 0", 12000, nullptr, 0)) {
        ESP_LOGW(TAG, "[MQTT] CMQTTSTART falhou (pode já estar ativo). Continuando.");
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    /* AT+CMQTTACCQ — adquire cliente MQTT índice 0 */
    {
        char cmd[80];
        snprintf(cmd, sizeof(cmd), "AT+CMQTTACCQ=0,\"%s\",0", POSE_MQTT_CLIENT_ID);
        if (!enviar_at(cmd, "OK", POSE_AT_TMO_MS, nullptr, 0)) {
            ESP_LOGE(TAG, "[MQTT] CMQTTACCQ falhou.");
            return false;
        }
    }

    /* AT+CMQTTCONNECT — conecta ao broker com clean_session=1 */
    {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "AT+CMQTTCONNECT=0,\"%s\",%d,1",
                 POSE_MQTT_BROKER, POSE_MQTT_KEEPALIVE);
        if (!enviar_at(cmd, "+CMQTTCONNECT: 0,0", POSE_AT_TMO_LONGO, nullptr, 0)) {
            ESP_LOGE(TAG, "[MQTT] CMQTTCONNECT falhou.");
            return false;
        }
    }
    ESP_LOGI(TAG, "[MQTT] Conectado ao broker.");
    vTaskDelay(pdMS_TO_TICKS(500));

    /* AT+CMQTTSUBTOPIC + AT+CMQTTSUB — assina hdrop/comando */
    {
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "AT+CMQTTSUBTOPIC=0,%d,1", POSE_TOPIC_SUB_LEN);
        if (!enviar_at(cmd, ">", POSE_AT_TMO_MS, nullptr, 0)) {
            ESP_LOGE(TAG, "[MQTT] CMQTTSUBTOPIC sem prompt."); return false;
        }
        if (!enviar_dado_prompt(POSE_MQTT_TOPIC_SUB, "OK", POSE_AT_TMO_MS)) {
            ESP_LOGE(TAG, "[MQTT] Envio do tópico SUB falhou."); return false;
        }
    }
    if (!enviar_at("AT+CMQTTSUB=0,1", "+CMQTTSUB: 0,0", POSE_AT_TMO_LONGO, nullptr, 0)) {
        ESP_LOGE(TAG, "[MQTT] Subscribe rejeitado.");
        return false;
    }

    ESP_LOGI(TAG, "[MQTT] Subscribe em '%s' confirmado.", POSE_MQTT_TOPIC_SUB);
    s_cont_erros_mqtt = 0;
    return true;
}

/* ================================================================
 * GNSS — leitura e parsing
 * ================================================================ */

/**
 * @brief Envia AT+CGNSSINFO e extrai a string bruta de posição.
 * @param saida   Buffer de destino para o conteúdo após "+CGNSSINFO:".
 * @param max_len Tamanho máximo de saida.
 * @return true se fix presente (primeiro campo numérico); false se sem fix.
 */
static bool ler_gnss_info(char *saida, size_t max_len)
{
    if (!enviar_at("AT+CGNSSINFO", "+CGNSSINFO:", POSE_AT_TMO_MS,
                   s_buf_at, sizeof(s_buf_at))) {
        /* Modem não respondeu ou retornou ERROR — módulo GNSS pode não estar pronto */
        ESP_LOGW(TAG, "[GNSS] AT+CGNSSINFO sem resposta/erro. Buf='%.80s'", s_buf_at);
        strncpy(saida, "gnss_err", max_len - 1);
        saida[max_len - 1] = '\0';
        return false;
    }

    char *p = strstr(s_buf_at, "+CGNSSINFO:");
    if (!p) {
        strncpy(saida, "parse_err", max_len - 1);
        saida[max_len - 1] = '\0';
        return false;
    }

    p += strlen("+CGNSSINFO:");
    while (*p == ' ') p++;

    char *fim = strpbrk(p, "\r\n");
    size_t len = fim ? (size_t)(fim - p) : strlen(p);
    if (len >= max_len) len = max_len - 1;

    strncpy(saida, p, len);
    saida[len] = '\0';

    /* Fix presente quando o primeiro campo é numérico (mode "2" ou "3") */
    return (saida[0] != '\0' && saida[0] != ',');
}

/* ================================================================
 * MQTT — publicação de telemetria
 * ================================================================ */

/**
 * @brief Publica JSON de telemetria em hdrop/raw com campo "pose".
 * @details Sequência AT (manual §18.2.10-12):
 *          AT+CMQTTTOPIC → prompt → tópico → OK
 *          AT+CMQTTPAYLOAD → prompt → payload → OK
 *          AT+CMQTTPUB=0,0,60 → +CMQTTPUB: 0,0
 * @param gnss_str String bruta do +CGNSSINFO (campo "g" do JSON).
 * @return true se publicação confirmada; false em qualquer falha AT.
 */
static bool publicar_telemetria(const char *gnss_str)
{
    hdrop_pose_t pose = pose_get();

    /* JSON expandido com campo "pose" conforme especificação da Etapa 3 */
    int pay_len = snprintf(s_buf_json, sizeof(s_buf_json),
        "{\"g\":\"%s\",\"pose\":{\"x\":%.2f,\"y\":%.2f,\"t\":%.3f,\"fix\":%d}}",
        gnss_str, pose.x, pose.y, pose.theta, pose.gnss_valid ? 1 : 0);

    if (pay_len <= 0 || pay_len >= (int)sizeof(s_buf_json)) {
        ESP_LOGE(TAG, "[PUB] JSON overflow (%d bytes).", pay_len);
        return false;
    }

    /* ── Tópico ── */
    {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+CMQTTTOPIC=0,%d", POSE_TOPIC_PUB_LEN);
        if (!enviar_at(cmd, ">", POSE_AT_TMO_MS, nullptr, 0))           return false;
        if (!enviar_dado_prompt(POSE_MQTT_TOPIC_PUB, "OK", POSE_AT_TMO_MS)) return false;
    }

    /* ── Payload ── */
    {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+CMQTTPAYLOAD=0,%d", pay_len);
        if (!enviar_at(cmd, ">", POSE_AT_TMO_MS, nullptr, 0))          return false;
        if (!enviar_dado_prompt(s_buf_json, "OK", POSE_AT_TMO_MS))      return false;
    }

    /* ── Publicar QoS=0 (sem ACK do servidor — compatível com 2 Hz) ── */
    if (!enviar_at("AT+CMQTTPUB=0,0,60", "+CMQTTPUB: 0,0", POSE_AT_TMO_PUB, nullptr, 0)) {
        return false;
    }

    ESP_LOGI(TAG, "[PUB] → %s", s_buf_json);
    return true;
}

/* ================================================================
 * URCs — mensagens assíncronas do modem
 * ================================================================ */

/**
 * @brief Lê bytes disponíveis no UART sem bloquear e acumula em s_buf_urc.
 * @details Chamada no início de cada iteração do estado OPERANDO.
 *          Mantém s_buf_urc abaixo de 2 KB (trim circular quando próximo do limite).
 */
static void capturar_urcs(void)
{
    uint8_t byte;
    size_t  len = strlen(s_buf_urc);

    while (uart_read_bytes(UART_NUM_2, &byte, 1, 0) == 1) {
        if (len < sizeof(s_buf_urc) - 1) {
            s_buf_urc[len++] = (char)byte;
            s_buf_urc[len]   = '\0';
        } else {
            /* Descarta a primeira metade para dar espaço — preserva URCs mais recentes */
            memmove(s_buf_urc, s_buf_urc + 1024, 1024);
            s_buf_urc[1024] = '\0';
            len = 1024;
        }
    }
}

/**
 * @brief Processa s_buf_urc buscando mensagens de hdrop/comando e eventos de conexão.
 * @details Formato URC de mensagem recebida (manual A7670SA §18.4):
 *            +CMQTTRXSTART: 0,<topic_len>,<payload_len>
 *            +CMQTTRXTOPIC: 0,<sub_len> → <topic>
 *            +CMQTTRXPAYLOAD: 0,<sub_len> → <payload>
 *            +CMQTTRXEND: 0
 *          Payloads JSON são impressos com prefixo "ROTA:" para processamento externo.
 */
static void processar_urcs(void)
{
    if (strlen(s_buf_urc) == 0) return;

    /* Perda passiva de conexão: força reconexão MQTT no próximo ciclo da FSM */
    if (strstr(s_buf_urc, "+CMQTTCONNLOST:")) {
        ESP_LOGW(TAG, "[URC] +CMQTTCONNLOST → forçar reconexão MQTT.");
        s_cont_erros_mqtt = POSE_MAX_ERROS_MQTT;
        s_buf_urc[0] = '\0';
        return;
    }

    char *rx_start = strstr(s_buf_urc, "+CMQTTRXSTART:");
    char *rx_end   = strstr(s_buf_urc, "+CMQTTRXEND:");
    if (!rx_start || !rx_end) return;

    char *pay_hdr = strstr(rx_start, "+CMQTTRXPAYLOAD:");
    if (!pay_hdr) { s_buf_urc[0] = '\0'; return; }

    /* Payload começa após o '\n' do header de +CMQTTRXPAYLOAD */
    char *newline = strchr(pay_hdr, '\n');
    if (!newline) return;  /* pacote incompleto — aguardar mais dados */

    char   payload[256] = {0};
    size_t len = (size_t)(rx_end - (newline + 1));
    if (len >= sizeof(payload)) len = sizeof(payload) - 1;
    strncpy(payload, newline + 1, len);
    payload[len] = '\0';

    /* Remove espaços e quebras de linha ao redor do payload */
    char *p = payload;
    while (*p == '\r' || *p == '\n' || *p == ' ') p++;
    char *end = p + strlen(p) - 1;
    while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = '\0';

    if (strlen(p) > 0) {
        ESP_LOGI(TAG, "[CMD] Recebido em hdrop/comando: %s", p);
        /* Prefixo "ROTA:" sinaliza ao backend (via log serial) coordenadas de rota */
        if (p[0] == '{') {
            ESP_LOGI(TAG, "ROTA:%s", p);
        }
    }

    /* Descarta URCs já processadas, preserva dados após +CMQTTRXEND: */
    char *fim_urc = strchr(rx_end + strlen("+CMQTTRXEND:"), '\n');
    if (fim_urc) {
        memmove(s_buf_urc, fim_urc + 1, strlen(fim_urc + 1) + 1);
    } else {
        s_buf_urc[0] = '\0';
    }
}

/* ================================================================
 * Task principal da FSM
 * ================================================================ */

/**
 * @brief Task FreeRTOS que executa a FSM de conectividade e telemetria.
 * @details Prioridade 4 (abaixo de heading_task=5). Stack de 8 KB para
 *          acomodar buffers de AT e JSON na pilha das funções locais.
 *          No estado OPERANDO:
 *            - Lê GNSS via AT+CGNSSINFO
 *            - Atualiza pose (pose_update_gnss + pose_update_heading)
 *            - Publica JSON a 2 Hz
 *            - Processa URCs de hdrop/comando
 */
static void pose_task(void *pvParameters)
{
    (void)pvParameters;

    /* Aguarda o A7670SA passar o pico de corrente de registro na rede celular.
     * O modem puxa ~1-2 A durante os primeiros 8-12 s após power-on, o que
     * pode derrubar o rail 3.3V do ESP32 a ponto de causar reset de hardware.
     * Este delay deixa o pico passar antes de qualquer comunicação AT. */
    ESP_LOGI(TAG, "[MODEM] Aguardando estabilizacao do A7670SA (12 s)...");
    vTaskDelay(pdMS_TO_TICKS(12000));
    ESP_LOGI(TAG, "[MODEM] Iniciando comunicacao AT.");

    fsm_estado_t estado          = FSM_CONECTANDO_REDE;
    uint32_t     ultima_telemetria = 0;

    while (true) {
        switch (estado) {

            /* ── Estado 1: conecta rede celular e inicializa GNSS ── */
            case FSM_CONECTANDO_REDE:
                ESP_LOGI(TAG, "[FSM] ▶ CONECTANDO_REDE");
                if (conectar_rede()) {
                    /* ── A-GPS: baixar efemérides via LTE ──────────────
                     * Reduz cold start GNSS de ~60-90 s para ~3-5 s.
                     * Executado uma vez por conexão. Falha não é fatal. */
                    ESP_LOGI(TAG, "[GNSS] Baixando efemérides A-GPS via LTE...");
                    if (enviar_at("AT+CAGPS", "+AGPS: success", 15000, nullptr, 0)) {
                        ESP_LOGI(TAG, "[GNSS] A-GPS OK — fix GPS rapido garantido.");
                    } else {
                        ESP_LOGW(TAG, "[GNSS] A-GPS falhou — fix normal ~60 s.");
                    }
                    ESP_LOGI(TAG, "[FSM] ✓ Rede pronta → CONECTANDO_MQTT");
                    estado = FSM_CONECTANDO_MQTT;
                } else {
                    ESP_LOGE(TAG, "[FSM] ✗ Falha de rede. Aguardando 15 s...");
                    vTaskDelay(pdMS_TO_TICKS(15000));
                }
                break;

            /* ── Estado 2: inicia MQTT e assina tópico de comandos ── */
            case FSM_CONECTANDO_MQTT:
                ESP_LOGI(TAG, "[FSM] ▶ CONECTANDO_MQTT");
                liberar_sessao_mqtt();
                vTaskDelay(pdMS_TO_TICKS(1000));

                if (conectar_mqtt()) {
                    ESP_LOGI(TAG, "[FSM] ✓ MQTT pronto → OPERANDO");
                    g_mqtt_operando = true;
                    estado = FSM_OPERANDO;
                    /* Agenda primeiro envio imediatamente */
                    ultima_telemetria = agora_ms() - POSE_TELEMETRIA_MS;
                } else {
                    ESP_LOGE(TAG, "[FSM] ✗ Falha MQTT. Aguardando 8 s...");
                    vTaskDelay(pdMS_TO_TICKS(8000));
                    /* Verifica se PDP ainda está ativo antes de retry MQTT */
                    if (!enviar_at("AT+CGACT?", "+CGACT: 1,1", POSE_AT_TMO_MS, nullptr, 0)) {
                        ESP_LOGE(TAG, "[FSM] PDP perdido → CONECTANDO_REDE");
                        estado = FSM_CONECTANDO_REDE;
                    }
                    /* Caso contrário permanece em CONECTANDO_MQTT (retry automático) */
                }
                break;

            /* ── Estado 3: loop de telemetria 2 Hz + URCs ── */
            case FSM_OPERANDO:
            {
                capturar_urcs();
                processar_urcs();

                /* Verifica necessidade de reconexão MQTT ao atingir limite de erros */
                if (s_cont_erros_mqtt >= POSE_MAX_ERROS_MQTT) {
                    if (!mqtt_conectado()) {
                        ESP_LOGE(TAG, "[FSM] MQTT desconectado → CONECTANDO_MQTT");
                        g_mqtt_operando = false;
                        estado = FSM_CONECTANDO_MQTT;
                        break;
                    }
                    s_cont_erros_mqtt = 0;
                }

                /* Controle de temporização 2 Hz via timestamp de tick */
                if (agora_ms() - ultima_telemetria < POSE_TELEMETRIA_MS) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    break;
                }
                ultima_telemetria = agora_ms();

                /* Leitura GNSS via AT */
                bool tem_fix = ler_gnss_info(s_buf_gnss, sizeof(s_buf_gnss));
                if (tem_fix) {
                    ESP_LOGI(TAG, "[GNSS] Fix: %s", s_buf_gnss);
                    pose_update_gnss(s_buf_gnss);
                } else {
                    /* LOGI (não LOGD) para que fique visível no monitor serial */
                    ESP_LOGI(TAG, "[GNSS] Sem fix. Raw='%s'", s_buf_gnss);
                }

                /* Heading do magnetômetro — chamada direta é thread-safe no driver I2C */
                float theta = 0.0f;
                if (heading_read(&theta)) {
                    pose_update_heading(theta);
                }

                /* Publicação */
                if (publicar_telemetria(s_buf_gnss)) {
                    s_cont_erros_mqtt = 0;
                } else {
                    s_cont_erros_mqtt++;
                    ESP_LOGW(TAG, "[FSM] Erro MQTT consecutivo #%d", s_cont_erros_mqtt);
                }

                break;
            }

        } /* switch */
    } /* while */
}

/* ================================================================
 * Conversão de coordenadas — funções auxiliares
 * ================================================================ */

/**
 * @brief Converte string DDMM.MMMMMM + hemisfério para graus decimais.
 * @details Formato do A7670SA (ex: "1547.123456" com 'S' → -15.78540760°):
 *            graus = DD (parte inteira de DDMM / 100)
 *            minutos = DDMM - DD×100
 *            resultado = graus + minutos / 60
 *          S e W negam o resultado.
 * @param ddmm      String com valor DDMM.MMMMMM (null-terminated).
 * @param hemisferio 'N', 'S', 'E' ou 'W'.
 * @return Graus decimais com sinal.
 */
static float ddmm_para_graus(const char *ddmm, char hemisferio)
{
    float val     = atof(ddmm);
    int   graus   = (int)(val / 100.0f);
    float minutos = val - (float)(graus * 100);
    float resultado = (float)graus + minutos / 60.0f;
    if (hemisferio == 'S' || hemisferio == 'W') resultado = -resultado;
    return resultado;
}

/* ================================================================
 * Dead reckoning — propagação de pose entre fixes GNSS
 * ================================================================ */

/**
 * @brief Estima velocidade linear do veículo a partir dos últimos PWMs comandados.
 * @details v_norm = ((pwm_esq − 1520) + (pwm_dir − 1520)) / (2 × 480)
 *          A média dos desvios dos dois motores é a componente de avanço líquido.
 *          Ignora slip, corrente e resistência da água — calibrar V_MAX_MS em campo.
 * @return Velocidade linear estimada em m/s.
 */
static float calcular_v_dos_pwms(void)
{
    int   pwm_esq = motor_get_last_pwm_left();
    int   pwm_dir = motor_get_last_pwm_right();
    float v_norm  = ((float)(pwm_esq - MOTOR_PONTO_MORTO_US) +
                     (float)(pwm_dir - MOTOR_PONTO_MORTO_US)) /
                    (2.0f * (float)MOTOR_DELTA_MAX_US);
    return v_norm * V_MAX_MS;
}

/**
 * @brief Propaga x e y da pose via integração Euler com θ do cache da pose.
 * @details θ é lido de g_pose.theta, que a heading_task mantém atualizado a 10 Hz
 *          via pose_update_heading(). Isso evita qualquer chamada I2C bloqueante
 *          dentro do callback do esp_timer (stack de apenas 4 KB na task do timer).
 *          O GNSS corrige x e y a ~1 Hz sobrescrevendo sem suavização.
 *          Thread-safe — toda a leitura e escrita ocorre dentro do mesmo mutex.
 * @param v_linear Velocidade linear em m/s (de calcular_v_dos_pwms).
 * @param dt_s     Período de integração em segundos (0.1 s a 10 Hz).
 */
static void pose_dead_reckon(float v_linear, float dt_s)
{
    if (xSemaphoreTake(g_pose_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        /* θ já atualizado pela heading_task via pose_update_heading() — sem I2C aqui */
        float theta = g_pose.theta;
        g_pose.x += v_linear * cosf(theta) * dt_s;
        g_pose.y += v_linear * sinf(theta) * dt_s;
        xSemaphoreGive(g_pose_mutex);
    }
}

/**
 * @brief Callback do timer periódico de dead reckoning (10 Hz).
 * @details Executado pelo esp_timer a cada 100 ms. Estima v dos PWMs atuais
 *          e propaga a pose. A correção GNSS é aplicada assincronamente em
 *          pose_update_gnss() ao chegar cada fix (~1 Hz).
 * @param arg Não utilizado.
 */
static void pose_dr_cb(void *arg)
{
    (void)arg;
    float v_est = calcular_v_dos_pwms();
    pose_dead_reckon(v_est, 0.1f);
}

/* ================================================================
 * Implementação da API pública
 * ================================================================ */

esp_err_t pose_init(void)
{
    /* ----------------------------------------------------------------
     * Configuração do UART2 para comunicação com o A7670SA.
     * Sem controle de fluxo hardware — modem e ESP32 operam com RTS/CTS
     * desabilitados neste projeto.
     * ---------------------------------------------------------------- */
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate  = POSE_UART_BAUD;
    uart_cfg.data_bits  = UART_DATA_8_BITS;
    uart_cfg.parity     = UART_PARITY_DISABLE;
    uart_cfg.stop_bits  = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t ret = uart_param_config(UART_NUM_2, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar UART2: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(UART_NUM_2, POSE_PINO_TX, POSE_PINO_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar pinos UART2: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Buffers de RX e TX de 1024 bytes — suficiente para respostas AT e URCs */
    ret = uart_driver_install(UART_NUM_2, 1024, 1024, 0, nullptr, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao instalar driver UART2: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Mutex protege g_pose contra leituras simultâneas de pose_get()
       e escritas de pose_update_gnss() / pose_update_heading() */
    g_pose_mutex = xSemaphoreCreateMutex();
    if (g_pose_mutex == nullptr) {
        ESP_LOGE(TAG, "Falha ao criar mutex de pose.");
        return ESP_FAIL;
    }

    /* ----------------------------------------------------------------
     * Timer periódico de dead reckoning a 10 Hz (100 ms = 100 000 µs).
     * Propaga x e y com heading do magnetômetro entre fixes GNSS (~1 Hz).
     * Criado antes da pose_task para que o predictor já esteja ativo
     * quando a FSM entrar em OPERANDO e o primeiro fix GNSS chegar.
     * ---------------------------------------------------------------- */
    {
        esp_timer_create_args_t dr_args = {};
        dr_args.callback        = pose_dr_cb;
        dr_args.arg             = nullptr;
        dr_args.dispatch_method = ESP_TIMER_TASK;
        dr_args.name            = "pose_dr";

        ret = esp_timer_create(&dr_args, &g_dr_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao criar timer DR: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = esp_timer_start_periodic(g_dr_timer, 100000);  /* 100 ms em µs */
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao iniciar timer DR: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Timer de dead reckoning iniciado (10 Hz).");
    }

    /* ----------------------------------------------------------------
     * Stack de 12 KB: 8 KB originais + 4 KB de margem para coexistência
     * com WiFi (esp_timer task de alta prioridade + lwIP TCP/IP stack).
     * Sem essa margem, o ESP32 reinicia por stack overflow quando todas
     * as tasks estão ativas simultaneamente.
     * ---------------------------------------------------------------- */
    BaseType_t task_ret = xTaskCreate(
        pose_task,
        "pose_task",
        12288,
        nullptr,
        4,
        nullptr
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar pose_task.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Componente pose inicializado (UART2 %d baud, task FSM).", POSE_UART_BAUD);
    return ESP_OK;
}

bool pose_update_gnss(const char *cgnssinfo_str)
{
    if (!cgnssinfo_str || cgnssinfo_str[0] == '\0') return false;

    /* ----------------------------------------------------------------
     * Copia a string pois strtok_r modifica o buffer in-place.
     * O cgnssinfo_str é o conteúdo após "+CGNSSINFO:" (sem prefixo).
     * Formato: mode,GPS-SVs,GLONASS-SVs,BEIDOU-SVs,lat,N/S,lon,E/W,...
     * ---------------------------------------------------------------- */
    char buf[160];
    strncpy(buf, cgnssinfo_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr;

    /* Campo 0: mode — fix válido apenas com "2" (2D) ou "3" (3D) */
    char *tok = strtok_r(buf, ",", &saveptr);
    if (!tok || (tok[0] != '2' && tok[0] != '3')) return false;

    /* Campos 1-3: SVs (GPS, GLONASS, BEIDOU) — ignorados para a pose */
    strtok_r(nullptr, ",", &saveptr);
    strtok_r(nullptr, ",", &saveptr);
    strtok_r(nullptr, ",", &saveptr);

    /* Campo 4: latitude em DDMM.MMMMMM */
    tok = strtok_r(nullptr, ",", &saveptr);
    if (!tok) return false;
    char lat_str[20];
    strncpy(lat_str, tok, sizeof(lat_str) - 1);
    lat_str[sizeof(lat_str) - 1] = '\0';

    /* Campo 5: hemisfério N/S */
    tok = strtok_r(nullptr, ",", &saveptr);
    if (!tok) return false;
    char ns = tok[0];

    /* Campo 6: longitude em DDDMM.MMMMMM */
    tok = strtok_r(nullptr, ",", &saveptr);
    if (!tok) return false;
    char lon_str[20];
    strncpy(lon_str, tok, sizeof(lon_str) - 1);
    lon_str[sizeof(lon_str) - 1] = '\0';

    /* Campo 7: hemisfério E/W */
    tok = strtok_r(nullptr, ",", &saveptr);
    if (!tok) return false;
    char ew = tok[0];

    float lat_deg = ddmm_para_graus(lat_str, ns);
    float lon_deg = ddmm_para_graus(lon_str, ew);

    /* ----------------------------------------------------------------
     * Define ponto de referência no primeiro fix válido após o boot.
     * Todos os deslocamentos posteriores são calculados relativamente a ele.
     * ---------------------------------------------------------------- */
    if (!g_ref_set) {
        g_lat0_deg = lat_deg;
        g_lon0_deg = lon_deg;
        g_ref_set  = true;
        ESP_LOGI(TAG, "Ponto de referência definido: lat=%.7f lon=%.7f", lat_deg, lon_deg);
    }

    /* ----------------------------------------------------------------
     * Conversão geodésica → XY local (Terra plana).
     * Válida para deslocamentos < 50 km (erro < 0.5% por projeção equiretangular).
     *   pose.y = Δlat_rad × R
     *   pose.x = Δlon_rad × cos(lat₀) × R       R = 6371000 m
     * ---------------------------------------------------------------- */
    float lat0_rad  = g_lat0_deg * (float)M_PI / 180.0f;
    float delta_lat = (lat_deg - g_lat0_deg) * (float)M_PI / 180.0f;
    float delta_lon = (lon_deg - g_lon0_deg) * (float)M_PI / 180.0f;

    float x_novo = delta_lon * cosf(lat0_rad) * 6371000.0f;
    float y_novo = delta_lat * 6371000.0f;

    float dr_err = 0.0f;
    if (xSemaphoreTake(g_pose_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        /* Distância entre estimativa DR acumulada e fix GNSS — qualidade do predictor */
        float ex    = x_novo - g_pose.x;
        float ey    = y_novo - g_pose.y;
        dr_err      = sqrtf(ex * ex + ey * ey);

        g_pose.x            = x_novo;
        g_pose.y            = y_novo;
        g_pose.gnss_valid   = true;
        g_pose.last_gnss_ms = agora_ms();
        xSemaphoreGive(g_pose_mutex);
    }

    ESP_LOGI(TAG, "Fix corrigido: x=%.2f y=%.2f err_dr=%.2f m", x_novo, y_novo, dr_err);
    ESP_LOGD(TAG, "Pose GNSS: x=%.2f m y=%.2f m (lat=%.7f lon=%.7f)",
             x_novo, y_novo, lat_deg, lon_deg);

    return true;
}

void pose_update_heading(float theta_rad)
{
    if (xSemaphoreTake(g_pose_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_pose.theta = theta_rad;
        xSemaphoreGive(g_pose_mutex);
    }
}

hdrop_pose_t pose_get(void)
{
    hdrop_pose_t copia = {0.0f, 0.0f, 0.0f, false, 0};

    if (g_pose_mutex != nullptr &&
        xSemaphoreTake(g_pose_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        copia = g_pose;
        xSemaphoreGive(g_pose_mutex);
    } else {
        ESP_LOGW(TAG, "pose_get: timeout no mutex — retornando cópia possivelmente desatualizada.");
    }

    return copia;
}

bool pose_mqtt_connected(void)
{
    return g_mqtt_operando;
}
