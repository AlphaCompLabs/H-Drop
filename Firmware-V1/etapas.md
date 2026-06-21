# H-DROP — Prompts de Desenvolvimento por Etapa (ESP-IDF)

> Enviar UM prompt por vez, em sequência, após a mensagem inicial ter sido enviada.
> Cada prompt pressupõe que os componentes das etapas anteriores já existem.
> O padrão de comentários, nomenclatura e diretórios está definido na mensagem inicial — não repeti-lo aqui, mas ele é sempre obrigatório.

---

## ETAPA 1 — Controle primitivo dos motores (ESC via LEDC)

**Arquivos a anexar:** `firm_diogo.ino`

---

Implementar o componente `hdrop_motor` para controle dos dois ESCs do H-DROP via periférico LEDC do ESP-IDF.

### Hardware

| Componente | Pino |
|---|---|
| ESC esquerdo | GPIO4 |
| ESC direito | GPIO15 |
| Periférico | LEDC (`driver/ledc.h`) |

### Fundamentação teórica (AR8 — Cinemática e Controle)

**Modelo diferencial e ICR**

O catamarã opera como robô diferencial: dois propulsores no mesmo eixo transversal. O ICR (Centro Instantâneo de Rotação) é o ponto em torno do qual o veículo gira instantaneamente. Com L = distância entre propulsores (bitola):

```
v  = (vR + vL) / 2      velocidade linear resultante
ω  = (vR - vL) / L      velocidade angular resultante
vL = v - ω·(L/2)        velocidade do propulsor esquerdo
vR = v + ω·(L/2)        velocidade do propulsor direito
```

**Mapeamento velocidade → PWM**

ESCs recebem sinal PWM de 50 Hz com largura de pulso em microssegundos. PONTO_MORTO = 1520 µs (neutro). Range: 1000–2000 µs, delta efetivo ±480 µs desde o neutro:

```
pwm_us = 1520 + round(speed_norm × 480)
speed_norm ∈ [-1.0, +1.0]
```

**Arming obrigatório**

ESCs requerem sinal neutro (1520 µs) mantido por 4 segundos no boot antes de aceitar qualquer comando de movimento. Sem arming, o ESC ignora os comandos.

**API LEDC do ESP-IDF (não usar ESP32Servo)**

```cpp
/* Configurar timer LEDC a 50 Hz com resolução de 16 bits.
   Resolução de 16 bits → 65535 steps → precisão de ~0.3 µs por step a 50 Hz */
ledc_timer_config_t timer_cfg = {
    .speed_mode      = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_16_BIT,
    .timer_num       = LEDC_TIMER_0,
    .freq_hz         = 50,
    .clk_cfg         = LEDC_AUTO_CLK
};
ledc_timer_config(&timer_cfg);

/* Conversão de microssegundos para duty LEDC de 16 bits.
   Período total a 50 Hz = 20000 µs. duty = (us / 20000) × 65535 */
uint32_t duty = (uint32_t)((float)us / 20000.0f * 65535.0f);
ledc_set_duty(LEDC_LOW_SPEED_MODE, canal, duty);
ledc_update_duty(LEDC_LOW_SPEED_MODE, canal);
```

### Referência de código

O arquivo `firm_diogo.ino` contém a lógica existente de controle de ESCs com interface web. Extrair: pinos usados, valor de PONTO_MORTO, mapeamento de velocidade para writeMicroseconds e sequência de arming. **Substituir completamente** ESP32Servo pela API LEDC descrita acima. Ignorar as partes de WiFi e WebServer.

### Tarefa

Criar `components/hdrop_motor/hdrop_motor.cpp`, `components/hdrop_motor/include/hdrop_motor.h` e `components/hdrop_motor/CMakeLists.txt`.

```cmake
# components/hdrop_motor/CMakeLists.txt
idf_component_register(
    SRCS "hdrop_motor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos
)
```

Funções a implementar:

```cpp
/* Inicializa LEDC (2 canais, 50 Hz, 16 bits) e executa arming de 4 s */
esp_err_t motor_init(void);

/* Define velocidade normalizada de cada propulsor. Thread-safe com mutex.
   left e right ∈ [-1.0, +1.0] onde +1.0 = avanço máximo */
void motor_set_speeds(float left, float right);

/* Envia 1520 µs para ambos os ESCs imediatamente. Chamada de emergência. */
void motor_stop(void);

/* Realiza mistura cinemática ICR e chama motor_set_speeds().
   v em m/s, omega em rad/s, L_bitola em metros */
void motor_mix(float v, float omega, float L_bitola);

/* Retorna o último valor PWM enviado ao ESC esquerdo (em µs). */
int motor_get_last_pwm_left(void);

/* Retorna o último valor PWM enviado ao ESC direito (em µs). */
int motor_get_last_pwm_right(void);
```

