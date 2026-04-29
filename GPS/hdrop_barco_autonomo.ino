/*
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║           HDrop – Firmware de Barco Autônomo  v1.0                      ║
 * ║──────────────────────────────────────────────────────────────────────────║
 * ║  Hardware:                                                               ║
 * ║    ESP32                                                                 ║
 * ║    A7670SA (LTE 4G)  ← Serial2  RX=GPIO16  TX=GPIO17                   ║
 * ║    QMC5883L (Magneto) ← I2C     SDA=GPIO21  SCL=GPIO22                 ║
 * ║                                                                          ║
 * ║  Referência AT: A76XX Series AT Command Manual V1.06 (SIMCom)           ║
 * ║    Seção 18 – MQTT(S) | Seção 24 – GNSS                                ║
 * ║                                                                          ║
 * ║  Máquina de estados:                                                     ║
 * ║    CONECTANDO_REDE → CONECTANDO_MQTT → OPERANDO                         ║
 * ║                                                                          ║
 * ║  Telemetria (2 Hz) → tópico  "hdrop/raw"                                ║
 * ║  Comandos de rota  ← tópico  "hdrop/comando"                            ║
 * ╚══════════════════════════════════════════════════════════════════════════╝
 */

#include <Arduino.h>
#include <Wire.h>

// ═════════════════════════════════════════════════════════════════════════════
//  1. CONFIGURAÇÕES DE HARDWARE
// ═════════════════════════════════════════════════════════════════════════════

#define SERIAL2_RX_PIN   16
#define SERIAL2_TX_PIN   17
#define MODEM_BAUD       115200

#define I2C_SDA_PIN      21
#define I2C_SCL_PIN      22

// ═════════════════════════════════════════════════════════════════════════════
//  2. CONFIGURAÇÕES DE REDE E MQTT
// ═════════════════════════════════════════════════════════════════════════════

#define APN              "tim.br"
#define MQTT_BROKER      "tcp://broker.hivemq.com:1883"
#define MQTT_CLIENT_ID   "hdrop_boat_001"   // ← Altere para ID único por embarcação
#define MQTT_KEEPALIVE   60                 // segundos
#define MQTT_TOPIC_PUB   "hdrop/raw"
#define MQTT_TOPIC_SUB   "hdrop/comando"

// Comprimentos dos tópicos – necessários pelos comandos AT+CMQTTTOPIC e AT+CMQTTSUBTOPIC
// Ajuste manualmente se mudar os tópicos acima.
#define MQTT_TOPIC_PUB_LEN  9    // strlen("hdrop/raw")
#define MQTT_TOPIC_SUB_LEN  13   // strlen("hdrop/comando")

// ═════════════════════════════════════════════════════════════════════════════
//  3. QMC5883L – REGISTRADORES
// ═════════════════════════════════════════════════════════════════════════════

#define QMC5883L_ADDR    0x0D
#define QMC5883L_REG_DATA   0x00  // X_L, X_H, Y_L, Y_H, Z_L, Z_H (6 bytes)
#define QMC5883L_REG_CTRL1  0x09  // Modo operação
#define QMC5883L_REG_SET    0x0B  // Set/Reset period

// ═════════════════════════════════════════════════════════════════════════════
//  4. TEMPORIZAÇÃO
// ═════════════════════════════════════════════════════════════════════════════

#define INTERVALO_TELEMETRIA  500UL    // ms → 2 Hz
#define AT_TIMEOUT            5000UL   // Padrão para comandos AT
#define AT_TIMEOUT_LONGO      15000UL  // Operações de rede lentas
#define AT_TIMEOUT_GNSS       9000UL   // Aguardar "+CGNSSPWR: READY!" (manual: 9s)
#define AT_TIMEOUT_PUB        10000UL  // Resposta de AT+CMQTTPUB (QoS 0 é rápido)
#define MAX_ERROS_MQTT        3        // Falhas consecutivas antes de reconectar MQTT

// ═════════════════════════════════════════════════════════════════════════════
//  5. MÁQUINA DE ESTADOS
// ═════════════════════════════════════════════════════════════════════════════

enum Estado {
  CONECTANDO_REDE,   // Configura APN, ativa PDP e liga GNSS
  CONECTANDO_MQTT,   // Inicia serviço MQTT e subscreve tópico de comandos
  OPERANDO           // Loop de telemetria 2 Hz + escuta de comandos
};

Estado estadoAtual = CONECTANDO_REDE;

// ═════════════════════════════════════════════════════════════════════════════
//  6. VARIÁVEIS GLOBAIS E BUFFERS
// ═════════════════════════════════════════════════════════════════════════════

unsigned long ultimaTelemetria = 0;
int contErrosMQTT = 0;

// Reutilizável para respostas de comandos AT
char bufAt[512];

