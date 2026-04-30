#include <Arduino.h>

// =========================
// Definição de pinos
// =========================
const int TRIG_PIN   = 8;
const int ECHO_PIN   = 9;
const int REF_PIN    = A1;  // potenciômetro → threshold de distância
const int LED_PIN    = 7;   // LED vermelho → sensor ok
const int BUZZER_PIN = 5;   // buzzer → alarme de obstáculo

// =========================
// Parâmetros de amostragem
// =========================
float Ts = 0.05f;  // 50ms

// =========================
// Faixa do sensor
// =========================
const float DIST_MIN_VALIDA  = 20.0f;
const float DIST_MAX_VALIDA  = 300.0f;
const float DIST_MIN_PROJETO = 20.0f;
const float DIST_MAX_PROJETO = 300.0f;
const float DIST_RANGE       = DIST_MAX_PROJETO - DIST_MIN_PROJETO;

// =========================
// Filtro exponencial da saída
// =========================
const float ALPHA = 0.75f;

// =========================
// Filtro do potenciômetro
// =========================
const float ALPHA_REF = 0.85f;

// =========================
// Média móvel
// =========================
const int N_MEDIA = 6;
float janela[N_MEDIA];
int   janela_idx   = 0;
bool  janela_cheia = false;

// =========================
// Rejeição de saltos
// =========================
const float SALTO_MAX_CM = 50.0f;

// =========================
// Robustez de leitura
// =========================
const unsigned long PULSE_TIMEOUT_US = 50000UL;
const int FALHAS_MAX = 5;
int  falhas_consecutivas = 0;
bool sensor_ok = false;

// =========================
// Confirmação do alarme
// =========================
const int CONFIRMACOES_ALARME = 2;
const int CONFIRMACOES_LIVRE  = 3;
int  contador_alarme = 0;
int  contador_livre  = 0;
bool alarme_ativo = false;

// =========================
// Variáveis de estado
// =========================
float dist_valida   = 100.0f;
float dist_media    = 100.0f;
float dist_filtrada = 100.0f;
float ref_filtrado  = 0.5f;

bool primeira_leitura = true;

unsigned long t_prev_ms = 0;

// =========================
// Leitura do sensor
// =========================
bool lerUltrassom(float &dist_cm) {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(15);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duracao = pulseIn(ECHO_PIN, HIGH, PULSE_TIMEOUT_US);
  if (duracao == 0) return false;

  float medida = (duracao * 0.0343f) / 2.0f;
  if (medida < DIST_MIN_VALIDA || medida > DIST_MAX_VALIDA) return false;

  dist_cm = medida;
  return true;
}

// =========================
// Média móvel
// =========================
float calcularMedia(float novo_valor) {
  janela[janela_idx] = novo_valor;
  janela_idx = (janela_idx + 1) % N_MEDIA;
  if (janela_idx == 0) janela_cheia = true;

  int n = janela_cheia ? N_MEDIA : janela_idx;
  float soma = 0.0f;
  for (int i = 0; i < n; i++) soma += janela[i];
  return soma / n;
}

void setup() {
  pinMode(TRIG_PIN,   OUTPUT);
  pinMode(ECHO_PIN,   INPUT);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(TRIG_PIN,   LOW);
  digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);

  for (int i = 0; i < N_MEDIA; i++) janela[i] = dist_valida;

  ref_filtrado = analogRead(REF_PIN) / 1023.0f;

  Serial.begin(115200);
  t_prev_ms = millis();
}

void loop() {
  if (millis() - t_prev_ms >= (unsigned long)(Ts * 1000.0f)) {
    t_prev_ms += (unsigned long)(Ts * 1000.0f);

    // =========================
    // 1. Leitura do potenciômetro
    // =========================
    float ref_bruto = analogRead(REF_PIN) / 1023.0f;
    ref_filtrado = ALPHA_REF * ref_filtrado + (1.0f - ALPHA_REF) * ref_bruto;
    float ref = ref_filtrado;

    // =========================
    // 2. Leitura bruta do sensor
    // =========================
    float dist_bruta = 0.0f;
    bool leitura_ok = lerUltrassom(dist_bruta);

    if (leitura_ok) {
      falhas_consecutivas = 0;
      sensor_ok = true;

      if (primeira_leitura) {
        dist_valida = dist_bruta;
        primeira_leitura = false;
      } else {
        float salto = fabs(dist_bruta - dist_valida);
        if (salto <= SALTO_MAX_CM) {
          dist_valida = dist_bruta;
        } else {
          dist_valida = 0.7f * dist_valida + 0.3f * dist_bruta;
        }
      }

    } else {
      falhas_consecutivas++;
      sensor_ok = (falhas_consecutivas <= FALHAS_MAX);
    }

    // =========================
    // 3. Média móvel
    // =========================
    dist_media = calcularMedia(dist_valida);

    // =========================
    // 4. Filtro exponencial
    // =========================
    dist_filtrada = ALPHA * dist_filtrada + (1.0f - ALPHA) * dist_media;

    // =========================
    // 5. Normalização [0..1]
    // =========================
    float y_bruta    = constrain((dist_valida   - DIST_MIN_PROJETO) / DIST_RANGE, 0.0f, 1.0f);
    float y_media    = constrain((dist_media    - DIST_MIN_PROJETO) / DIST_RANGE, 0.0f, 1.0f);
    float y_filtrada = constrain((dist_filtrada - DIST_MIN_PROJETO) / DIST_RANGE, 0.0f, 1.0f);

    // =========================
    // 6. Lógica de alarme com confirmação
    // =========================
    if (sensor_ok) {
      if (y_filtrada < ref) {
        contador_alarme++;
        contador_livre = 0;
        if (contador_alarme >= CONFIRMACOES_ALARME) {
          alarme_ativo = true;
        }
      } else {
        contador_livre++;
        contador_alarme = 0;
        if (contador_livre >= CONFIRMACOES_LIVRE) {
          alarme_ativo = false;
        }
      }
    } else {
      alarme_ativo    = false;
      contador_alarme = 0;
      contador_livre  = 0;
    }

    // =========================
    // 7. LED e Buzzer
    // LED aceso  → sensor ok (leitura confiável)
    // LED apagado → sensor com falha
    // Buzzer apita → alarme ativo (objeto passou do threshold)
    // =========================
    digitalWrite(LED_PIN, sensor_ok ? HIGH : LOW);
    digitalWrite(BUZZER_PIN, alarme_ativo ? HIGH : LOW);

    // =========================
    // 8. Log para Teleplot
    // =========================
    Serial.print(">");

    Serial.print("ref:");
    Serial.print(ref, 3);
    Serial.print(",");

    Serial.print("y_bruta:");
    Serial.print(y_bruta, 3);
    Serial.print(",");

    Serial.print("y_media:");
    Serial.print(y_media, 3);
    Serial.print(",");

    Serial.print("y_filtrada:");
    Serial.print(y_filtrada, 3);
    Serial.print(",");

    Serial.print("alarme:");
    Serial.print(alarme_ativo ? 1 : 0);
    Serial.print(",");

    Serial.print("sensor_ok:");
    Serial.println(sensor_ok ? 1 : 0);
  }
}