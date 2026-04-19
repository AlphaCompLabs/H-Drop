<div align="center">

# H-DROP — Backend

### Hub Central de Telemetria do Veículo de Superfície Autônomo

<br>

<p>
  <img src="https://img.shields.io/badge/Python-3.11+-3776AB?style=for-the-badge&logo=python&logoColor=white" alt="Python">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/FastAPI-0.115+-009688?style=for-the-badge&logo=fastapi&logoColor=white" alt="FastAPI">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Async-aiomqtt_2.3+-660066?style=for-the-badge&logo=mqtt&logoColor=white" alt="aiomqtt">
  <br><br>
  <img src="https://img.shields.io/badge/Protocol-WebSocket-00d4ff?style=for-the-badge&logo=socketdotio&logoColor=white" alt="WebSocket">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Validation-Pydantic_2-E92063?style=for-the-badge&logo=pydantic&logoColor=white" alt="Pydantic">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Docs-Swagger_UI-85EA2D?style=for-the-badge&logo=swagger&logoColor=black" alt="Swagger">
</p>

</div>

---

## Sobre

Serviço Python responsável por atuar como **hub central de comunicação** entre a embarcação H-DROP e os operadores da Estação de Controle. O backend mantém um cliente MQTT assíncrono conectado ao broker, processa a telemetria bruta do ESP32, calcula variáveis derivadas (proa magnética calibrada, extração NMEA, distância Haversine ao destino) e distribui o resultado aos clientes Angular via WebSocket.

Arquitetura 100% assíncrona (FastAPI + aiomqtt + lifespan events), em camadas bem definidas e com injeção de dependência — pronta para receber persistência MongoDB, autenticação e novas métricas de firmware (INA226 + HX711) sem reescrita estrutural.

---

## Stack Tecnológico

| Componente | Tecnologia | Versão |
|:---|:---|:---|
| Linguagem | Python | 3.11+ |
| Framework Web | FastAPI | 0.115.6 |
| Servidor ASGI | Uvicorn | 0.32.1 |
| Cliente MQTT Assíncrono | aiomqtt | 2.3.0 |
| Validação de Dados | Pydantic | 2.10.3 |
| Configuração | pydantic-settings | 2.7.0 |
| WebSocket | websockets | 13.1 |
| Broker MQTT (atual) | HiveMQ (público) | — |

---

## Instalação e Configuração

### 1. Pré-requisitos

- Python **3.11+** no PATH
- pip (gerenciador de pacotes Python)
- Acesso à internet para alcançar `broker.hivemq.com:1883`

### 2. Ambiente Virtual

**Windows:**
```bash
cd BackEnd
python -m venv venv
venv\Scripts\activate
```

**Linux / macOS:**
```bash
cd BackEnd
python3 -m venv venv
source venv/bin/activate
```

### 3. Dependências

```bash
pip install -r requirements.txt
```

### 4. Variáveis de Ambiente

Copie o template para um arquivo `.env` local:

**Windows:** `copy .env.example .env`
**Linux/macOS:** `cp .env.example .env`

Conteúdo padrão (todos os valores têm fallback no código, mas o `.env` é a forma recomendada de configurar):

```env
APP_NAME="H-DROP Backend"
APP_VERSION="3.1.0"
DEBUG=true

MQTT_BROKER="broker.hivemq.com"
MQTT_PORT=1883
MQTT_CLIENT_ID="hdrop_backend_fastapi"
MQTT_KEEPALIVE=60

TOPICO_RAW="hdrop/raw"
TOPICO_TELEMETRIA="hdrop/telemetria"
TOPICO_COMANDO="hdrop/comando"

MAG_OFFSET_X=1648
MAG_OFFSET_Y=119

CORS_ORIGINS=["http://localhost:4200","http://127.0.0.1:4200"]
```

> **Importante:** os offsets do magnetômetro foram validados em campo — não alterar sem recalibrar o hardware. O arquivo `.env` está no `.gitignore` e nunca deve ser versionado.

