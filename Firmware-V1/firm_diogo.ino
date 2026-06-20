//===============================================================
//  H-DROP ASV — Firmware Motores v2.0 
//  Plataforma : ESP32-WROOM (ArduinoIDE)
//  Projeto    : Módulo Logístico Autônomo de Entrega
//===============================================================

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <Update.h>
#include <ESP32Servo.h>
#include <WebServer.h>
#include <ArduinoOTA.h>

// ==========================================
// CONFIGURAÇÕES DA REDE DO BARCO (MODO AP)
// ==========================================
const char* ap_ssid = "H-DROP-Control";
const char* ap_password = "senha_segura_123"; // Mínimo de 8 caracteres

// ==========================================
// CONFIGURAÇÕES DOS MOTORES
// ==========================================
Servo escEsquerdo;
Servo escDireito;
const int pinoESCEsquerdo = 4;
const int pinoESCDireito = 15;  
const int PONTO_MORTO = 1520;

// Cria o servidor web na porta 80 (Padrão HTTP)
WebServer server(80);

// Flag para controlar se o OTA foi iniciado com sucesso
bool otaIniciado = false;

// ==========================================
// FRONTEND: PÁGINA DE ATUALIZAÇÃO VIA NAVEGADOR
// ==========================================
const char paginaUpdateHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="UTF-8">
  <title>H-DROP - OTA via Web</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 0; padding: 20px; background-color: #222; color: white;}
    .card { background-color: #333; padding: 30px; border-radius: 10px; margin: 40px auto; max-width: 400px; box-shadow: 0px 4px 10px rgba(0,0,0,0.5); }
    h2 { color: #ffeb3b; margin-top: 0; }
    input[type=file] { margin: 20px 0; background: #555; padding: 10px; border-radius: 5px; width: 80%; color: white; cursor: pointer; }
    input[type=submit] { background-color: #4CAF50; color: white; width: 100%; height: 50px; font-size: 18px; font-weight: bold; border-radius: 10px; border: none; cursor: pointer; }
    input[type=submit]:hover { background-color: #45a049; }
    .btn-voltar { display: block; margin-top: 20px; color: #2196F3; text-decoration: none; font-weight: bold; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Atualização de Firmware Web</h2>
    <p>Selecione o arquivo .bin compilado pela IDE do Arduino.</p>
    <form method='POST' action='/update' enctype='multipart/form-data'>
      <input type='file' name='update' accept='.bin'>
      <input type='submit' value='Enviar Novo Código'>
    </form>
    <a href='/' class='btn-voltar'>← Voltar ao Painel</a>
  </div>
</body>
</html>
)rawliteral";

// ==========================================
// FRONTEND: PAINEL DE CONTROLE PRINCIPAL
// ==========================================
const char paginaHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="UTF-8">
  <title>H-DROP Control</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 0; padding: 20px; background-color: #222; color: white;}
    h1 { margin-bottom: 5px; }
    button { width: 100%; max-width: 400px; height: 60px; font-size: 20px; font-weight: bold; margin: 10px auto; border-radius: 10px; border: none; cursor: pointer; display: block; }
    .btn-frente { background-color: #4CAF50; color: white; }
    .btn-re { background-color: #2196F3; color: white; }
    .btn-parar { background-color: #f44336; color: white; height: 80px; font-size: 24px; box-shadow: 0px 4px 10px rgba(244, 67, 54, 0.5);}
    .slider-container { background-color: #333; padding: 15px; border-radius: 10px; margin: 20px auto; max-width: 400px; }
    .slider { -webkit-appearance: none; width: 100%; height: 25px; border-radius: 5px; background: #555; outline: none; margin-top: 10px; }
    .slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 35px; height: 35px; border-radius: 50%; background: #FFF; cursor: pointer; }
    .slider::-moz-range-thumb { width: 35px; height: 35px; border-radius: 50%; background: #FFF; cursor: pointer; }
    .label-pwm { font-size: 18px; font-weight: bold; color: #ffeb3b; }
    .link-ota { display: inline-block; margin-top: 20px; color: #bbb; text-decoration: none; font-size: 14px; }
  </style>
</head>
<body>
  <h1><span style="color: #FF5722;">H</span>-DROP</h1>
  <p>Painel de Controle de Tração</p>
 
  <button class="btn-parar" onclick="enviarComandoGeral('/parar', 1520)">PARAR (1520us)</button>

  <div class="slider-container">
    <label>Motor Esquerdo: <span id="valE" class="label-pwm">1520</span> us</label>
    <input type="range" id="sliderE" class="slider" min="1000" max="2000" value="1520" oninput="atualizarMotor('E', this.value)">
  </div>

  <div class="slider-container">
    <label>Motor Direito: <span id="valD" class="label-pwm">1520</span> us</label>
    <input type="range" id="sliderD" class="slider" min="1000" max="2000" value="1520" oninput="atualizarMotor('D', this.value)">
  </div>

  <button class="btn-frente" onclick="enviarComandoGeral('/frente', 1600)">AMBOS P/ FRENTE (1600us)</button>
  <button class="btn-re" onclick="enviarComandoGeral('/re', 1400)">AMBOS P/ RÉ (1400us)</button>

  <a href="/update" class="link-ota">⚙ Atualizar Sistema Manualmente (.bin)</a>

  <script>
    function atualizarMotor(lado, valor) {
      document.getElementById('val' + lado).innerText = valor;
      fetch('/motor?lado=' + lado + '&pwm=' + valor);
    }

    function enviarComandoGeral(rota, pwmBase) {
      fetch(rota);
      document.getElementById('sliderE').value = pwmBase;
      document.getElementById('sliderD').value = pwmBase;
      document.getElementById('valE').innerText = pwmBase;
      document.getElementById('valD').innerText = pwmBase;
    }
  </script>
</body>
</html>
)rawliteral";

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Iniciando H-DROP Web Server ---");

  // 1. CRIAÇÃO DA REDE WI-FI PRÓPRIA (MODO AP)
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
 
  IPAddress IP = WiFi.softAPIP();
 
  Serial.println("\n[OK] Rede Wi-Fi do H-DROP Criada com Sucesso!");
  Serial.print("Nome da Rede: "); Serial.println(ap_ssid);
  Serial.print(">>> ACESSE PELO NAVEGADOR: http://");
  Serial.println(IP); // O IP será SEMPRE 192.168.4.1
 
  MDNS.begin("h-drop-core");

  // ==========================================
  // INICIALIZAÇÃO DO ARDUINO OTA (IDE)
  // ==========================================
  ArduinoOTA.setHostname("h-drop-core");
  ArduinoOTA.onStart([]() {
    // Segurança: Desliga motores se uma atualização via IDE começar
    escEsquerdo.writeMicroseconds(PONTO_MORTO);
    escDireito.writeMicroseconds(PONTO_MORTO);
    Serial.println("Iniciando atualização OTA via IDE...");
  });
  ArduinoOTA.begin();
  otaIniciado = true;
  Serial.println("OTA via IDE Arduino ativado e escutando!");

  // 2. CONFIGURAÇÃO DOS MOTORES E ARMING (1520us)
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  escEsquerdo.setPeriodHertz(50);
  escDireito.setPeriodHertz(50);
  escEsquerdo.attach(pinoESCEsquerdo, 1000, 2000);
  escDireito.attach(pinoESCDireito, 1000, 2000);
 
  escEsquerdo.writeMicroseconds(PONTO_MORTO);
  escDireito.writeMicroseconds(PONTO_MORTO);
  delay(4000);
 
  // ==========================================
  // ROTAS DO SERVIDOR WEB (BACKEND)
  // ==========================================
 
  // Rota do painel de controle
  server.on("/", []() {
    server.send(200, "text/html", paginaHTML);
  });

  // Rota que exibe o formulário de upload
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html", paginaUpdateHTML);
  });

  // Rota que processa o arquivo .bin recebido via navegador
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FALHA NA ATUALIZACAO" : "SUCESSO! REINICIANDO...");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Atualizando Firmware Web: %s\n", upload.filename.c_str());
     
      // Desliga os motores imediatamente por segurança
      escEsquerdo.writeMicroseconds(PONTO_MORTO);
      escDireito.writeMicroseconds(PONTO_MORTO);

      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Sucesso: %u bytes gravados.\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  // Rota para controle dos sliders
  server.on("/motor", []() {
    if (server.hasArg("lado") && server.hasArg("pwm")) {
      String lado = server.arg("lado");
      int pwm = server.arg("pwm").toInt();
     
      if (lado == "E") escEsquerdo.writeMicroseconds(pwm);
      if (lado == "D") escDireito.writeMicroseconds(pwm);
     
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Faltam argumentos");
    }
  });

  // Rotas dos botões rápidos
  server.on("/frente", []() {
    escEsquerdo.writeMicroseconds(1600);
    escDireito.writeMicroseconds(1600);
    server.send(200, "text/plain", "OK");
  });

  server.on("/parar", []() {
    escEsquerdo.writeMicroseconds(PONTO_MORTO);
    escDireito.writeMicroseconds(PONTO_MORTO);
    server.send(200, "text/plain", "OK");
  });

  server.on("/re", []() {
    escEsquerdo.writeMicroseconds(1400);
    escDireito.writeMicroseconds(1400);
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

// ==========================================
// LOOP PRINCIPAL
// ==========================================
void loop() {
  // Mantém o servidor web ouvindo comandos do celular
  server.handleClient();

  // Mantém a porta do Arduino OTA aberta para uploads da IDE
  if (otaIniciado) {
    ArduinoOTA.handle();
  }
}