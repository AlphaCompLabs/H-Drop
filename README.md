<div align="center">

# H-DROP

### Veículo de Superfície Autônomo para Logística Humanitária

<br>

<p>
  <img src="https://img.shields.io/badge/Vehicle-ASV_Catamar%C3%A3-0a4d8c?style=for-the-badge&logo=sailboat&logoColor=white" alt="ASV">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Embedded-ESP32-E7352C?style=for-the-badge&logo=espressif&logoColor=white" alt="ESP32">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Modem-4G_A7670SA-ffc233?style=for-the-badge&logo=4g&logoColor=black" alt="4G">
  <br><br>
  <img src="https://img.shields.io/badge/Backend-FastAPI-009688?style=for-the-badge&logo=fastapi&logoColor=white" alt="FastAPI">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Frontend-Angular_19-DD0031?style=for-the-badge&logo=angular&logoColor=white" alt="Angular">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Protocol-MQTT-660066?style=for-the-badge&logo=mqtt&logoColor=white" alt="MQTT">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Map-Leaflet-199900?style=for-the-badge&logo=leaflet&logoColor=white" alt="Leaflet">
</p>

<p>
  <strong>Projeto H-DROP · Relatório PC1 · 2026</strong>
</p>

</div>

---

## Sobre o Projeto

O **H-DROP** é um **Veículo de Superfície Autônomo (ASV)** do tipo catamarã, projetado para missões de **logística humanitária e entregas em regiões de difícil acesso terrestre** — áreas alagadas, comunidades ribeirinhas e cenários pós-desastre onde a via aquática é a rota mais viável.

A embarcação opera de forma autônoma a partir de um sistema embarcado ESP32, com propulsão por motores brushless em configuração diferencial, posicionamento via GNSS, bússola magnética e conectividade móvel 4G LTE através do módulo A7670SA. O software da estação base — composto por **backend FastAPI** e **frontend Angular** — atua como **Estação de Controle de Missão**, recebendo telemetria em tempo real, monitorando subsistemas críticos e despachando waypoints ao barco.

### Objetivos

- Automatizar entregas aquáticas em rotas programáveis por waypoints.
- Monitorar em tempo real a posição (GPS suavizado), proa magnética, estado de bateria (LiFePO4 + INA226) e presença de carga (célula HX711).
- Oferecer uma interface operacional tática (Mission Control) responsiva, com suporte a dispositivos móveis para operação em campo.
- Garantir rastreabilidade completa da missão via logs estruturados e persistência (roadmap).

---

## Arquitetura da Solução

O sistema é composto por três camadas independentes que se comunicam via protocolo MQTT e WebSocket:

```
H-Drop/
├── BackEnd/     → API REST em Python/FastAPI + cliente MQTT assíncrono
├── FrontEnd/    → SPA (Single Page Application) em Angular 19 com Leaflet
└── Docs/        → Documentação técnica, relatório PC1 e diagramas dos subsistemas
```

### Fluxo de Dados

```
┌──────────────┐      MQTT       ┌──────────────┐    WebSocket    ┌──────────────┐
│   ESP32      │ ─────────────▶  │   Backend    │ ──────────────▶ │   Angular    │
│   + GNSS     │   hdrop/raw     │   FastAPI    │  /ws/telemetria │   Dashboard  │
│   + Magnet.  │ ◀─────────────  │   (Hub)      │ ◀────────────── │   (Operador) │
└──────────────┘  hdrop/comando  └──────────────┘   POST /comando └──────────────┘
                                         │
                                         ▼
                                 hdrop/telemetria
                                 (eco processado)
```

1. O ESP32 publica telemetria bruta (string NMEA + vetor do magnetômetro) no tópico **`hdrop/raw`**.
2. O backend processa em tempo real — calcula a proa calibrada e extrai coordenadas GPS — e re-publica o resultado limpo em **`hdrop/telemetria`**, simultaneamente fazendo broadcast via **WebSocket** para todos os operadores conectados.
3. O operador clica no mapa, o frontend envia **`POST /api/v1/comando`** ao backend, que publica a coordenada em **`hdrop/comando`** — o ESP32 recebe e recalcula a rota.

---

## Subsistemas Monitorados (Relatório PC1)

| Subsistema | Hardware | Responsabilidade |
|:---|:---|:---|
| **Controle Embarcado** | ESP32 | Orquestração de sensores, atuação dos motores, comunicação MQTT |
| **Posicionamento** | GNSS + A7670SA | Geolocalização via constelação de satélites + fix assistido |
| **Comunicação** | A7670SA (4G LTE) | Uplink de telemetria e downlink de comandos através de broker MQTT |
| **Navegação** | Magnetômetro calibrado | Determinação de proa magnética (0°–360°) |
| **Propulsão** | 2× motores brushless | Controle diferencial para avanço, ré e curvas |
| **Energia** | LiFePO4 + INA226 | Monitoramento de tensão, corrente e SoC da bateria |
| **Carga Útil** | Célula de carga HX711 | Detecção de compartimento ocupado/vazio |

