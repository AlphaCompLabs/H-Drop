# Simulações de Bancada — H-DROP Firmware

Registro de todas as adaptações feitas para viabilizar testes sem o veículo na água.
Cada simulação documenta o que foi alterado, por quê e como reverter para operação real.

---

## Simulação 1 — Calibração hard-iron desabilitada (Etapa 2)

**Arquivo:** `main/main.cpp`

**O que foi feito:**
A chamada a `heading_calibrate(100)` foi comentada. Em bancada, o veículo não realiza rotação completa, o que produziria `x_min ≈ x_max` e `y_min ≈ y_max` — offsets inválidos que comprometeriam todas as leituras de heading posteriores.

**Trecho atual (bancada):**
```cpp
// heading_calibrate(100);
```

**Como reverter para campo:**
1. Descomentar a linha acima em `main/main.cpp`.
2. Garantir que o veículo realize ao menos uma rotação completa (~360°) nos primeiros ~2 s após o boot (100 amostras × 20 ms).
3. Após a primeira calibração bem-sucedida, os offsets são gravados em NVS e persistem entre reinicializações — a linha pode ser comentada novamente para boots subsequentes no mesmo local.

**Critério de aceitação em campo:**
`ESP_LOGI` do componente heading deve exibir `x=[A,B] y=[C,D]` com intervalo significativo (ex: `x=[-1200, 800]`). Valores com diferença < 50 indicam calibração inválida.

---

## Simulação 2 — Controle de heading desabilitado (Etapa 2)

**Arquivo:** `main/main.cpp`

**O que foi feito:**
A chamada a `heading_hold(0.0f)` foi comentada. Sem calibração válida e sem flutuação no meio aquático, acionar `motor_mix()` com setpoint de orientação produziria movimentos imprevisíveis e possivelmente danosos ao hardware.

**Trecho atual (bancada):**
```cpp
// heading_hold(0.0f);
```

**Como reverter para campo:**
1. Descomentar após Simulação 1 (calibração) ter sido executada e validada.
2. O argumento `0.0f` orienta para norte magnético. Alterar conforme o heading desejado em campo (em radianos, intervalo `[0, 2π)`).
3. O controle é contínuo: a `heading_task` aplica `motor_mix()` a 10 Hz até `|eθ| < 0.1 rad`.

**Critério de aceitação em campo:**
Logs da `heading_task` devem mostrar `e` decrescente e `ω` convergindo para zero. O veículo deve estabilizar com `|eθ| < 0.1 rad` em menos de 10 s.

---

## Simulação 3 — Loop principal de monitoramento passivo (Etapa 2)

**Arquivo:** `main/main.cpp`

**O que foi feito:**
O loop principal da Etapa 2 foi substituído por um loop de monitoramento passivo que lê o heading a cada 500 ms e loga a variação — substituindo o fluxo ativo de `heading_hold()` + controle por motor. Além disso, o loop chama `motor_set_speeds(0.0f, 0.0f)` a cada iteração para alimentar o watchdog do motor.

**Trecho atual (bancada):**
```cpp
while (true) {
    motor_set_speeds(0.0f, 0.0f);  // alimenta watchdog sem acionar ESCs
    float theta = 0.0f;
    if (heading_read(&theta)) {
        float variacao = fabsf(theta - theta_ant);
        ESP_LOGI(TAG, "Heading: %.3f rad | var: %.4f rad", theta, variacao);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}
```

**Por que não funciona em campo com `heading_hold` ativo:**
Quando `heading_hold(target)` está ativo, a `heading_task` chama `motor_mix()` a cada 100 ms para aplicar o comando de rotação calculado pelo controlador P. O `motor_set_speeds(0.0f, 0.0f)` do loop principal (executando a cada 500 ms) sobrescreve esse comando, zerando os motores a cada meio segundo e impedindo a convergência do controlador.

Em bancada isso é inofensivo (motores parados de qualquer forma). Em campo, causa oscilação e falha de controle.

