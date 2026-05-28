╔══════════════════════════════════════════════════════════════════════════════╗
║              HDrop v2.0 — MAPA DE EXECUÇÃO DO FIRMWARE                       ║
╠══════════════════════════════════════════════════════════════════════════════╣
║                                                                              ║
║  [ BOOT ]                                                                    ║
║     │                                                                        ║
║     ├─ init_uart_modem()   Configura UART2 p/ modem A7670SA (115200 bps)     ║
║     ├─ init_i2c()          Configura I2C para magnetômetro (400 kHz)         ║
║     ├─ qmc_init()          Reset + configura QMC5883L (200 Hz, ±8G, OSR512)  ║
║     ├─ us_init_gpio()      Reserva GPIO18/19 para o ultrassônico             ║
║     ├─ Pré-carrega janela de média móvel (150 cm)                            ║
║     └─ delay 2000 ms → estabilização do modem                                ║
║                                                                              ║
╠══ MÁQUINA DE ESTADOS (loop infinito) ═══════════════════════════════════════╣
║                                                                              ║
║  ┌─────────────────────────────────────────────────────────────────────┐     ║
║  │  ESTADO 1: CONECTANDO_REDE                                          │     ║
║  │                                                                     │     ║
║  │  1. Testa modem → "AT" → aguarda "OK"  (até 10 tentativas, 2s cada) │     ║
║  │  2. Desativa eco → "ATE0"                                           │     ║
║  │  3. Aguarda registro celular → "AT+CREG?" → busca ",1" ou ",5"      │     ║
║  │     (até 30 tentativas, 2s cada = até 60 s)                         │     ║
║  │  4. Configura APN → "AT+CGDCONT=1,"IP","zap.vivo.com.br""           │     ║
║  │  5. Ativa contexto PDP → "AT+CGACT=1,1" → aguarda "OK"             │     ║
║  │  6. Desliga GNSS → "AT+CGNSSPWR=0" (reset limpo)                   │     ║
║  │  7. Liga GNSS → "AT+CGNSSPWR=1" → aguarda "+CGNSSPWR: READY!"      │     ║
║  │                                                                     │     ║
║  │  ✓ Sucesso → vai para ESTADO 2                                      │     ║
║  │  ✗ Falha → aguarda 15 s → tenta ESTADO 1 novamente                 │     ║
║  └─────────────────────────────────────────────────────────────────────┘     ║
║                          │                                                   ║
║                          ▼                                                   ║
║  ┌────────────────────────────────────────────────────────────────────┐     ║
║  │  ESTADO 2: CONECTANDO_MQTT                                         │     ║
║  │                                                                    │     ║
║  │  0. liberarSessaoMQTT() → DISC → REL → STOP (limpeza)              │     ║
║  │  1. Inicia serviço MQTT → "AT+CMQTTSTART" → aguarda "+CMQTTSTART:0"│     ║
║  │  2. Cria cliente → "AT+CMQTTACCQ=0,"hdrop_boat_001",0"             │     ║
║  │  3. Conecta broker → "AT+CMQTTCONNECT=0,"tcp://broker.hivemq..."   │     ║
║  │     → aguarda "+CMQTTCONNECT: 0,0"                                 │     ║
║  │  4. Define tópico de subscribe → "AT+CMQTTSUBTOPIC=0,13,1" → ">"   │     ║
║  │     → envia "hdrop/comando"                                        │     ║
║  │  5. Confirma subscribe → "AT+CMQTTSUB=0,1" → "+CMQTTSUB: 0,0"      │     ║
║  │                                                                    │     ║
║  │  ✓ Sucesso → vai para ESTADO 3                                     │     ║
║  │  ✗ Falha MQTT + PDP OK  → aguarda 8 s → retry ESTADO 2             │     ║
║  │  ✗ Falha MQTT + PDP caiu → vai para ESTADO 1                       │     ║
║  └────────────────────────────────────────────────────────────────────┘     ║
║                          │                                                   ║
║                          ▼                                                   ║
║  ┌─────────────────────────────────────────────────────────────────────┐     ║
║  │  ESTADO 3: OPERANDO  (loop a ~2 Hz)                                 │     ║
║  │                                                                     │     ║
║  │  A cada iteração do loop:                                           │     ║
║  │                                                                     │     ║
║  │  [A] capturarURCs()  ← lê bytes livres da UART (não bloqueia)      │     ║
║  │      processarURCs() ← parseia bufURC procurando CMQTTRXSTART..END │     ║
║  │        │                                                            │     ║
║  │        ├─ Achou "+CMQTTCONNLOST:" → força contErrosMQTT = MAX      │     ║
║  │        └─ Achou payload JSON → imprime "ROTA:{json}"               │     ║
║  │                                                                     │     ║
║  │  [B] contErrosMQTT >= 3?                                            │     ║
║  │        ├─ Não → continua                                            │     ║
║  │        └─ Sim → mqttConectado()?                                    │     ║
║  │              ├─ Não → vai para ESTADO 2 (reconecta MQTT)           │     ║
║  │              └─ Sim → reseta contador, continua                     │     ║
║  │                                                                     │     ║
║  │  [C] Passou 500 ms desde última telemetria?                         │     ║
║  │        ├─ Não → delay 5 ms → volta ao início do loop               │     ║
║  │        └─ Sim → executa pipeline de telemetria:                     │     ║
║  │                                                                     │     ║
║  │     [C.1] lerGNSSInfo()                                             │     ║
║  │           → "AT+CGNSSINFO" → extrai string bruta de posição        │     ║
║  │           → bufGnss = "2,09,05,00,3113.33,N,12121.26,E,..."        │     ║
║  │                                                                     │     ║
║  │     [C.2] qmc_read(&mx, &my, &mz)                                  │     ║
║  │           → I2C lê 6 bytes do QMC5883L (registrador 0x00)          │     ║
║  │           → mx, my, mz = campo magnético bruto em cada eixo        │     ║
║  │                                                                     │     ║
║  │     [C.3] us_processar(...)   [SIMULADO por enquanto]               │     ║
║  │           → us_ler_simulado() → onda senoidal 50–250 cm + ruído    │     ║
║  │           → Rejeição de saltos (>50 cm → mistura 70/30)            │     ║
║  │           → Média móvel (janela de 6 amostras)                     │     ║
║  │           → Filtro exponencial (α=0.75)                            │     ║
║  │           → Normalização [0..1]                                     │     ║
║  │           → Alarme com histerese (ativa em 2, desativa em 3)       │     ║
║  │           → saída: dist_cm, y_norm, alarme_ativo                   │     ║
║  │                                                                     │     ║
║  │     [C.4] publicarTelemetria(...)                                   │     ║
║  │           → Monta JSON:                                             │     ║
║  │             {"g":"<gnss>","m":[x,y,z],"u":{"d":142.3,"y":0.44,"al":0}}│ ║
║  │           → AT+CMQTTTOPIC=0,9   → ">"  → envia "hdrop/raw"         │     ║
║  │           → AT+CMQTTPAYLOAD=0,N → ">"  → envia JSON                │     ║
║  │           → AT+CMQTTPUB=0,0,60  → "+CMQTTPUB: 0,0"                     │     ║
║  │           → Sucesso? contErrosMQTT=0 : contErrosMQTT++                 │     ║
║  │                                                                        │     ║
║  │  volta ao início do ESTADO 3 →→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→         │     ║
║  └────────────────────────────────────────────────────────────────────────┘     ║
║                                                                              ║
╠══ FLUXO DE DADOS ═══════════════════════════════════════════════════════════╣
║                                                                              ║
║   SENSORES              ESP32                    NUVEM (HiveMQ)              ║
║                                                                              ║
║   QMC5883L  ──I2C──►                                                        ║
║   AJ-SR04M  (simul)──►  Monta JSON  ──MQTT──►  tópico: hdrop/raw            ║
║   A7670SA GNSS ──AT──►                                                       ║
║                                                                              ║
║                         ◄──MQTT──  tópico: hdrop/comando  ◄── Backend       ║
║                         Imprime "ROTA:{json}" na serial                      ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