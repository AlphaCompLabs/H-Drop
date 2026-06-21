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
