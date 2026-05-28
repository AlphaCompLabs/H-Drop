# HDrop — Pipeline de Controle (v2.0)

> Documento de referência gerado a partir de `main/main.c`, `main/README.md` e `MAPEAMENTO.txt`.
> Descreve o estado atual do firmware, a arquitetura de controle planejada e as próximas etapas de desenvolvimento.
>
> **Última atualização:** Blocos 1–4 implementados — controle completo, desvio de obstáculos e gestão de bateria concluídos.

---

## 0. O que o sistema faz hoje

### Firmware (`main/main.c`) — o que roda no ESP32

**Tarefa lenta (`task_telemetria`, 2×/segundo, Core 0):**
Tenta se conectar à internet pelo modem 4G e ao broker MQTT. Quando conectado, lê o GPS e o magnetômetro a cada 500 ms, monta um JSON e publica no tópico `hdrop/raw`. Se chegar uma mensagem com um waypoint no tópico `hdrop/comando`, ela é impressa na serial — mas nenhuma ação é tomada com ela.

**Tarefa rápida (`task_controle`, 20×/segundo, Core 1):**
Executa o pipeline completo do sensor ultrassônico (filtragem, alarme com histerese) e salva o resultado na struct compartilhada. Lê o valor de PWM da mesma struct e aplica nos ESCs via LEDC. O problema: esse valor de PWM foi inicializado em 1500 µs (neutro) e ninguém escreve outro valor — os motores ficam parados.

```
O que funciona hoje (Blocos 1–4 concluídos):
  ✓ Conexão 4G + MQTT (LTE, APN, PDP, GNSS ready)
  ✓ Publicação de telemetria JSON enriquecida (GNSS, mag, IMU, bateria, US)
  ✓ Recepção de waypoints (hdrop/comando → parsear_waypoint → missao_ativa)
  ✓ Pipeline do ultrassônico (filtro, média móvel, alarme com histerese)
  ✓ LEDC PWM 50 Hz nos ESCs bombordo e estibordo
  ✓ Duas tarefas FreeRTOS com struct compartilhada e mutex
  ✓ Simulação IMU MPU-6050 (ax/ay/az/gx/gy/gz, 20 Hz)
  ✓ Simulação bateria INA226 (coulomb counting, SOC, Wh, 2 Hz)
  ✓ GNSS parsing: DDMM.MMMM → graus decimais (lat, lon, sog, cog, hdop)
  ✓ Heading = atan2(my, mx) a partir do QMC5883L
  ✓ Controle PD de heading (Kp=3, Kd=8, banda morta 5°)
  ✓ Controle PI de velocidade (Kp=60, Ki=15, anti-windup ±150 µs)
  ✓ Lógica de missão: haversine, bearing, encerramento em 5 m
  ✓ v_ref adaptativa por SOC (3 / 1.5–3 / 1 km/h)
  ✓ Proteção de corrente (> 28 A reduz PWM_base)
  ✓ Desvio de obstáculos em 3 zonas (30 / 80 / 120 cm)
  ✓ Emergência de tensão (< 11.4 V → neutro + cancela missão)
  ✓ Alarmes MQTT QoS 1 (soc_critico, tensao_corte)

O que falta para hardware real:
  ✗ Driver INA226 real (substituir bat_simular)
  ✗ Driver MPU-6050 real (substituir imu_simular)
  ✗ Ativar us_ler_real() no AJ-SR04M (descomentar)
  ✗ Ajuste fino de Kp/Kd/Ki em campo
```

### Simulação (`hdrop_sim.py` + `dashboard.html`)

Servidor Python que simula o barco inteiro (GPS, 4G, magnetômetro, ultrassônico, bateria, controle PD simples). O dashboard HTML exibe tudo visualmente em tempo real. **Esta simulação é independente do firmware** — existe para permitir visualização durante o desenvolvimento, enquanto o hardware físico não está disponível.

---

## 1. Visão Geral — O que o sistema faz

O HDrop é um barco não tripulado autônomo (ASV) embarcado em ESP32. Seu objetivo é sair de um ponto A, navegar até um ponto B para realizar uma entrega, e retornar — tudo de forma autônoma, sem intervenção humana durante o trajeto.

Para isso, o sistema precisa resolver quatro problemas ao mesmo tempo:

- **Saber onde está** — via GNSS integrado ao modem A7670SA
- **Saber para onde ir** — via waypoint recebido por MQTT (4G)
- **Saber como girar** — via magnetômetro QMC5883L (bússola eletrônica)
- **Saber se há obstáculos** — via sensor ultrassônico AJ-SR04M