Adicionar watchdog FreeRTOS: `xTimerCreate` de 2 s que chama `motor_stop()` se `motor_set_speeds()` não for chamada dentro do intervalo.

Atualizar `main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES hdrop_motor
)
```

### Restrições

- Não usar `#include <Arduino.h>`, `ESP32Servo`, `delay()` ou `printf()`
- Mutex obrigatório para as variáveis de PWM e para os getters
- PONTO_MORTO = 1520 deve ser define documentado no header
- Não criar arquivos fora de `components/hdrop_motor/` e `main/`
- Não modificar `sdkconfig` nem `CMakeLists.txt` da raiz
- Seguir o padrão de comentários definido na mensagem inicial

### Critérios de aceitação

- `idf.py build` sem erros ou warnings críticos
- `motor_mix(0.5f, 0.0f, 0.35f)` → ambos os ESCs acima de 1520 µs
- `motor_mix(0.0f, 1.0f, 0.35f)` → ESC direito > 1520, ESC esquerdo < 1520
- `motor_stop()` envia 1520 µs em menos de 5 ms
- Watchdog dispara após 2 s sem chamada
- Arming de 4 s completo antes de qualquer comando de movimento

---

## ETAPA 2 — Estabilização de heading (QMC5883L via I2C)

**Arquivos a anexar:** `hdrop_barco_autonomo.ino` + componente `hdrop_motor` (gerado na E1)

---

Implementar o componente `hdrop_heading` para leitura do magnetômetro QMC5883L e controle de heading em malha fechada.

### Hardware

| Componente | Detalhe |
|---|---|
| QMC5883L | I2C: SDA=GPIO21, SCL=GPIO22 |
| Endereço I2C | 0x0D |
| CTRL1 | 0b00011101 → 200 Hz, ±8G, OSR=512, Modo Contínuo |
| Reg. dados | 0x00 → X_LSB, X_MSB, Y_LSB, Y_MSB, Z_LSB, Z_MSB (6 bytes) |

### Fundamentação teórica (AR8 — Cinemática e Controle)

**Controle em malha fechada**

A posição não é medida instantaneamente em robótica móvel — precisa ser estimada. O controle em malha fechada realimenta a saída real do sistema:
```
e = r - b         (erro = referência - saída medida)
u = controlador(e)
```

**Heading a partir do magnetômetro**

O QMC5883L retorna `int16_t` x, y, z. O heading θ no plano horizontal:
```
θ = atan2f(y_cal, x_cal)    resultado em (-π, π] radianos
```
Normalizar para [0, 2π): `if (θ < 0.0f) θ += 2.0f * (float)M_PI`

**Problema do atan2 — fundamental para este projeto**

`arctan(Δy/Δx)` retorna apenas (-π/2, π/2) — 180°. Vetores em quadrantes opostos têm o mesmo arctan. O robô não consegue distinguir "frente" de "trás".

`atan2f(y, x)` usa os sinais separados de y e x para cobrir os 360° completos:
```
Q1 (+,+): atan2 ∈ (0, π/2)      Q2 (-,+): atan2 ∈ (π/2, π)
Q3 (-,-): atan2 ∈ (-π, -π/2)    Q4 (+,-): atan2 ∈ (-π/2, 0)
```

**Erro angular com wrap — obrigatório**

Para evitar descontinuidades quando o ângulo cruza 0°/360°:
```
eθ = atan2f(sinf(θd - θ), cosf(θd - θ))     resultado sempre em (-π, π)
```

**Calibração hard-iron**

Offset sistemático causado por campos magnéticos estáticos próximos:
```
x_cal = x_raw - (x_max + x_min) / 2.0f
y_cal = y_raw - (y_max + y_min) / 2.0f
```

**Controlador P**
```
ω_cmd = Kp_heading × eθ
```
Passado para `motor_mix(0.0f, ω_cmd, L)` — velocidade linear zero, apenas rotação.

**API I2C do ESP-IDF (não usar Wire.h)**

```cpp
#include "driver/i2c.h"

/* Configurar I2C_NUM_0 como master a 400 kHz */
i2c_config_t cfg = {
    .mode             = I2C_MODE_MASTER,
    .sda_io_num       = GPIO_NUM_21,
    .scl_io_num       = GPIO_NUM_22,
    .sda_pullup_en    = GPIO_PULLUP_ENABLE,
    .scl_pullup_en    = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 400000
};
i2c_param_config(I2C_NUM_0, &cfg);
i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

/* Ler 6 bytes a partir do registrador 0x00 do QMC5883L */
uint8_t reg = 0x00, raw[6];
i2c_master_write_read_device(I2C_NUM_0, 0x0D, &reg, 1, raw, 6, pdMS_TO_TICKS(100));
int16_t x = (int16_t)(raw[0] | (raw[1] << 8));
int16_t y = (int16_t)(raw[2] | (raw[3] << 8));
```