// String bruta do GNSS (extrato de +CGNSSINFO:)
char bufGnss[160];

// Payload JSON de telemetria
char bufJson[256];

// Acumulador de URCs (mensagens assíncronas do modem) recebidas entre comandos AT
String bufURC = "";

// ═════════════════════════════════════════════════════════════════════════════
//  7. QMC5883L – IMPLEMENTAÇÃO
// ═════════════════════════════════════════════════════════════════════════════

/*
 * Inicializa o QMC5883L via I2C.
 * CTRL1 = 0b00011101
 *   [7:6] OSR  = 00 → 512 amostras (máxima filtragem de ruído)
 *   [5:4] RNG  = 01 → ±8 Gauss
 *   [3:2] ODR  = 11 → 200 Hz
 *   [1:0] MODE = 01 → Modo Contínuo
 */
void qmc_init() {
  // Soft reset
  Wire.beginTransmission(QMC5883L_ADDR);
  Wire.write(QMC5883L_REG_SET);
  Wire.write(0x01);
  Wire.endTransmission();
  delay(10);

  // Configurar CTRL1
  Wire.beginTransmission(QMC5883L_ADDR);
  Wire.write(QMC5883L_REG_CTRL1);
  Wire.write(0b00011101);
  Wire.endTransmission();

  Serial.println(F("[MAG] QMC5883L OK (200 Hz, ±8G, OSR=512, Contínuo)"));
}

/*
 * Lê eixos X, Y, Z do QMC5883L.
 * Registro 0x00: X_LSB, X_MSB, Y_LSB, Y_MSB, Z_LSB, Z_MSB
 * Retorna false em falha de comunicação I2C.
 */