A propulsão é feita por dois motores brushless independentes (um em cada lado do casco), controlados por ESCs bidirecionais via sinal PWM gerado pelo ESP32. A diferença de potência entre os dois motores é o que faz o barco virar — não há leme.

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          VISÃO GERAL DO SISTEMA                         │
│                                                                         │
│  ENTRADAS                    ESP32 (Firmware)              SAÍDAS       │
│  ─────────                   ────────────────              ──────       │
│  A7670SA GNSS  ──UART2──►                                               │
│  QMC5883L Mag  ──I2C────►    Máquina de Estados       ──PWM──►  ESC     │
│  MPU-6050 IMU  ──I2C────►    +                                  Motor   │
│  AJ-SR04M US   ──GPIO───►    Controle PID/PD          ──PWM──►  ESC     │
│  INA226 Bat    ──I2C────►                                       Motor   │
│                                                                         │
│  Backend/Op.   ◄──MQTT──    hdrop/raw (telemetria 2 Hz)                 │
│                  ──MQTT──►  hdrop/comando (waypoint)                    │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Hardware Mapeado

### Atuadores (Saídas)

| Motor | Posição | GPIO | Protocolo | Neutro | Ré máx. | Frente máx. |
|---|---|---|---|---|---|---|
| Bombordo | Casco esquerdo / popa esquerda | GPIO18 | PWM 50 Hz (LEDC) | 1500 µs | 1000 µs | 2000 µs |
| Estibordo | Casco direito / popa direita | GPIO19 | PWM 50 Hz (LEDC) | 1500 µs | 1000 µs | 2000 µs |

> Os ESCs são bidirecionais 50A e têm UBEC interno de 5V/5A, que pode alimentar o ESP32 diretamente, sem regulador externo. A corrente combinada máxima dos dois motores deve ficar abaixo de 28 A (margem de segurança de 2 A abaixo do corte do BMS de 30 A).

**Manobras possíveis com ESC bidirecional:**

| Manobra | Bombordo | Estibordo |
|---|---|---|
| Curva suave à direita | 1600 µs (mais força) | 1400 µs (menos força) |
| Pivô no lugar à direita | 1800 µs (frente) | 1200 µs (ré) |
| Frenagem simétrica | < 1500 µs | < 1500 µs |
| Desvio urgente de obstáculo | ré (< 1500 µs) | frente (> 1500 µs) |

### Sensores (Entradas)

| Sensor | Função | Interface | Endereço / Pinos | Status no firmware |
|---|---|---|---|---|
| A7670SA (GNSS) | Posição, velocidade, curso | UART2 | GPIO16 (RX), GPIO17 (TX) | Implementado (string bruta) |
| A7670SA (4G LTE) | Comunicação MQTT | UART2 | GPIO16 / GPIO17 | Implementado |
| QMC5883L | Magnetômetro / bússola | I2C0 | 0x0D, GPIO21/22 | Implementado (valores brutos) |
| MPU-6050 | Acelerômetro + Giroscópio | I2C0 | 0x68, GPIO21/22 | **Não implementado** |
| AJ-SR04M | Ultrassônico / obstáculos | GPIO | TRIG=GPIO4, ECHO=GPIO5 | Driver pronto, **sensor ausente** (simulado) |
| INA226 | Corrente e tensão da bateria | I2C0 | 0x40, GPIO21/22 | **Não implementado** |

> **Nota:** GPIO18 e GPIO19 são exclusivos dos ESCs (LEDC PWM). O ultrassônico foi remapeado para GPIO4 (TRIG) e GPIO5 (ECHO).

**Bateria:** LiFePO4 4S (4 × 3,2 V = 12,8 V nominal) protegida por BMS 30A.

---

## 3. Boot e Inicialização

*O que acontece nos primeiros instantes após ligar o barco:*

Quando a ESP32 é energizada, ela executa uma sequência de configurações de hardware antes de começar qualquer operação. É como a ESP32 "apresentando-se" a cada componente e combinando como vão conversar.

```
1. init_uart_modem()
   └─ Abre a linha de comunicação serial com o modem A7670SA
      Velocidade: 115200 bps | Buffer de recepção: 2048 bytes
      Pinos: GPIO16 (RX) e GPIO17 (TX)

2. init_i2c()
   └─ Inicializa o barramento I2C que conecta o magnetômetro (e futuramente
      o IMU e o INA226) ao ESP32
      Velocidade: 400 kHz | Pull-ups internos habilitados

3. qmc_init()
   └─ Envia comandos de configuração para o magnetômetro QMC5883L:
      - Reset suave para limpar qualquer estado anterior
      - Modo contínuo a 200 Hz, faixa ±8 Gauss, filtro máximo (OSR=512)

4. us_init_gpio()
   └─ Reserva os pinos GPIO18 e GPIO19 para o sensor ultrassônico
      (TRIG como saída, ECHO como entrada)

5. Pré-carrega janela de média móvel
   └─ Inicializa o buffer de filtragem do ultrassônico com 150 cm
      para evitar leituras falsas no primeiro ciclo

6. delay 2000 ms
   └─ Aguarda o modem terminar a inicialização interna após ligar
```

---

## 4. Máquina de Estados — A "Espinha Dorsal" do Sistema

*Como o barco decide o que fazer em cada momento:*

O firmware é organizado como uma máquina de estados com três fases. O barco nunca pula etapas — ele precisa completar cada fase antes de avançar para a próxima. Se algo falha, ele tenta novamente de forma controlada.