**Como reverter para campo:**
1. Remover `motor_set_speeds(0.0f, 0.0f)` do loop principal — a `heading_task` já reseta o watchdog internamente ao chamar `motor_set_speeds()` via `motor_mix()`.
2. O loop principal deve apenas monitorar (leitura passiva sem enviar comandos de motor):
```cpp
// Loop em campo: apenas monitora, sem interferir no controle
while (true) {
    hdrop_pose_t p = pose_get();
    float theta = 0.0f;
    heading_read(&theta);
    ESP_LOGI(TAG, "Heading: %.3f rad | Pose: x=%.2f y=%.2f fix=%d",
             theta, p.x, p.y, p.gnss_valid);
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

**Critério de aceitação em campo:**
Com o loop acima (sem `motor_set_speeds`) e `heading_hold(0.0f)` ativo, os logs da `heading_task` devem mostrar `ω` não-zero e convergindo. Os ESCs devem responder ao controlador sem interrupções do loop principal.

---

## Simulação 4 — Strings GNSS sintéticas para validação de parsing (Etapa 3)

**Arquivo:** `main/main.cpp`

**O que foi feito:**
A função `executar_teste_bancada_pose()` chama `pose_update_gnss()` diretamente com strings `+CGNSSINFO` pré-calculadas, sem necessidade do modem A7670SA ou de fix GNSS real. As strings foram calculadas analiticamente para a região de Brasília/DF e verificadas numericamente.

**Strings sintéticas usadas:**

| String | Descrição | y esperado | x esperado |
|---|---|---|---|
| `GNSS_FIX_REF` | Referência (15°47'S, 47°20'W), mode=2 | 0.0 m | 0.0 m |
| `GNSS_FIX_100M_N` | 100 m ao norte da referência, mode=3 | 100.0 m | 0.0 m |
| `GNSS_FIX_100M_E` | 100 m ao leste da referência, mode=3 | 0.0 m | 100.0 m |

Cálculo de verificação (Python):
```python
# 100 m ao norte: delta_lat = 100 / 6371000 rad → lat DDMM = 1546.946041,S
# 100 m ao leste: delta_lon = 100 / (6371000 × cos(lat0_rad)) rad → lon DDMM = 04719.943927,W
# Erro numérico verificado: < 0.001 m nos dois eixos
```

**Como reverter para campo:**
1. Remover ou comentar a chamada a `executar_teste_bancada_pose()` em `app_main()`.
2. Remover as três constantes `GNSS_FIX_*` do arquivo.
3. A `pose_task` opera de forma autônoma — nenhuma chamada explícita a `pose_update_gnss()` é necessária em `main.cpp`. A task chama a função internamente após cada resposta válida de `AT+CGNSSINFO`.

**Critério de aceitação em campo:**
Logs da `pose_task` devem exibir `[GNSS] Fix: <string real>` e `Pose GNSS: x=... y=...` com valores coerentes com a posição geográfica. O primeiro fix define o ponto de referência `(lat₀, lon₀)`.

---

## Simulação 5 — pose_task em modo de timeout contínuo (Etapa 3)

**Arquivo:** implícito em `pose_init()` → `pose_task`

**O que foi feito:**
Não é uma modificação explícita de código, mas um comportamento esperado em bancada: a `pose_task` inicia a FSM normalmente e tenta executar `AT` no UART2 (GPIO17/16). Sem o A7670SA conectado, cada `enviar_at()` aguarda seu timeout antes de prosseguir. A FSM ficará em loop em `FSM_CONECTANDO_REDE` com delay de 15 s entre tentativas.

**Impacto em bancada:**
- Logs `[AT>] AT` / `[AT<TMO]` repetidos a cada ~15 s — esperado e inofensivo.
- A `heading_task` e o loop de monitoramento em `app_main` não são afetados.
- O teste de parsing GNSS (`executar_teste_bancada_pose`) chama `pose_update_gnss()` diretamente e não depende da task.

**Como reverter para campo:**
Conectar o A7670SA ao UART2 (TX=GPIO17, RX=GPIO16, GND comum, alimentação 3.8–4.2 V). A FSM reconectará automaticamente sem nenhuma modificação de código.

**Critério de aceitação em campo:**
Sequência de logs esperada:
```
[REDE] Registrado na rede.
[GNSS] Módulo GNSS pronto.
[MQTT] Conectado ao broker.
[MQTT] Subscribe em 'hdrop/comando' confirmado.
[PUB] → {"g":"...","pose":{...}}
```

---

## Resumo de reversões para primeiro teste em campo

| # | O que fazer | Arquivo | Detalhe |
|---|---|---|---|
| 1 | Descomentar `heading_calibrate(100)` | `main/main.cpp` | bloco `[SIMULAÇÃO]` Etapa 2 |
| 2 | Descomentar `heading_hold(0.0f)` | `main/main.cpp` | bloco `[SIMULAÇÃO]` Etapa 2 |
| 3 | Remover `motor_set_speeds(0.0f, 0.0f)` do loop principal | `main/main.cpp` | ver Simulação 3 |
| 4 | Remover chamada a `executar_teste_bancada_pose()` | `main/main.cpp` | logo após `pose_init()` |
| 5 | Remover constantes `GNSS_FIX_*` | `main/main.cpp` | topo do arquivo |
| 6 | Conectar A7670SA ao UART2 | hardware | GPIO17 TX / GPIO16 RX |

Após as reversões 1–5, o `main.cpp` deve conter apenas as três chamadas de init seguidas do loop de monitoramento passivo (leitura sem `motor_set_speeds`) — sem nenhum bloco marcado `[SIMULAÇÃO]`.
