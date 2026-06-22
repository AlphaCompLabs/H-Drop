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