```
CONECTANDO_REDE ──► CONECTANDO_MQTT ──► OPERANDO
      ▲                    │                │
      │◄───────────────────┘ (PDP caiu)     │ (3 erros seguidos de publicação)
      │                    ▲────────────────┘ (MQTT caiu)
      │                    │ (falha MQTT, mas internet ainda ok → tenta de novo)
```

---

### Estado 1 — CONECTANDO_REDE

*O barco acorda e tenta se conectar à internet e ligar o GPS.*

Antes de fazer qualquer coisa útil, o barco precisa ter sinal de celular e GPS funcionando. Este estado garante exatamente isso — e só passa para frente quando ambos estão prontos.

```
Passo 1 — Verificar se o modem está acordado
          Envia "AT" e espera "OK"
          Tenta até 10 vezes com 2 s de intervalo
          → Se o modem não responder após todas as tentativas: falha crítica

Passo 2 — Desativar eco do modem (ATE0)
          Evita que o modem repita os comandos recebidos nas respostas

Passo 3 — Aguardar registro na rede celular
          Consulta AT+CREG? a cada 2 s
          Aguarda resposta ",1" (rede local) ou ",5" (roaming)
          Pode esperar até 60 s (30 tentativas)
          → Sem registro após 60 s: falha

Passo 4 — Configurar o APN da operadora
          AT+CGDCONT=1,"IP","zap.vivo.com.br"
          Define qual porta de saída da operadora usar para internet

Passo 5 — Ativar conexão de dados (contexto PDP)
          AT+CGACT=1,1
          É como "abrir" a conexão de dados do celular

Passo 6 — Ligar o módulo GPS
          AT+CGNSSPWR=0 (reset limpo) → AT+CGNSSPWR=1
          Aguarda "+CGNSSPWR: READY!" como confirmação de prontidão
```

Resultado: rede celular ativa + GPS ligado → avança para Estado 2.
Falha em qualquer passo: aguarda 15 s e tenta o Estado 1 novamente.

---

### Estado 2 — CONECTANDO_MQTT

*O barco estabelece o canal de comunicação com o servidor na nuvem.*

Com internet disponível, o barco agora abre uma conexão MQTT — o protocolo leve de mensagens que permite tanto receber comandos de rota quanto enviar telemetria em tempo real.

```
Passo 0 — Limpar sessão MQTT anterior (se houver)
          Sequência: DISC → REL → STOP
          Garante que não há "sujeira" de uma conexão anterior quebrada

Passo 1 — Iniciar o serviço MQTT no modem
          AT+CMQTTSTART → aguarda "+CMQTTSTART: 0"

Passo 2 — Criar um cliente MQTT com identidade única
          AT+CMQTTACCQ=0,"hdrop_boat_001",0
          O ID "hdrop_boat_001" identifica este barco no broker

Passo 3 — Conectar ao broker HiveMQ
          AT+CMQTTCONNECT=0,"tcp://broker.hivemq.com:1883",60,1
          → aguarda "+CMQTTCONNECT: 0,0" (código 0 = sucesso)
          Keepalive de 60 s: o modem envia ping automático ao broker

Passo 4 — Se inscrever no tópico de comandos de rota
          AT+CMQTTSUBTOPIC=0,13,1 → envia "hdrop/comando"
          A partir daqui, qualquer mensagem publicada neste tópico
          chega ao barco como uma URC (notificação assíncrona)

Passo 5 — Confirmar a inscrição
          AT+CMQTTSUB=0,1 → aguarda "+CMQTTSUB: 0,0"
```

Resultado: canal MQTT aberto, escutando comandos → avança para Estado 3.
Falha + internet ok: aguarda 8 s e tenta novamente.
Falha + internet caiu: volta ao Estado 1.

---

### Estado 3 — OPERANDO (loop a 2 Hz)

*O barco está em plena operação: lendo sensores, publicando dados e escutando comandos.*

Este é o coração do sistema. A cada 500 ms, o barco coleta dados de todos os sensores, monta um pacote JSON e envia para a nuvem. Ao mesmo tempo, fica de olho em mensagens chegando com novos destinos.

#### A. Recepção de comandos (não bloqueante, roda a cada iteração)

*O barco "espia" a UART a todo momento sem travar o resto do sistema.*

```
capturarURCs()
└─ Lê qualquer byte disponível na UART do modem e acumula no buffer
   Não bloqueia: se não há dados, retorna imediatamente

processarURCs()
└─ Analisa o buffer procurando mensagens completas do protocolo MQTT
   Formato esperado de chegada:
     +CMQTTRXSTART → +CMQTTRXTOPIC → +CMQTTRXPAYLOAD → +CMQTTRXEND
   │
   ├─ Achou payload JSON válido (começa com '{')
   │  └─ Imprime "ROTA:{json}" na serial (ainda sem atuação nos motores)
   │
   └─ Achou "+CMQTTCONNLOST:"
      └─ Marca contErrosMQTT = 3 para forçar reconexão
```

