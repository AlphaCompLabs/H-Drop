 /*
 * ╔════════════════════════════════════════════════════════════════════════╗
 * ║        HDrop – Firmware de Barco Autônomo  v2.0                        ║
 * ║────────────────────────────────────────────────────────────────────────║
 * ║  Plataforma : ESP32 + ESP-IDF (VS Code)                                ║
 * ║  Hardware   :                                                          ║
 * ║    A7670SA  (LTE 4G + GNSS integrado)  UART2  RX=GPIO16  TX=GPIO17     ║
 * ║    QMC5883L (Magnetômetro)              I2C    SDA=GPIO21  SCL=GPIO22  ║
 * ║    AJ-SR04M (Ultrassônico)              GPIO   TRIG=GPIO4  ECHO=GPIO5  ║
 * ║      ↑ Sensor ausente: função real COMENTADA; simulação ATIVA          ║
 * ║    ESC Bombordo  (casco esq.)           LEDC   GPIO18  (50 Hz PWM)     ║
 * ║    ESC Estibordo (casco dir.)           LEDC   GPIO19  (50 Hz PWM)     ║
 * ║                                                                        ║
 * ║  Referência AT: A76XX Series AT Command Manual V1.06 (SIMCom)          ║
 * ║    Seção 18 – MQTT(S)  |  Seção 24 – GNSS                              ║
 * ║                                                                        ║
 * ║  Máquina de estados: CONECTANDO_REDE → CONECTANDO_MQTT → OPERANDO      ║
 * ║                                                                        ║
 * ║  Telemetria (2 Hz) → tópico  "hdrop/raw"                               ║
 * ║  Comandos de rota  ← tópico  "hdrop/comando"                           ║
 * ║                                                                        ║
 * ║  JSON publicado:                                                       ║
 * ║   {"g":"<gnss_bruto>","m":[x,y,z],"u":{"d":<cm>,"y":<norm>,"al":<b>}}  ║
 * ║                                                                        ║
 * ║  Tarefas FreeRTOS:                                                     ║
 * ║    task_telemetria  2 Hz  Core 0  prio 5   FSM + sensores lentos       ║
 * ║    task_controle   20 Hz  Core 1  prio 10  sensores rápidos + PWM      ║
 * ╚════════════════════════════════════════════════════════════════════════╝
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  1. CONFIGURAÇÕES DE HARDWARE
 * ═══════════════════════════════════════════════════════════════════════════*/

/* Modem A7670SA – UART2 */
#define MODEM_UART_NUM      UART_NUM_2
#define MODEM_RX_PIN        16
#define MODEM_TX_PIN        17
#define MODEM_BAUD          115200
#define MODEM_RX_BUF        2048

/* Magnetômetro QMC5883L – I2C */
#define I2C_PORT            I2C_NUM_0
#define I2C_SDA_PIN         21
#define I2C_SCL_PIN         22
#define I2C_FREQ_HZ         400000

/* Sensor Ultrassônico AJ-SR04M – GPIO
 * Remapeado para GPIO4/5: GPIO18 e GPIO19 são reservados para os ESCs. */
#define US_TRIG_PIN         4
#define US_ECHO_PIN         5

/* ESC Bidirecional – LEDC (PWM por hardware, 50 Hz)
 * Período: 20 ms | Resolução: 16 bits (65 536 ticks)
 * Fórmula de conversão: duty = micros * 65536 / 20000               */
#define ESC_BOMBORDO_GPIO   18
#define ESC_ESTIBORDO_GPIO  19
#define ESC_LEDC_FREQ_HZ    50
#define ESC_LEDC_RES_BITS   LEDC_TIMER_16_BIT
#define ESC_LEDC_TIMER      LEDC_TIMER_0
#define ESC_CH_BOMBORDO     LEDC_CHANNEL_0
#define ESC_CH_ESTIBORDO    LEDC_CHANNEL_1
#define ESC_US_NEUTRO       1500u
#define ESC_US_MIN          1000u
#define ESC_US_MAX          2000u
#define ESC_US_TO_DUTY(us)  ((uint32_t)((us) * 65536u / 20000u))

/* ═══════════════════════════════════════════════════════════════════════════
 *  2. CONFIGURAÇÕES DE REDE E MQTT
 * ═══════════════════════════════════════════════════════════════════════════*/

#define APN                 "zap.vivo.com.br"
#define MQTT_BROKER         "tcp://broker.hivemq.com:1883"
#define MQTT_CLIENT_ID      "hdrop_boat_001"
#define MQTT_KEEPALIVE      60
#define MQTT_TOPIC_PUB      "hdrop/raw"
#define MQTT_TOPIC_SUB      "hdrop/comando"
#define MQTT_TOPIC_ALARME   "hdrop/alarme"
#define MQTT_TOPIC_PUB_LEN  9
#define MQTT_TOPIC_SUB_LEN  13
#define MQTT_TOPIC_ALARME_LEN 12

/* ═══════════════════════════════════════════════════════════════════════════
 *  3. QMC5883L – ENDEREÇO E REGISTRADORES
 * ═══════════════════════════════════════════════════════════════════════════*/

#define QMC5883L_ADDR       0x0D
#define QMC5883L_REG_DATA   0x00
#define QMC5883L_REG_CTRL1  0x09
#define QMC5883L_REG_SET    0x0B

/* ═══════════════════════════════════════════════════════════════════════════
 *  4. SENSOR ULTRASSÔNICO AJ-SR04M – PARÂMETROS DE PROCESSAMENTO
 * ═══════════════════════════════════════════════════════════════════════════*/

#define US_DIST_MIN_CM          20.0f
#define US_DIST_MAX_CM         300.0f
#define US_DIST_RANGE          (US_DIST_MAX_CM - US_DIST_MIN_CM)
#define US_ALPHA                0.75f
#define US_N_MEDIA              6
#define US_SALTO_MAX_CM         50.0f
#define US_PULSE_TIMEOUT_US     50000UL
#define US_FALHAS_MAX           5
#define US_CONF_ALARME          2
#define US_CONF_LIVRE           3

/* ═══════════════════════════════════════════════════════════════════════════
 *  5. TEMPORIZAÇÃO
 * ═══════════════════════════════════════════════════════════════════════════*/

#define INTERVALO_TELEMETRIA_MS  500UL
#define AT_TIMEOUT_MS            5000UL
#define AT_TIMEOUT_LONGO_MS      15000UL
#define AT_TIMEOUT_GNSS_MS       9000UL
#define AT_TIMEOUT_PUB_MS        10000UL
#define MAX_ERROS_MQTT           3

/* ═══════════════════════════════════════════════════════════════════════════
 *  6. PARÂMETROS DE CONTROLE (Bloco 3)
 * ═══════════════════════════════════════════════════════════════════════════*/

/* Controlador PD de Heading */
#define KP_HDG           3.0f    /* proporcional: 1° de erro → 3 µs de diff */
#define KD_HDG           8.0f    /* derivativo:   1°/s de gz → 8 µs de amortecimento */
#define HDG_BANDA_MORTA  5.0f    /* °  — histerese: não corrige se |erro| < 5° */
#define DPWM_MAX         280u    /* µs — diferencial máximo entre os dois ESCs */

/* Controlador PI de Velocidade */
#define KP_VEL           60.0f   /* proporcional: 1 km/h de erro → 60 µs de PWM */
#define KI_VEL           15.0f   /* integral por segundo */
#define VEL_INTEGRAL_MAX 150.0f  /* µs — anti-windup */
#define PWM_BASE_MIN     1500u   /* µs — neutro (não vai a ré em modo missão) */
#define PWM_BASE_MAX     2000u   /* µs — frente máxima */

/* Proteção de corrente */
#define CORRENTE_LIMITE_A  28.0f /* A  — acima disso, reduz PWM_base */

/* Distância de chegada */
#define DIST_CHEGADA_M     5.0f  /* m  — encerra missão */

/* Gestão de bateria (Bloco 4) */
#define BAT_V_CORTE        11.4f /* V  — BMS próximo do corte → neutro imediato */
#define BAT_SOC_ALARME     10.0f /* %  — publicar alarme MQTT */
#define BAT_SOC_RETORNO    20.0f /* %  — modo retorno (v_ref mínima) */

/* Obstáculos (Bloco 4) */
#define US_ZONA_CRITICA_CM  30.0f /* cm — parada de emergência imediata */
#define US_ZONA_AGRESSIVA_CM 80.0f /* cm — ré bombordo, estibordo reduzido */
#define US_ZONA_MODERADA_CM 120.0f /* cm — bombordo a neutro */

/* ═══════════════════════════════════════════════════════════════════════════
 *  7. MÁQUINA DE ESTADOS
 * ═══════════════════════════════════════════════════════════════════════════*/

typedef enum {
    CONECTANDO_REDE,
    CONECTANDO_MQTT,
    OPERANDO
} Estado;

