# HDrop – Firmware do Barco Autônomo v1.0
### ESP32 + A7670SA (LTE 4G) + QMC5883L (Magnetômetro) + GNSS Integrado

---

## O que este código faz

Este firmware transforma o **ESP32** em um gateway de telemetria celular para o catamarã autônomo HDrop. Ele integra três subsistemas em um loop contínuo:

- **Modem LTE A7670SA** — comunicação celular via comandos AT (Serial2)
- **Magnetômetro QMC5883L** — leitura de orientação magnética via I2C
- **GNSS integrado ao A7670SA** — posição GPS/GLONASS/BeiDou

A execução é controlada por uma **máquina de estados de 3 fases**:

```
CONECTANDO_REDE  →  CONECTANDO_MQTT  →  OPERANDO
```

---

## Etapas do Código

### Fase 1 — `CONECTANDO_REDE`
- Verifica comunicação com o modem (`AT`)
- Desabilita eco (`ATE0`)
- Aguarda registro na rede celular (`AT+CREG?`) — aceita rede local (`0,1`) e roaming (`0,5`)
- Configura o APN (`AT+CGDCONT`)
- Ativa o contexto de dados PDP (`AT+CGACT=1,1`)
- Liga o módulo GNSS (`AT+CGNSSPWR=1`) e aguarda o URC `+CGNSSPWR: READY!`

### Fase 2 — `CONECTANDO_MQTT`
- Limpa sessão MQTT anterior com a sequência: `DISC → REL → STOP`
- Inicia o serviço MQTT (`AT+CMQTTSTART`)
- Adquire um cliente MQTT (`AT+CMQTTACCQ`)
- Conecta ao broker HiveMQ (`AT+CMQTTCONNECT`)
- Inscreve-se no tópico `hdrop/comando` para receber rotas (`AT+CMQTTSUBTOPIC` + `AT+CMQTTSUB`)

### Fase 3 — `OPERANDO` (loop a cada 500 ms / 2 Hz)
A cada ciclo, quatro etapas são executadas em sequência:

| Etapa | O que faz |
|---|---|
| **1. URCs** | Captura mensagens assíncronas do modem (comandos recebidos, perdas de conexão) |
| **2. GNSS** | Executa `AT+CGNSSINFO` e extrai a string bruta de posição |
| **3. Magnetômetro** | Lê 6 bytes via I2C do registrador `0x00` do QMC5883L (eixos X, Y, Z como `int16_t`) |
| **4. Publicação** | Monta o JSON e publica no tópico `hdrop/raw` via MQTT |

### Protocolo de Publicação MQTT (A7670SA)
O modem exige uma sequência específica de comandos AT para publicar:
```
AT+CMQTTTOPIC=0,<len>   →  modem retorna ">"
hdrop/raw               →  modem retorna "OK"
AT+CMQTTPAYLOAD=0,<len> →  modem retorna ">"
{json}                  →  modem retorna "OK"
AT+CMQTTPUB=0,0,60      →  modem retorna "+CMQTTPUB: 0,0"
```

### Recuperação de Erros
- Após **3 falhas consecutivas** de publicação, verifica a conexão com `AT+CMQTTCONNECT?`
- Se desconectado do broker → volta para `CONECTANDO_MQTT` (GNSS permanece ativo)
- Se o contexto PDP cair → volta para `CONECTANDO_REDE`

---

## Formato da Saída (Tópico `hdrop/raw`)

### JSON publicado via MQTT
```json
{"g":"<string_bruta_gnss>","m":[x,y,z]}
```

| Campo | Tipo | Descrição |
|---|---|---|
| `"g"` | string | Saída bruta do `+CGNSSINFO:` — posição, satélites, data, hora, altitude, velocidade, curso, PDOP, HDOP, VDOP |
| `"m"` | array de 3 inteiros | Leituras brutas do magnetômetro: `[X, Y, Z]` em unidades do sensor (não convertidas para Gauss) |

### Exemplos do campo `"g"`

**Com fix GPS:**
```
2,09,05,00,3113.330650,N,12121.262554,E,131117,091918.0,32.9,0.0,255.0,1.1,0.8,0.7
```
Campos em ordem: `modo, sat-GPS, sat-GLONASS, sat-BeiDou, lat, N/S, lon, E/W, data, hora-UTC, alt(m), vel(km/h), curso(°), PDOP, HDOP, VDOP`

**Sem fix:**
```
,,,,,,,,,,,,,,,
```