#### B. Watchdog de conexão MQTT

*O barco monitora a saúde da conexão e reconecta sozinho se necessário.*

```
Se contErrosMQTT >= 3:
  └─ Consulta AT+CMQTTCONNECT? para verificar status real
     ├─ MQTT ainda conectado → falso alarme, zera contador
     └─ MQTT caiu           → volta para Estado 2 (sem desligar o GPS)
```

#### C. Pipeline de telemetria (a cada 500 ms)

*A cada meio segundo, o barco coleta todos os dados e os envia para a nuvem.*

**C.1 — Leitura de posição (GNSS)**

*O barco pergunta ao modem: "onde estou agora?"*

```
AT+CGNSSINFO → resposta bruta do módulo GPS
Exemplo com fix ativo:
  "2,09,05,00,3113.330650,N,12121.262554,E,131117,091918.0,..."
   │  │  │  │  └─ latitude    └─ longitude      └─ hora UTC
   │  │  │  └─ satélites usados no fix
   │  └─ satélites visíveis
   └─ modo do fix (2 = 2D, 3 = 3D)

Sem fix: ",,,,,,,,,,,,,,,,"

Resultado: string bruta armazenada em bufGnss
           temFix = true se o primeiro campo for numérico
```

**C.2 — Leitura da bússola (Magnetômetro)**

*O barco lê o campo magnético para saber para onde sua proa está apontando.*

```
Leitura I2C do QMC5883L (registrador 0x00, 6 bytes):
  Byte 0+1 → X_LSB + X_MSB → mx (int16_t)
  Byte 2+3 → Y_LSB + Y_MSB → my (int16_t)
  Byte 4+5 → Z_LSB + Z_MSB → mz (int16_t)

Resultado: mx, my, mz em unidades brutas (LSB)
           Heading ainda não calculado — falta conversão atan2(my, mx)
```

**C.3 — Processamento do ultrassônico** *(atualmente simulado)*

*O barco mede a distância de obstáculos à frente e decide se precisa desviar.*

```
Entrada: leitura bruta em cm (atualmente simulada por onda senoidal 50–250 cm)

Pipeline de filtragem:
  1. Rejeição de saltos abruptos
     └─ Se a nova leitura difere > 50 cm da última válida:
        mistura suavizada 70% anterior + 30% nova (em vez de rejeitar)

  2. Média móvel circular
     └─ Janela de 6 amostras para suavizar oscilações rápidas

  3. Filtro exponencial (IIR)
     └─ α = 0,75: pesa 75% no valor anterior, 25% na nova média
        Resultado: curva suave, sem picos, resposta mais lenta

  4. Normalização
     └─ Converte cm para escala [0..1]
        0 = 20 cm (objeto muito próximo) | 1 = 300 cm (caminho livre)

  5. Alarme com histerese
     └─ Ativa alarme: 2 leituras consecutivas abaixo do threshold (0,5 ≈ 170 cm)
        Desativa alarme: 3 leituras consecutivas acima do threshold
        (histerese evita que o alarme fique "piscando" no limiar)

Saídas: dist_cm (float), y_norm [0..1], alarme_ativo (bool)
```

**C.4 — Publicação de telemetria (MQTT)**

*O barco empacota tudo e envia para a nuvem.*

```
JSON montado:
{
  "g": "<string_bruta_gnss>",     ← posição completa
  "m": [mx, my, mz],              ← campo magnético bruto
  "u": {
    "d": 142.3,                   ← distância ultrassônico (cm)
    "y": 0.44,                    ← distância normalizada [0..1]
    "al": 0                       ← alarme de obstáculo (0 ou 1)
  }
}

Sequência de publicação AT (conforme manual A76XX §18):
  AT+CMQTTTOPIC=0,9   → ">" → envia "hdrop/raw"
  AT+CMQTTPAYLOAD=0,N → ">" → envia o JSON
  AT+CMQTTPUB=0,0,60  → "+CMQTTPUB: 0,0"   (QoS 0 — sem confirmação)

Sucesso → contErrosMQTT = 0
Falha   → contErrosMQTT++ (ao atingir 3 → watchdog aciona reconexão)
```

---

## 5. Entradas de Controle — Visão Completa (Planejada)

Esta seção descreve como cada sensor alimentará o sistema de controle quando totalmente implementado.

### [1] Destino / Missão — via MQTT

*A "ordem de missão" que diz ao barco para onde ir.*

O operador (ou sistema backend) publica no tópico `hdrop/comando` um JSON com as coordenadas do destino. O barco recebe isso pelo modem 4G e calcula tudo que precisa para navegar até lá.

```
Formato recebido: {"lat": -15.8140, "lon": -47.8301}
Caminho: Broker HiveMQ → A7670SA (4G) → UART2 (GPIO16/17) → ESP32

O que o firmware faz com isso:
├── Calcula o bearing (ângulo 0–360°) da posição atual até o waypoint
├── Calcula a distância restante ao destino (em metros)
├── Define a velocidade de cruzeiro proporcional à bateria disponível
│       v_cruzeiro = f(Wh_restantes, distância, margem_segurança %)
└── Encerra a missão quando distância < 5 m → neutro nos dois ESCs
```