---

## Início Rápido

### Pré-requisitos

- **Python 3.11+** e pip
- **Node.js 20+** e npm
- Conexão à internet (para alcançar o broker público `broker.hivemq.com`)

### 1. Iniciar o Backend

```bash
cd BackEnd
python -m venv venv
venv\Scripts\activate          # Windows
# source venv/bin/activate     # Linux/macOS
pip install -r requirements.txt
copy .env.example .env         # Windows
# cp .env.example .env         # Linux/macOS
python main.py
```

API disponível em **`http://localhost:8000`** · Swagger em **`http://localhost:8000/docs`**

### 2. Iniciar o Frontend

```bash
cd FrontEnd
npm install
npm start
```

Mission Control disponível em **`http://localhost:4200`**

### 3. Simular Telemetria do ESP32 (opcional)

Sem o hardware em mãos, você pode publicar mensagens diretamente no broker usando `mosquitto_pub` ou qualquer cliente MQTT:

```bash
mosquitto_pub -h broker.hivemq.com -t hdrop/raw \
  -m '{"g":",,,,,-15.87500,S,-48.08500,W,...","m":[1700,150,0]}'
```

O dashboard deve atualizar a posição e a proa em tempo real.

---

## Stack Tecnológica

| Camada | Tecnologia | Versão |
|:---|:---|:---|
| Framework Backend | FastAPI | 0.115+ |
| Cliente MQTT Assíncrono | aiomqtt | 2.3+ |
| Servidor ASGI | Uvicorn | 0.32+ |
| Validação de Dados | Pydantic | 2.10+ |
| Configuração | pydantic-settings | 2.7+ |
| Framework Frontend | Angular | 19+ |
| Linguagem | TypeScript | 5.6+ |
| Biblioteca de Mapas | Leaflet | 1.9+ |
| Ícones | Iconify (Phosphor Icons) | 2.1+ (via CDN) |
| Reatividade | RxJS + Angular Signals | — |
| Broker MQTT | HiveMQ (público) | — |

---

## Identidade Visual (PC1)

A paleta **Mission Control** foi derivada do Relatório PC1 e pensada para leitura rápida em situações críticas:

| Token | Cor | Uso |
|:---|:---:|:---|
| **Critical Ocean Blue** | `#0a4d8c` | Interface principal, mapas, destaques |
| **Life-Saving Green** | `#00d66b` | Estados OK, bateria cheia, missão concluída |
| **Safety Yellow** | `#ffc233` | Avisos, proximidade de obstáculos, sync pendente |
| **Strategic Red** | `#ff3b3b` | Erros críticos, parada de emergência, bateria crítica |

Dois temas disponíveis via toggle no header: **Mission Dark** (padrão, otimizado para cabines de controle) e **Utility Light** (otimizado para luz solar direta em campo).

---

## Funcionalidades

| Status | Funcionalidade |
|:---:|:---|
| ✓ | Telemetria GPS em tempo real com suavização (média móvel de 5 pontos) |
| ✓ | Cálculo assíncrono da proa magnética com calibração de offsets |
| ✓ | Mapa Leaflet interativo com 3 camadas (dark/street/satellite) |
| ✓ | Envio de waypoints por clique no mapa (publish MQTT) |
| ✓ | WebSocket broadcast para múltiplos operadores simultâneos |
| ✓ | Dashboard responsivo com abas mobile (mapa/telemetria/sync) |
| ✓ | Aba Sync Test com comandos rápidos (START/ACK/HOME/STOP) |
| ✓ | Sistema de alertas críticos com modal bloqueante |
| ✓ | Troca dinâmica Mission Dark / Utility Light |
| ○ | Persistência de histórico de missões (MongoDB — roadmap Fase 3) |
| ○ | Monitoramento do SoC da bateria via INA226 (firmware em desenvolvimento) |
| ○ | Detecção de carga via HX711 (firmware em desenvolvimento) |
| ○ | Autenticação e perfis de operador (roadmap) |

---

## Documentação por Módulo

| Módulo | Documentação |
|:---|:---|
| **Backend — API FastAPI + MQTT** | [BackEnd/README.md](./BackEnd/README.md) |
| **Frontend — Mission Control Angular** | [FrontEnd/README.md](./FrontEnd/README.md) |
| **Relatório PC1 e subsistemas** | [Docs/](./Docs/) |

---

<div align="center">
  <br>
  &copy; 2026 Projeto H-DROP. Todos os direitos reservados.
  <br><br>
  <em>Autonomous Surface Vehicle · Humanitarian Logistics · Mission Control</em>
</div>
