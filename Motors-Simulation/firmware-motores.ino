#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP32Servo.h>
#include <WebServer.h>

// ==========================================
// CONFIGURAÇÕES DE REFE (Wi-Fi)
// ==========================================
const char* ssid = "NOME REDE ROTEADOR TELEFONE";
const char* password = "SENHA REDE ROTEADOR TELEFONE";

// ==========================================
// CONFIGURAÇÕES DOS MOTORES (Pinos Verdes/Seguros)
// ==========================================
Servo escEsquerdo;
Servo escDireito;
const int pinoESCEsquerdo = 4; // Fio Amarelo do ESC Esquerdo no D18
const int pinoESCDireito = 15;  // Fio Amarelo do ESC Direito no D19

const int PONTO_MORTO = 1520;

// Cria o servidor web na porta 80 (Padrão HTTP)
WebServer server(80);
bool otaIniciado = false;

// ==========================================
// FRONTEND (HTML + CSS + JS)
// Usando rawliteral para manter o código limpo
// ==========================================
const char paginaHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="UTF-8">
  <title>Stella Acqua</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 0; padding: 20px; background-color: #222; color: white;}
    h1 { margin-bottom: 5px; }
    button { width: 100%; max-width: 400px; height: 60px; font-size: 20px; font-weight: bold; margin: 10px auto; border-radius: 10px; border: none; cursor: pointer; display: block; }
    .btn-frente { background-color: #4CAF50; color: white; }
    .btn-re { background-color: #2196F3; color: white; }
    .btn-parar { background-color: #f44336; color: white; height: 80px; font-size: 24px; box-shadow: 0px 4px 10px rgba(244, 67, 54, 0.5);}
    
    /* Estilos dos Sliders */
    .slider-container { background-color: #333; padding: 15px; border-radius: 10px; margin: 20px auto; max-width: 400px; }
    .slider { -webkit-appearance: none; width: 100%; height: 25px; border-radius: 5px; background: #555; outline: none; margin-top: 10px; }
    .slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 35px; height: 35px; border-radius: 50%; background: #FFF; cursor: pointer; }
    .slider::-moz-range-thumb { width: 35px; height: 35px; border-radius: 50%; background: #FFF; cursor: pointer; }
    .label-pwm { font-size: 18px; font-weight: bold; color: #ffeb3b; }
  </style>
</head>
<body>
  <h1>Stella Acqua</h1>
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

  <script>
    // Função para atualizar um motor individualmente usando o Slider
    function atualizarMotor(lado, valor) {
      document.getElementById('val' + lado).innerText = valor;
      fetch('/motor?lado=' + lado + '&pwm=' + valor);
    }

    // Função para os botões gerais (Frente, Ré, Parar)
    function enviarComandoGeral(rota, pwmBase) {
      // Envia a requisição HTTP em segundo plano
      fetch(rota);
      
      // Atualiza visualmente os sliders e os textos para refletirem a realidade
      document.getElementById('sliderE').value = pwmBase;
      document.getElementById('sliderD').value = pwmBase;
      document.getElementById('valE').innerText = pwmBase;
      document.getElementById('valD').innerText = pwmBase;
    }
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Iniciando Stella Acqua Web Server ---");

  // 1. TENTA CONECTAR NO HOTSPOT DO CELULAR
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  Serial.print("Procurando o roteador do celular...");
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) { 
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[OK] Conectado ao Celular!");
    Serial.print(">>> DIGITE ESTE IP NO NAVEGADOR DO CELULAR: ");
    Serial.println(WiFi.localIP());

    ArduinoOTA.setHostname("StellaAcqua-Core");
    ArduinoOTA.onStart([]() {
      escEsquerdo.writeMicroseconds(PONTO_MORTO);
      escDireito.writeMicroseconds(PONTO_MORTO);
    });
    ArduinoOTA.begin();
    otaIniciado = true;
  } else {
    Serial.println("\n[ERRO] Não achou o celular.");
  }

  // 2. CONFIGURAÇÃO DOS MOTORES E ARMING (AGORA COM 1520us)
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  escEsquerdo.setPeriodHertz(50);
  escDireito.setPeriodHertz(50);
  escEsquerdo.attach(pinoESCEsquerdo, 1000, 2000);
  escDireito.attach(pinoESCDireito, 1000, 2000);
  
  // Enviando 1520us para armar os ESCs corretamente
  escEsquerdo.writeMicroseconds(PONTO_MORTO);
  escDireito.writeMicroseconds(PONTO_MORTO);
  delay(4000); 
  
  // ==========================================
  // ROTAS DO SERVIDOR WEB (BACKEND)
  // ==========================================
  
  // Rota principal: entrega o HTML
  server.on("/", []() {
    server.send(200, "text/html", paginaHTML);
  });

  // Rota para controle individual via Sliders (/motor?lado=E&pwm=1600)
  server.on("/motor", []() {
    if (server.hasArg("lado") && server.hasArg("pwm")) {
      String lado = server.arg("lado");
      int pwm = server.arg("pwm").toInt();
      
      if (lado == "E") escEsquerdo.writeMicroseconds(pwm);
      if (lado == "D") escDireito.writeMicroseconds(pwm);
      
      Serial.printf("Motor %s: %d us\n", lado.c_str(), pwm);
      server.send(200, "text/plain", "OK"); // Responde sem recarregar a página
    } else {
      server.send(400, "text/plain", "Faltam argumentos");
    }
  });

  // Botão: Ambos para Frente
  server.on("/frente", []() {
    escEsquerdo.writeMicroseconds(1600);
    escDireito.writeMicroseconds(1600);
    Serial.println("Comando WEB: AMBOS FRENTE (1600)");
    server.send(200, "text/plain", "OK");
  });

  // Botão: Parar
  server.on("/parar", []() {
    escEsquerdo.writeMicroseconds(PONTO_MORTO);
    escDireito.writeMicroseconds(PONTO_MORTO);
    Serial.println("Comando WEB: PARAR (1520)");
    server.send(200, "text/plain", "OK");
  });

  // Botão: Ambos para Ré
  server.on("/re", []() {
    escEsquerdo.writeMicroseconds(1400);
    escDireito.writeMicroseconds(1400);
    Serial.println("Comando WEB: AMBOS RE (1400)");
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

void loop() {
  if (otaIniciado) {
    ArduinoOTA.handle();
  }
  server.handleClient();
}

void ExibirMenu() {
  Serial.println("\n==================================================");
  Serial.println("                 TABELA DE COMANDOS               ");
  Serial.println("==================================================");
  Serial.println(" Digite a LETRA do motor seguida do VALOR do PWM (1000 a 2000)");
  Serial.println(" Exemplos:");
  Serial.println("   E1600  -> Liga apenas o Motor ESQUERDO para FRENTE suave");
  Serial.println("   D1350  -> Liga apenas o Motor DIREITO para RÉ moderada");
  Serial.println("   A1500  -> Coloca AMBOS os motores em PONTO MORTO (Parado)");
  Serial.println("   S      -> PARADA DE EMERGÊNCIA (Zera ambos os motores)");
  Serial.println("==================================================\n");
}