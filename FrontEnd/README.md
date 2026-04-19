<div align="center">

# H-DROP — Frontend

### Estação de Controle de Missão (Mission Control) do ASV H-DROP

<br>

<p>
  <img src="https://img.shields.io/badge/Angular-19+-DD0031?style=for-the-badge&logo=angular&logoColor=white" alt="Angular">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/TypeScript-5.6+-3178C6?style=for-the-badge&logo=typescript&logoColor=white" alt="TypeScript">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Standalone-Signals_%2B_RxJS-B7178C?style=for-the-badge&logo=reactivex&logoColor=white" alt="RxJS">
  <br><br>
  <img src="https://img.shields.io/badge/Map-Leaflet_1.9+-199900?style=for-the-badge&logo=leaflet&logoColor=white" alt="Leaflet">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Icons-Iconify_%2B_Phosphor-1C78C0?style=for-the-badge&logo=iconify&logoColor=white" alt="Iconify">
  &nbsp;&nbsp;
  <img src="https://img.shields.io/badge/Protocol-WebSocket-00d4ff?style=for-the-badge&logo=socketdotio&logoColor=white" alt="WebSocket">
</p>

</div>

---

## Sobre

SPA (Single Page Application) construída em **Angular 19 standalone** que consome o backend H-DROP via **WebSocket** (telemetria em streaming) e **REST** (comandos de missão). A interface é desenhada como uma **Estação de Controle de Missão tática** — assertiva, clean e priorizada para leitura rápida em situações operacionais críticas, com abordagem **mobile-first** para operação em campo.

O dashboard oferece um mapa Leaflet como centro da UI (com suavização de GPS, rotação de proa em tempo real, 3 camadas cartográficas e waypoints por clique), uma bússola digital, cartões de telemetria, barra de status agregada dos 6 subsistemas monitorados pelo PC1, sistema de alertas categorizado e uma aba dedicada a testes de sincronização end-to-end.

---

## Stack Tecnológico

| Componente | Tecnologia | Versão |
|:---|:---|:---|
| Framework | Angular (standalone) | 19+ |
| Linguagem | TypeScript | 5.6+ |
| Mapas | Leaflet | 1.9.4 |
| Ícones | Iconify + Phosphor Icons | 2.1 (CDN) |
| Reatividade | Angular Signals + RxJS | 7.8+ |
| HTTP Client | Angular HttpClient | — |
| WebSocket | API nativa do browser | — |
| Controle de Fluxo | `@if` / `@for` / `@empty` | — |
| Bootstrap | `bootstrapApplication` (sem NgModules) | — |

---

## Instalação e Execução

### 1. Pré-requisitos

- **Node.js 20+** e npm
- **Angular CLI** (opcional, para usar `ng`): `npm install -g @angular/cli`
- Backend H-DROP rodando em `http://localhost:8000` (veja [BackEnd/README.md](../BackEnd/README.md))

### 2. Instalar Dependências

```bash
cd FrontEnd
npm install
```

### 3. Desenvolvimento

```bash
npm start
# equivalente a: ng serve --open
```

Aplicação disponível em **`http://localhost:4200`** — hot reload ativado a cada salvamento.

### 4. Build de Produção

```bash
npm run build
# equivalente a: ng build
```

Artefatos otimizados gerados em `dist/hdrop/browser/`.

---

## Arquitetura de Pastas