### Tópico de entrada `hdrop/comando`
O firmware recebe comandos neste formato:
```json
{"lat": -15.8140, "lon": -47.8301}
```
Estes são repassados pela Serial USB com o prefixo `ROTA:` para o backend processar.

### Log da Serial USB (115200 baud)

| Prefixo | Significado |
|---|---|
| `[AT>]` | Comando enviado ao modem |
| `[AT<]` | Resposta recebida do modem |
| `[GNSS]` | Leitura de posição |
| `[MAG]` | Leitura do magnetômetro |
| `[PUB]` | Telemetria publicada com sucesso |
| `[CMD]` | Comando de rota recebido |
| `ROTA:` | JSON de rota para o backend |
| `[FSM]` | Mudança de estado da máquina |

---

## Hardware Necessário

| Componente | Conexão | Pino ESP32 |
|---|---|---|
| A7670SA — RX do modem | Serial2 TX | GPIO 17 |
| A7670SA — TX do modem | Serial2 RX | GPIO 16 |
| QMC5883L — SDA | I2C SDA | GPIO 21 |
| QMC5883L — SCL | I2C SCL | GPIO 22 |

---

## Configuração Antes de Usar

Abra o arquivo `.ino` e edite as seguintes constantes no topo:

```cpp
#define APN             "tim.br"              // APN da sua operadora
#define MQTT_BROKER     "tcp://broker.hivemq.com:1883"  // Endereço do broker
#define MQTT_CLIENT_ID  "hdrop_boat_001"      // ID único por embarcação
#define MQTT_TOPIC_PUB  "hdrop/raw"           // Tópico de publicação
#define MQTT_TOPIC_SUB  "hdrop/comando"       // Tópico de comandos
```

> **Atenção:** Se mudar os nomes dos tópicos, atualize também `MQTT_TOPIC_PUB_LEN` e `MQTT_TOPIC_SUB_LEN` com o `strlen()` correto dos novos nomes.

---

## Passo a Passo: Como Usar

### 1. Instalar o ambiente
- Instale o [Arduino IDE](https://www.arduino.cc/en/software) ou [PlatformIO](https://platformio.org/)
- Adicione o suporte ao **ESP32** no gerenciador de placas
- Instale a biblioteca **Wire** (já inclusa no core do ESP32)

### 2. Conectar o hardware
- Ligue o A7670SA nos GPIOs 16 (RX) e 17 (TX) conforme a tabela acima
- Ligue o QMC5883L nos GPIOs 21 (SDA) e 22 (SCL)
- Insira o chip SIM com plano de dados ativo e APN correto

### 3. Configurar o código
- Altere o `APN` para o da sua operadora (ex: `"zap.vivo.com.br"`, `"tim.br"`, `"claro.com.br"`)
- Altere `MQTT_CLIENT_ID` para um ID único se usar mais de uma embarcação

### 4. Compilar e gravar
- Selecione a placa **ESP32 Dev Module** no IDE
- Selecione a porta COM correta
- Clique em **Upload**

### 5. Monitorar via Serial
- Abra o **Serial Monitor** a **115200 baud**
- Acompanhe a sequência de conexão pelos prefixos `[FSM]`, `[REDE]`, `[MQTT]`
- Quando aparecer `[FSM] ✓ MQTT pronto → OPERANDO`, o sistema está funcionando

### 6. Verificar telemetria
- Use um cliente MQTT (ex: [MQTT Explorer](https://mqtt-explorer.com/)) conectado a `broker.hivemq.com:1883`
- Inscreva-se no tópico `hdrop/raw`
- Você verá os JSONs chegando a cada 500 ms

### 7. Enviar uma rota
- Publique no tópico `hdrop/comando`:
```json
{"lat": -15.8140, "lon": -47.8301}
```
- O firmware recebe, imprime na Serial com prefixo `ROTA:` e aguarda o backend processar

---

## Dicas de Diagnóstico

| Sintoma | Possível Causa | O que verificar |
|---|---|---|
| Modem não responde | Cabeamento, baud rate | Verificar GPIOs 16/17 e `MODEM_BAUD` |
| Sem registro de rede | SIM sem sinal ou APN errado | Verifique o SIM e o `#define APN` |
| MQTT não conecta | Firewall, broker fora do ar | Tente outro broker ou verifique PDP com `AT+CGACT?` |
| GNSS sem fix | Antena interna, local fechado | Mova para área aberta; o fix pode demorar até 2 min na primeira vez |
| `[MAG] ERRO: Falha I2C` | QMC5883L desconectado | Verificar fiação nos GPIOs 21/22 |