bool qmc_read(int16_t &x, int16_t &y, int16_t &z) {
  Wire.beginTransmission(QMC5883L_ADDR);
  Wire.write(QMC5883L_REG_DATA);
  if (Wire.endTransmission(false) != 0) return false;  // false = repeated start
  if (Wire.requestFrom((uint8_t)QMC5883L_ADDR, (uint8_t)6) != 6) return false;

  x = (int16_t)(Wire.read() | (Wire.read() << 8));
  y = (int16_t)(Wire.read() | (Wire.read() << 8));
  z = (int16_t)(Wire.read() | (Wire.read() << 8));
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  8. AT COMMANDS – FUNÇÕES DE COMUNICAÇÃO
// ═════════════════════════════════════════════════════════════════════════════

/*
 * drenarSerial2()
 * ────────────────
 * Requisito: "Antes de cada leitura de comando AT, limpe o buffer da Serial2
 * para evitar que 'lixo' ou respostas de comandos anteriores sujem o JSON."
 *
 * Lê tudo disponível na Serial2 por ~30 ms. URCs importantes (+CMQTTRXSTART,
 * +CMQTTCONNLOST) são preservadas em bufURC para processamento posterior;
 * o restante (lixo, OKs residuais) é descartado.
 */
void drenarSerial2() {
  String lixo = "";
  unsigned long t = millis();

  while (millis() - t < 30) {
    while (Serial2.available()) {
      lixo += (char)Serial2.read();
      t = millis();  // Reinicia janela se ainda chegar dado
    }
    delay(2);
  }

  if (lixo.length() == 0) return;

  // Preservar URCs críticas em vez de descartar
  if (lixo.indexOf("+CMQTTRXSTART:") >= 0 ||
      lixo.indexOf("+CMQTTCONNLOST:") >= 0) {
    bufURC += lixo;
    Serial.print(F("[DRAIN] URC salva: "));
    Serial.println(lixo);
  } else {
    Serial.print(F("[DRAIN] Descartado: "));
    Serial.println(lixo);
  }
}

/*
 * enviarAT()
 * ───────────
 * Envia um comando AT e aguarda a "resposta esperada" dentro do timeout.
 *
 * @param cmd       Comando sem \r\n  (ex: "AT+CGNSSINFO")
 * @param esperado  Substring de sucesso (ex: "+CGNSSINFO:")
 * @param timeout   Tempo máximo em ms
 * @param out       Buffer opcional para capturar resposta completa
 * @param outLen    Tamanho do buffer out
 *
 * Retorna: true  → resposta esperada encontrada
 *          false → timeout ou "ERROR" recebido
 */
bool enviarAT(const char* cmd, const char* esperado, unsigned long timeout,
              char* out = nullptr, size_t outLen = 0) {
  drenarSerial2();

  Serial2.print(cmd);
  Serial2.print(F("\r\n"));

  Serial.print(F("[AT>] "));
  Serial.println(cmd);

  String resp = "";
  unsigned long inicio = millis();

  while (millis() - inicio < timeout) {
    while (Serial2.available()) {
      resp += (char)Serial2.read();
    }

    if (resp.indexOf(esperado) >= 0) {
      Serial.print(F("[AT<] "));
      Serial.println(resp);
      if (out && outLen > 0) {
        strncpy(out, resp.c_str(), outLen - 1);
        out[outLen - 1] = '\0';
      }
      return true;
    }

    // Retorno imediato em caso de ERROR (evita esperar timeout completo)
    if (resp.indexOf(F("ERROR")) >= 0) {
      Serial.print(F("[AT<ERR] "));
      Serial.println(resp);
      if (out && outLen > 0) {
        strncpy(out, resp.c_str(), outLen - 1);
        out[outLen - 1] = '\0';
      }
      return false;
    }

    delay(5);
  }

  Serial.print(F("[AT<TMO] "));
  Serial.println(resp);
  return false;
}

/*
 * enviarDadoPrompt()
 * ───────────────────
 * Envia dados depois que o modem exibe o prompt ">".
 * Usado após AT+CMQTTTOPIC, AT+CMQTTPAYLOAD, AT+CMQTTSUBTOPIC.
 * NÃO adiciona \r\n: o modem aceita o dado puro no modo prompt.
 */
bool enviarDadoPrompt(const char* dado, const char* esperado, unsigned long timeout) {
  Serial2.print(dado);

  Serial.print(F("[AT>>] "));
  Serial.println(dado);

  String resp = "";
  unsigned long inicio = millis();

  while (millis() - inicio < timeout) {
    while (Serial2.available()) {
      resp += (char)Serial2.read();
    }
    if (resp.indexOf(esperado) >= 0) {
      Serial.print(F("[AT<<] "));
      Serial.println(resp);
      return true;
    }
    if (resp.indexOf(F("ERROR")) >= 0) {
      Serial.print(F("[AT<<ERR] "));
      Serial.println(resp);
      return false;
    }
    delay(5);
  }

  Serial.println(F("[AT<<TMO]"));
  return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  9. MQTT – RECUPERAÇÃO DE ERROS
// ═════════════════════════════════════════════════════════════════════════════

/*
 * liberarSessaoMQTT()
 * ────────────────────
 * Executa a sequência de limpeza conforme o manual:
 *   AT+CMQTTDISC → AT+CMQTTREL → AT+CMQTTSTOP
 * Chamada antes de toda tentativa de reconexão para garantir estado limpo.
 * Se o modem retornar ERROR persistente, AT+CMQTTREL=0 e AT+CMQTTSTOP
 * forçam o reset do subsistema MQTT interno (conforme regras de código).
 */
void liberarSessaoMQTT() {
  Serial.println(F("[MQTT] Liberando sessão anterior (DISC → REL → STOP)..."));

  // Desconectar do broker (timeout=120s conforme parâmetros do manual)
  enviarAT("AT+CMQTTDISC=0,120", "+CMQTTDISC: 0,0", AT_TIMEOUT_LONGO);
  delay(300);

  // Liberar cliente (deve ser chamado após DISC e antes de STOP)
  enviarAT("AT+CMQTTREL=0", "OK", AT_TIMEOUT);
  delay(300);

  // Parar serviço MQTT (timeout de resposta: 12s conforme manual)
  enviarAT("AT+CMQTTSTOP", "+CMQTTSTOP: 0", 12000UL);
  delay(500);

  Serial.println(F("[MQTT] Sessão liberada."));
}

// ═════════════════════════════════════════════════════════════════════════════
//  10. ESTADO: CONECTANDO_REDE
// ═════════════════════════════════════════════════════════════════════════════

bool conectarRede() {
  Serial.println(F("\n[REDE] ══════ Conectando à Rede ══════"));

  // ── 10.1 Verificar comunicação básica com o modem ──────────────────────────
  bool modemOk = false;
  for (int i = 0; i < 10; i++) {
    if (enviarAT("AT", "OK", 2000UL)) { modemOk = true; break; }
    Serial.print(F("[REDE] Aguardando modem, tentativa "));
    Serial.print(i + 1);
    Serial.println(F("/10..."));
    delay(1000);
  }
  if (!modemOk) {
    Serial.println(F("[REDE] FALHA CRÍTICA: Modem não responde."));
    return false;
  }

  enviarAT("ATE0", "OK", 2000UL);  // Desabilitar eco para não poluir respostas

  // ── 10.2 Aguardar registro na rede celular (até ~60 s) ─────────────────────
  Serial.println(F("[REDE] Aguardando registro na rede..."));
  bool registrado = false;
  for (int i = 0; i < 30; i++) {
    // +CREG: 0,1 = registrado em rede local | +CREG: 0,5 = itinerância
    enviarAT("AT+CREG?", "+CREG:", AT_TIMEOUT, bufAt, sizeof(bufAt));
    if (strstr(bufAt, "+CREG: 0,1") || strstr(bufAt, "+CREG: 0,5")) {
      registrado = true;
      Serial.println(F("[REDE] Registrado na rede!"));
      break;
    }
    delay(2000);
  }
  if (!registrado) {
    Serial.println(F("[REDE] FALHA: Sem registro após 60 s."));
    return false;
  }

  // ── 10.3 Configurar APN ────────────────────────────────────────────────────
  {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
    if (!enviarAT(cmd, "OK", AT_TIMEOUT)) {
      Serial.println(F("[REDE] AVISO: Falha ao configurar APN (pode já estar OK)."));
    }
  }

  // ── 10.4 Ativar contexto PDP – AT+CGACT=1,1 ───────────────────────────────
  // Manual §5.2.4: AT+CGACT=<state>[,<cid>]  state=1 ativa, cid=1 contexto
  if (!enviarAT("AT+CGACT=1,1", "OK", AT_TIMEOUT_LONGO)) {
    // Pode já estar ativo – verifica antes de falhar
    if (!enviarAT("AT+CGACT?", "+CGACT: 1,1", AT_TIMEOUT)) {
      Serial.println(F("[REDE] FALHA: Contexto PDP não pôde ser ativado."));
      return false;
    }
    Serial.println(F("[REDE] Contexto PDP já estava ativo."));
  } else {
    Serial.println(F("[REDE] Contexto PDP ativado (AT+CGACT=1,1 OK)."));
  }

  // ── 10.5 Ligar módulo GNSS – AT+CGNSSPWR=1 ────────────────────────────────
  // Manual §24.2.1: "In ASR1603, GNSS will take about 9 seconds to update the
  // version. Please see '+CGNSSPWR: READY!' before controlling the GNSS."
  Serial.println(F("[GNSS] Ligando módulo GNSS (AT+CGNSSPWR=1)..."));
  enviarAT("AT+CGNSSPWR=0", "OK", 3000UL);  // Reset limpo
  delay(300);

  if (enviarAT("AT+CGNSSPWR=1", "+CGNSSPWR: READY!", AT_TIMEOUT_GNSS)) {
    Serial.println(F("[GNSS] Módulo GNSS pronto!"));
  } else {
    // Alguns firmwares emitem READY! com delay maior; continua mesmo assim
    Serial.println(F("[GNSS] AVISO: '+CGNSSPWR: READY!' não detectado. Continuando."));
  }

  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  11. ESTADO: CONECTANDO_MQTT
// ═════════════════════════════════════════════════════════════════════════════

bool conectarMQTT() {
  Serial.println(F("\n[MQTT] ══════ Conectando ao Broker MQTT ══════"));

  // ── 11.1 Iniciar serviço MQTT – AT+CMQTTSTART ─────────────────────────────
  // Manual §18.2.1: "You must execute this command before any other MQTT
  // related operations."
  // Resposta esperada: OK\r\n+CMQTTSTART: 0
  if (!enviarAT("AT+CMQTTSTART", "+CMQTTSTART: 0", 12000UL)) {
    Serial.println(F("[MQTT] AVISO: CMQTTSTART falhou (pode já estar ativo)."));
    // Continua — pode estar ativo de sessão anterior mal finalizada
  }
  delay(300);

  // ── 11.2 Adquirir cliente MQTT – AT+CMQTTACCQ=0,"<id>",0 ─────────────────
  // Manual §18.2.3: client_index=0; server_type=0 (TCP); clientID=1-128 chars
  {
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "AT+CMQTTACCQ=0,\"%s\",0", MQTT_CLIENT_ID);
    if (!enviarAT(cmd, "OK", AT_TIMEOUT)) {
      Serial.println(F("[MQTT] FALHA: CMQTTACCQ não retornou OK."));
      return false;
    }
  }
  Serial.println(F("[MQTT] Cliente MQTT adquirido."));

  // ── 11.3 Conectar ao broker – AT+CMQTTCONNECT=0,"tcp://...",60,1 ──────────
  // Manual §18.2.8: server_addr deve começar com "tcp://"; clean_session=1
  {
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "AT+CMQTTCONNECT=0,\"%s\",%d,1",
             MQTT_BROKER, MQTT_KEEPALIVE);
    // Resposta de sucesso: OK\r\n+CMQTTCONNECT: 0,0  (err=0 → sucesso)
    if (!enviarAT(cmd, "+CMQTTCONNECT: 0,0", AT_TIMEOUT_LONGO)) {
      Serial.println(F("[MQTT] FALHA: CMQTTCONNECT."));
      return false;
    }
  }
  Serial.println(F("[MQTT] Conectado ao broker HiveMQ!"));
  delay(500);

  // ── 11.4 Subscribe no tópico de comandos ──────────────────────────────────
  // Manual §18.2.13 + §18.2.14:
  //   AT+CMQTTSUBTOPIC=0,<len>,<qos>  → modem retorna ">"
  //   <topic_string>                   → modem retorna OK
  //   AT+CMQTTSUB=0,1                 → modem retorna +CMQTTSUB: 0,0
  {
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+CMQTTSUBTOPIC=0,%d,1", MQTT_TOPIC_SUB_LEN);
    if (!enviarAT(cmd, ">", AT_TIMEOUT)) {
      Serial.println(F("[MQTT] FALHA: CMQTTSUBTOPIC sem prompt."));
      return false;
    }
    if (!enviarDadoPrompt(MQTT_TOPIC_SUB, "OK", AT_TIMEOUT)) {
      Serial.println(F("[MQTT] FALHA: Erro ao enviar tópico de subscribe."));
      return false;
    }
  }

  // Executar o subscribe propriamente dito
  if (!enviarAT("AT+CMQTTSUB=0,1", "+CMQTTSUB: 0,0", AT_TIMEOUT_LONGO)) {
    Serial.println(F("[MQTT] FALHA: CMQTTSUB rejeitado pelo broker."));
    return false;
  }
  Serial.println(F("[MQTT] Subscribe em 'hdrop/comando' confirmado."));

  contErrosMQTT = 0;
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  12. GNSS – LEITURA DE POSIÇÃO
// ═════════════════════════════════════════════════════════════════════════════

/*
 * lerGNSSInfo()
 * ──────────────
 * Executa AT+CGNSSINFO e extrai a string bruta de posição.
 *
 * Resposta do manual (§24.2.12):
 *   +CGNSSINFO: [mode],[GPS-SVs],[GLONASS-SVs],[BEIDOU-SVs],
 *               [lat],[N/S],[lon],[E/W],[date],[UTC-time],
 *               [alt],[speed],[course],[PDOP],[HDOP],[VDOP]
 *
 * Exemplo com fix (do manual):
 *   +CGNSSINFO: 2,09,05,00,3113.330650,N,12121.262554,E,131117,091918.0,32.9,0.0,255.0,1.1,0.8,0.7
 *
 * Sem fix:
 *   +CGNSSINFO:,,,,,,,,,,,,,,,
 *
 * @param saida   Buffer de destino
 * @param maxLen  Tamanho máximo
 * @return true se há fix (primeiro campo = modo numérico 2 ou 3)
 */
bool lerGNSSInfo(char* saida, size_t maxLen) {
  if (!enviarAT("AT+CGNSSINFO", "+CGNSSINFO:", AT_TIMEOUT,
                bufAt, sizeof(bufAt))) {
    strncpy(saida, "gnss_err", maxLen - 1);
    saida[maxLen - 1] = '\0';
    return false;
  }

  // Localizar a linha "+CGNSSINFO:"
  char* p = strstr(bufAt, "+CGNSSINFO:");
  if (!p) {
    strncpy(saida, "parse_err", maxLen - 1);
    saida[maxLen - 1] = '\0';
    return false;
  }

  // Avançar após o prefixo e espaço opcional
  p += strlen("+CGNSSINFO:");
  while (*p == ' ') p++;

  // Copiar até fim de linha
  char* fim = strpbrk(p, "\r\n");
  size_t len = fim ? (size_t)(fim - p) : strlen(p);
  if (len >= maxLen) len = maxLen - 1;

  strncpy(saida, p, len);
  saida[len] = '\0';

  // Fix presente: primeiro campo não é vírgula nem nulo
  return (saida[0] != '\0' && saida[0] != ',');
}

// ═════════════════════════════════════════════════════════════════════════════
//  13. MQTT – PUBLICAÇÃO DE TELEMETRIA
// ═════════════════════════════════════════════════════════════════════════════

/*
 * publicarTelemetria()
 * ─────────────────────
 * Publica JSON compacto no tópico "hdrop/raw".
 * Formato: {"g":"<string_bruta_gnss>","m":[x,y,z]}
 *
 * Sequência AT (manual §18.2.10, §18.2.11, §18.2.12):
 *   1. AT+CMQTTTOPIC=0,<topic_len>   → ">"
 *   2. hdrop/raw                      → OK
 *   3. AT+CMQTTPAYLOAD=0,<pay_len>   → ">"
 *   4. {json}                         → OK
 *   5. AT+CMQTTPUB=0,0,60            → +CMQTTPUB: 0,0
 *        QoS=0 → sem ACK do servidor → retorno rápido (compatível com 2 Hz)
 *
 * Obs: pub_timeout=60 (mínimo permitido pelo manual: 60-180s)
 */
bool publicarTelemetria(const char* gnssStr, int16_t mx, int16_t my, int16_t mz) {
  // Montar JSON
  int payLen = snprintf(bufJson, sizeof(bufJson),
                        "{\"g\":\"%s\",\"m\":[%d,%d,%d]}",
                        gnssStr, mx, my, mz);
  if (payLen <= 0 || payLen >= (int)sizeof(bufJson)) {
    Serial.println(F("[PUB] ERRO: JSON overflow."));
    return false;
  }

  // ── Passo 1+2: Enviar tópico ──
  {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMQTTTOPIC=0,%d", MQTT_TOPIC_PUB_LEN);
    if (!enviarAT(cmd, ">", AT_TIMEOUT)) {
      Serial.println(F("[PUB] FALHA: CMQTTTOPIC sem prompt."));
      return false;
    }
    if (!enviarDadoPrompt(MQTT_TOPIC_PUB, "OK", AT_TIMEOUT)) {
      Serial.println(F("[PUB] FALHA: Envio do tópico."));
      return false;
    }
  }

  // ── Passo 3+4: Enviar payload ──
  {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMQTTPAYLOAD=0,%d", payLen);
    if (!enviarAT(cmd, ">", AT_TIMEOUT)) {
      Serial.println(F("[PUB] FALHA: CMQTTPAYLOAD sem prompt."));
      return false;
    }
    if (!enviarDadoPrompt(bufJson, "OK", AT_TIMEOUT)) {
      Serial.println(F("[PUB] FALHA: Envio do payload."));
      return false;
    }
  }

  // ── Passo 5: Publicar com QoS=0 ──
  // Resposta esperada: OK\r\n+CMQTTPUB: 0,0
  if (!enviarAT("AT+CMQTTPUB=0,0,60", "+CMQTTPUB: 0,0", AT_TIMEOUT_PUB)) {
    Serial.println(F("[PUB] FALHA: CMQTTPUB."));
    return false;
  }

  Serial.print(F("[PUB] ✓ → "));
  Serial.println(bufJson);
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  14. MQTT – VERIFICAÇÃO DE STATUS DE CONEXÃO
// ═════════════════════════════════════════════════════════════════════════════

/*
 * mqttConectado()
 * ────────────────
 * Consulta AT+CMQTTCONNECT? para verificar status sem tentar publicar.
 *
 * Manual §18.2.8 (Read Command):
 *   Conectado:     +CMQTTCONNECT: 0,"tcp://broker:1883",60,1
 *   Desconectado:  +CMQTTCONNECT: 1
 *
 * Retorna true se a resposta contém o endereço do broker configurado.
 */
bool mqttConectado() {
  if (!enviarAT("AT+CMQTTCONNECT?", "+CMQTTCONNECT:", AT_TIMEOUT,
                bufAt, sizeof(bufAt))) {
    return false;
  }
  // Presença do endereço do broker indica índice 0 com sessão ativa
  return strstr(bufAt, "broker.hivemq.com") != nullptr;
}

// ═════════════════════════════════════════════════════════════════════════════
//  15. MQTT – RECEBIMENTO DE MENSAGENS (URCs Assíncronas)
// ═════════════════════════════════════════════════════════════════════════════

/*
 * capturarURCs()
 * ───────────────
 * Lê tudo disponível na Serial2 e acumula em bufURC.
 * Não bloqueia — usa apenas o que já chegou (sem delay).
 * Chamada no início de cada iteração do estado OPERANDO.
 */
void capturarURCs() {
  while (Serial2.available()) {
    char c = Serial2.read();
    bufURC += c;
    // Proteção contra crescimento ilimitado: mantém os últimos 2 KB
    if (bufURC.length() > 2048) {
      bufURC = bufURC.substring(bufURC.length() - 1024);
    }
  }
}

/*
 * processarURCs()
 * ────────────────
 * Parseia bufURC procurando mensagens publicadas no tópico "hdrop/comando".
 *
 * Formato URC (manual §18.4):
 *   +CMQTTRXSTART: 0,<topic_len>,<payload_len>\r\n
 *   +CMQTTRXTOPIC: 0,<sub_topic_len>\r\n<topic>
 *   +CMQTTRXPAYLOAD: 0,<sub_payload_len>\r\n<payload>
 *   +CMQTTRXEND: 0
 *
 * O payload JSON com novas coordenadas é impresso no Serial com prefixo
 * "ROTA:" para o backend (host) processar via Serial USB.
 */
void processarURCs() {
  if (bufURC.length() == 0) return;

  // ── Verificar perda passiva de conexão ────────────────────────────────────
  if (bufURC.indexOf("+CMQTTCONNLOST:") >= 0) {
    Serial.println(F("[URC] +CMQTTCONNLOST detectado → forçar reconexão MQTT"));
    contErrosMQTT = MAX_ERROS_MQTT;  // Força transição de estado no próximo ciclo
    bufURC = "";
    return;
  }

  // ── Verificar mensagem publicada pelo servidor ────────────────────────────
  int idxRxStart = bufURC.indexOf("+CMQTTRXSTART:");
  int idxRxEnd   = bufURC.indexOf("+CMQTTRXEND:");
  if (idxRxStart < 0 || idxRxEnd < 0) return;

  // Localizar o bloco de payload
  int idxPayHdr = bufURC.indexOf("+CMQTTRXPAYLOAD:", idxRxStart);
  if (idxPayHdr < 0) { bufURC = ""; return; }

  // O payload começa após o \n da linha de header +CMQTTRXPAYLOAD:
  int idxNewline = bufURC.indexOf('\n', idxPayHdr);
  if (idxNewline < 0) return;  // Pacote incompleto — aguardar mais dados

  String payload = bufURC.substring(idxNewline + 1, idxRxEnd);
  payload.trim();

  if (payload.length() > 0) {
    Serial.print(F("[CMD] Recebido em hdrop/comando: "));
    Serial.println(payload);

    // Imprimir prefixado para o backend processar coordenadas de rota
    if (payload.startsWith("{")) {
      Serial.print(F("ROTA:"));
      Serial.println(payload);
    }
  }

  // Consumir URCs já processadas, preservar qualquer dado após +CMQTTRXEND:
  int fimURC = idxRxEnd + strlen("+CMQTTRXEND:");
  // Pular o índice de cliente e \r\n
  int idxFimLinha = bufURC.indexOf('\n', fimURC);
  bufURC = (idxFimLinha >= 0) ? bufURC.substring(idxFimLinha + 1) : "";
}

// ═════════════════════════════════════════════════════════════════════════════
//  16. SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println(F(""));
  Serial.println(F("  ╔══════════════════════════════════════════╗"));
  Serial.println(F("  ║    HDrop – Barco Autônomo  v1.0          ║"));
  Serial.println(F("  ║    ESP32 | A7670SA | QMC5883L            ║"));
  Serial.println(F("  ╚══════════════════════════════════════════╝"));
  Serial.println(F(""));

  // Inicializar I2C para o magnetômetro
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  qmc_init();

  // Inicializar Serial2 para o modem LTE
  Serial2.begin(MODEM_BAUD, SERIAL_8N1, SERIAL2_RX_PIN, SERIAL2_TX_PIN);
  delay(2000);  // Aguardar estabilização do modem pós-boot

  Serial.println(F("[INIT] Sistema pronto. Máquina de estados iniciando..."));
  Serial.println(F("──────────────────────────────────────────────────────"));
}

// ═════════════════════════════════════════════════════════════════════════════
//  17. LOOP PRINCIPAL – MÁQUINA DE ESTADOS
// ═════════════════════════════════════════════════════════════════════════════

void loop() {

  switch (estadoAtual) {

    // ══════════════════════════════════════════════════════════════════════════
    //  Estado 1: CONECTANDO_REDE
    //  Responsabilidades:
    //    - Verificar modem
    //    - Aguardar registro de rede
    //    - Configurar APN e ativar PDP (AT+CGACT=1,1)
    //    - Ligar GNSS (AT+CGNSSPWR=1) e aguardar READY!
    //  Transição: → CONECTANDO_MQTT (sucesso) | retry com delay (falha)
    // ══════════════════════════════════════════════════════════════════════════
    case CONECTANDO_REDE:
      Serial.println(F("\n[FSM] ▶ CONECTANDO_REDE"));
      if (conectarRede()) {
        Serial.println(F("[FSM] ✓ Rede e GNSS prontos → CONECTANDO_MQTT"));
        estadoAtual = CONECTANDO_MQTT;
      } else {
        Serial.println(F("[FSM] ✗ Falha. Aguardando 15 s para nova tentativa..."));
        delay(15000);
      }
      break;

    // ══════════════════════════════════════════════════════════════════════════
    //  Estado 2: CONECTANDO_MQTT
    //  Responsabilidades:
    //    - Liberar sessão MQTT anterior (DISC → REL → STOP)
    //    - AT+CMQTTSTART → AT+CMQTTACCQ → AT+CMQTTCONNECT
    //    - Subscribe em "hdrop/comando"
    //  Transição:
    //    → OPERANDO       (sucesso)
    //    → CONECTANDO_REDE (PDP perdido)
    //    → retry           (falha MQTT mas PDP OK)
    //  Importante: O GNSS NÃO é desligado neste estado.
    // ══════════════════════════════════════════════════════════════════════════
    case CONECTANDO_MQTT:
      Serial.println(F("\n[FSM] ▶ CONECTANDO_MQTT"));

      liberarSessaoMQTT();
      delay(1000);

      if (conectarMQTT()) {
        Serial.println(F("[FSM] ✓ MQTT pronto → OPERANDO"));
        estadoAtual = OPERANDO;
        // Agenda o primeiro envio imediatamente
        ultimaTelemetria = millis() - INTERVALO_TELEMETRIA;
      } else {
        Serial.println(F("[FSM] ✗ Falha no MQTT. Aguardando 8 s..."));
        delay(8000);

        // Verificar se o contexto PDP ainda está ativo antes de tentar de novo
        if (!enviarAT("AT+CGACT?", "+CGACT: 1,1", AT_TIMEOUT)) {
          Serial.println(F("[FSM] ✗ PDP perdido → CONECTANDO_REDE"));
          estadoAtual = CONECTANDO_REDE;
        }
        // Caso contrário, permanece em CONECTANDO_MQTT (retry automático)
      }
      break;

    // ══════════════════════════════════════════════════════════════════════════
    //  Estado 3: OPERANDO
    //  Loop de telemetria 2 Hz (controlado por millis):
    //    a. Capturar e processar URCs (não bloqueante)
    //    b. Se atingiu 500 ms:
    //       1. Ler AT+CGNSSINFO (string bruta)
    //       2. Ler QMC5883L via I2C (X, Y, Z)
    //       3. Publicar JSON em "hdrop/raw"
    //    c. Em falhas consecutivas:
    //       - Verificar AT+CMQTTCONNECT?
    //       - Reconectar apenas MQTT (GNSS permanece ativo)
    // ══════════════════════════════════════════════════════════════════════════
    case OPERANDO:
    {
      // ── a. URCs assíncronas (comandos do backend) ─────────────────────────
      capturarURCs();
      processarURCs();

      // Verificar se uma perda de conexão detectada por URC exige ação
      if (contErrosMQTT >= MAX_ERROS_MQTT) {
        Serial.println(F("[FSM] Limite de erros MQTT atingido → verificando conexão..."));
        if (!mqttConectado()) {
          Serial.println(F("[FSM] MQTT desconectado. Reconectando (GNSS ativo) → CONECTANDO_MQTT"));
          estadoAtual = CONECTANDO_MQTT;
          break;
        } else {
          Serial.println(F("[FSM] Conexão MQTT OK. Resetando contador de erros."));
          contErrosMQTT = 0;
        }
      }

      // ── b. Controle de temporização 2 Hz via millis() ────────────────────
      unsigned long agora = millis();
      if (agora - ultimaTelemetria < INTERVALO_TELEMETRIA) {
        delay(5);  // Ceder CPU sem bloquear
        break;
      }
      ultimaTelemetria = agora;

      // ── c.1 Ler GNSS ──────────────────────────────────────────────────────
      bool temFix = lerGNSSInfo(bufGnss, sizeof(bufGnss));
      if (temFix) {
        Serial.print(F("[GNSS] Fix: "));
        Serial.println(bufGnss);
      } else {
        Serial.println(F("[GNSS] Sem fix. String bruta: " ));
        Serial.println(bufGnss);
      }

      // ── c.2 Ler Magnetômetro ──────────────────────────────────────────────
      int16_t mx = 0, my = 0, mz = 0;
      if (!qmc_read(mx, my, mz)) {
        Serial.println(F("[MAG] ERRO: Falha I2C."));
      } else {
        Serial.print(F("[MAG] X="));
        Serial.print(mx);
        Serial.print(F(" Y="));
        Serial.print(my);
        Serial.print(F(" Z="));
        Serial.println(mz);
      }

      // ── c.3 Publicar Telemetria ───────────────────────────────────────────
      if (publicarTelemetria(bufGnss, mx, my, mz)) {
        contErrosMQTT = 0;  // Reset em sucesso

      } else {
        contErrosMQTT++;
        Serial.print(F("[FSM] Erro de publicação MQTT consecutivo #"));
        Serial.println(contErrosMQTT);

        // Limite atingido → verificar conexão no próximo ciclo (topo do case)
        // Não chama verificação aqui para não bloquear o loop
      }

      break;
    }

  }  // end switch
}

// ═════════════════════════════════════════════════════════════════════════════
//  FIM DO ARQUIVO
//
//  Tópicos MQTT:
//    Publica  → hdrop/raw     {"g":"<gnss_bruto>","m":[x,y,z]}
//    Assina   → hdrop/comando {"lat":..., "lon":..., ...}   (impresso como ROTA:)
//
//  Monitorar via Serial (115200 baud):
//    Prefixo [AT>]  = comando enviado ao modem
//    Prefixo [AT<]  = resposta recebida
//    Prefixo [PUB]  = telemetria publicada
//    Prefixo [CMD]  = comando de rota recebido
//    Prefixo ROTA:  = JSON para o backend processar
// ═════════════════════════════════════════════════════════════════════════════
