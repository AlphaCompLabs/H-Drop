/**
 * @file hdrop_heading.cpp
 * @brief Estimação de heading via QMC5883L e controle proporcional de orientação.
 * @details Implementa leitura I2C do magnetômetro QMC5883L, calibração hard-iron
 *          com persistência em NVS e controlador P de heading integrado à task
 *          FreeRTOS. A inicialização do sensor replica a sequência do firmware de
 *          referência (hdrop_barco_autonomo.ino): soft reset via REG_SET, depois
 *          configuração de CTRL1 com modo contínuo a 200 Hz.
 *
 *          Cálculo de heading:
 *            θ = atan2f(y_cal, x_cal)      → (-π, π]
 *            if θ < 0: θ += 2π             → [0, 2π)
 *
 *          Erro angular com wrap (evita descontinuidade em 0/360°):
 *            eθ = atan2f(sinf(θd - θ), cosf(θd - θ))  → (-π, π)
 *
 * @hardware  QMC5883L: I2C_NUM_0 | SDA=GPIO21 | SCL=GPIO22 | Endereço=0x0D
 * @depends   driver/i2c.h, nvs_flash.h, nvs.h, freertos, hdrop_motor
 */

#include "hdrop_heading.h"
#include "hdrop_motor.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <cmath>

/** Tag de log do módulo. */
static const char *TAG = "HEADING";

/* ================================================================
 * Estado interno do módulo
 * ================================================================ */

/** Coeficientes de calibração hard-iron carregados da NVS ou zerados no boot. */
static hdrop_cal_t g_cal = {0, 0, 0, 0};

/** Mutex que protege g_current_heading e g_heading_valid entre a heading_task
 *  e chamadas externas a heading_read(). */
static SemaphoreHandle_t g_heading_mutex = nullptr;

/** Handle da heading_task para suspensão durante calibração. */
static TaskHandle_t g_heading_task_handle = nullptr;

/** Heading atual em radianos [0, 2π), atualizado a 10 Hz pela heading_task. */
static float g_current_heading_rad = 0.0f;

/** Indica se a última leitura I2C foi válida. */
static bool g_heading_valid = false;

/** Heading alvo do controlador P, em radianos. Sentinela -1.0f = desabilitado. */
static volatile float g_target_heading_rad = -1.0f;

/** Habilita ou desabilita o controlador de heading dentro da task. */
static volatile bool g_hold_enabled = false;

/* ================================================================
 * Funções internas (static)
 * ================================================================ */

/**
 * @brief Escreve um byte em um registrador do QMC5883L via I2C.
 * @param reg   Endereço do registrador.
 * @param value Valor a escrever.
 * @return ESP_OK em sucesso, ou código de erro ESP-IDF em falha de barramento.
 */
/**
 * @brief Recupera o barramento I2C caso um slave esteja travado segurando SDA baixo.
 * @details Ocorre quando o ESP32 reseta no meio de uma transação I2C (ex: brownout).
 *          O slave fica esperando mais clocks para completar o byte em curso.
 *          Solução: pulsar SCL 9 vezes como GPIO puro para liberar o slave,
 *          depois enviar condição de STOP. Conforme I2C spec §3.1.16.
 */
