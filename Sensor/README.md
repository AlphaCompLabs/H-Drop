
# HDrop – Sensor Ultrassônico com Filtragem  
### Arduino + AJSR04M (HC-SR04 compatível) + Potenciômetro de Threshold

---

## O que este código faz

Este firmware implementa uma **cadeia completa de processamento de sinal** para um sensor ultrassônico, transformando a leitura bruta de distância em uma saída robusta, filtrada e normalizada. O código roda a **20 Hz** (`Ts = 50 ms`), ajustando a frequência de amostragem para uma análise mais estável em tempo real, com várias camadas de filtragem para eliminar ruídos, saltos e leituras espúrias.

O resultado é publicado via **Serial** no formato do protocolo **Teleplot**, permitindo visualização em tempo real de todas as etapas do processamento simultaneamente.

---

## Pipeline de Processamento (8 etapas por ciclo)

Cada ciclo de 50 ms executa as seguintes etapas em ordem:

### Etapa 1 — Leitura do Potenciômetro (Threshold)
- Lê o valor analógico do pino `A1` (0–1023) e normaliza para `[0, 1]`
- Aplica filtro exponencial com `α = 0.85` para suavizar variações manuais do potenciômetro
- O valor resultante (`ref`) define dinamicamente o **limiar de detecção de obstáculo**

### Etapa 2 — Leitura Bruta do Sensor
- Dispara pulso de **15 µs** no pino `TRIG`
- Mede a duração do ECHO com `pulseIn()` e timeout de **50 ms**
- Converte duração para centímetros: `dist = (duração × 0.0343) / 2`
- Rejeita leituras fora da faixa válida: **20 cm a 300 cm**
- Mantém contador de falhas consecutivas (máximo: 5 antes de marcar sensor como inoperante)

### Etapa 3 — Rejeição de Saltos
- Compara a nova leitura com a última válida
- Se o salto for **≤ 50 cm** → aceita a leitura diretamente
- Se o salto for **> 50 cm** (ruído/eco falso) → mistura ponderada: `70% antigo + 30% novo`
- Isso evita que reflexos espúrios causem picos abruptos na saída

### Etapa 4 — Média Móvel
- Mantém uma **janela circular de 6 amostras**
- Calcula a média aritmética das amostras disponíveis
- Suaviza variações ciclo a ciclo sem introduzir atraso excessivo

### Etapa 5 — Filtro Exponencial de Saída
- Aplica filtro de primeira ordem com `α = 0.75` sobre a média móvel
- Fórmula: `y(t) = 0.75 × y(t-1) + 0.25 × média(t)`
- Produz a saída mais suave (`dist_filtrada`), usada na lógica de alarme

### Etapa 6 — Normalização
- As três versões da distância são normalizadas para o intervalo `[0, 1]`:

``` id="ag15qk"
y = (dist_cm - 20) / (300 - 20)
```

- Usa `constrain()` para garantir que nunca saia de `[0, 1]`
- **Quanto maior o valor, mais longe está o obstáculo**

### Etapa 7 — Lógica de Alarme com Histerese
- Compara `y_filtrada` com o threshold `ref` vindo do potenciômetro
- **Ativação:** alarme só dispara após **2 leituras consecutivas** abaixo do threshold
- **Desativação:** alarme só apaga após **3 leituras consecutivas** acima do threshold
- Isso evita *bouncing* (alarme piscando por ruído na fronteira do threshold)

### Etapa 8 — Atuadores e Log
- **LED** (pino 7): aceso quando sensor está OK, apagado em caso de falhas
- **Buzzer** (pino 5): ligado quando alarme está ativo
- Envia log completo via Serial no formato Teleplot

---

## Formato da Saída Serial (Teleplot)

### Protocolo
Cada linha começa com `>` seguido de pares `chave:valor` separados por vírgula:

``` id="uhnvzt"
>ref:0.450,y_bruta:0.234,y_media:0.241,y_filtrada:0.238,alarme:1,sensor_ok:1
```

### Campos da saída

| Campo | Tipo | Range | Descrição |
|---|---|---|---|
| `ref` | float | 0.000 – 1.000 | Threshold normalizado do potenciômetro |
| `y_bruta` | float | 0.000 – 1.000 | Distância após rejeição de saltos, normalizada |
| `y_media` | float | 0.000 – 1.000 | Distância após média móvel de 6 amostras, normalizada |
| `y_filtrada` | float | 0.000 – 1.000 | Distância após filtro exponencial, normalizada |
| `alarme` | binário | 0 ou 1 | `1` = obstáculo confirmado abaixo do threshold |
| `sensor_ok` | binário | 0 ou 1 | `1` = sensor funcionando, `0` = falhas excessivas |