### 5. Iniciar o Servidor

```bash
python main.py
```

Alternativamente:

```bash
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

| Recurso | URL |
|:---|:---|
| API | `http://localhost:8000` |
| Documentação Swagger UI | `http://localhost:8000/docs` |
| Documentação ReDoc | `http://localhost:8000/redoc` |
| WebSocket de telemetria | `ws://localhost:8000/api/v1/ws/telemetria` |

---

## Arquitetura de Pastas

Inspirada em **Arquitetura em Camadas (Layered Architecture)** orientada a serviços, adaptada para um hub de tempo real sem banco de dados inicial:

```
BackEnd/
│
├── main.py                            # Entry point: instancia FastAPI, CORS, registra router v1 e lifespan
├── requirements.txt                   # Dependências Python com versões fixas
├── .env.example                       # Template de variáveis de ambiente (sem segredos)
├── .gitignore                         # Exclui venv/, .env, __pycache__, logs
│
└── src/                               # Código-fonte principal
    │
    ├── api/
    │   └── v1/
    │       ├── router.py              # Agrega todos os routers sob o prefixo /api/v1
    │       └── routes/                # Endpoints organizados por domínio
    │           ├── health.py          # GET /health e GET /status — healthcheck e estado do hub
    │           ├── comando.py         # POST /comando e GET /comando/ultimo — envio de waypoints
    │           └── telemetria.py      # GET /telemetria/ultima e WS /ws/telemetria — streaming
    │
    ├── core/                          # Infraestrutura central compartilhada
    │   ├── config.py                  # Leitura e validação do .env via pydantic-settings (Settings)
    │   ├── lifespan.py                # Startup/shutdown do FastAPI: inicia e cancela a task MQTT
    │   └── logging.py                 # Configuração centralizada de logging estruturado
    │
    ├── services/                      # Camada de regra de negócio — desacoplada dos endpoints
    │   ├── mqtt_service.py            # Cliente aiomqtt em loop de background com reconexão automática
    │   ├── telemetria_service.py      # Pipeline: raw → proa calibrada + GPS parseado + estado interno
    │   └── websocket_manager.py       # ConnectionManager: set de WebSockets + broadcast thread-safe
    │
    ├── schemas/                       # DTOs de entrada e saída validados por Pydantic
    │   ├── telemetria.py              # RawTelemetria, TelemetriaProcessada, GPSProcessado, StatusGPS
    │   └── comando.py                 # ComandoDestino, ComandoResposta
    │
    ├── models/                        # Reservado para modelos MongoDB (roadmap Fase 3) — vazio hoje
    │
    └── utils/                         # Utilitários puros e testáveis
        ├── geo.py                     # haversine_metros() entre dois pontos geográficos
        └── sensores.py                # calcular_proa() e parse_nmea_gps() com índices validados em campo
```

### Por que essa arquitetura?

- **`api/` não conhece regra de negócio.** Controladores só fazem binding de request/response e invocam services — fáceis de ler e testar.
- **`services/` não conhece HTTP.** Podem ser reaproveitados por jobs, CLI ou testes unitários sem mocks complexos.
- **`schemas/` isolam a forma do dado que entra/sai** da camada de transporte, protegendo os modelos internos.
- **`models/` está pronta, mas vazia** — quando a Fase 3 (persistência MongoDB) chegar, basta criar `fato_telemetria.py`, `fato_missao.py` etc. sem tocar em mais nada.

---

## Tópicos MQTT

O backend é simultaneamente **subscriber** (telemetria do barco) e **publisher** (eco processado + comandos dos operadores):

| Tópico | Direção | Payload | Responsável |
|:---|:---:|:---|:---|
| `hdrop/raw` | ESP32 → Backend | `{"g":"<NMEA>","m":[x,y,z]}` | Firmware embarcado |
| `hdrop/telemetria` | Backend → Mundo | `{"proa":145.3,"gps":{"status":"FIXADO","lat":-15.87,"lng":-48.08}}` | `telemetria_service` |
| `hdrop/comando` | Frontend → Backend → ESP32 | `{"lat":-15.87,"lng":-48.08}` | `comando` route |