```
src/
│
├── index.html                              # HTML raiz + tag <script> do iconify-icon via CDN
├── main.ts                                 # Bootstrap standalone (sem NgModules)
├── styles.scss                             # Estilos globais + tokens CSS (paleta PC1 e temas)
│
├── environments/
│   ├── environment.ts                      # Dev: apiBaseUrl e wsUrl apontam para localhost:8000
│   └── environment.prod.ts                 # Prod: paths relativos ao host (mesma origem do backend)
│
├── assets/
│   ├── favicon/                            # favicon.ico e variantes PWA (user-provided)
│   └── images/                             # logo-hdrop-dark.png e logo-hdrop-light.png
│
└── app/
    │
    ├── app.component.ts                    # Componente raiz: hospeda <router-outlet>
    ├── app.config.ts                       # Providers globais: Router, HttpClient, Zone.js
    ├── app.routes.ts                       # Rotas com lazy loading via loadComponent()
    │
    ├── core/                               # Infraestrutura central — sem UI
    │   ├── models/
    │   │   ├── telemetria.model.ts         # Telemetria, GPSProcessado, StatusGPS
    │   │   ├── comando.model.ts            # ComandoDestino, ComandoRapido, ComandoResposta
    │   │   ├── alerta.model.ts             # Alerta, Severidade (critical | warning | info | ok)
    │   │   └── status.model.ts             # StatusSistema, EstadoConexao
    │   │
    │   └── services/
    │       ├── telemetria.service.ts       # Cliente WebSocket: auto-reconexão + média móvel de 5 pontos
    │       ├── comando.service.ts          # POST /comando e GET /comando/ultimo via HttpClient
    │       ├── theme.service.ts            # Alterna Mission Dark / Utility Light (signal + localStorage)
    │       ├── alerts.service.ts           # Emissão e fila de alertas (signals)
    │       └── geo.util.ts                 # haversineMetros() e formatarDistancia() no cliente
    │
    ├── shared/
    │   └── components/                     # Componentes reutilizáveis entre features
    │       ├── compass/                    # Bússola SVG animada — rotaciona com a proa
    │       ├── telemetry-card/             # Card genérico de telemetria (label + valor + tone)
    │       ├── status-bar/                 # Barra inferior com 6 indicadores (WS, MQTT, GNSS, BAT, 4G, CARGA)
    │       ├── alert-overlay/              # Modal bloqueante para critical + stack de toasts
    │       └── theme-toggle/               # Botão sol/lua com Phosphor icons
    │
    └── features/                           # Módulos funcionais (lazy loaded)
        ├── mission-control/                # Orquestrador: header, grid responsivo, abas mobile
        ├── map/                            # Leaflet + divIcon do barco (ph:navigation-arrow-fill rotativo)
        ├── sidebar/                        # Painel esquerdo: bússola + cards GPS + destino atual
        └── sync-test/                      # Painel direito: botões START/ACK/HOME/STOP + log de envios
```

---

## Filosofia de Design — Mission Control

A UI segue os princípios estabelecidos no Relatório PC1 para operação de missões críticas:

- **Mobile-first rigoroso:** layout se adapta de 3 colunas (desktop) → 2 colunas (tablet) → abas (mobile <780px).
- **Ruído visual minimizado:** apenas o essencial está sempre visível; logs e configurações ficam em gavetas.
- **Hierarquia tática:** o mapa ocupa o centro, telemetria fica à esquerda, comandos à direita.
- **Feedback imediato:** todo comando gera um alerta, todo erro crítico exige confirmação explícita.
- **Leitura diurna e noturna:** dois temas (Mission Dark e Utility Light) com contraste calibrado.

### Paleta PC1 (CSS Custom Properties)

| Token CSS | Cor | Uso |
|:---|:---:|:---|
| `--c-accent` | `#00d4ff` (dark) / `#0072c6` (light) | Acento principal, proa, links |
| `--c-life-green` | `#00d66b` | Status OK, bateria cheia, missão concluída |
| `--c-safety-yellow` | `#ffc233` | Avisos, sync pendente, destino |
| `--c-strategic-red` | `#ff3b3b` | Erros críticos, parada de emergência |

Troca de tema via `ThemeService.alternar()` — persiste em `localStorage` e aplica `data-theme` no `<body>` para cascata CSS instantânea, sem recompilação.

---

## Sistema de Ícones

**Não existem arquivos SVG manuais no projeto.** Ícones são servidos pelo web component oficial do **Iconify** (carregado via CDN no `index.html`), usando o conjunto **Phosphor Icons** (prefixo `ph:`).

```html
<iconify-icon icon="ph:battery-medium-bold"></iconify-icon>
<iconify-icon icon="ph:navigation-arrow-fill"></iconify-icon>
```

