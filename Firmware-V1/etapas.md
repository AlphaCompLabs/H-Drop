# H-DROP Firmware — Mensagem Inicial para Claude Code

> Envie esta mensagem ANTES de qualquer etapa de desenvolvimento.
> Ela define as regras, padrões e o contexto que valem para toda a sessão.

---

## Contexto do projeto

Você está desenvolvendo o firmware embarcado do **H-DROP**, um Veículo de Superfície Autônomo (ASV) tipo catamarã para detecção e georreferenciamento de resíduos flutuantes em corpos d'água. O desenvolvimento é feito em **ESP-IDF (não Arduino Framework)** com linguagem **C++** e **FreeRTOS nativo**.

O firmware será desenvolvido em etapas incrementais. Cada etapa produz um componente novo que se integra aos anteriores. Você receberá as etapas uma a uma após esta mensagem inicial.

---

## Stack tecnológico

| Item | Detalhe |
|---|---|
| MCU | ESP32-WROOM-32 |
| Framework | **ESP-IDF v5.4.x — não usar Arduino.h** |
| Linguagem | C++ (arquivos .cpp/.h) |
| RTOS | FreeRTOS nativo (já incluído no ESP-IDF) |
| Build | CMake via `idf.py` |
| Logging | `esp_log.h` — não usar `printf` em código de produção |
| PWM | `driver/ledc.h` — não usar ESP32Servo |
| I2C | `driver/i2c.h` — não usar Wire.h |
| UART | `driver/uart.h` — não usar Serial/Serial2 |
| Timer | `esp_timer.h` e FreeRTOS timers |
| NVS | `nvs_flash.h` e `nvs.h` |
| GPIO | `driver/gpio.h` |
| Delay | `vTaskDelay(pdMS_TO_TICKS(ms))` — não usar delay() |

---

## Estrutura de diretórios — IMUTÁVEL

```
hdrop_firmware/              ← raiz do projeto ESP-IDF
├── CMakeLists.txt
├── sdkconfig
├── build/                   ← gerado automaticamente, não modificar
├── main/
│   ├── CMakeLists.txt
│   └── main.cpp
└── components/
    ├── hdrop_motor/
    │   ├── CMakeLists.txt
    │   ├── hdrop_motor.cpp
    │   └── include/
    │       └── hdrop_motor.h
    ├── hdrop_heading/
    │   ├── CMakeLists.txt
    │   ├── hdrop_heading.cpp
    │   └── include/
    │       └── hdrop_heading.h
    ├── hdrop_pose/
    │   ├── CMakeLists.txt
    │   ├── hdrop_pose.cpp
    │   └── include/
    │       └── hdrop_pose.h
    ├── hdrop_nav/
    │   ├── CMakeLists.txt
    │   ├── hdrop_nav.cpp
    │   └── include/
    │       └── hdrop_nav.h
    └── hdrop_avoidance/
        ├── CMakeLists.txt
        ├── hdrop_avoidance.cpp
        └── include/
            └── hdrop_avoidance.h
```

### Regras de diretório (OBRIGATÓRIAS)

1. **Nunca criar pastas ou arquivos fora desta estrutura** sem solicitação explícita.
2. **Nunca renomear** componentes, arquivos ou pastas já existentes.
3. **Nunca mover** arquivos entre diretórios sem solicitação.
4. Arquivos de cabeçalho (`.h`) sempre dentro de `include/` do respectivo componente.
5. Arquivos de implementação (`.cpp`) sempre na raiz do componente.
6. Cada componente tem exatamente **um** `CMakeLists.txt` próprio.
7. O `main/CMakeLists.txt` lista apenas os componentes necessários naquela etapa como dependências — nunca todos de uma vez.
8. Se for necessário criar um arquivo auxiliar não previsto, perguntar antes de criar.

---

## Padrão de comentários — OBRIGATÓRIO em todos os arquivos

Todos os arquivos produzidos devem seguir este padrão. Comentários devem ser:
- **Escritos em português**
- **Impessoais** (sem "eu", "nós", "você"; usar infinitivo ou voz passiva)
- **Explicativos do porquê**, não apenas do quê (o código já mostra o quê)
- **Presentes em todas as funções públicas, structs e defines relevantes**

### 1. Cabeçalho de arquivo (todo .cpp e todo .h)

```cpp
/**
 * @file hdrop_motor.cpp
 * @brief Controle dos motores ESC do H-DROP via periférico LEDC do ESP-IDF.
 * @details Implementa primitivos PWM para dois ESCs brushless em configuração
 *          diferencial, mistura cinemática ICR e watchdog de segurança por
 *          software. Não depende da biblioteca ESP32Servo — usa driver LEDC nativo.
 *
 * @hardware  ESC Esquerdo: GPIO4 | ESC Direito: GPIO15
 * @depends   driver/ledc.h, driver/gpio.h, freertos
 */
```

### 2. Comentário de função pública (toda função no .h e no .cpp)

```cpp
/**
 * @brief Inicializa os dois canais LEDC e executa a sequência de arming dos ESCs.
 * @details Configura timer LEDC a 50 Hz com resolução de 16 bits e mantém
 *          sinal neutro (1520 µs) por 4 segundos antes de liberar comandos.
 *          Deve ser chamada uma única vez no início de app_main().
 * @return ESP_OK em sucesso, ESP_FAIL se a configuração LEDC falhar.
 * @note   O sistema não aceita comandos de movimento antes que esta função
 *         retorne ESP_OK. A sequência de arming não pode ser pulada.
 */
esp_err_t motor_init(void);
```