---

## Endpoints REST e WebSocket

### Health e Observabilidade

| Método | Rota | Descrição |
|:---|:---|:---|
| `GET` | `/` | Metadados da API e URL do WebSocket |
| `GET` | `/api/v1/health` | Healthcheck simples (status + versão) |
| `GET` | `/api/v1/status` | Estado do cliente MQTT + número de clientes WebSocket conectados |

### Comando

| Método | Rota | Descrição |
|:---|:---|:---|
| `POST` | `/api/v1/comando` | Publica um waypoint no tópico `hdrop/comando` (QoS 1) |
| `GET` | `/api/v1/comando/ultimo` | Retorna o último destino registrado |

### Telemetria

| Método | Rota | Descrição |
|:---|:---|:---|
| `GET` | `/api/v1/telemetria/ultima` | Última telemetria processada (snapshot) |
| `WS`  | `/api/v1/ws/telemetria` | Stream contínuo de telemetria processada (broadcast) |

Ao conectar no WebSocket, o cliente recebe imediatamente o último estado conhecido (se houver), evitando delay de até 1 segundo até a próxima mensagem do barco.

---

## Fluxo de Processamento

```
1. aiomqtt recebe mensagem em hdrop/raw
2. mqtt_service._tratar_raw()
3. → RawTelemetria.model_validate_json()  (Pydantic)
4. → telemetria_service.processar_raw()
       ├─ utils.sensores.parse_nmea_gps()    → (lat, lng, tem_fix)
       └─ utils.sensores.calcular_proa()     → graus 0°–360°
5. → TelemetriaProcessada (DTO de saída)
6. → mqtt_service.publicar('hdrop/telemetria')     (re-publica processado)
7. → websocket_manager.broadcast()                  (envia ao Angular)
```

---

## Roadmap

| Fase | Item | Status |
|:---:|:---|:---:|
| 1 | Hub MQTT → WebSocket com processamento de proa e GPS | ✓ Concluído |
| 1 | Endpoint REST para envio de waypoints | ✓ Concluído |
| 2 | Ingestão de SoC da bateria (INA226) | ○ Aguardando firmware |
| 2 | Ingestão de estado da célula de carga (HX711) | ○ Aguardando firmware |
| 3 | Persistência de telemetria em MongoDB (coleção time-series) | ○ Planejado |
| 3 | Endpoint de histórico de missões (`GET /api/v1/historico`) | ○ Planejado |
| 4 | Autenticação JWT e perfis de operador | ○ Planejado |

---

## Troubleshooting

| Problema | Causa Provável | Solução |
|:---|:---|:---|
| `MQTT erro: Connection refused` | Firewall bloqueando porta 1883 | Liberar `broker.hivemq.com:1883` ou trocar para TLS (8883) |
| `Publish ignorado — MQTT desconectado` | Broker ainda reconectando no startup | Aguardar 5s (reconexão automática) |
| `422 Unprocessable Entity` em `POST /comando` | Payload fora de `{lat, lng}` válidos | Validar range: lat ∈ [-90, 90], lng ∈ [-180, 180] |
| WebSocket fecha sozinho | CORS ou proxy reverso sem upgrade WS | Conferir `CORS_ORIGINS` e headers `Upgrade: websocket` |
| `ModuleNotFoundError: aiomqtt` | Venv não ativado ou `pip install` pulado | Ativar venv e rodar `pip install -r requirements.txt` |
| Proa sempre em 0° ou errada | Magnetômetro não calibrado | Recalibrar e atualizar `MAG_OFFSET_X/Y` no `.env` |

---

<div align="center">
  <br>
  &copy; 2026 Projeto H-DROP. Todos os direitos reservados.
  <br><br>
  <em>FastAPI Hub · aiomqtt · Real-Time Telemetry</em>
</div>