/* ═══════════════════════════════════════════════════════════════════════════
 *  7. STRUCT COMPARTILHADA – EstadoSistema_t + Mutex
 *
 *  Toda troca de dados entre task_telemetria e task_controle passa por aqui.
 *  Acesso sempre protegido por g_mutex (xSemaphoreTake / xSemaphoreGive).
 *  Timeout curto em task_controle (5 ms) para não atrasar o ciclo de 20 Hz.
 * ═══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    /* ── GNSS (atualizado por task_telemetria, ~1 Hz) ── */
    float   lat;
    float   lon;
    float   sog_kmh;
    float   cog_deg;
    float   hdop;
    bool    gnss_fix;
    char    gnss_raw[160];

    /* ── Magnetômetro QMC5883L (atualizado por task_telemetria, 2 Hz) ── */
    int16_t mag_x, mag_y, mag_z;
    float   heading_deg;       /* calculado via atan2(my, mx) – Bloco 2 */

    /* ── IMU MPU-6050 (simulado – Bloco 2) ── */
    float   ax, ay, az;        /* aceleração (g)       */
    float   gx, gy, gz;        /* velocidade angular (°/s) */

    /* ── Bateria INA226 (simulado – Bloco 2) ── */
    float   voltage_v;
    float   current_a;
    float   power_w;
    float   soc_pct;
    float   energy_wh;

    /* ── Ultrassônico AJ-SR04M (atualizado por task_controle, 20 Hz) ── */
    float   us_dist_cm;
    float   us_y_norm;
    bool    us_alarme;
    bool    us_ok;

    /* ── Missão (atualizado ao receber hdrop/comando) ── */
    float   waypoint_lat;
    float   waypoint_lon;
    bool    missao_ativa;
    float   bearing_ref;       /* ângulo alvo 0–360° (Bloco 3) */
    float   dist_destino_m;    /* distância ao waypoint em metros (Bloco 3) */
    float   v_ref;             /* velocidade de cruzeiro alvo (Bloco 3) */

    /* ── ESCs – saída de controle (escrita por task_controle) ── */
    uint32_t pwm_bombordo_us;
    uint32_t pwm_estibordo_us;

    /* ── FSM e MQTT (espelho interno, escrito por task_telemetria) ── */
    Estado  estado_fsm;
    int     erros_mqtt;
} EstadoSistema_t;

static EstadoSistema_t   g_estado;
static SemaphoreHandle_t g_mutex;

/* ═══════════════════════════════════════════════════════════════════════════
 *  8. VARIÁVEIS GLOBAIS E BUFFERS
 *  (Acesso exclusivo de task_telemetria – sem necessidade de mutex)
 * ═══════════════════════════════════════════════════════════════════════════*/

static char   bufAt[512];
static char   bufGnss[160];
static char   bufJson[512];
static char   bufURC[2048];
static size_t bufURC_len = 0;

/* Estado interno do pipeline do ultrassônico */
static float   us_janela[US_N_MEDIA];
static int     us_janela_idx       = 0;
static bool    us_janela_cheia     = false;
static float   us_dist_valida      = 150.0f;
static float   us_dist_filtrada    = 150.0f;
static bool    us_primeira_leitura = true;
static int     us_falhas           = 0;
static bool    us_sensor_ok        = false;
static int     us_cnt_alarme       = 0;
static int     us_cnt_livre        = 0;
static bool    us_alarme_ativo     = false;
static float   us_sim_fase         = 0.0f;

/* Estado interno da simulação do IMU (MPU-6050) */
static float   imu_sim_fase        = 0.0f;

/* Estado interno da simulação da bateria (INA226)
 * Capacidade nominal: 50 Wh (LiFePO4 4S ~2,5 Ah × 12,8 V) */
static float   bat_soc_sim         = 100.0f;
static float   bat_energy_sim      = 50.0f;

/* ═══════════════════════════════════════════════════════════════════════════
 *  9. UTILITÁRIOS
 * ═══════════════════════════════════════════════════════════════════════════*/