### 3. Seção de código (agrupa blocos lógicos dentro de funções)

```cpp
/* ----------------------------------------------------------------
 * Configuração do timer LEDC a 50 Hz com resolução de 16 bits.
 * A resolução de 16 bits (65535 steps) é necessária para representar
 * larguras de pulso ESC de 1 µs com precisão suficiente.
 * ---------------------------------------------------------------- */
ledc_timer_config_t timer_cfg = { ... };
```

### 4. Comentário de linha (explica decisão ou detalhe não óbvio)

```cpp
/* Normaliza para (-π, π) via atan2f(sin, cos) para evitar
   descontinuidade quando o ângulo cruza 0°/360° */
alpha = atan2f(sinf(alpha), cosf(alpha));

/* Limita a força repulsiva a zero além do limiar para
   evitar que obstáculos distantes afetem a navegação normal */
if (dist_cm >= AVO_D_THRESH_FRONT) return 0.0f;
```

### 5. Define e constantes (toda constante relevante)

```cpp
/** Largura de pulso neutra dos ESCs em microssegundos.
 *  Valor calibrado para os ESCs do H-DROP (não é o padrão de 1500 µs). */
#define MOTOR_PONTO_MORTO_US  1520

/** Delta máximo de pulso a partir do ponto morto.
 *  Range completo: 1520 ± 480 µs = [1040, 2000] µs efetivos. */
#define MOTOR_DELTA_MAX_US    480
```

### 6. Struct e enum (toda definição de tipo)

```cpp
/**
 * @brief Representa a pose completa do veículo no plano de navegação.
 * @details Segue a representação q = (x, y, θ) ∈ SE(2) da cinemática
 *          diferencial planar. x e y são expressos em metros relativos
 *          ao ponto de referência (primeiro fix GNSS válido após boot).
 */
typedef struct {
    float x;            /**< Posição leste-oeste em metros (eixo X local) */
    float y;            /**< Posição norte-sul em metros (eixo Y local) */
    float theta;        /**< Orientação em radianos, intervalo [0, 2π) */
    bool  gnss_valid;   /**< Indica se o último fix GNSS foi aceito como válido */
    uint32_t last_gnss_ms; /**< Timestamp do último fix em milissegundos (millis) */
} hdrop_pose_t;
```

---

## Convenções de nomenclatura

| Elemento | Convenção | Exemplo |
|---|---|---|
| Arquivos | `hdrop_<modulo>.cpp/.h` | `hdrop_motor.cpp` |
| Funções públicas | `<modulo>_<verbo>_<complemento>()` | `motor_set_speeds()` |
| Funções internas (static) | `<verbo>_<complemento>()` | `set_esc_pulse_us()` |
| Tipos (struct/enum) | `hdrop_<nome>_t` | `hdrop_pose_t` |
| Constantes/Defines | `MODULO_NOME_UNIDADE` | `MOTOR_PONTO_MORTO_US` |
| Variáveis globais | `g_<nome>` | `g_pose_atual` |
| Tasks FreeRTOS | `<modulo>_task()` | `heading_task()` |
| Tags de log | `"MODULO"` em maiúsculas | `"MOTOR"`, `"NAV"` |

---

## Regras gerais de implementação

1. **Nenhum `#include <Arduino.h>`** em qualquer arquivo — ESP-IDF puro.
2. **Nenhum `delay()`** — usar `vTaskDelay(pdMS_TO_TICKS(ms))`.
3. **Nenhum `printf()`** em código de produção — usar `ESP_LOGI/LOGE/LOGW`.
4. **Nenhum acesso direto a variáveis globais entre componentes** — usar getters/setters ou notificações FreeRTOS.
5. **Mutex obrigatório** em toda variável global lida por múltiplas tasks.
6. Todo retorno de função pública deve ser `esp_err_t` quando há possibilidade de falha.
7. Verificar o retorno de todas as funções ESP-IDF e registrar erros com `ESP_LOGE`.
8. Cada componente deve compilar de forma independente (`idf.py build` deve passar após cada etapa).
9. **Não alterar arquivos de etapas anteriores** sem solicitação explícita.

---

## Como as etapas serão enviadas

As etapas de desenvolvimento serão enviadas uma a uma, em sequência, como arquivos `.md` individuais. Cada arquivo de etapa contém:

- Descrição do hardware envolvido
- Fundamentação teórica relevante
- Referência aos arquivos de código existentes (quando aplicável)
- A tarefa de implementação com assinaturas de função
- Restrições específicas da etapa
- Critérios de aceitação mensuráveis

**Ao receber cada etapa:**
1. Criar os arquivos do componente conforme especificado
2. Seguir integralmente o padrão de comentários desta mensagem inicial
3. Não modificar componentes de etapas anteriores, salvo adição de dependência no `CMakeLists.txt`
4. Confirmar ao final que `idf.py build` passaria sem erros (verificar dependências e includes)
5. Não antecipar implementações de etapas futuras

Aguarde o envio da **Etapa 1** após esta mensagem.

---

*Projeto H-DROP | Firmware v1.0 | ESP-IDF v5.4.x | ESP32-WROOM*