### Referência de código

`hdrop_barco_autonomo.ino` contém `qmc_init()` e `qmc_read()` com a lógica correta de inicialização (registradores, modo contínuo). Reutilizar a lógica de configuração dos registradores, substituindo `Wire.h` pela API `driver/i2c.h`.

### Tarefa

Criar `components/hdrop_heading/` com `.cpp`, `.h` e `CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "hdrop_heading.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos nvs_flash hdrop_motor
)
```

```cpp
esp_err_t heading_init(void);
bool heading_read(float *theta_rad);
esp_err_t heading_calibrate(uint16_t n_samples);
void heading_hold(float target_rad);
```

`heading_calibrate()` armazena x_min/max, y_min/max em NVS para persistir entre reinicializações.

Task FreeRTOS `heading_task` a 10 Hz: chama `heading_read()`, atualiza variável compartilhada com mutex.

### Restrições

- `driver/i2c.h` — não usar Wire.h
- Calibração em NVS — não em variável volátil
- `heading_read()` retorna `false` em falha I2C; verificar retorno de `i2c_master_write_read_device`
- Não criar arquivos fora de `components/hdrop_heading/`
- Não modificar `hdrop_motor`
- Seguir padrão de comentários da mensagem inicial

### Critérios de aceitação

- `idf.py build` sem erros
- `heading_read()` retorna valores estáveis com variação < 0.05 rad/s em repouso
- `heading_hold(0.0f)` orienta o barco para norte magnético e estabiliza com |eθ| < 0.1 rad
- Calibração gravada em NVS persiste após reboot

---

## ETAPA 3A — Estimativa de pose via GNSS (A7670SA via UART)

**Arquivos a anexar:** `hdrop_barco_autonomo.ino` + componentes E1 e E2

---

Implementar o componente `hdrop_pose` com leitura de posição via A7670SA e cálculo da pose q=(x,y,θ).

### Hardware

| Componente | Detalhe |
|---|---|
| A7670SA | UART2: TX=GPIO17, RX=GPIO16, 115200 baud |
| MQTT Broker | broker.hivemq.com:1883 |
| Publicação | hdrop/raw |
| Subscrição | hdrop/comando |

### Fundamentação teórica (AR7 — Transformações Espaciais)

**Vetor de configuração q = (x, y, θ) ∈ SE(2)**

O robô diferencial plano tem exatamente 3 graus de liberdade:
- x, y: posição no plano em metros
- θ: orientação (yaw) em radianos

SE(2) é o grupo especial euclidiano do plano — o conjunto de todas as poses possíveis.

**Transformação homogênea planar**

```
T = | cos(θ)  -sin(θ)  x |
    | sin(θ)   cos(θ)  y |
    |   0        0     1 |
```

Permite transformar vetores entre o frame global e o frame local do robô.

**Conversão GNSS → XY local (Terra plana)**

Usar ponto de referência (lat₀, lon₀) fixo no primeiro fix válido:
```
Δlat_rad = (lat_atual_deg - lat0_deg) × π / 180.0
Δlon_rad = (lon_atual_deg - lon0_deg) × π / 180.0
pose.y   = Δlat_rad × 6371000.0
pose.x   = Δlon_rad × cosf(lat0_rad) × 6371000.0
```

**Conversão DDMM.MMMMMM → graus decimais**

O A7670SA retorna lat/lon em DDMM.MMMMMM (graus + minutos decimais):
```
graus_decimais = DD + MM.MMMMMM / 60.0
Exemplo: "1547.123456,S" → -(15.0 + 47.123456/60.0) = -15.78540760°
```

**Fix GNSS válido**

Resposta do +CGNSSINFO: `+CGNSSINFO: mode,SVs,lat,N/S,lon,E/W,...`
Fix válido quando o campo `mode` = "2" (2D) ou "3" (3D).

**Adaptação para ASV (AR9 — Odometria)**

Nos sistemas com encoders, a pose é obtida por integração dos pulsos de roda. No H-DROP, o GNSS substitui os encoders com a vantagem de não acumular drift posicional, mas com menor taxa de atualização (~1 Hz) e latência de fix. θ vem sempre do magnetômetro (200 Hz, sem deriva quando calibrado).

**API UART do ESP-IDF (não usar Serial2)**

```cpp
#include "driver/uart.h"

uart_config_t cfg = {
    .baud_rate  = 115200,
    .data_bits  = UART_DATA_8_BITS,
    .parity     = UART_PARITY_DISABLE,
    .stop_bits  = UART_STOP_BITS_1,
    .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT
};
uart_param_config(UART_NUM_2, &cfg);
uart_set_pin(UART_NUM_2, 17, 16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
uart_driver_install(UART_NUM_2, 1024, 1024, 0, NULL, 0);

/* Enviar comando AT */
uart_write_bytes(UART_NUM_2, "AT+CGNSSINFO\r\n", 14);

/* Ler resposta com timeout de 5 s */
uint8_t buf[512];
int len = uart_read_bytes(UART_NUM_2, buf, sizeof(buf)-1, pdMS_TO_TICKS(5000));
```