### [2] Posição e Velocidade Reais — GNSS

*O GPS é a "verdade absoluta" sobre onde o barco está e quão rápido vai.*

```
Fonte: A7670SA (AT+CGNSSINFO), ~1 Hz

Campos extraídos:
  lat / lon  → posição atual (coordenadas decimais)
  vel (km/h) → velocidade sobre o fundo (SOG — Speed Over Ground)
  curso (°)  → curso sobre o fundo (COG — Course Over Ground)
  HDOP       → qualidade do fix (< 2.5 = aceitável; > 2.5 = degradado)

Uso no controle:
├── Posição + waypoint → recalcular bearing a cada ciclo
├── Cross-track error  → desvio lateral da linha reta A→B
│       Se o barco derivou para o lado → corrigir heading para voltar à linha
├── SOG → feedback primário de velocidade no PID
├── COG → comparar com heading magnético para detectar deriva por corrente
│       heading_efetivo = QMC5883L + correção (COG − heading_mag)
├── HDOP > 2.5 → fix degradado → congelar controle de posição
└── Distância acumulada → estimar consumo em Wh/km
```

### [3] Orientação — Magnetômetro QMC5883L

*A bússola eletrônica que diz para onde a proa do barco está apontando.*

```
Fonte: QMC5883L via I2C (0x0D), ~2 Hz
Valores: mx, my, mz (int16_t, unidades brutas)

Derivação do heading:
  heading_mag = atan2(my, mx) × (180 / π)   → resultado em 0–360°
  (requer calibração de hard-iron e soft-iron para precisão)

Uso no controle:
├── Erro de heading = heading_mag − bearing_para_waypoint
│       → sinal e magnitude do diferencial de PWM entre motores
│       → positivo = virar à direita (bombordo sobe, estibordo desce)
│       → negativo = virar à esquerda (inverso)
├── Histerese de ±5° → evitar oscilações quando o barco está em linha reta
└── Variação de heading sem comando → deriva por corrente de água
        → adicionar viés permanente no diferencial para compensar

Atenção: ESCs e cabos de potência interferem no magnetômetro.
         Montar o QMC5883L o mais distante possível dos ESCs.
```

### [4] Obstáculos — Ultrassônico AJ-SR04M

*O "radar de proximidade" que evita colisões.*

```
Fonte: AJ-SR04M via GPIO (TRIG + ECHO), incluído no ciclo de telemetria
Saídas processadas: dist_cm, y_norm [0..1], alarme (bool)

Lógica de desvio por zona:
├── dist_cm < 30 cm → ZONA CRÍTICA: parada de emergência imediata
│       PWM_bombordo = PWM_estibordo = 1500 µs (neutro)
├── dist_cm < 80 cm → ZONA DE PRECAUÇÃO: reduzir para 50% da velocidade
│       PWM_base escalonado para metade da velocidade de cruzeiro
├── alarme == true → MANOBRA DE DESVIO (3 intensidades):
│   ├── Suave    : motor do lado do obstáculo reduz (ainda em frente)
│   ├── Moderada : motor do lado do obstáculo vai a neutro (1500 µs)
│   └── Agressiva: motor do lado do obstáculo vai a ré (< 1500 µs)
└── y_norm > threshold e alarme == false → CAMINHO LIVRE
        → retomar heading e velocidade de cruzeiro
```

### [5] Dinâmica do Barco — MPU-6050 (IMU)

*O giroscópio e acelerômetro que "sentem" o movimento do barco quadro a quadro.*

O GNSS atualiza a ~1 Hz — lento demais para controle fino. O IMU atualiza a ~20 Hz e preenche essa lacuna, permitindo controle suave e sem oscilações.

```
Fonte: MPU-6050 via I2C (0x68), ~20 Hz
Saídas: ax, ay, az (aceleração em g) | gx, gy, gz (rotação em °/s)

Uso no controle:
├── gz (yaw rate — velocidade de rotação vertical)
│   └─ TERMO DERIVATIVO do PD de heading
│      Se gz está alto → o barco já está girando → reduzir correção
│      Evita sobrecorreção ("servo hunting" nas viradas)
├── ax (aceleração longitudinal)
│   └─ Estima variação de velocidade entre updates do GNSS
│      Preenche 20 ciclos de controle por cada fix de 1 Hz do GPS
├── ay (aceleração lateral)
│   └─ Detecta desvio lateral antes que o GNSS perceba → antecipar correção
└── az (aceleração vertical — arfagem)
    └─ |az − 1g| > 0.4g → ondas fortes → reduzir velocidade máxima
```

### [6] Energia da Bateria — INA226

*O "combustível" do barco: saber quanto resta para decidir com segurança.*