> **Para reconstruir a distância em centímetros:** `dist_cm = y × 280 + 20`

### Exemplos de leitura

**Obstáculo a ~50 cm (threshold em 60%):**
``` id="6sfqra"
>ref:0.600,y_bruta:0.138,y_media:0.141,y_filtrada:0.143,alarme:1,sensor_ok:1
```

**Caminho livre (nenhum obstáculo no alcance):**
``` id="3o7jsf"
>ref:0.600,y_bruta:0.945,y_media:0.943,y_filtrada:0.940,alarme:0,sensor_ok:1
```

**Sensor com falha (cabo desconectado ou timeout):**
``` id="dt64zh"
>ref:0.600,y_bruta:0.000,y_media:0.310,y_filtrada:0.512,alarme:0,sensor_ok:0
```

---

## Hardware Necessário

| Componente | Pino Arduino |
|---|---|
| Sensor AJSR04M — TRIG | Pino 8 |
| Sensor AJSR04M — ECHO | Pino 9 |
| Potenciômetro (threshold) | A1 |
| LED vermelho + resistor (330Ω) | Pino 7 |
| Buzzer | Pino 5 |

> O sensor AJSR04M opera com **3.3 V ou 5 V** e é compatível com o protocolo do HC-SR04.

---

## Parâmetros Configuráveis

Todas as constantes estão no topo do arquivo e podem ser ajustadas sem mexer na lógica:

| Constante | Valor padrão | O que controla |
|---|---|---|
| `Ts` | `0.05f` (50 ms) | Período de amostragem (frequência: 1/Ts Hz) |
| `ALPHA` | `0.75f` | Suavidade do filtro exponencial de saída (maior = mais lento) |
| `ALPHA_REF` | `0.85f` | Suavidade do filtro do potenciômetro |
| `N_MEDIA` | `6` | Tamanho da janela de média móvel |
| `SALTO_MAX_CM` | `50.0f` | Máximo salto aceito antes de aplicar mistura ponderada |
| `PULSE_TIMEOUT_US` | `50000UL` | Timeout do `pulseIn` em microssegundos |
| `FALHAS_MAX` | `5` | Falhas consecutivas antes de marcar sensor como inoperante |
| `CONFIRMACOES_ALARME` | `2` | Leituras abaixo do threshold para **ativar** alarme |
| `CONFIRMACOES_LIVRE` | `3` | Leituras acima do threshold para **desativar** alarme |
| `DIST_MIN_VALIDA` | `20.0f` cm | Distância mínima aceita pelo sensor |
| `DIST_MAX_VALIDA` | `300.0f` cm | Distância máxima aceita pelo sensor |

---

## Passo a Passo: Como Usar

### 1. Montar o circuito
- Conecte o sensor AJSR04M nos pinos 8 (TRIG) e 9 (ECHO)
- Conecte o potenciômetro no pino analógico A1 (terminais nos extremos em 5V e GND, cursor no A1)
- Conecte o LED com resistor de 330Ω no pino 7
- Conecte o buzzer no pino 5
- Alimente o sensor com 5V e GND do Arduino

### 2. Carregar o código
- Abra o arquivo `.ino` no **Arduino IDE**
- Selecione a placa correta (ex: **Arduino Uno** ou **Arduino Nano**)
- Selecione a porta COM
- Clique em **Upload**

### 3. Visualizar com Teleplot
- Instale a extensão **Teleplot** no [VSCode](https://marketplace.visualstudio.com/items?itemName=alexnesnes.teleplot) ou use o [cliente web](https://teleplot.fr/)
- Abra a Serial a **115200 baud** apontando para a porta do Arduino
- As 4 curvas (`ref`, `y_bruta`, `y_media`, `y_filtrada`) e os dois indicadores binários (`alarme`, `sensor_ok`) aparecem automaticamente em tempo real

### 4. Calibrar o threshold
- Gire o potenciômetro até o campo `ref` na saída Serial estar na distância desejada (normalizada)
- Exemplo: para alarmar em 50 cm → `ref` deve ser `(50 - 20) / 280 ≈ 0.107`
- O LED acende confirmando que o sensor está operacional
- Quando um objeto entrar abaixo do threshold, o buzzer dispara após 2 confirmações

### 5. Integrar com o HDrop
- No firmware do catamarã (HDrop v2.0), a saída `y_filtrada` equivale a `leitura_ultrassom` (em metros)
- Para integrar: leia a Serial do Arduino no ESP32 e converta: `dist_m = y_filtrada × 2.8 + 0.1`

---