### Referência de código

`hdrop_barco_autonomo.ino` contém implementação completa e testada de:
- FSM de conectividade: CONECTANDO_REDE → CONECTANDO_MQTT → OPERANDO
- Funções AT: `enviarAT()`, `drenarSerial2()`, `enviarDadoPrompt()` — portar lógica, substituir Serial2 por UART_NUM_2
- `lerGNSSInfo()` com parse do +CGNSSINFO e detecção de fix
- `publicarTelemetria()` com sequência AT+CMQTTTOPIC/PAYLOAD/PUB
- `processarURCs()` com extração do payload de hdrop/comando (prefixo "ROTA:")

Manter toda a lógica AT e FSM. Substituir apenas o acesso à serial.

### Tarefa

Criar `components/hdrop_pose/`:

```cmake
idf_component_register(
    SRCS "hdrop_pose.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos hdrop_heading
)
```

```cpp
typedef struct {
    float x;              /* metros, relativo ao ponto de referência */
    float y;              /* metros, relativo ao ponto de referência */
    float theta;          /* radianos [0..2π), lido do magnetômetro */
    bool  gnss_valid;     /* indica se o último fix GNSS foi válido */
    uint32_t last_gnss_ms;/* timestamp do último fix (esp_timer_get_time / 1000) */
} hdrop_pose_t;

esp_err_t pose_init(void);
bool      pose_update_gnss(const char *cgnssinfo_str);
void      pose_update_heading(float theta_rad);
hdrop_pose_t pose_get(void);
```

JSON de telemetria expandido:
```json
{"g":"...","m":[x,y,z],"pose":{"x":1.23,"y":4.56,"t":1.57,"fix":1}}
```

### Restrições

- FSM de conectividade mantida integralmente do arquivo de referência
- `driver/uart.h` — não usar Serial2
- lat₀/lon₀ definidos no primeiro fix válido (variável estática)
- Thread-safety obrigatória em `pose_get()`
- Não criar arquivos fora de `components/hdrop_pose/`
- Seguir padrão de comentários da mensagem inicial

### Critérios de aceitação

- `idf.py build` sem erros
- `pose_update_gnss()` retorna `true` para strings com mode="2" ou "3"
- Dois fixes em posições conhecidas produzem distância euclidiana correta (erro < 5 m)
- JSON publicado em hdrop/raw com campo "pose"

---

## ETAPA 3B — Dead reckoning entre fixes GNSS

**Arquivos a anexar:** componentes E1, E2 e E3A

---

Adicionar dead reckoning ao componente `hdrop_pose` para elevar a taxa de atualização da pose de ~1 Hz para 10 Hz entre os fixes GNSS.

### Fundamentação teórica (AR9 — Odometria)

**Integração de pose por dead reckoning**

A cada ciclo de controle com período Δt:
```
Δx = v · cos(θ) · Δt
Δy = v · sin(θ) · Δt
pose.x += Δx
pose.y += Δy
pose.θ  = heading_read()    ← SEMPRE do magnetômetro, nunca integrado
```

**Estimativa de v a partir dos PWMs comandados**

Como não há encoders de roda, v é estimado dos pulsos PWM:
```
v_norm = ((pwm_esq - 1520) + (pwm_dir - 1520)) / (2 × 480)
v_est  = v_norm × V_MAX_MS          (V_MAX_MS ≈ 1.0 m/s, calibrar em campo)
```
Esta estimativa ignora slip, corrente e vento. O erro acumula-se rapidamente na água.

**Por que θ nunca é integrado**

Integrar ω comandado para obter θ acumularia erro angular ao longo do tempo. O magnetômetro fornece θ absoluto a 200 Hz. A abordagem correta é sempre ler θ do sensor, nunca integrá-lo a partir de ω.

**Modelo predictor-corrector**

```
A cada ciclo de 100 ms (10 Hz):
    θ     = heading_read()
    v_est = calcular_v_dos_pwms()
    pose  = dead_reckon(pose, v_est, θ, 0.1f)

A cada fix GNSS válido (~1 Hz):
    pose.x = x_gnss        ← sobrescreve a estimativa por dead reckoning
    pose.y = y_gnss        ← sobrescreve a estimativa
    pose.θ = heading_read() (θ não drift, mas atualiza para coerência)
```

**Timer periódico ESP-IDF (preferível a xTaskCreate para callbacks de controle)**