```
Fonte: INA226 via I2C (0x40)
Shunt: 100 mΩ | CAL = 51 | Current_LSB = 1 mA
Mede corrente total: motores + ESP32 + modem (~0,5 A fixo)
  corrente_motores ≈ current_a − 0,5 A

Saídas: voltage_v, current_a, power_w, soc_pct, energy_wh

Faixas de operação por SOC:
├── SOC > 50%    → velocidade de cruzeiro calculada normalmente
├── SOC 20–50%   → redução linear da velocidade máxima permitida
├── SOC < 20%    → modo retorno / velocidade mínima de navegação
└── SOC < 10%    → parada + publicar alarme de emergência via MQTT

Proteções por tensão e corrente:
├── voltage_v < 11.4 V → BMS próximo do corte → neutro imediato nos ESCs
├── Queda ΔV > 0.5 V em 1 ciclo → pico de corrente → reduzir PWM
└── current_a > 28 A → escalonar PWM_base para baixo (proteção BMS 30A)

Planejamento de rota:
  autonomia_estimada = energy_wh / consumo_médio_Wh_por_km
  v_cruzeiro = min(v_desejada, f(autonomia_estimada, distância_B, margem %))
```

---

## 6. Malha Fechada — Os Dois Laços de Controle

*Como o barco decide exatamente o quanto cada motor deve girar.*

O sistema de controle é organizado em dois laços interdependentes que rodam simultaneamente. O Laço 1 controla a velocidade geral; o Laço 2 controla a direção.

### Laço 1 — Controle de Velocidade (PWM_base)

*"Rápido o suficiente para chegar, mas sem gastar bateria desnecessariamente."*

```
Setpoint: v_ref = velocidade calculada para cumprir a missão com bateria disponível
                  limitada pelas faixas de SOC e proteção de corrente

Feedback (fusão de 3 fontes):
  F1 [~1 Hz,  primária]  SOG via GNSS
     └─ Velocidade absoluta sobre o fundo — a "verdade" do sistema
        Alta latência → usada para resetar a integral do PID e corrigir deriva

  F2 [~20 Hz, secundária] Integração de ax via MPU-6050
     └─ Estima variação de velocidade entre fixes do GNSS
        Deriva com o tempo → resetada a cada fix pelo SOG

  F3 [~2 Hz,  auxiliar]  current_a via INA226
     └─ Mais corrente ≈ mais força ≈ mais velocidade (com carga estável)
        Corrente subiu mas SOG não → possível obstáculo ou falha de motor

Controlador: PID
  P → proporcional ao erro instantâneo: v_ref − SOG
  I → acumula erro ao longo do tempo (enquanto current_a < limite)
  D → usa taxa de variação de ax (IMU, 20 Hz) para antecipar mudanças

Saída: PWM_base (igual nos dois ESCs em linha reta)
       Faixa: 1500 µs (parado) → 2000 µs (frente máxima)
```

### Laço 2 — Controle de Heading (ΔPWMdiff)

*"Manter o nariz do barco apontado para o destino."*

```
Setpoint: bearing_ref = ângulo calculado da posição atual até o waypoint
                        recalculado a cada ~1 Hz com novo fix do GNSS

Feedback (fusão de 3 fontes):
  F1 [~2 Hz,  primária]  Heading magnético via QMC5883L
     └─ Orientação absoluta da proa → erro estático de heading
        Atenção: sensível à interferência dos ESCs e cabos de potência

  F2 [~20 Hz, secundária] gz (yaw rate) via MPU-6050
     └─ Velocidade angular atual → TERMO DERIVATIVO
        Se gz está alto e o erro é pequeno → o barco está corrigindo
        → zerar o diferencial para não exagerar na correção

  F3 [~1 Hz,  auxiliar]  COG via GNSS
     └─ Curso real sobre o fundo → detecta deriva não capturada pela bússola
        heading_efetivo = QMC5883L + correção (COG − heading_mag)

Controlador: PD
  P → erro de heading: bearing_ref − heading_mag        → ΔPWMdiff_P
  D → yaw rate negativo (amortecimento): −gz (MPU-6050) → ΔPWMdiff_D

Aplicação nos motores:
  PWM_bombordo  = PWM_base + ΔPWMdiff   [limitado entre 1000–2000 µs]
  PWM_estibordo = PWM_base − ΔPWMdiff   [limitado entre 1000–2000 µs]
```

---

## 7. Arquitetura de Tarefas FreeRTOS ✓ Implementado

*Como o trabalho está dividido entre as partes do firmware.*