- **Tamanho** segue `font-size` do elemento.
- **Cor** segue `currentColor` (herda do CSS do tema).
- **Rotação** (ex.: proa do barco no mapa) via `transform: rotate(Xdeg)`.
- **Catálogo completo:** https://phosphoricons.com

Componentes que usam `<iconify-icon>` declaram `schemas: [CUSTOM_ELEMENTS_SCHEMA]` para o Angular aceitar o custom element.

---

## Fluxo Reativo

```
WebSocket (/ws/telemetria)
        │
        ▼
TelemetriaService
  ├─ telemetria$ (BehaviorSubject) ───────▶ Sidebar (bússola, cards GPS)
  ├─ posicaoSuavizada$ (Subject)  ────────▶ Map (marker do barco)
  └─ estado (Signal<EstadoConexao>) ──────▶ Header (indicador ONLINE/OFFLINE)

Map.click()
        │
        ▼
ComandoService.enviarDestino() ─── POST /api/v1/comando ─── Backend ─── MQTT ─── ESP32
```

O **buffer de suavização (média móvel de 5 pontos)** fica no frontend e elimina o jitter de ±2–5m do GPS quando o barco está parado. O buffer é invalidado ao reconectar.

---

## Rotas

Aplicação single-page com uma rota principal (sem necessidade de autenticação nesta fase):

| Rota | Componente | Acesso |
|:---|:---|:---|
| `/` | `MissionControlComponent` (lazy) | Público |
| `/**` | Redirect para `/` | — |

Usa `withHashLocation()` no `provideRouter` — URLs ficam `#/` evitando problemas de deploy em hosts estáticos sem fallback para index.html.

---

## Convenções de Desenvolvimento

- **Standalone components** — todos os componentes usam `standalone: true`, sem NgModules.
- **ChangeDetectionStrategy.OnPush** — padrão em todos os componentes para performance.
- **Signals** para estado local sincronizado, **RxJS** para streams externos (WS, HTTP).
- **Control Flow moderno** — `@if`, `@for`, `@empty` no template (não usar `*ngIf`/`*ngFor`).
- **`input.required<T>()`** e **`output<T>()`** — API nova de inputs/outputs.
- **`inject()`** em vez de constructor injection quando possível.
- **SCSS com CSS Custom Properties** — sem variáveis SCSS para cores (tokens de tema mandam).
- **`iconify-icon` via CDN** — não instalar `@iconify/angular` ou `iconify-icon` como dep npm (evita problemas de pre-bundling do Vite).

---

## Troubleshooting

| Problema | Causa Provável | Solução |
|:---|:---|:---|
| `WebSocket connection failed` | Backend não está rodando | Iniciar `python main.py` em `BackEnd/` |
| Mapa aparece cinza / sem tiles | Sem internet ou bloqueio de CDN do CartoDB | Conferir rede; trocar para camada `street` ou `satellite` no canto superior direito |
| Ícones `<iconify-icon>` não aparecem | Script CDN bloqueado por proxy corporativo | Verificar `https://code.iconify.design` acessível, ou instalar `iconify-icon` como dep local |
| Logo não aparece no header | Arquivos ausentes em `assets/images/` | Colocar `logo-hdrop-dark.png` e `logo-hdrop-light.png` em `src/assets/images/` |
| Failed to resolve dependency (Vite) | Cache antigo do esbuild/Vite | `rm -rf node_modules package-lock.json .angular/cache && npm install` |
| Proa do barco não rotaciona | Backend enviou proa mas o effect não disparou | Conferir se a telemetria `proa` chega no WS (DevTools → Network → WS) |
| Toggle de tema não persiste | `localStorage` bloqueado (modo privado) | Normal — tema volta ao padrão Mission Dark a cada sessão |

---

<div align="center">
  <br>
  &copy; 2026 Projeto H-DROP. Todos os direitos reservados.
  <br><br>
  <em>Mission Control · Real-Time Dashboard · Mobile-First Tactical UI</em>
</div>