static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline void delay_ms(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

static inline float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static void trim_str(char *s) {
    if (!s || !*s) return;
    char *end = s + strlen(s) - 1;
    while (end >= s && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = '\0';
    char *start = s;
    while (*start == '\r' || *start == '\n' || *start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  10. INICIALIZAÇÃO DE PERIFÉRICOS (UART e I2C)
 * ═══════════════════════════════════════════════════════════════════════════*/

static void init_uart_modem(void) {
    const uart_config_t cfg = {
        .baud_rate  = MODEM_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_param_config(MODEM_UART_NUM, &cfg);
    uart_set_pin(MODEM_UART_NUM, MODEM_TX_PIN, MODEM_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(MODEM_UART_NUM, MODEM_RX_BUF, 0, 0, NULL, 0);
}

static void init_i2c(void) {
    const i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA_PIN,
        .scl_io_num       = I2C_SCL_PIN,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    i2c_param_config(I2C_PORT, &cfg);
    i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  11. ESC – LEDC PWM (50 Hz, 16 bits)
 *
 *  Período: 20 ms (50 Hz)  |  Resolução: 65 536 ticks  |  1 tick ≈ 0,305 µs
 *  Faixa válida: 1000 µs (ré máx.) → 1500 µs (neutro) → 2000 µs (frente máx.)
 * ═══════════════════════════════════════════════════════════════════════════*/

static void esc_init(void) {
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = ESC_LEDC_RES_BITS,
        .timer_num       = ESC_LEDC_TIMER,
        .freq_hz         = ESC_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    const ledc_channel_config_t ch_bb = {
        .gpio_num   = ESC_BOMBORDO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = ESC_CH_BOMBORDO,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = ESC_LEDC_TIMER,
        .duty       = ESC_US_TO_DUTY(ESC_US_NEUTRO),
        .hpoint     = 0,
    };
    ledc_channel_config(&ch_bb);

    const ledc_channel_config_t ch_eb = {
        .gpio_num   = ESC_ESTIBORDO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = ESC_CH_ESTIBORDO,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = ESC_LEDC_TIMER,
        .duty       = ESC_US_TO_DUTY(ESC_US_NEUTRO),
        .hpoint     = 0,
    };
    ledc_channel_config(&ch_eb);

    printf("[ESC] LEDC OK: 50 Hz, 16 bits, neutro=%u us (duty=%u)\n",
           ESC_US_NEUTRO, ESC_US_TO_DUTY(ESC_US_NEUTRO));
}

/* Define a largura de pulso de um canal ESC em microssegundos. */
static void esc_set_us(ledc_channel_t canal, uint32_t micros) {
    if (micros < ESC_US_MIN) micros = ESC_US_MIN;
    if (micros > ESC_US_MAX) micros = ESC_US_MAX;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, canal, ESC_US_TO_DUTY(micros));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, canal);
}

/* Coloca ambos os ESCs em neutro (parado). */
static void esc_neutro(void) {
    esc_set_us(ESC_CH_BOMBORDO,  ESC_US_NEUTRO);
    esc_set_us(ESC_CH_ESTIBORDO, ESC_US_NEUTRO);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  12. QMC5883L – IMPLEMENTAÇÃO I2C (ESP-IDF)
 *      Convertido de Wire.h (Arduino) para driver/i2c.h (ESP-IDF)
 * ═══════════════════════════════════════════════════════════════════════════*/

static esp_err_t qmc_write_reg(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (QMC5883L_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, reg, true);
    i2c_master_write_byte(h, val, true);
    i2c_master_stop(h);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
    return ret;
}

/*
 * Inicializa o QMC5883L.
 * CTRL1 = 0b00011101
 *   [7:6] OSR  = 00 → 512 amostras (máxima filtragem de ruído)
 *   [5:4] RNG  = 01 → ±8 Gauss
 *   [3:2] ODR  = 11 → 200 Hz
 *   [1:0] MODE = 01 → Modo Contínuo
 */
static void qmc_init(void) {
    qmc_write_reg(QMC5883L_REG_SET,   0x01);
    delay_ms(10);
    qmc_write_reg(QMC5883L_REG_CTRL1, 0b00011101);
    printf("[MAG] QMC5883L OK (200 Hz, +-8G, OSR=512, Continuo)\n");
}

/*
 * Lê eixos X, Y, Z do QMC5883L via I2C.
 * Registrador 0x00: X_LSB, X_MSB, Y_LSB, Y_MSB, Z_LSB, Z_MSB
 */
static bool qmc_read(int16_t *x, int16_t *y, int16_t *z) {
    uint8_t data[6];
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (QMC5883L_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, QMC5883L_REG_DATA, true);
    i2c_master_start(h);
    i2c_master_write_byte(h, (QMC5883L_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(h, data, 6, I2C_MASTER_LAST_NACK);
    i2c_master_stop(h);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
    if (ret != ESP_OK) return false;
    *x = (int16_t)(data[0] | ((uint16_t)data[1] << 8));
    *y = (int16_t)(data[2] | ((uint16_t)data[3] << 8));
    *z = (int16_t)(data[4] | ((uint16_t)data[5] << 8));
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  13. SENSOR ULTRASSÔNICO AJ-SR04M
 * ═══════════════════════════════════════════════════════════════════════════*/

static void us_init_gpio(void) {
    const gpio_config_t trig_cfg = {
        .pin_bit_mask = (1ULL << US_TRIG_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&trig_cfg);
    gpio_set_level(US_TRIG_PIN, 0);

    const gpio_config_t echo_cfg = {
        .pin_bit_mask = (1ULL << US_ECHO_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&echo_cfg);
}

/* ── LEITURA REAL DO AJ-SR04M ─────────────────────────────────────────────
 * COMENTADA: sensor não disponível no hardware atual.
 * Para ativar: descomente esta função e troque us_ler_simulado() por
 * us_ler_real() dentro de us_processar() conforme indicado.
 *
 * static bool us_ler_real(float *dist_cm) {
 *     gpio_set_level(US_TRIG_PIN, 0);
 *     ets_delay_us(5);
 *     gpio_set_level(US_TRIG_PIN, 1);
 *     ets_delay_us(15);
 *     gpio_set_level(US_TRIG_PIN, 0);
 *     uint64_t t_start = esp_timer_get_time();
 *     while (gpio_get_level(US_ECHO_PIN) == 0) {
 *         if ((esp_timer_get_time() - t_start) > US_PULSE_TIMEOUT_US) return false;
 *     }
 *     uint64_t t_rise = esp_timer_get_time();
 *     while (gpio_get_level(US_ECHO_PIN) == 1) {
 *         if ((esp_timer_get_time() - t_rise) > US_PULSE_TIMEOUT_US) return false;
 *     }
 *     float medida = ((esp_timer_get_time() - t_rise) * 0.0343f) / 2.0f;
 *     if (medida < US_DIST_MIN_CM || medida > US_DIST_MAX_CM) return false;
 *     *dist_cm = medida;
 *     return true;
 * }
 */

/*
 * us_ler_simulado()
 * Gera onda senoidal lenta entre 50–250 cm com ruído ±5 cm.
 * Período: ~50 s (fase avança 2π/100 por chamada a 20 Hz → 5 s de fato).
 */
static bool us_ler_simulado(float *dist_cm) {
    us_sim_fase += 0.0628f;
    if (us_sim_fase > 2.0f * (float)M_PI) us_sim_fase -= 2.0f * (float)M_PI;
    float base = 150.0f + 100.0f * sinf(us_sim_fase);
    static uint32_t rand_seed = 42u;
    rand_seed = rand_seed * 1664525u + 1013904223u;
    float ruido = ((float)(int32_t)rand_seed / (float)0x80000000u) * 5.0f;
    *dist_cm = base + ruido;
    return true;
}

static float us_calcular_media(float novo) {
    us_janela[us_janela_idx] = novo;
    us_janela_idx = (us_janela_idx + 1) % US_N_MEDIA;
    if (us_janela_idx == 0) us_janela_cheia = true;
    int n = us_janela_cheia ? US_N_MEDIA : us_janela_idx;
    float soma = 0.0f;
    for (int i = 0; i < n; i++) soma += us_janela[i];
    return soma / n;
}

/*
 * us_processar()
 * Pipeline: leitura → rejeição de saltos → média móvel → filtro exponencial
 *           → normalização → alarme com histerese.
 * Chamado por task_controle a 20 Hz.
 */
static void us_processar(float *out_dist_cm, float *out_y,
                         bool *out_alarme, bool *out_ok, float threshold) {
    float dist_bruta = 0.0f;
    bool leitura_ok = us_ler_simulado(&dist_bruta);
    /* bool leitura_ok = us_ler_real(&dist_bruta); */

    if (leitura_ok) {
        us_falhas    = 0;
        us_sensor_ok = true;
        if (us_primeira_leitura) {
            us_dist_valida      = dist_bruta;
            us_primeira_leitura = false;
        } else {
            float salto = fabsf(dist_bruta - us_dist_valida);
            us_dist_valida = (salto <= US_SALTO_MAX_CM)
                             ? dist_bruta
                             : 0.7f * us_dist_valida + 0.3f * dist_bruta;
        }
    } else {
        us_falhas++;
        us_sensor_ok = (us_falhas <= US_FALHAS_MAX);
    }

    float dist_media = us_calcular_media(us_dist_valida);
    us_dist_filtrada = US_ALPHA * us_dist_filtrada + (1.0f - US_ALPHA) * dist_media;
    float y_norm = clampf((us_dist_filtrada - US_DIST_MIN_CM) / US_DIST_RANGE, 0.0f, 1.0f);

    if (us_sensor_ok) {
        if (y_norm < threshold) {
            us_cnt_alarme++;
            us_cnt_livre = 0;
            if (us_cnt_alarme >= US_CONF_ALARME) us_alarme_ativo = true;
        } else {
            us_cnt_livre++;
            us_cnt_alarme = 0;
            if (us_cnt_livre >= US_CONF_LIVRE) us_alarme_ativo = false;
        }
    } else {
        us_alarme_ativo = false;
        us_cnt_alarme   = 0;
        us_cnt_livre    = 0;
    }

    *out_dist_cm = us_dist_filtrada;
    *out_y       = y_norm;
    *out_alarme  = us_alarme_ativo;
    *out_ok      = us_sensor_ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  14. SENSORES ADICIONAIS — SIMULAÇÕES (Bloco 2)
 *
 *  MPU-6050 e INA226 ainda não estão fisicamente disponíveis.
 *  Estas funções geram valores plausíveis para preencher EstadoSistema_t
 *  e validar o pipeline de telemetria antes do hardware chegar.
 * ═══════════════════════════════════════════════════════════════════════════*/

/*
 * imu_simular()
 * Gera ax, ay, az, gx, gy, gz simulados para o MPU-6050 (20 Hz).
 *   az ≈ 1g  com variação suave de ondas
 *   ax ≈ 0   (aceleração longitudinal mínima em cruzeiro)
 *   gz ≈ 0   (yaw quasi-nulo em linha reta)
 */
static void imu_simular(float *ax, float *ay, float *az,
                        float *gx, float *gy, float *gz) {
    imu_sim_fase += 0.05f;
    if (imu_sim_fase > 2.0f * (float)M_PI) imu_sim_fase -= 2.0f * (float)M_PI;

    float r = (float)(rand() % 1000 - 500);   /* ruído: -500..+500 */

    *az = 1.00f + 0.05f * sinf(imu_sim_fase)       + r * 2e-4f;
    *ax = 0.01f + 0.02f * sinf(imu_sim_fase * 0.7f) + r * 1e-4f;
    *ay = 0.00f + 0.01f * cosf(imu_sim_fase * 1.3f) + r * 1e-4f;

    *gz = 0.0f + r * 5e-4f;                         /* ±0.25 °/s de ruído */
    *gx = 0.5f * sinf(imu_sim_fase)       + r * 2e-3f;
    *gy = 0.3f * cosf(imu_sim_fase * 1.1f) + r * 2e-3f;
}

/*
 * bat_simular()
 * Simula tensão, corrente, SOC e energia restante da LiFePO4 4S (2 Hz).
 * Corrente proporcional ao afastamento médio do neutro (esforço dos ESCs).
 */
static void bat_simular(uint32_t pwm_bb, uint32_t pwm_eb,
                        float *v, float *i_out, float *soc_out, float *wh_out) {
    float avg   = (float)(pwm_bb + pwm_eb) * 0.5f;
    float load  = fabsf(avg - 1500.0f) / 500.0f;       /* 0 = parado, 1 = plena */
    float r     = (float)(rand() % 100 - 50) * 0.01f;  /* ruído ±0.5 A */

    float cur   = 0.5f + 11.0f * load + r;
    if (cur < 0.5f) cur = 0.5f;

    float vlt   = 12.8f - (100.0f - bat_soc_sim) * 0.013f
                        + (float)(rand() % 100 - 50) * 0.001f;

    /* Coulomb counting: dt = 0.5 s (chamado a 2 Hz) */
    bat_energy_sim -= vlt * cur * (0.5f / 3600.0f);
    if (bat_energy_sim < 0.0f) bat_energy_sim = 0.0f;
    bat_soc_sim = (bat_energy_sim / 50.0f) * 100.0f;
    if (bat_soc_sim < 0.0f) bat_soc_sim = 0.0f;

    *v       = vlt;
    *i_out   = cur;
    *soc_out = bat_soc_sim;
    *wh_out  = bat_energy_sim;
}

/*
 * parse_gnss()
 * Extrai campos numéricos da string bruta do AT+CGNSSINFO.
 * Formato: mode,fixSats,visSats,beidouSats,lat_ddmm,N/S,lon_dddmm,E/W,
 *          date,utcTime,alt,speed_kmh,course,pdop,hdop,vdop
 * Converte lat/lon de DDMM.MMMM → graus decimais.
 */
static bool parse_gnss(const char *raw,
                       float *lat, float *lon,
                       float *sog_kmh, float *cog_deg, float *hdop) {
    if (!raw || raw[0] == '\0' || raw[0] == ',') return false;

    char buf[160];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *ctx = buf;
    char *tok;

    /* campo 0: mode  (2 = 2D fix, 3 = 3D fix; 0/1 = sem fix) */
    tok = strtok_r(ctx, ",", &ctx);
    if (!tok || atoi(tok) < 2) return false;

    /* campos 1-3: satélites (ignorados) */
    strtok_r(NULL, ",", &ctx);
    strtok_r(NULL, ",", &ctx);
    strtok_r(NULL, ",", &ctx);

    /* campo 4: latitude DDMM.MMMMMM */
    tok = strtok_r(NULL, ",", &ctx);
    if (!tok || tok[0] == '\0') return false;
    double lat_raw = atof(tok);
    int    lat_d   = (int)(lat_raw / 100.0);
    double lat_dec = lat_d + (lat_raw - lat_d * 100.0) / 60.0;

    /* campo 5: N / S */
    tok = strtok_r(NULL, ",", &ctx);
    if (tok && tok[0] == 'S') lat_dec = -lat_dec;

    /* campo 6: longitude DDDMM.MMMMMM */
    tok = strtok_r(NULL, ",", &ctx);
    if (!tok || tok[0] == '\0') return false;
    double lon_raw = atof(tok);
    int    lon_d   = (int)(lon_raw / 100.0);
    double lon_dec = lon_d + (lon_raw - lon_d * 100.0) / 60.0;

    /* campo 7: E / W */
    tok = strtok_r(NULL, ",", &ctx);
    if (tok && tok[0] == 'W') lon_dec = -lon_dec;

    /* campos 8-9: data e hora (ignorados) */
    strtok_r(NULL, ",", &ctx);
    strtok_r(NULL, ",", &ctx);

    /* campo 10: altitude (ignorada) */
    strtok_r(NULL, ",", &ctx);

    /* campo 11: velocidade (km/h) */
    tok = strtok_r(NULL, ",", &ctx);
    double spd = tok ? atof(tok) : 0.0;

    /* campo 12: curso (°) */
    tok = strtok_r(NULL, ",", &ctx);
    double crs = tok ? atof(tok) : 0.0;

    /* campo 13: PDOP (ignorado) */
    strtok_r(NULL, ",", &ctx);

    /* campo 14: HDOP */
    tok = strtok_r(NULL, ",", &ctx);
    double hdop_val = tok ? atof(tok) : 9.9;

    *lat     = (float)lat_dec;
    *lon     = (float)lon_dec;
    *sog_kmh = (float)spd;
    *cog_deg = (float)crs;
    *hdop    = (float)hdop_val;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  15. AT COMMANDS – COMUNICAÇÃO COM MODEM
 *      Acesso exclusivo de task_telemetria (sem proteção de mutex necessária).
 * ═══════════════════════════════════════════════════════════════════════════*/

static void drenarModem(void) {
    char lixo[512];
    size_t lixo_len = 0;
    uint32_t t = millis();
    while (millis() - t < 30) {
        uint8_t byte;
        int n = uart_read_bytes(MODEM_UART_NUM, &byte, 1, 1);
        if (n > 0) {
            if (lixo_len < sizeof(lixo) - 1) {
                lixo[lixo_len++] = (char)byte;
                lixo[lixo_len]   = '\0';
            }
            t = millis();
        }
    }
    if (lixo_len == 0) return;
    if (strstr(lixo, "+CMQTTRXSTART:") || strstr(lixo, "+CMQTTCONNLOST:")) {
        size_t espaco = sizeof(bufURC) - 1 - bufURC_len;
        strncat(bufURC, lixo, espaco);
        bufURC_len = strlen(bufURC);
        printf("[DRAIN] URC salva: %s\n", lixo);
    } else {
        printf("[DRAIN] Descartado: %s\n", lixo);
    }
}

static bool enviarAT(const char *cmd, const char *esperado, uint32_t timeout,
                     char *out, size_t outLen) {
    drenarModem();
    uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);
    printf("[AT>] %s\n", cmd);

    char resp[512] = "";
    size_t resp_len = 0;
    uint32_t inicio = millis();

    while (millis() - inicio < timeout) {
        uint8_t byte;
        int n = uart_read_bytes(MODEM_UART_NUM, &byte, 1, 1);
        if (n > 0 && resp_len < sizeof(resp) - 1) {
            resp[resp_len++] = (char)byte;
            resp[resp_len]   = '\0';
        }
        if (strstr(resp, esperado)) {
            printf("[AT<] %s\n", resp);
            if (out && outLen > 0) { strncpy(out, resp, outLen - 1); out[outLen - 1] = '\0'; }
            return true;
        }
        if (strstr(resp, "ERROR")) {
            printf("[AT<ERR] %s\n", resp);
            if (out && outLen > 0) { strncpy(out, resp, outLen - 1); out[outLen - 1] = '\0'; }
            return false;
        }
    }
    printf("[AT<TMO] %s\n", resp);
    return false;
}

static bool enviarDadoPrompt(const char *dado, const char *esperado, uint32_t timeout) {
    uart_write_bytes(MODEM_UART_NUM, dado, strlen(dado));
    printf("[AT>>] %s\n", dado);
    char resp[512] = "";
    size_t resp_len = 0;
    uint32_t inicio = millis();
    while (millis() - inicio < timeout) {
        uint8_t byte;
        int n = uart_read_bytes(MODEM_UART_NUM, &byte, 1, 1);
        if (n > 0 && resp_len < sizeof(resp) - 1) {
            resp[resp_len++] = (char)byte;
            resp[resp_len]   = '\0';
        }
        if (strstr(resp, esperado)) { printf("[AT<<] %s\n", resp); return true;  }
        if (strstr(resp, "ERROR"))  { printf("[AT<<ERR] %s\n", resp); return false; }
    }
    printf("[AT<<TMO]\n");
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  15. MQTT – RECUPERAÇÃO DE ERROS
 * ═══════════════════════════════════════════════════════════════════════════*/

static void liberarSessaoMQTT(void) {
    printf("[MQTT] Liberando sessao anterior (DISC -> REL -> STOP)...\n");
    enviarAT("AT+CMQTTDISC=0,120", "+CMQTTDISC: 0,0", AT_TIMEOUT_LONGO_MS, NULL, 0);
    delay_ms(300);
    enviarAT("AT+CMQTTREL=0", "OK", AT_TIMEOUT_MS, NULL, 0);
    delay_ms(300);
    enviarAT("AT+CMQTTSTOP", "+CMQTTSTOP: 0", 12000UL, NULL, 0);
    delay_ms(500);
    printf("[MQTT] Sessao liberada.\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  16. ESTADO: CONECTANDO_REDE
 * ═══════════════════════════════════════════════════════════════════════════*/

static bool conectarRede(void) {
    printf("\n[REDE] ====== Conectando a Rede ======\n");
    bool modemOk = false;
    for (int i = 0; i < 10; i++) {
        if (enviarAT("AT", "OK", 2000UL, NULL, 0)) { modemOk = true; break; }
        printf("[REDE] Aguardando modem, tentativa %d/10...\n", i + 1);
        delay_ms(1000);
    }
    if (!modemOk) { printf("[REDE] FALHA CRITICA: Modem nao responde.\n"); return false; }

    enviarAT("ATE0", "OK", 2000UL, NULL, 0);

    printf("[REDE] Aguardando registro na rede...\n");
    bool registrado = false;
    for (int i = 0; i < 30; i++) {
        enviarAT("AT+CREG?", "OK", AT_TIMEOUT_MS, bufAt, sizeof(bufAt));
        if (strstr(bufAt, "+CREG: 0,1") || strstr(bufAt, "+CREG: 0,5")) {
            registrado = true;
            printf("[REDE] Registrado na rede!\n");
            break;
        }
        delay_ms(2000);
    }
    if (!registrado) { printf("[REDE] FALHA: Sem registro apos 60 s.\n"); return false; }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
    if (!enviarAT(cmd, "OK", AT_TIMEOUT_MS, NULL, 0))
        printf("[REDE] AVISO: Falha ao configurar APN (pode ja estar OK).\n");

    if (!enviarAT("AT+CGACT=1,1", "OK", AT_TIMEOUT_LONGO_MS, NULL, 0)) {
        if (!enviarAT("AT+CGACT?", "+CGACT: 1,1", AT_TIMEOUT_MS, NULL, 0)) {
            printf("[REDE] FALHA: Contexto PDP nao pode ser ativado.\n");
            return false;
        }
        printf("[REDE] Contexto PDP ja estava ativo.\n");
    } else {
        printf("[REDE] Contexto PDP ativado.\n");
    }

    printf("[GNSS] Ligando modulo GNSS...\n");
    enviarAT("AT+CGNSSPWR=0", "OK", 3000UL, NULL, 0);
    delay_ms(300);
    if (enviarAT("AT+CGNSSPWR=1", "+CGNSSPWR: READY!", AT_TIMEOUT_GNSS_MS, NULL, 0)) {
        printf("[GNSS] Modulo GNSS pronto!\n");
    } else {
        printf("[GNSS] AVISO: '+CGNSSPWR: READY!' nao detectado. Continuando.\n");
    }
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  17. ESTADO: CONECTANDO_MQTT
 * ═══════════════════════════════════════════════════════════════════════════*/

static bool conectarMQTT(void) {
    printf("\n[MQTT] ====== Conectando ao Broker MQTT ======\n");

    if (!enviarAT("AT+CMQTTSTART", "+CMQTTSTART: 0", 12000UL, NULL, 0))
        printf("[MQTT] AVISO: CMQTTSTART falhou (pode ja estar ativo).\n");
    delay_ms(300);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CMQTTACCQ=0,\"%s\",0", MQTT_CLIENT_ID);
    if (!enviarAT(cmd, "OK", AT_TIMEOUT_MS, NULL, 0)) {
        printf("[MQTT] FALHA: CMQTTACCQ nao retornou OK.\n");
        return false;
    }
    printf("[MQTT] Cliente MQTT adquirido.\n");

    snprintf(cmd, sizeof(cmd), "AT+CMQTTCONNECT=0,\"%s\",%d,1", MQTT_BROKER, MQTT_KEEPALIVE);
    if (!enviarAT(cmd, "+CMQTTCONNECT: 0,0", AT_TIMEOUT_LONGO_MS, NULL, 0)) {
        printf("[MQTT] FALHA: CMQTTCONNECT.\n");
        return false;
    }
    printf("[MQTT] Conectado ao broker HiveMQ!\n");
    delay_ms(500);

    snprintf(cmd, sizeof(cmd), "AT+CMQTTSUBTOPIC=0,%d,1", MQTT_TOPIC_SUB_LEN);
    if (!enviarAT(cmd, ">", AT_TIMEOUT_MS, NULL, 0)) {
        printf("[MQTT] FALHA: CMQTTSUBTOPIC sem prompt.\n");
        return false;
    }
    if (!enviarDadoPrompt(MQTT_TOPIC_SUB, "OK", AT_TIMEOUT_MS)) {
        printf("[MQTT] FALHA: Erro ao enviar topico de subscribe.\n");
        return false;
    }
    if (!enviarAT("AT+CMQTTSUB=0,1", "+CMQTTSUB: 0,0", AT_TIMEOUT_LONGO_MS, NULL, 0)) {
        printf("[MQTT] FALHA: CMQTTSUB rejeitado.\n");
        return false;
    }
    printf("[MQTT] Subscribe em 'hdrop/comando' confirmado.\n");
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  18. GNSS – LEITURA DE POSIÇÃO
 * ═══════════════════════════════════════════════════════════════════════════*/

static bool lerGNSSInfo(char *saida, size_t maxLen) {
    if (!enviarAT("AT+CGNSSINFO", "+CGNSSINFO:", AT_TIMEOUT_MS, bufAt, sizeof(bufAt))) {
        strncpy(saida, "gnss_err", maxLen - 1);
        saida[maxLen - 1] = '\0';
        return false;
    }
    char *p = strstr(bufAt, "+CGNSSINFO:");
    if (!p) {
        strncpy(saida, "parse_err", maxLen - 1);
        saida[maxLen - 1] = '\0';
        return false;
    }
    p += strlen("+CGNSSINFO:");
    while (*p == ' ') p++;
    char *fim = strpbrk(p, "\r\n");
    size_t len = fim ? (size_t)(fim - p) : strlen(p);
    if (len >= maxLen) len = maxLen - 1;
    strncpy(saida, p, len);
    saida[len] = '\0';
    return (saida[0] != '\0' && saida[0] != ',');
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  19. MQTT – PUBLICAÇÃO DE TELEMETRIA  (Bloco 2: JSON enriquecido)
 *
 *  JSON publicado em hdrop/raw:
 *  {"g":{lat,lon,sog,cog,hdop,fix},"m":{x,y,z,hdg},"u":{d,y,al},
 *   "b":{v,i,soc,wh},"a":{ax,ay,az,gz}}
 * ═══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    /* GNSS parseado */
    float lat, lon, sog_kmh, cog_deg, hdop;
    bool  gnss_fix;
    /* Magnetômetro + heading calculado */
    int16_t mag_x, mag_y, mag_z;
    float   heading_deg;
    /* Ultrassônico */
    float us_dist_cm, us_y_norm;
    bool  us_alarme;
    /* Bateria */
    float voltage_v, current_a, soc_pct, energy_wh;
    /* IMU */
    float ax, ay, az, gz;
} DadosTelemetria_t;

static bool publicarTelemetria(const DadosTelemetria_t *d) {
    int payLen = snprintf(bufJson, sizeof(bufJson),
        "{\"g\":{\"lat\":%.6f,\"lon\":%.6f,\"sog\":%.1f,"
             "\"cog\":%.1f,\"hdop\":%.2f,\"fix\":%d},"
        "\"m\":{\"x\":%d,\"y\":%d,\"z\":%d,\"hdg\":%.1f},"
        "\"u\":{\"d\":%.1f,\"y\":%.3f,\"al\":%d},"
        "\"b\":{\"v\":%.2f,\"i\":%.2f,\"soc\":%.1f,\"wh\":%.1f},"
        "\"a\":{\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"gz\":%.2f}}",
        d->lat, d->lon, d->sog_kmh,
        d->cog_deg, d->hdop, d->gnss_fix ? 1 : 0,
        d->mag_x, d->mag_y, d->mag_z, d->heading_deg,
        d->us_dist_cm, d->us_y_norm, d->us_alarme ? 1 : 0,
        d->voltage_v, d->current_a, d->soc_pct, d->energy_wh,
        d->ax, d->ay, d->az, d->gz);

    if (payLen <= 0 || payLen >= (int)sizeof(bufJson)) {
        printf("[PUB] ERRO: JSON overflow (%d bytes).\n", payLen);
        return false;
    }

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMQTTTOPIC=0,%d", MQTT_TOPIC_PUB_LEN);
    if (!enviarAT(cmd, ">", AT_TIMEOUT_MS, NULL, 0))              { printf("[PUB] FALHA: CMQTTTOPIC.\n");   return false; }
    if (!enviarDadoPrompt(MQTT_TOPIC_PUB, "OK", AT_TIMEOUT_MS))   { printf("[PUB] FALHA: topico.\n");       return false; }

    snprintf(cmd, sizeof(cmd), "AT+CMQTTPAYLOAD=0,%d", payLen);
    if (!enviarAT(cmd, ">", AT_TIMEOUT_MS, NULL, 0))              { printf("[PUB] FALHA: CMQTTPAYLOAD.\n"); return false; }
    if (!enviarDadoPrompt(bufJson, "OK", AT_TIMEOUT_MS))          { printf("[PUB] FALHA: payload.\n");      return false; }

    if (!enviarAT("AT+CMQTTPUB=0,0,60", "+CMQTTPUB: 0,0", AT_TIMEOUT_PUB_MS, NULL, 0)) {
        printf("[PUB] FALHA: CMQTTPUB.\n");
        return false;
    }
    printf("[PUB] OK (%d bytes) lat=%.5f lon=%.5f sog=%.1f hdg=%.1f soc=%.0f%%\n",
           payLen, d->lat, d->lon, d->sog_kmh, d->heading_deg, d->soc_pct);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  19b. ALARMES MQTT (Bloco 4)
 *
 *  Publica no tópico "hdrop/alarme" com QoS 1 (AT+CMQTTPUB=0,1,60).
 *  Chamado exclusivamente de task_telemetria — sem conflito de UART.
 * ═══════════════════════════════════════════════════════════════════════════*/

static void publicarAlarme(const char *motivo, float valor) {
    char payload[80];
    int len = snprintf(payload, sizeof(payload),
                       "{\"tipo\":\"%s\",\"val\":%.2f}", motivo, valor);
    if (len <= 0 || len >= (int)sizeof(payload)) return;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMQTTTOPIC=0,%d", MQTT_TOPIC_ALARME_LEN);
    if (!enviarAT(cmd, ">", AT_TIMEOUT_MS, NULL, 0))                   return;
    if (!enviarDadoPrompt(MQTT_TOPIC_ALARME, "OK", AT_TIMEOUT_MS))     return;

    snprintf(cmd, sizeof(cmd), "AT+CMQTTPAYLOAD=0,%d", len);
    if (!enviarAT(cmd, ">", AT_TIMEOUT_MS, NULL, 0))                   return;
    if (!enviarDadoPrompt(payload, "OK", AT_TIMEOUT_MS))               return;

    /* QoS 1 para garantir entrega do alarme */
    if (!enviarAT("AT+CMQTTPUB=0,1,60", "+CMQTTPUB: 0,0", AT_TIMEOUT_PUB_MS, NULL, 0)) return;
    printf("[ALM] Alarme publicado: %s=%.2f\n", motivo, valor);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  20. NAVEGAÇÃO — FUNÇÕES GEOGRÁFICAS E PARSER DE WAYPOINT (Bloco 3)
 * ═══════════════════════════════════════════════════════════════════════════*/

/* Distância em metros entre dois pontos (fórmula de Haversine). */
static float nav_haversine(float la1, float lo1, float la2, float lo2) {
    const float R  = 6371000.0f;
    float df = (la2 - la1) * (float)M_PI / 180.0f;
    float dl = (lo2 - lo1) * (float)M_PI / 180.0f;
    float a  = sinf(df * 0.5f) * sinf(df * 0.5f)
             + cosf(la1 * (float)M_PI / 180.0f)
             * cosf(la2 * (float)M_PI / 180.0f)
             * sinf(dl * 0.5f) * sinf(dl * 0.5f);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

/* Bearing (ângulo 0–360°, Norte = 0) de (la1,lo1) para (la2,lo2). */
static float nav_bearing(float la1, float lo1, float la2, float lo2) {
    float f1 = la1 * (float)M_PI / 180.0f;
    float f2 = la2 * (float)M_PI / 180.0f;
    float dl = (lo2 - lo1) * (float)M_PI / 180.0f;
    float x  = sinf(dl) * cosf(f2);
    float y  = cosf(f1) * sinf(f2) - sinf(f1) * cosf(f2) * cosf(dl);
    return fmodf(atan2f(x, y) * 180.0f / (float)M_PI + 360.0f, 360.0f);
}

/* Normaliza ângulo para o intervalo [-180, +180]. */
static float nav_norm180(float a) {
    while (a >  180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

/*
 * parsear_waypoint()
 * Extrai lat e lon de um payload JSON simples: {"lat": X, "lon": Y}
 * Usa strstr + atof — sem dependências externas.
 */
static bool parsear_waypoint(const char *json, float *lat, float *lon) {
    const char *p;

    p = strstr(json, "lat");
    if (!p) return false;
    p += 3;
    while (*p && (*p == '"' || *p == ' ' || *p == ':')) p++;
    if (!*p) return false;
    *lat = (float)atof(p);

    p = strstr(json, "lon");
    if (!p) return false;
    p += 3;
    while (*p && (*p == '"' || *p == ' ' || *p == ':')) p++;
    if (!*p) return false;
    *lon = (float)atof(p);

    return (fabsf(*lat) > 0.001f || fabsf(*lon) > 0.001f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  21. MQTT – STATUS DE CONEXÃO E URCs ASSÍNCRONAS
 * ═══════════════════════════════════════════════════════════════════════════*/

static bool mqttConectado(void) {
    if (!enviarAT("AT+CMQTTCONNECT?", "+CMQTTCONNECT:", AT_TIMEOUT_MS, bufAt, sizeof(bufAt)))
        return false;
    return strstr(bufAt, "broker.hivemq.com") != NULL;
}

static void capturarURCs(void) {
    uint8_t byte;
    while (uart_read_bytes(MODEM_UART_NUM, &byte, 1, 0) > 0) {
        if (bufURC_len < sizeof(bufURC) - 1) {
            bufURC[bufURC_len++] = (char)byte;
            bufURC[bufURC_len]   = '\0';
        } else {
            memmove(bufURC, bufURC + bufURC_len - 1024, 1024);
            bufURC_len = 1024;
            bufURC[bufURC_len] = '\0';
        }
    }
}

/*
 * processarURCs()
 * Recebe ponteiro para o contador local de erros MQTT de task_telemetria,
 * pois o contador deixou de ser global e vive apenas naquela tarefa.
 */
static void processarURCs(int *cont_erros_mqtt) {
    if (bufURC_len == 0) return;

    if (strstr(bufURC, "+CMQTTCONNLOST:")) {
        printf("[URC] +CMQTTCONNLOST detectado -> forcar reconexao MQTT\n");
        *cont_erros_mqtt = MAX_ERROS_MQTT;
        bufURC[0] = '\0';
        bufURC_len = 0;
        return;
    }

    char *pRxStart = strstr(bufURC, "+CMQTTRXSTART:");
    char *pRxEnd   = strstr(bufURC, "+CMQTTRXEND:");
    if (!pRxStart || !pRxEnd) return;

    char *pPayHdr = strstr(pRxStart, "+CMQTTRXPAYLOAD:");
    if (!pPayHdr) { bufURC[0] = '\0'; bufURC_len = 0; return; }

    char *pNewline = strchr(pPayHdr, '\n');
    if (!pNewline) return;

    char *payloadStart  = pNewline + 1;
    ptrdiff_t payloadLen = pRxEnd - payloadStart;

    if (payloadLen > 0 && payloadLen < 256) {
        char payload[256];
        strncpy(payload, payloadStart, (size_t)payloadLen);
        payload[payloadLen] = '\0';
        trim_str(payload);
        if (strlen(payload) > 0) {
            printf("[CMD] Recebido em hdrop/comando: %s\n", payload);
            if (payload[0] == '{') {
                float wp_lat = 0.0f, wp_lon = 0.0f;
                if (parsear_waypoint(payload, &wp_lat, &wp_lon)) {
                    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        g_estado.waypoint_lat = wp_lat;
                        g_estado.waypoint_lon = wp_lon;
                        g_estado.missao_ativa = true;
                        xSemaphoreGive(g_mutex);
                    }
                    printf("[CMD] Waypoint aceito: lat=%.6f lon=%.6f\n", wp_lat, wp_lon);
                } else {
                    printf("[CMD] AVISO: payload JSON invalido: %s\n", payload);
                }
            }
        }
    }

    char *pFimLinha = strchr(pRxEnd + strlen("+CMQTTRXEND:"), '\n');
    if (pFimLinha) {
        size_t restante = bufURC_len - (size_t)(pFimLinha + 1 - bufURC);
        memmove(bufURC, pFimLinha + 1, restante + 1);
        bufURC_len = restante;
    } else {
        bufURC[0] = '\0';
        bufURC_len = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  21. TAREFA FreeRTOS: task_controle
 *
 *  Core 1 | Prioridade 10 | Ciclo: 20 Hz (50 ms via vTaskDelayUntil)
 *
 *  Responsabilidades (Bloco 1 + Bloco 2 + Bloco 3):
 *    - Processar ultrassônico (simulado) e simular IMU a 20 Hz
 *    - Ler snapshot de g_estado (heading, sog, waypoint, soc, corrente)
 *    - Se missao_ativa:
 *        · Recalcular bearing e distância ao waypoint
 *        · PD heading → ΔPWMdiff (Kp × erro + Kd × (−gz))
 *        · PI velocidade → PWM_base (Kp × err_v + integral)
 *        · Proteção de corrente: reduz PWM_base se current > 28 A
 *        · Encerrar missão quando dist < 5 m
 *    - Escrever pwm_bombordo_us / pwm_estibordo_us em g_estado
 *    - Aplicar PWM nos ESCs via LEDC
 * ═══════════════════════════════════════════════════════════════════════════*/

static void task_controle(void *arg) {
    const TickType_t periodo     = pdMS_TO_TICKS(50);   /* 20 Hz */
    const float      dt          = 0.05f;               /* s */
    TickType_t       ultimo_wake = xTaskGetTickCount();
    uint32_t         ciclos      = 0;

    /* Estado interno dos controladores */
    float vel_integral = 0.0f;   /* acumulador PI de velocidade (µs) */

    printf("[CTR] task_controle iniciada (20 Hz, Core 1)\n");

    while (1) {
        vTaskDelayUntil(&ultimo_wake, periodo);
        ciclos++;

        /* ── 1. Sensores rápidos ── */
        float us_dist = 0.0f, us_y = 0.0f;
        bool  us_ok = false, us_al = false;
        us_processar(&us_dist, &us_y, &us_al, &us_ok, 0.5f);

        float ax = 0.0f, ay = 0.0f, az = 1.0f;
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        imu_simular(&ax, &ay, &az, &gx, &gy, &gz);

        /* ── 2. Ler snapshot de g_estado ── */
        float    heading = 0.0f, sog = 0.0f, current_a = 0.0f, soc = 100.0f;
        float    voltage_v = 12.8f;
        float    lat = 0.0f, lon = 0.0f, wp_lat = 0.0f, wp_lon = 0.0f;
        bool     missao = false;
        uint32_t pwm_bb = ESC_US_NEUTRO, pwm_eb = ESC_US_NEUTRO;

        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            heading   = g_estado.heading_deg;
            sog       = g_estado.sog_kmh;
            current_a = g_estado.current_a;
            soc       = g_estado.soc_pct;
            voltage_v = g_estado.voltage_v;
            lat       = g_estado.lat;
            lon       = g_estado.lon;
            wp_lat    = g_estado.waypoint_lat;
            wp_lon    = g_estado.waypoint_lon;
            missao    = g_estado.missao_ativa;
            xSemaphoreGive(g_mutex);
        }

        /* ── 3. Lógica de controle ── */
        if (missao) {
            float dist    = nav_haversine(lat, lon, wp_lat, wp_lon);
            float bearing = nav_bearing(lat, lon, wp_lat, wp_lon);

            /* Missão concluída */
            if (dist < DIST_CHEGADA_M) {
                pwm_bb = ESC_US_NEUTRO;
                pwm_eb = ESC_US_NEUTRO;
                vel_integral = 0.0f;
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    g_estado.missao_ativa      = false;
                    g_estado.pwm_bombordo_us   = ESC_US_NEUTRO;
                    g_estado.pwm_estibordo_us  = ESC_US_NEUTRO;
                    g_estado.dist_destino_m    = 0.0f;
                    xSemaphoreGive(g_mutex);
                }
                printf("[CTR] *** MISSAO CONCLUIDA — distancia=%.1fm ***\n", dist);

            } else {
                /* ── 3a. v_ref em função do SOC ── */
                float v_ref;
                if      (soc > 50.0f) v_ref = 3.0f;
                else if (soc > 20.0f) v_ref = 1.5f + (soc - 20.0f) / 30.0f * 1.5f;
                else                  v_ref = 1.0f;

                /* ── 3b. PD Heading ── */
                float err_hdg  = nav_norm180(bearing - heading);
                float d_pwm_diff = 0;
                if (fabsf(err_hdg) >= HDG_BANDA_MORTA) {
                    d_pwm_diff = KP_HDG * err_hdg - KD_HDG * gz;
                    if (d_pwm_diff >  (float)DPWM_MAX) d_pwm_diff =  (float)DPWM_MAX;
                    if (d_pwm_diff < -(float)DPWM_MAX) d_pwm_diff = -(float)DPWM_MAX;
                }

                /* ── 3c. PI Velocidade ── */
                float err_vel = v_ref - sog;
                vel_integral += err_vel * dt * KI_VEL;
                if (vel_integral >  VEL_INTEGRAL_MAX) vel_integral =  VEL_INTEGRAL_MAX;
                if (vel_integral < -VEL_INTEGRAL_MAX) vel_integral = -VEL_INTEGRAL_MAX;

                float pwm_offset = KP_VEL * err_vel + vel_integral;
                if (pwm_offset < 0.0f) pwm_offset = 0.0f;   /* só avança */

                /* ── 3d. Proteção de corrente ── */
                if (current_a > CORRENTE_LIMITE_A) {
                    pwm_offset -= (current_a - CORRENTE_LIMITE_A) * 20.0f;
                    vel_integral *= 0.9f;   /* deflacionar integral */
                    if (pwm_offset < 0.0f) pwm_offset = 0.0f;
                }

                uint32_t pwm_base = (uint32_t)(1500.0f + pwm_offset);
                if (pwm_base > PWM_BASE_MAX) pwm_base = PWM_BASE_MAX;

                /* ── 3e. Aplicar diferencial de heading ── */
                int32_t diff = (int32_t)d_pwm_diff;
                int32_t bb   = (int32_t)pwm_base + diff;
                int32_t eb   = (int32_t)pwm_base - diff;
                pwm_bb = (uint32_t)clampf((float)bb, (float)ESC_US_MIN, (float)ESC_US_MAX);
                pwm_eb = (uint32_t)clampf((float)eb, (float)ESC_US_MIN, (float)ESC_US_MAX);

                /* Escrever resultados na struct compartilhada */
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    g_estado.bearing_ref      = bearing;
                    g_estado.dist_destino_m   = dist;
                    g_estado.v_ref            = v_ref;
                    g_estado.pwm_bombordo_us  = pwm_bb;
                    g_estado.pwm_estibordo_us = pwm_eb;
                    xSemaphoreGive(g_mutex);
                }
            }
        } else {
            /* Sem missão ativa: zerar integral e manter neutro */
            vel_integral = 0.0f;
        }

        /* ── 3f. Emergência de tensão (Bloco 4) ── */
        if (voltage_v < BAT_V_CORTE) {
            pwm_bb = ESC_US_NEUTRO;
            pwm_eb = ESC_US_NEUTRO;
            vel_integral = 0.0f;
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                g_estado.missao_ativa     = false;
                g_estado.pwm_bombordo_us  = ESC_US_NEUTRO;
                g_estado.pwm_estibordo_us = ESC_US_NEUTRO;
                xSemaphoreGive(g_mutex);
            }
            printf("[CTR] EMERGENCIA BATERIA: %.2fV < %.1fV — ESCs neutros\n",
                   voltage_v, BAT_V_CORTE);
        }

        /* ── 3g. Desvio de obstáculo (Bloco 4) ── */
        if (us_dist < US_ZONA_CRITICA_CM) {
            /* Parada total imediata */
            pwm_bb = ESC_US_NEUTRO;
            pwm_eb = ESC_US_NEUTRO;
            vel_integral = 0.0f;
            printf("[CTR] OBSTACULO CRITICO: %.1fcm — PARADA TOTAL\n", us_dist);
        } else if (us_dist < US_ZONA_AGRESSIVA_CM) {
            /* Ré bombordo + estibordo reduzido → gira à direita */
            pwm_bb = 1380u;
            pwm_eb = (uint32_t)clampf((float)pwm_eb - 100.0f,
                                      (float)ESC_US_MIN, (float)ESC_US_MAX);
            vel_integral *= 0.5f;
        } else if (us_al && us_dist < US_ZONA_MODERADA_CM) {
            /* Bombordo a neutro → desvio suave à direita */
            pwm_bb = ESC_US_NEUTRO;
        }

        /* ── 4. Escrever sensores rápidos em g_estado ── */
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_estado.us_dist_cm = us_dist;
            g_estado.us_y_norm  = us_y;
            g_estado.us_alarme  = us_al;
            g_estado.us_ok      = us_ok;
            g_estado.ax = ax;  g_estado.ay = ay;  g_estado.az = az;
            g_estado.gx = gx;  g_estado.gy = gy;  g_estado.gz = gz;
            if (!missao) {
                g_estado.pwm_bombordo_us  = ESC_US_NEUTRO;
                g_estado.pwm_estibordo_us = ESC_US_NEUTRO;
            }
            /* Re-ler PWM para aplicar nos ESCs */
            pwm_bb = g_estado.pwm_bombordo_us;
            pwm_eb = g_estado.pwm_estibordo_us;
            xSemaphoreGive(g_mutex);
        }

        /* ── 5. Aplicar PWM nos ESCs ── */
        esc_set_us(ESC_CH_BOMBORDO,  pwm_bb);
        esc_set_us(ESC_CH_ESTIBORDO, pwm_eb);

        /* ── 6. Log consolidado a cada 1 s ── */
        if (ciclos % 20 == 0) {
            if (missao) {
                float dist_log = nav_haversine(lat, lon, wp_lat, wp_lon);
                printf("[CTR] dist=%.0fm brg=%.1f° hdg=%.1f° sog=%.1f km/h "
                       "| ESC bb=%u eb=%u | I=%.0f\n",
                       dist_log,
                       nav_bearing(lat, lon, wp_lat, wp_lon),
                       heading, sog,
                       (unsigned)pwm_bb, (unsigned)pwm_eb,
                       vel_integral);
            } else {
                printf("[CTR] Aguardando missao | US=%.1fcm gz=%.2f°/s\n",
                       us_dist, gz);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  22. TAREFA FreeRTOS: task_telemetria
 *
 *  Core 0 | Prioridade 5 | Ciclo: 2 Hz (500 ms) no estado OPERANDO
 *
 *  Responsabilidades:
 *    - Executar a FSM de conectividade (CONECTANDO_REDE → CONECTANDO_MQTT)
 *    - No estado OPERANDO: ler GNSS e magnetômetro a 2 Hz, publicar MQTT
 *    - Parsear string bruta do GNSS → lat, lon, sog, cog, hdop numéricos
 *    - Calcular heading_deg = atan2(my, mx) após leitura do QMC5883L
 *    - Simular INA226: voltage, current, SOC, energy_wh (2 Hz)
 *    - Capturar e processar URCs assíncronas do modem
 *    - Consolidar snapshot de g_estado e publicar JSON enriquecido
 *
 *  Variáveis de estado são locais à tarefa (não há mais globais para FSM):
 *    estado_atual, cont_erros_mqtt, ultima_telemetria
 * ═══════════════════════════════════════════════════════════════════════════*/

static void task_telemetria(void *arg) {
    Estado   estado_atual      = CONECTANDO_REDE;
    int      cont_erros_mqtt   = 0;
    uint32_t ultima_telemetria = 0;

    printf("[TEL] task_telemetria iniciada (FSM + 2 Hz, Core 0)\n");

    while (1) {

        /* Espelhar estado FSM na g_estado para visibilidade de outras tarefas */
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_estado.estado_fsm = estado_atual;
            g_estado.erros_mqtt = cont_erros_mqtt;
            xSemaphoreGive(g_mutex);
        }

        switch (estado_atual) {

            /* ════════════════════════════════════════════════════════════
             *  Estado 1: CONECTANDO_REDE
             * ════════════════════════════════════════════════════════════*/
            case CONECTANDO_REDE:
                printf("\n[FSM] > CONECTANDO_REDE\n");
                if (conectarRede()) {
                    printf("[FSM] Rede e GNSS prontos -> CONECTANDO_MQTT\n");
                    estado_atual = CONECTANDO_MQTT;
                } else {
                    printf("[FSM] Falha. Aguardando 15 s para nova tentativa...\n");
                    delay_ms(15000);
                }
                break;

            /* ════════════════════════════════════════════════════════════
             *  Estado 2: CONECTANDO_MQTT
             * ════════════════════════════════════════════════════════════*/
            case CONECTANDO_MQTT:
                printf("\n[FSM] > CONECTANDO_MQTT\n");
                liberarSessaoMQTT();
                delay_ms(1000);

                if (conectarMQTT()) {
                    printf("[FSM] MQTT pronto -> OPERANDO\n");
                    estado_atual      = OPERANDO;
                    cont_erros_mqtt   = 0;
                    ultima_telemetria = millis() - INTERVALO_TELEMETRIA_MS;
                } else {
                    printf("[FSM] Falha no MQTT. Aguardando 8 s...\n");
                    delay_ms(8000);
                    if (!enviarAT("AT+CGACT?", "+CGACT: 1,1", AT_TIMEOUT_MS, NULL, 0)) {
                        printf("[FSM] PDP perdido -> CONECTANDO_REDE\n");
                        estado_atual = CONECTANDO_REDE;
                    }
                }
                break;

            /* ════════════════════════════════════════════════════════════
             *  Estado 3: OPERANDO  (loop 2 Hz)
             * ════════════════════════════════════════════════════════════*/
            case OPERANDO: {
                /* [A] URCs assíncronas (comandos do backend) */
                capturarURCs();
                processarURCs(&cont_erros_mqtt);

                /* [B] Watchdog de conexão MQTT */
                if (cont_erros_mqtt >= MAX_ERROS_MQTT) {
                    printf("[FSM] Limite de erros MQTT -> verificando conexao...\n");
                    if (!mqttConectado()) {
                        printf("[FSM] MQTT desconectado -> CONECTANDO_MQTT (GNSS ativo)\n");
                        estado_atual = CONECTANDO_MQTT;
                        break;
                    } else {
                        printf("[FSM] Conexao MQTT OK. Resetando erros.\n");
                        cont_erros_mqtt = 0;
                    }
                }

                /* [C] Controle de temporização 2 Hz */
                uint32_t agora = millis();
                if (agora - ultima_telemetria < INTERVALO_TELEMETRIA_MS) {
                    delay_ms(5);
                    break;
                }
                ultima_telemetria = agora;

                /* [C.1] Ler GNSS e parsear campos numéricos */
                bool tem_fix = lerGNSSInfo(bufGnss, sizeof(bufGnss));
                float gnss_lat = 0.0f, gnss_lon = 0.0f;
                float gnss_sog = 0.0f, gnss_cog = 0.0f, gnss_hdop = 9.9f;
                if (tem_fix) {
                    tem_fix = parse_gnss(bufGnss, &gnss_lat, &gnss_lon,
                                         &gnss_sog, &gnss_cog, &gnss_hdop);
                    if (tem_fix)
                        printf("[GNSS] lat=%.6f lon=%.6f sog=%.1f cog=%.1f hdop=%.2f\n",
                               gnss_lat, gnss_lon, gnss_sog, gnss_cog, gnss_hdop);
                }
                if (!tem_fix) printf("[GNSS] Sem fix.\n");

                /* [C.2] Ler Magnetômetro e calcular heading */
                int16_t mx = 0, my = 0, mz = 0;
                float   heading = 0.0f;
                if (!qmc_read(&mx, &my, &mz)) {
                    printf("[MAG] ERRO: Falha I2C.\n");
                } else {
                    heading = atan2f((float)my, (float)mx) * (180.0f / (float)M_PI);
                    if (heading < 0.0f) heading += 360.0f;
                    printf("[MAG] X=%d Y=%d Z=%d heading=%.1f°\n", mx, my, mz, heading);
                }

                /* [C.3] Simular bateria INA226 com base no PWM atual */
                float bat_v = 12.8f, bat_i = 0.5f, bat_soc = 100.0f, bat_wh = 50.0f;
                uint32_t pwm_bb_tel = ESC_US_NEUTRO, pwm_eb_tel = ESC_US_NEUTRO;
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    pwm_bb_tel = g_estado.pwm_bombordo_us;
                    pwm_eb_tel = g_estado.pwm_estibordo_us;
                    xSemaphoreGive(g_mutex);
                }
                bat_simular(pwm_bb_tel, pwm_eb_tel, &bat_v, &bat_i, &bat_soc, &bat_wh);

                /* [C.3b] Alarmes de bateria (Bloco 4) */
                if (bat_soc < BAT_SOC_ALARME) {
                    publicarAlarme("soc_critico", bat_soc);
                }
                if (bat_v < BAT_V_CORTE) {
                    publicarAlarme("tensao_corte", bat_v);
                    /* Cancelar missão por aqui também (task_telemetria conhece o estado) */
                    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                        g_estado.missao_ativa = false;
                        xSemaphoreGive(g_mutex);
                    }
                }

                /* Escrever todos os dados lentos em g_estado */
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    g_estado.gnss_fix = tem_fix;
                    g_estado.lat      = gnss_lat;
                    g_estado.lon      = gnss_lon;
                    g_estado.sog_kmh  = gnss_sog;
                    g_estado.cog_deg  = gnss_cog;
                    g_estado.hdop     = gnss_hdop;
                    strncpy(g_estado.gnss_raw, bufGnss, sizeof(g_estado.gnss_raw) - 1);
                    g_estado.gnss_raw[sizeof(g_estado.gnss_raw) - 1] = '\0';
                    g_estado.mag_x      = mx;
                    g_estado.mag_y      = my;
                    g_estado.mag_z      = mz;
                    g_estado.heading_deg = heading;
                    g_estado.voltage_v  = bat_v;
                    g_estado.current_a  = bat_i;
                    g_estado.power_w    = bat_v * bat_i;
                    g_estado.soc_pct    = bat_soc;
                    g_estado.energy_wh  = bat_wh;
                    xSemaphoreGive(g_mutex);
                }

                /* [C.4] Montar snapshot e publicar telemetria MQTT */
                DadosTelemetria_t snap = {0};
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    snap.lat        = g_estado.lat;
                    snap.lon        = g_estado.lon;
                    snap.sog_kmh    = g_estado.sog_kmh;
                    snap.cog_deg    = g_estado.cog_deg;
                    snap.hdop       = g_estado.hdop;
                    snap.gnss_fix   = g_estado.gnss_fix;
                    snap.mag_x      = g_estado.mag_x;
                    snap.mag_y      = g_estado.mag_y;
                    snap.mag_z      = g_estado.mag_z;
                    snap.heading_deg = g_estado.heading_deg;
                    snap.us_dist_cm = g_estado.us_dist_cm;
                    snap.us_y_norm  = g_estado.us_y_norm;
                    snap.us_alarme  = g_estado.us_alarme;
                    snap.voltage_v  = g_estado.voltage_v;
                    snap.current_a  = g_estado.current_a;
                    snap.soc_pct    = g_estado.soc_pct;
                    snap.energy_wh  = g_estado.energy_wh;
                    snap.ax         = g_estado.ax;
                    snap.ay         = g_estado.ay;
                    snap.az         = g_estado.az;
                    snap.gz         = g_estado.gz;
                    xSemaphoreGive(g_mutex);
                }

                if (publicarTelemetria(&snap)) {
                    cont_erros_mqtt = 0;
                } else {
                    cont_erros_mqtt++;
                    printf("[FSM] Erro de publicacao MQTT #%d\n", cont_erros_mqtt);
                }
                break;
            }

        } /* end switch */
    }     /* end while(1) */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  23. PONTO DE ENTRADA – app_main
 *
 *  Inicializa hardware, mutex e ESCs, cria as duas tarefas FreeRTOS e
 *  se encerra (vTaskDelete). Todo o comportamento do sistema vive em
 *  task_telemetria (Core 0) e task_controle (Core 1).
 * ═══════════════════════════════════════════════════════════════════════════*/

void app_main(void) {
    printf("\n");
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║    HDrop - Barco Autonomo  v2.0          ║\n");
    printf("  ║    ESP32 | A7670SA | QMC5883L | AJ-SR04M ║\n");
    printf("  ╚══════════════════════════════════════════╝\n\n");

    /* ── Inicializar periféricos ── */
    init_uart_modem();
    init_i2c();
    qmc_init();
    us_init_gpio();

    for (int i = 0; i < US_N_MEDIA; i++) us_janela[i] = us_dist_valida;

    /* ── Criar mutex antes de qualquer acesso à g_estado ── */
    g_mutex = xSemaphoreCreateMutex();
    configASSERT(g_mutex != NULL);

    /* ── Inicializar g_estado com valores seguros ── */
    memset(&g_estado, 0, sizeof(g_estado));
    g_estado.pwm_bombordo_us  = ESC_US_NEUTRO;
    g_estado.pwm_estibordo_us = ESC_US_NEUTRO;
    g_estado.us_dist_cm       = 150.0f;
    g_estado.us_y_norm        = 0.44f;
    g_estado.voltage_v        = 12.8f;
    g_estado.soc_pct          = 100.0f;
    g_estado.estado_fsm       = CONECTANDO_REDE;

    /* ── Inicializar ESCs: LEDC configurado → neutro (1500 µs) ── */
    esc_init();
    esc_neutro();

    delay_ms(2000);

    printf("[INIT] Hardware pronto. Criando tarefas FreeRTOS...\n");
    printf("──────────────────────────────────────────────────────\n");

    /* task_controle: Core 1, prioridade 10, stack 4096 bytes */
    xTaskCreatePinnedToCore(
        task_controle, "controle",
        4096, NULL, 10, NULL,
        1
    );

    /* task_telemetria: Core 0, prioridade 5, stack 8192 bytes
     * Stack maior: AT commands e strings de MQTT consomem mais heap de stack. */
    xTaskCreatePinnedToCore(
        task_telemetria, "telemetria",
        8192, NULL, 5, NULL,
        0
    );

    printf("[INIT] Tarefas criadas. app_main encerrando.\n");
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  FIM DO ARQUIVO
 *
 *  Tópicos MQTT:
 *    Publica  → hdrop/raw
 *               {"g":{lat,lon,sog,cog,hdop,fix},"m":{x,y,z,hdg},
 *                "u":{d,y,al},"b":{v,i,soc,wh},"a":{ax,ay,az,gz}}
 *    Assina   → hdrop/comando {"lat":..., "lon":...}
 *
 *  Prefixos de log:
 *    [AT>]   Comando enviado ao modem
 *    [AT<]   Resposta do modem
 *    [GNSS]  Leitura de posição (task_telemetria)
 *    [MAG]   Leitura do magnetômetro (task_telemetria)
 *    [CTR]   Log consolidado 1 Hz da task_controle
 *    [PUB]   Telemetria publicada
 *    [CMD]   Comando de rota recebido
 *    ROTA:   JSON de rota (Bloco 3: aciona algoritmo de navegação)
 *    [FSM]   Mudança de estado da máquina
 *    [ESC]   Configuração do LEDC
 *    [TEL]   Eventos da task_telemetria
 *
 *  Para ativar o AJ-SR04M real (quando disponível):
 *    1. Descomente us_ler_real() na seção 13
 *    2. Em us_processar(): troque us_ler_simulado() por us_ler_real()
 * ═══════════════════════════════════════════════════════════════════════════*/
