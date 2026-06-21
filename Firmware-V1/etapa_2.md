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