```
task_controle    │ 20 Hz │ Prioridade ALTA  │ Core 1
─────────────────┤        │                  │
                 │ ├─ Lê IMU (I2C) — ax, ay, az, gx, gy, gz
                 │ ├─ Lê ultrassônico (GPIO) — dist_cm, alarme
                 │ ├─ Executa PID velocidade + PD heading
                 │ └─ Aplica PWM via LEDC (GPIO18, GPIO19)

task_telemetria  │ 2 Hz  │ Prioridade MÉDIA │ Core 0
─────────────────┤        │                  │
                 │ ├─ Lê GNSS (AT+CGNSSINFO — pode bloquear centenas de ms)
                 │ ├─ Lê magnetômetro QMC5883L (I2C)
                 │ ├─ Lê bateria INA226 (I2C)
                 │ └─ Publica MQTT (sequência AT — pode bloquear centenas de ms)

EstadoSistema_t  │ Struct compartilhada protegida por mutex
─────────────────┤
                 │ Do GNSS:        lat, lon, sog_kmh, cog_deg, hdop
                 │ Do magnetômetro: heading_deg
                 │ Do IMU:         ax, ay, az, gx, gy, gz
                 │ Da bateria:     voltage_v, current_a, soc_pct, energy_wh
                 │ Da missão:      waypoint_lat, waypoint_lon, missao_ativa
                 │ Do controle:    pwm_bombordo_us, pwm_estibordo_us,
                 │                 bearing_ref, v_ref
```

---

## 8. Pontos de Melhoria no Código Atual

### 8.1 GNSS — string bruta sem parsing numérico
A resposta de `AT+CGNSSINFO` é publicada inteira como texto. O backend precisa decodificá-la, o que aumenta o acoplamento e impede que qualquer lógica de navegação rode localmente na ESP32.

**Melhoria:** implementar `parse_gnss()` que extrai `lat_deg`, `lon_deg`, `sog_kmh`, `cog_deg` e `hdop` como campos numéricos, publicando JSON estruturado e alimentando diretamente os cálculos de bearing.

### 8.2 Magnetômetro — heading não calculado
Os valores mx/my/mz são publicados brutos. Não há cálculo de heading, sem calibração de hard-iron/soft-iron e sem fusão com COG do GNSS.

**Melhoria:** calcular `heading_deg = atan2(my, mx) × 180/π` com correção de declinação magnética e calibração de offset, publicando o campo `"h"` no JSON.

### 8.3 Ultrassônico — operando apenas em simulação
O driver real `us_ler_real()` está implementado mas comentado. O threshold de alarme está fixo em 0,5 (≈ 170 cm), sem ajuste dinâmico baseado na velocidade do barco.

**Melhoria:** conectar o sensor físico, ativar `us_ler_real()` e implementar threshold adaptativo proporcional à velocidade de cruzeiro atual.

### 8.4 Comandos de rota — sem atuação sobre motores
O payload recebido em `hdrop/comando` é apenas impresso na serial (`ROTA:{json}`). Não há extração de coordenadas, cálculo de bearing nem qualquer efeito sobre os ESCs.

**Melhoria:** implementar parser de waypoint que armazene lat/lon de destino na `EstadoSistema_t` e dispare o algoritmo de navegação.

### ~~8.5 Propulsão — completamente ausente no firmware~~ ✓ Resolvido
LEDC configurado a 50 Hz, 16 bits. GPIO18 (bombordo) e GPIO19 (estibordo) mapeados. `esc_init()`, `esc_set_us()` e `esc_neutro()` implementados. ESCs inicializam em 1500 µs (neutro). **Falta:** escrever os valores de PWM calculados pelo controlador.

### 8.6 IMU (MPU-6050) — não implementado
O sensor que fornece o termo derivativo do PD de heading (gz) e preenche a lacuna de 1 Hz do GNSS com estimativa a 20 Hz não tem nenhuma linha de código no firmware atual.

**Melhoria:** implementar driver MPU-6050 via I2C, ler ax, ay, az, gz a 20 Hz na `task_controle`.

### 8.7 INA226 — não implementado
Sem monitoramento de bateria, o barco não tem como saber quando vai descarregar, não pode limitar velocidade por SOC e não tem proteção contra descarga profunda da LiFePO4 4S.

**Melhoria:** implementar driver INA226 com shunt de 100 mΩ, calcular SOC por integração de corrente e publicar `"b":{"v":...,"i":...,"soc":...}` no JSON de telemetria.

### ~~8.8 Firmware de tarefa única — bloqueios frequentes~~ ✓ Resolvido
`task_controle` (20 Hz, Core 1) e `task_telemetria` (2 Hz, Core 0) implementadas. Comunicação via `EstadoSistema_t` protegida por `g_mutex`. `app_main` inicializa o hardware e se encerra com `vTaskDelete(NULL)`.

### 8.9 QoS 0 sem estratégia de fallback
A telemetria é publicada sem confirmação de entrega (QoS 0). Em ambiente com sinal instável (em movimento, sobre água), pacotes são perdidos silenciosamente sem qualquer retry.

**Melhoria:** usar QoS 1 para comandos críticos de rota (waypoints), manter QoS 0 apenas para telemetria de alta frequência onde perda pontual é aceitável.

---

## 9. Próximas Fases de Desenvolvimento

As fases abaixo seguem a sequência definida no `MAPEAMENTO.txt`, priorizando o que desbloqueia o próximo passo.

---

### ~~Passo 1 — EstadoSistema_t + Mutex~~ ✓ Concluído

