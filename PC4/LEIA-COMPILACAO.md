# Compilar o relatório PC4 (LaTeX local)

Ambiente montado para compilar `main.tex` localmente, sem Overleaf.

## Toolchain
- **MiKTeX** instalado em `C:\Program Files\MiKTeX` (pdflatex, biber, etc.).
- Auto-install de pacotes faltantes JÁ ligado (`AutoInstall=1`).
- `latexmk` NÃO é usado: requer Perl (ausente). Build é via pdflatex+biber direto.

## Como compilar

### Opção A — script (recomendado)
Dentro da pasta `PC4/`, no PowerShell:
```
.\build.ps1
```
Roda pdflatex → biber → pdflatex ×2 e gera `main.pdf`. Avisa se houver imagem ausente.

### Opção B — VSCode
Extensão **LaTeX Workshop**. A recipe já está em `.vscode/settings.json`
(pdflatex → biber → pdflatex ×2, sem latexmk). Abre `main.tex` → Ctrl+S ou
"Build LaTeX project".

### Opção C — manual
```
pdflatex -interaction=nonstopmode main.tex
biber main
pdflatex -interaction=nonstopmode main.tex
pdflatex -interaction=nonstopmode main.tex
```

## Estrutura
- `main.tex` — relatório (fonte único)
- `Imagens e LIB/` — todas as imagens + `referencias.bib`
- `.latexmkrc`, `.vscode/`, `.gitignore` — config de build

> O `main.tex` usa `\graphicspath{{Imagens e LIB/}{./}}`, então as imagens
> ficam na subpasta. Para adicionar imagem nova: jogue o arquivo em
> `Imagens e LIB/` com o nome exato referenciado no `.tex`.

## Imagens ainda AUSENTES (referenciadas mas não existem)
Enquanto não forem adicionadas, aparecem como caixa **[IMAGEM AUSENTE]** no PDF
(a compilação NÃO quebra). Coloque-as em `Imagens e LIB/` com estes nomes:

| Arquivo esperado | Onde aparece (figura) |
|---|---|
| `topologia-conexoes-2.png` | Topologia do barramento de potência |
| `topologia-escs.png` | Detalhe das conexões dos ESCs |
| `tela-app.jpeg` | Interface web servida pela ESP32 |
| `print-rota-a-b.png` | Mapa tático rota A→B |
| `print-painel-log.png` | Log de eventos da FSM |
| `carga.jpeg` | Bancada HX711 + célula de carga |
| `led.jpeg` | Fita LED / MOSFET |

## Notas
- `.bib`: entradas `silva2019` e `bohnjunior2023` corrigidas de `@monography`
  (inválido no biblatex) para `@thesis`.
- Caminho do projeto tem espaços/acento — MiKTeX lida bem.