```cpp
#include "esp_timer.h"

esp_timer_handle_t dr_timer;
esp_timer_create_args_t args = {
    .callback = pose_dr_callback,
    .name     = "pose_dr"
};
esp_timer_create(&args, &dr_timer);
esp_timer_start_periodic(dr_timer, 100000); /* 100 ms = 100000 µs */
```

### Tarefa

Modificar `components/hdrop_pose/hdrop_pose.cpp` (não criar novos arquivos):

1. `void pose_dead_reckon(float v_linear, float dt_s)` — propaga x,y; θ sempre de `heading_read()`
2. Timer ESP-IDF periódico a 10 Hz com callback `pose_dr_cb` que chama dead reckoning e aplica correção GNSS quando disponível
3. Getters no `hdrop_motor` (adicionar ao `hdrop_motor.cpp`): `motor_get_last_pwm_left()` e `motor_get_last_pwm_right()` (já declarados na E1)
4. Log a cada fix: `ESP_LOGI(TAG, "Fix corrigido: x=%.2f y=%.2f err_dr=%.2f m", x, y, dr_err)`

### Restrições

- `pose.theta` NUNCA integrado — sempre lido de `heading_read()`
- Correção GNSS sobrescreve x,y sem suavização
- Thread-safety obrigatória
- Modificar apenas `hdrop_pose.cpp` — não tocar em outros componentes (exceto adicionar getter ao motor se ainda não existir)
- Seguir padrão de comentários da mensagem inicial

### Critérios de aceitação

- Taxa de atualização da pose confirmada a 10 Hz via log serial com timestamps
- Com barco parado, pose.x/y variam menos de 0.5 m/min
- Log `err_dr` presente a cada fix GNSS

---

## ETAPA 4 — Navegação ponto a ponto (controlador polar)

**Arquivos a anexar:** componentes E1, E2, E3A e E3B

---

Implementar o componente `hdrop_nav` com controlador polar para navegação autônoma ponto a ponto.

### Fundamentação teórica completa (AR8 — Controlador Polar)

**Transformação do erro global → referencial local do robô**

O erro de pose deve ser expresso no referencial local do robô para que os comandos sejam fisicamente interpretáveis pela plataforma:

```
| ex  |   | cos(θ)   sin(θ)  0 |   | xd - x   |
| ey  | = | -sin(θ)  cos(θ)  0 | × | yd - y   |
| eθ  |   |    0       0     1 |   | θd - θ   |
```

- ex: distância na direção de avanço
- ey: desvio lateral em relação ao eixo do robô
- eθ: erro de orientação

**Coordenadas polares do erro**

```
ρ = sqrtf((xd-x)² + (yd-y)²)         distância euclidiana ao waypoint
α = atan2f(yd-y, xd-x) - θ           erro de alinhamento com o alvo
β = θd - θ - α                         erro de orientação final
```

Normalizar α e β para (-π, π):
```
α = atan2f(sinf(α), cosf(α))
β = atan2f(sinf(β), cosf(β))
```

**Lei de controle linear proporcional**

```
v = kρ · ρ           velocidade linear proporcional à distância
ω = kα · α + kβ · β  velocidade angular para alinhar e orientar
```

Interpretação:
- `kρ · ρ`: o barco anda rápido quando longe e desacelera ao se aproximar (ρ → 0 implica v → 0)
- `kα · α`: corrige o alinhamento com o alvo; domina enquanto o robô está longe
- `kβ · β`: ajusta a orientação de chegada para que o robô chegue com θd correto

Valores de partida para calibração em campo: kρ=0.6, kα=1.5, kβ=-0.8

**Limites obrigatórios**

```cpp
v = fmaxf(-V_MAX, fminf(V_MAX, v));
ω = fmaxf(-OMEGA_MAX, fminf(OMEGA_MAX, ω));
```

**Mistura ICR → motor_mix()**

Já implementado no componente `hdrop_motor`:
```
vL = v - ω · (L/2)    →  pwm_L = 1520 + round(vL / V_MAX × 480)
vR = v + ω · (L/2)    →  pwm_R = 1520 + round(vR / V_MAX × 480)
```

**Critério de chegada**

```
ρ < ARRIVAL_THRESHOLD    (default: 1.5 m, ajustar em campo)
```

### Tarefa

Criar `components/hdrop_nav/`:

```cmake
idf_component_register(
    SRCS "hdrop_nav.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos hdrop_motor hdrop_pose
)
```

```cpp
typedef struct {
    float lat;
    float lon;
    float theta_d;   /* heading desejado na chegada; default 0.0 (norte) */
    bool  valid;
} hdrop_waypoint_t;

typedef struct {
    float rho, alpha, beta;
    float v_cmd, omega_cmd;
    bool  arrived;
} hdrop_nav_result_t;

esp_err_t          nav_init(void);
void               nav_set_waypoint(hdrop_waypoint_t wp);
hdrop_nav_result_t nav_update(void);
bool               nav_arrived(void);
```