`EstadoSistema_t` declarada com todos os campos de GNSS, magnetômetro, IMU, bateria, ultrassônico, missão e ESCs. `SemaphoreHandle_t g_mutex` protege todos os acessos concorrentes.

---

### ~~Passo 2 — LEDC PWM (Saída para os ESCs)~~ ✓ Concluído

LEDC configurado: 50 Hz, 16 bits. GPIO18 → bombordo, GPIO19 → estibordo. Funções `esc_init()`, `esc_set_us()`, `esc_neutro()` implementadas. Ultrassônico remapeado para GPIO4/GPIO5.

---

### ~~Passo 3 — Separar em Duas Tarefas FreeRTOS~~ ✓ Concluído

`task_telemetria` (2 Hz, Core 0): FSM + GNSS + magnetômetro + MQTT.
`task_controle` (20 Hz, Core 1): ultrassônico + PWM nos ESCs.
`app_main` inicializa hardware e se encerra.

---

### Passo 4 — Controle de Heading (PD)

*Fazer o barco apontar para o destino e se manter nessa direção.*

Este é o primeiro controle real de direção. A partir daqui, o barco consegue virar para um ângulo alvo e se estabilizar — sem o GPS, apenas com a bússola e o giroscópio.

```
O que fazer:
  - Calcular heading_deg = atan2(my, mx) × 180/π a partir do QMC5883L
  - Implementar controlador PD:
      erro       = bearing_ref − heading_deg
      derivativo = −gz (do MPU-6050, para amortecer a rotação)
      ΔPWMdiff   = Kp × erro + Kd × (−gz)
  - Aplicar nos ESCs:
      PWM_bombordo  = 1500 + ΔPWMdiff   [clamp 1000–2000]
      PWM_estibordo = 1500 − ΔPWMdiff   [clamp 1000–2000]
  - Testar: girar o barco à mão e observar os ESCs reagirem para compensar
  - Ajustar Kp e Kd até a resposta ser estável (sem oscilar)
```

---

### Passo 5 — Controle de Velocidade (PID)

*Fazer o barco manter uma velocidade constante independente de vento ou corrente.*

Com a direção controlada, o próximo passo é controlar a velocidade. O PID usa o GPS como feedback primário e o IMU para interpolação entre os fixes lentos do GPS.

```
O que fazer:
  - Implementar PID com SOG (GNSS) como feedback primário
  - Integrar ax (IMU, 20 Hz) entre fixes do GNSS para estimativa contínua
  - Proteção de corrente: se current_a > 28 A → reduzir PWM_base
  - Testar em bancada: simular SOG variável e verificar resposta do PWM_base
  - Ajustar Kp, Ki, Kd do PID de velocidade
```

---

### Passo 6 — Lógica de Missão A→B

*O barco recebe o destino e navega até lá sozinho.*

Com velocidade e heading controlados, este passo conecta tudo: o barco recebe um waypoint, calcula o que precisa fazer e executa autonomamente até chegar.

```
O que fazer:
  - Parser do payload JSON de hdrop/comando → extrair lat/lon do waypoint
  - Calcular bearing_ref = atan2(Δlon, Δlat) corrigido para 0–360°
  - Calcular distância ao destino em metros (fórmula de Haversine)
  - Calcular v_ref em função de energy_wh e distância restante
  - Encerrar missão quando distância < 5 m → neutro nos dois ESCs
  - Publicar status de missão via MQTT (em rota / chegou / retornando)
```

---

### ~~Passo 7 — Desvio de Obstáculos~~ ✓ Concluído

Três zonas de reação implementadas em `task_controle` (passo 3g), após cálculo de PWM da missão:

```
dist < 30 cm  → parada total (ambos ESCs neutro, vel_integral = 0)
dist < 80 cm  → ré bombordo (1380 µs) + estibordo reduzido − 100 µs → gira à direita
alarme && dist < 120 cm → bombordo a neutro → desvio suave à direita
```

Nota: sensor AJ-SR04M ainda simulado. Para ativar o real, descomentar `us_ler_real()` em `us_processar()`.

---

### ~~Passo 8 — Gestão de Bateria~~ ✓ Concluído

Simulação INA226 (`bat_simular`) com coulomb counting a 2 Hz. Implementado em `task_controle` (passo 3f) e `task_telemetria` (passo C.3b):

```
Emergência de tensão (task_controle):
  voltage_v < 11.4 V → ESCs neutros + missao_ativa = false + log

Alarmes MQTT (task_telemetria, tópico hdrop/alarme, QoS 1):
  soc_pct < 10%  → publicarAlarme("soc_critico", soc_pct)
  voltage_v < 11.4 V → publicarAlarme("tensao_corte", voltage_v) + cancela missão

v_ref em função do SOC (task_controle, passo 3a):
  SOC > 50%      → 3.0 km/h
  SOC 20–50%     → interpolação linear 1.5–3.0 km/h
  SOC < 20%      → 1.0 km/h (modo retorno mínimo)

Telemetria enriquecida: campo "b" no JSON com v, i, soc, wh.
```
