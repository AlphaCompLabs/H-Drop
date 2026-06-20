## ETAPA 1 — Controle primitivo dos motores (ESC via LEDC)
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