Timer ESP-IDF a 10 Hz executa `nav_update()` quando waypoint ativo.

Integração MQTT: ao receber "ROTA:{lat,lon}" via `hdrop/comando`, chamar `nav_set_waypoint()`. Ao chegar, publicar `"nav":{"state":"ARRIVED"}` em `hdrop/raw` e chamar `motor_stop()`.

Log a cada ciclo:
```cpp
ESP_LOGI("NAV", "rho=%.2f alpha=%.3f beta=%.3f v=%.2f w=%.2f", rho, alpha, beta, v, omega);
```

Defines configuráveis no header:
```cpp
#define NAV_KP_RHO           0.6f
#define NAV_KP_ALPHA         1.5f
#define NAV_KP_BETA         -0.8f
#define NAV_V_MAX            0.8f
#define NAV_OMEGA_MAX        1.571f
#define NAV_L_BITOLA         0.35f
#define NAV_ARRIVAL_THRESH   1.5f
```

### Restrições

- Todos os ângulos normalizados via `atan2f(sinf, cosf)` — sem exceção
- v e ω limitados com `fmaxf/fminf` antes do `motor_mix()`
- `nav_update()` não roda sem waypoint ativo
- Não modificar componentes anteriores
- Seguir padrão de comentários da mensagem inicial

### Critérios de aceitação

- `idf.py build` sem erros
- Com waypoint a 5 m: barco navega e para dentro do raio de chegada
- ρ decresce monotonicamente na maior parte da trajetória
- Log mostra valores fisicamente coerentes

---

## ETAPA 5 — Seguimento de rota (sequência de waypoints)

**Arquivos a anexar:** componentes E1, E2, E3A, E3B e E4

---

Expandir o componente `hdrop_nav` com suporte a missões multiponto (sequência de waypoints).

### Fundamentação teórica (AR9 — Planejamento de Caminhos)

**Objetivos do planejamento**

1. Encontrar caminho realizável: conectar início ao goal respeitando restrições cinemáticas (não-holonômico — só avanço/recuo e rotação diferencial)
2. Evitar colisões: nenhum ponto da trajetória coincide com obstáculo
3. Minimizar custo: distância, energia, tempo ou segurança

**Propriedades dos algoritmos**

- Completude: garantia de encontrar solução em tempo finito se existir
- Eficácia: qualidade do caminho encontrado
- Complexidade: eficiência computacional

**Decisão de navegação com base na pose**

A pose estimada alimenta a lógica de decisão:
- ρ euclidiana indica progresso em direção ao waypoint atual
- Erro angular α determina a correção de heading necessária
- Quando ρ < limiar: avançar ao próximo waypoint da fila

**Sequenciamento de waypoints**

```
fila = [wp0, wp1, ..., wpN-1]
índice = 0

a cada ciclo:
    se nav_arrived():
        índice++
        se índice >= N:
            MISSION_ARRIVED → motor_stop()
        senão:
            nav_set_waypoint(fila[índice])
```

### Tarefa

Modificar `components/hdrop_nav/hdrop_nav.cpp` (não criar novos arquivos):

```cpp
#define MAX_WAYPOINTS 20

typedef enum {
    MISSION_IDLE,
    MISSION_NAVIGATING,
    MISSION_ARRIVED
} hdrop_mission_state_t;

esp_err_t           mission_load(const char *json_route);
void                mission_update(void);
hdrop_mission_state_t mission_get_state(void);
```

Formato JSON esperado em `hdrop/comando`:
```json
{"route":[{"lat":-15.78,"lon":-47.92},{"lat":-15.79,"lon":-47.93}]}
```

Parse com `cJSON` do ESP-IDF (já incluído) ou `sscanf` manual.

Telemetria expandida em `hdrop/raw`:
```json
{"g":"...","m":[x,y,z],"pose":{...},"nav":{"state":"NAV","wp":2,"total":5,"rho":3.21}}
```

Log ao avançar waypoint:
```cpp
ESP_LOGI("NAV", "WP %d/%d atingido. Proximo: lat=%.5f lon=%.5f", idx, total, lat, lon);
```

Ao completar missão: publicar `"nav":{"state":"ARRIVED"}`, chamar `motor_stop()`.

### Restrições

- Nova rota recebida durante MISSION_NAVIGATING aborta e reinicia
- `motor_stop()` obrigatório ao atingir MISSION_ARRIVED
- Modificar apenas `hdrop_nav.cpp` e `.h`
- Seguir padrão de comentários da mensagem inicial

### Critérios de aceitação

- Missão de 3 waypoints executada sequencialmente sem intervenção
- Telemetria reflete índice correto do waypoint ativo
- `motor_stop()` chamado ao final
- Nova rota durante missão reinicia corretamente