static void i2c_bus_recovery(void)
{
    /* Configura SDA e SCL como saída open-drain com pull-up — sem o driver I2C */
    gpio_config_t io = {};
    io.pin_bit_mask  = (1ULL << HEADING_PINO_SDA) | (1ULL << HEADING_PINO_SCL);
    io.mode          = GPIO_MODE_OUTPUT_OD;
    io.pull_up_en    = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    gpio_set_level((gpio_num_t)HEADING_PINO_SDA, 1);
    gpio_set_level((gpio_num_t)HEADING_PINO_SCL, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 9 pulsos de clock — libera qualquer slave travado no meio de um byte */
    for (int i = 0; i < 9; i++) {
        gpio_set_level((gpio_num_t)HEADING_PINO_SCL, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level((gpio_num_t)HEADING_PINO_SCL, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* Condição de STOP: SDA sobe enquanto SCL está alto */
    gpio_set_level((gpio_num_t)HEADING_PINO_SDA, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level((gpio_num_t)HEADING_PINO_SCL, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level((gpio_num_t)HEADING_PINO_SDA, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGW(TAG, "[I2C] Bus recovery concluida (9 pulsos SCL).");
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_write_to_device(
        I2C_NUM_0, QMC5883L_ADDR,
        buf, sizeof(buf),
        pdMS_TO_TICKS(100)
    );
}

/**
 * @brief Lê n bytes a partir de um registrador do QMC5883L via I2C.
 * @param reg   Registrador inicial de leitura.
 * @param data  Buffer de destino.
 * @param len   Número de bytes a ler.
 * @return ESP_OK em sucesso, ou código de erro ESP-IDF em falha de barramento.
 */
static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(
        I2C_NUM_0, QMC5883L_ADDR,
        &reg, 1,
        data, len,
        pdMS_TO_TICKS(100)
    );
}

/**
 * @brief Inicializa o QMC5883L com soft reset e CTRL1 conforme especificação.
 * @details Replica a sequência de qmc_init() do firmware de referência:
 *            1. Soft reset: REG_SET (0x0B) = 0x01
 *            2. Aguarda 10 ms para estabilização
 *            3. CTRL1 (0x09) = 0b00011101 → 200 Hz, ±8G, OSR=512, Contínuo
 * @return ESP_OK em sucesso; ESP_FAIL se alguma escrita I2C falhar.
 */
static esp_err_t qmc_init(void)
{
    /* ----------------------------------------------------------------
     * Soft reset: necessário para garantir estado inicial conhecido,
     * independente do estado anterior do sensor (ex: após reboot quente).
     * ---------------------------------------------------------------- */
    esp_err_t ret = write_reg(QMC5883L_REG_SET, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Soft reset QMC5883L falhou: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ----------------------------------------------------------------
     * CTRL1 = 0b00011101:
     *   [7:6] OSR  = 00 → 512 amostras (máxima filtragem de ruído)
     *   [5:4] RNG  = 01 → ±8 Gauss (adequado para ambiente com interferências)
     *   [3:2] ODR  = 11 → 200 Hz
     *   [1:0] MODE = 01 → Modo Contínuo
     * ---------------------------------------------------------------- */
    ret = write_reg(QMC5883L_REG_CTRL1, QMC5883L_CTRL1_VAL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Configuração CTRL1 QMC5883L falhou: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "QMC5883L inicializado (200 Hz, ±8G, OSR=512, Contínuo).");
    return ESP_OK;
}

/* ================================================================
 * INA226 — monitor de tensão da bateria
 * ================================================================ */

static bool s_ina226_ok = false;

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "[I2C SCAN] Varrendo enderecos 0x08 a 0x77...");
    int encontrados = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        uint8_t dummy;
        esp_err_t r = i2c_master_write_read_device(
            I2C_NUM_0, addr, &dummy, 0, &dummy, 1, pdMS_TO_TICKS(50));
        if (r == ESP_OK || r == ESP_ERR_TIMEOUT) {
            /* ESP_OK = dispositivo respondeu com ACK */
            if (r == ESP_OK) {
                ESP_LOGI(TAG, "[I2C SCAN] Encontrado: 0x%02X", addr);
                encontrados++;
            }
        }
    }
    if (encontrados == 0)
        ESP_LOGW(TAG, "[I2C SCAN] Nenhum dispositivo encontrado alem do QMC5883L.");
    else
        ESP_LOGI(TAG, "[I2C SCAN] Total: %d dispositivo(s).", encontrados);
}

static void ina226_init(void)
{
    /* Scan do barramento para diagnóstico — mostra todos os endereços ativos */
    i2c_scan();

    /* Config: 16 médias, 1.1 ms/conversão, modo contínuo shunt+bus
     * Registro 0x00 = 0x4527:
     *   [11:9] AVG=010 (16 amostras), [8:6]=100, [5:3]=100, [2:0]=111 */
    uint8_t cfg[3] = { INA226_REG_CONFIG, 0x45, 0x27 };
    esp_err_t ret = i2c_master_write_to_device(
        I2C_NUM_0, INA226_ADDR, cfg, sizeof(cfg), pdMS_TO_TICKS(200));
    if (ret == ESP_OK) {
        s_ina226_ok = true;
        ESP_LOGI(TAG, "[INA226] Monitor de bateria inicializado (endereco 0x%02X).", INA226_ADDR);
    } else {
        ESP_LOGE(TAG, "[INA226] Nao respondeu em 0x%02X. Veja o scan acima.", INA226_ADDR);
    }
}

int32_t heading_battery_mv(void)
{
    if (!s_ina226_ok) return -1;

    uint8_t reg = INA226_REG_BUS_VOLTAGE;
    uint8_t data[2] = {};
    esp_err_t ret = i2c_master_write_read_device(
        I2C_NUM_0, INA226_ADDR, &reg, 1, data, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return -1;

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    /* LSB = 1.25 mV → multiplicar por 5 e dividir por 4 para evitar float */
    return (int32_t)raw * 5 / 4;
}

/**
 * @brief Carrega coeficientes de calibração hard-iron da NVS.
 * @details Em caso de ausência das chaves (primeiro boot), mantém g_cal zerado
 *          (sem correção de offset). Não falha — calibração zero é válida.
 * @param handle Handle NVS já aberto para leitura.
 */
static void load_cal_from_nvs(nvs_handle_t handle)
{
    /* Tentativa de leitura silenciosa; ESP_ERR_NVS_NOT_FOUND é esperado na
       primeira inicialização e não representa erro — calibração permanece zero. */
    nvs_get_i16(handle, "x_min", &g_cal.x_min);
    nvs_get_i16(handle, "x_max", &g_cal.x_max);
    nvs_get_i16(handle, "y_min", &g_cal.y_min);
    nvs_get_i16(handle, "y_max", &g_cal.y_max);

    ESP_LOGI(TAG, "Calibração NVS: x=[%d,%d] y=[%d,%d]",
             g_cal.x_min, g_cal.x_max, g_cal.y_min, g_cal.y_max);
}

/**
 * @brief Calcula heading em radianos a partir de leituras brutas calibradas.
 * @details Aplica offset hard-iron, calcula atan2 e normaliza para [0, 2π).
 * @param x_raw Leitura bruta do eixo X do QMC5883L.
 * @param y_raw Leitura bruta do eixo Y do QMC5883L.
 * @return Heading em radianos no intervalo [0, 2π).
 */
static float compute_heading(int16_t x_raw, int16_t y_raw)
{
    /* ----------------------------------------------------------------
     * Calibração hard-iron: subtrai o centro do elipsoide de saturação.
     * Campos magnéticos estáticos (carcaça metálica, baterias) deslocam
     * sistematicamente a leitura — este offset compensa esse desvio.
     * ---------------------------------------------------------------- */
    float x_cal = (float)x_raw - ((float)(g_cal.x_max + g_cal.x_min) * 0.5f);
    float y_cal = (float)y_raw - ((float)(g_cal.y_max + g_cal.y_min) * 0.5f);

    /* ----------------------------------------------------------------
     * atan2f usa sinais separados de y e x para cobrir 360° completos.
     * atan(y/x) sozinho retorna apenas (-π/2, π/2) — 180° — e não
     * distingue vetores em quadrantes opostos (frente vs. trás do veículo).
     * ---------------------------------------------------------------- */
    float theta = atan2f(y_cal, x_cal);

    /* Normaliza para [0, 2π) para representação consistente de bússola */
    if (theta < 0.0f) {
        theta += 2.0f * (float)M_PI;
    }

    return theta;
}

/**
 * @brief Task FreeRTOS de estimação e controle de heading a 10 Hz.
 * @details Ciclo a cada 100 ms:
 *            1. Lê heading via I2C (heading_read).
 *            2. Atualiza g_current_heading_rad sob mutex.
 *            3. Se g_hold_enabled: calcula erro angular com wrap e aplica motor_mix.
 * @param pvParameters Não utilizado.
 */
static void heading_task(void *pvParameters)
{
    (void)pvParameters;

    const TickType_t periodo = pdMS_TO_TICKS(1000 / HEADING_TASK_RATE_HZ);
    TickType_t ultimo_wake   = xTaskGetTickCount();

    while (true) {
        float theta = 0.0f;
        bool  valido = heading_read(&theta);

        /* Atualiza estado compartilhado sob mutex */
        if (xSemaphoreTake(g_heading_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_current_heading_rad = theta;
            g_heading_valid       = valido;
            xSemaphoreGive(g_heading_mutex);
        }

        /* ----------------------------------------------------------------
         * Controlador P de heading: ativo apenas se g_hold_enabled e leitura
         * válida. O erro é calculado com wrap via atan2(sin, cos) para evitar
         * descontinuidade quando o ângulo cruza 0°/360°.
         * ---------------------------------------------------------------- */
        if (g_hold_enabled && valido) {
            float target = g_target_heading_rad;
            float diff   = target - theta;

            /* Normaliza para (-π, π) via atan2f(sin, cos) para evitar
               descontinuidade quando o ângulo cruza 0°/360° */
            float e_theta = atan2f(sinf(diff), cosf(diff));

            float omega_cmd = HEADING_KP * e_theta;

            motor_mix(HEADING_V_HOLD_MS, omega_cmd, HEADING_L_BITOLA_M);

            ESP_LOGD(TAG, "hold: θ=%.2f rad | alvo=%.2f rad | e=%.3f rad | ω=%.3f rad/s",
                     theta, target, e_theta, omega_cmd);
        }

        vTaskDelayUntil(&ultimo_wake, periodo);
    }
}

/* ================================================================
 * Implementação da API pública
 * ================================================================ */

esp_err_t heading_init(void)
{
    /* ----------------------------------------------------------------
     * Inicialização NVS: padrão recomendado pelo ESP-IDF para tratar
     * partição corrompida ou versão incompatível após atualização de firmware.
     * ---------------------------------------------------------------- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrompida ou versão incompatível — apagando partição.");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar NVS: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* ----------------------------------------------------------------
     * Configuração do barramento I2C_NUM_0 como master a 400 kHz.
     * Pull-ups internos habilitados para dispensar resistores externos em
     * protótipos, embora resistores externos (4.7 kΩ) sejam preferíveis em
     * produção para melhor integridade de sinal a 400 kHz.
     * ---------------------------------------------------------------- */
    i2c_config_t i2c_cfg = {};
    i2c_cfg.mode             = I2C_MODE_MASTER;
    i2c_cfg.sda_io_num       = HEADING_PINO_SDA;
    i2c_cfg.scl_io_num       = HEADING_PINO_SCL;
    i2c_cfg.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    i2c_cfg.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    i2c_cfg.master.clk_speed = HEADING_I2C_FREQ_HZ;

    /* Recupera o barramento antes de instalar o driver — libera SDA caso o
     * QMC5883L tenha ficado travado por um reset/brownout no meio de uma leitura. */
    i2c_bus_recovery();

    ret = i2c_param_config(I2C_NUM_0, &i2c_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar parâmetros I2C: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao instalar driver I2C: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Inicializa sensor com soft reset e CTRL1 — até 3 tentativas com 500 ms de intervalo.
     * O QMC5883L pode precisar de alguns milissegundos extras para estabilizar após
     * a bus recovery ou após um reset de hardware do host. */
    ret = ESP_FAIL;
    for (int tentativa = 1; tentativa <= 3 && ret != ESP_OK; tentativa++) {
        ret = qmc_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "QMC5883L tentativa %d/3 falhou. Aguardando 500 ms...", tentativa);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMC5883L nao respondeu apos 3 tentativas. Verifique VCC e fiacao.");
        return ret;
    }

    /* Inicializa INA226 no mesmo barramento I2C (não falha se ausente) */
    ina226_init();

    /* Carrega calibração da NVS (silencioso se ausente — usa zeros) */
    nvs_handle_t nvs_handle;
    ret = nvs_open(HEADING_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        load_cal_from_nvs(nvs_handle);
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "Namespace NVS '%s' não encontrado — calibração zero.", HEADING_NVS_NAMESPACE);
    }

    /* Criação do mutex antes de criar a task */
    g_heading_mutex = xSemaphoreCreateMutex();
    if (g_heading_mutex == nullptr) {
        ESP_LOGE(TAG, "Falha ao criar mutex de heading");
        return ESP_FAIL;
    }

    /* ----------------------------------------------------------------
     * Criação da heading_task a 10 Hz.
     * Prioridade 5: acima das tasks idle, abaixo de tasks críticas de
     * motor e navegação (que podem ter prioridade maior quando criadas).
     * ---------------------------------------------------------------- */
    BaseType_t task_ret = xTaskCreate(
        heading_task,
        "heading_task",
        4096,
        nullptr,
        5,
        &g_heading_task_handle
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar heading_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Componente heading inicializado (task 10 Hz, I2C 400 kHz).");
    return ESP_OK;
}

bool heading_read(float *theta_rad)
{
    uint8_t raw[6] = {0};

    /* Lê 6 bytes a partir do registrador 0x00: X_LSB, X_MSB, Y_LSB, Y_MSB, Z_LSB, Z_MSB */
    esp_err_t ret = read_regs(QMC5883L_REG_DATA, raw, sizeof(raw));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Falha I2C ao ler QMC5883L: %s", esp_err_to_name(ret));
        return false;
    }

    /* Reconstrói int16_t em little-endian (LSB primeiro, conforme datasheet QMC5883L) */
    int16_t x = (int16_t)(raw[0] | ((uint16_t)raw[1] << 8));
    int16_t y = (int16_t)(raw[2] | ((uint16_t)raw[3] << 8));
    /* Z não é usado para heading em plano horizontal, mas lido para completar a transação */

    *theta_rad = compute_heading(x, y);
    return true;
}

esp_err_t heading_calibrate(uint16_t n_samples)
{
    ESP_LOGI(TAG, "Iniciando calibração hard-iron (%u amostras, ~%.1f s). Gire o veículo em círculo completo.",
             n_samples, (float)n_samples * 0.02f);

    /* ----------------------------------------------------------------
     * Suspende a heading_task durante a calibração para evitar conflito
     * de acesso ao barramento I2C. A task é retomada ao final.
     * ---------------------------------------------------------------- */
    if (g_heading_task_handle != nullptr) {
        vTaskSuspend(g_heading_task_handle);
    }

    int16_t x_min = INT16_MAX, x_max = INT16_MIN;
    int16_t y_min = INT16_MAX, y_max = INT16_MIN;
    uint16_t validas = 0;

    for (uint16_t i = 0; i < n_samples; i++) {
        uint8_t raw[6] = {0};
        esp_err_t ret = read_regs(QMC5883L_REG_DATA, raw, sizeof(raw));
        if (ret == ESP_OK) {
            int16_t x = (int16_t)(raw[0] | ((uint16_t)raw[1] << 8));
            int16_t y = (int16_t)(raw[2] | ((uint16_t)raw[3] << 8));

            if (x < x_min) x_min = x;
            if (x > x_max) x_max = x;
            if (y < y_min) y_min = y;
            if (y > y_max) y_max = y;

            validas++;
        }

        /* 20 ms entre amostras → 50 Hz de coleta. O QMC5883L atualiza a 200 Hz
           (nova amostra a cada 5 ms), então 20 ms não perde nenhuma transição
           relevante e mantém 100 amostras em apenas ~2 s de rotação. */
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (g_heading_task_handle != nullptr) {
        vTaskResume(g_heading_task_handle);
    }

    if (validas == 0) {
        ESP_LOGE(TAG, "Calibração falhou: nenhuma leitura I2C válida.");
        return ESP_FAIL;
    }

    /* Atualiza calibração em RAM */
    g_cal.x_min = x_min;
    g_cal.x_max = x_max;
    g_cal.y_min = y_min;
    g_cal.y_max = y_max;

    ESP_LOGI(TAG, "Calibração concluída (%u/%u válidas): x=[%d,%d] y=[%d,%d]",
             validas, n_samples, x_min, x_max, y_min, y_max);

    /* ----------------------------------------------------------------
     * Persiste em NVS para sobreviver ao reboot.
     * nvs_commit() é obrigatório para garantir gravação física na flash.
     * ---------------------------------------------------------------- */
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(HEADING_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS para escrita: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    bool nvs_ok = true;
    nvs_ok &= (nvs_set_i16(nvs_handle, "x_min", x_min) == ESP_OK);
    nvs_ok &= (nvs_set_i16(nvs_handle, "x_max", x_max) == ESP_OK);
    nvs_ok &= (nvs_set_i16(nvs_handle, "y_min", y_min) == ESP_OK);
    nvs_ok &= (nvs_set_i16(nvs_handle, "y_max", y_max) == ESP_OK);
    nvs_ok &= (nvs_commit(nvs_handle) == ESP_OK);

    nvs_close(nvs_handle);

    if (!nvs_ok) {
        ESP_LOGE(TAG, "Falha ao gravar calibração na NVS.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Calibração gravada na NVS (namespace '%s').", HEADING_NVS_NAMESPACE);
    return ESP_OK;
}

void heading_hold(float target_rad)
{
    if (target_rad < 0.0f) {
        /* Sentinela: desativa o controle e para os motores */
        g_hold_enabled = false;
        g_target_heading_rad = -1.0f;
        motor_stop();
        ESP_LOGI(TAG, "Controle de heading desativado.");
        return;
    }

    /* Normaliza para [0, 2π) caso o chamador forneça valor fora do intervalo */
    while (target_rad >= 2.0f * (float)M_PI) target_rad -= 2.0f * (float)M_PI;
    while (target_rad <  0.0f)               target_rad += 2.0f * (float)M_PI;

    g_target_heading_rad = target_rad;
    g_hold_enabled       = true;

    ESP_LOGI(TAG, "Controle de heading ativado: alvo = %.3f rad (%.1f°).",
             target_rad, target_rad * 180.0f / (float)M_PI);
}