---

## ETAPA 6 — Desvio de obstáculos (APF + FSM de escape)

**Arquivos a anexar:** todos os componentes anteriores + `main.cpp`

---

Implementar o componente `hdrop_avoidance` com Campo Potencial Artificial (APF) e FSM de escape para mínimos locais, usando 3 sensores ultrassônicos.

### Hardware

| Sensor | Posição | TRIG | ECHO |
|---|---|---|---|
| Ultrassônico frontal | Frente (0°) | GPIO25 | GPIO26 |
| Ultrassônico esquerdo | Lateral esq. (+90°) | GPIO32 | GPIO33 |
| Ultrassônico direito | Lateral dir. (-90°) | GPIO34 | GPIO35 |

**ATENÇÃO:** O arquivo `main.cpp` de referência usa pinos de Arduino Uno (8, 9, A1). Os pinos devem ser remapeados para os GPIOs listados acima. Toda a lógica de filtragem deve ser portada fielmente.

### Fundamentação teórica (AR7 — C-Space e Geometria)

**Invólucro e margem de segurança**

O robô real tem forma e ocupa região no espaço. O C-space (espaço de configurações) define a região proibida:
```
C_obs = {q | A(q) ∩ W_obs ≠ ∅}
```

Para simplificar o cálculo de colisão, usa-se invólucro circular conservador (independente de θ):
```
r_inv = sqrt((comprimento/2)² + (largura/2)²) + margem_adicional
```

Para o catamaran (mais largo do que comprido), o raio é determinado pela meia-largura + margem. Este valor é somado ao limiar de detecção de cada sensor.

### Fundamentação teórica — APF (Khatib, 1986)

**Campo potencial artificial**

O APF modela a navegação como partícula em campo de forças:
- Goal gera campo **atrativo**: puxa o robô em direção ao waypoint
- Cada obstáculo detectado gera campo **repulsivo**: empurra o robô para longe

**Campo repulsivo (equação de Khatib)**

Para cada sensor i com leitura `dist_i` e limiar `d_threshold`:
```
F_rep_i = 0.0f                                          se dist_i >= d_threshold

F_rep_i = k_rep × (1/dist_i - 1/d_threshold) / dist_i²  se dist_i < d_threshold
```

A força é zero além do limiar e cresce rapidamente ao se aproximar. Isso produz desvios suaves e proporcionais — diferente de limiares binários (obstáculo sim/não).

**Mapeamento das forças repulsivas para comandos diferenciais**

O robô diferencial não pode receber forças em x,y diretamente. As forças modificam v e ω:
```
Sensor frontal (0°):     v_correction  += k_front × F_rep_front  (reduz v)
Sensor esquerdo (+90°):  ω_correction  += +k_side × F_rep_left   (vira direita)
Sensor direito (-90°):   ω_correction  += -k_side × F_rep_right  (vira esquerda)
```

**Composição com o controlador polar**

```
v_final = v_nav - v_correction             (v_nav = saída do hdrop_nav)
ω_final = ω_nav + ω_correction

v_final = fmaxf(0.0f, fminf(V_MAX, v_final))    não recuar com APF
ω_final = fmaxf(-OMEGA_MAX, fminf(OMEGA_MAX, ω_final))
```

**Vantagem sobre Bug2 para este projeto**

O APF produz desvios proporcionais e contínuos à distância. Ao se aproximar do obstáculo, a correção cresce gradualmente — sem saltos discretos. Para um ASV com inércia, mudanças abruptas de comando geram trajetórias indesejadas. O APF é intrinsecamente compatível com a cinemática não-holonômica do catamarã.

### Fundamentação teórica — FSM de escape

**Detecção de mínimo local**

O mínimo local ocorre quando o campo repulsivo equilibra o campo atrativo. Condições simultâneas:
1. Algum sensor ativo (F_rep > 0)
2. ρ não diminui significativamente
3. Condição persiste por mais de T_escape segundos

```
se |ρ_atual - ρ_há_T_escape| < EPSILON_RHO E algum_sensor_ativo:
    → AVOID_ESCAPE
```

**Comportamento de escape**

No estado AVOID_ESCAPE:
1. v = 0 (parar translação)
2. Identificar o lado com maior distância (mais espaço)
3. Girar ω_escape para esse lado
4. Após rotação de ~45°, avançar brevemente
5. Verificar se ρ volta a diminuir; se sim, retornar ao AVOID_APF

### Filtragem dos sensores (portada do main.cpp)

O `main.cpp` de referência implementa 5 camadas de filtragem — portar fielmente para ESP-IDF, sem simplificação:

1. Validação de range: ignorar medições fora de [20 cm, 300 cm] e timeouts
2. Rejeição de saltos: se |nova - última_válida| > 50 cm → `0.7×anterior + 0.3×nova`
3. Média móvel N=6: janela deslizante circular
4. Filtro EMA α=0.75: `dist_f = 0.75×dist_f + 0.25×dist_media`
5. Confirmação de alarme: ≥2 leituras abaixo do limiar para ativar; ≥3 para desativar

Leitura ultrassônica no ESP-IDF (sem pulseIn — não disponível):
```cpp
#include "driver/gpio.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

/* Pulso de trigger de 15 µs */
gpio_set_level(TRIG, 0); ets_delay_us(5);
gpio_set_level(TRIG, 1); ets_delay_us(15);
gpio_set_level(TRIG, 0);

/* Medir duração do echo com timeout de 50 ms */
int64_t t_start = esp_timer_get_time();
while (!gpio_get_level(ECHO) && (esp_timer_get_time()-t_start) < 50000);
int64_t t1 = esp_timer_get_time();
while (gpio_get_level(ECHO) && (esp_timer_get_time()-t1) < 50000);
int64_t t2 = esp_timer_get_time();
float dist_cm = (float)(t2 - t1) * 0.0343f / 2.0f;
```

### Modo de simulação via MQTT

Antes do sensor físico estar acoplado, simular via mensagem em `hdrop/comando`:
```json
{"obstacle":"front"}    dist_front=20, dist_left=280, dist_right=280
{"obstacle":"left"}     dist_front=280, dist_left=20, dist_right=280
{"obstacle":"right"}    dist_front=280, dist_left=280, dist_right=20
{"obstacle":"clear"}    todos = 300
```

### Tarefa

Criar `components/hdrop_avoidance/`:

```cmake
idf_component_register(
    SRCS "hdrop_avoidance.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer freertos hdrop_motor hdrop_nav
)
```

```cpp
typedef enum {
    AVOID_INACTIVE,   /* sem obstáculos; comandos de navegação passam sem alteração */
    AVOID_APF,        /* APF ativo; desvio suave proporcional à distância */
    AVOID_ESCAPE      /* mínimo local detectado; executando manobra de escape */
} hdrop_avoidance_state_t;

typedef struct {
    float v_final;
    float omega_final;
    hdrop_avoidance_state_t state;
    bool  obstacle_active;
} hdrop_avoidance_result_t;

esp_err_t               avoidance_init(void);
void                    avoidance_read_sensors(void);
hdrop_avoidance_result_t avoidance_compute(float v_nav, float omega_nav, float rho_to_goal);
void                    avoidance_set_simulation(const char *mode);
```

Timer ESP-IDF a 10 Hz: lê sensores, obtém v_nav/omega_nav do `hdrop_nav`, computa APF, chama `motor_mix()`.

**Regra arquitetural obrigatória:** `avoidance_timer_cb` é a **única** fonte de chamadas a `motor_mix()`. O `nav_timer_cb` calcula e armazena v_nav/omega_nav, mas NÃO chama `motor_mix()` diretamente.

Telemetria adicional:
```json
"obs":{"f":150,"l":280,"r":260,"st":"APF"}
```

Defines configuráveis:
```cpp
#define AVO_D_THRESH_FRONT   100.0f  /* cm — limiar de detecção frontal */
#define AVO_D_THRESH_SIDE     60.0f  /* cm — limiar de detecção lateral */
#define AVO_K_REP_FRONT        0.8f  /* ganho de repulsão frontal */
#define AVO_K_SIDE             0.5f  /* ganho de repulsão lateral */
#define AVO_T_ESCAPE_S         5.0f  /* s — tempo para detectar mínimo local */
#define AVO_EPSILON_RHO        0.3f  /* m — sem progresso abaixo deste valor */
```

### Restrições

- `avoidance_compute()` NUNCA produz `v_final` negativo no estado AVOID_APF
- Em AVOID_INACTIVE, comandos passam sem modificação para `motor_mix()`
- Lógica de filtragem do main.cpp portada sem simplificação
- Não usar `pulseIn()` — usar `esp_timer_get_time()` e `gpio_get_level()`
- Não modificar componentes anteriores (exceto `hdrop_nav` para expor getters de v_nav/omega_nav)
- Seguir padrão de comentários da mensagem inicial

### Critérios de aceitação

- `idf.py build` sem erros
- Simulação "front" → v reduzida e virada iniciada (log confirma AVOID_APF)
- Simulação "left" → barco vira para a direita
- Simulação "clear" → comportamento idêntico à E5
- Após 5 s em mínimo local simulado → FSM transiciona para AVOID_ESCAPE e executa rotação
- Quando sensor físico for acoplado: apenas pinos mudam; lógica inalterada

---

*H-DROP Firmware v1.1 | ESP-IDF v5.4.x | ESP32-WROOM | Prompts de desenvolvimento por etapa*