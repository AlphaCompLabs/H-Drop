# Logotipo H-DROP

A marca (H com catamarã + texto "H-DROP") é única para o projeto — coloque as versões geradas aqui.

## Arquivos esperados (nomes exatos que o código referencia)

- **`logo-hdrop-dark.png`** — variante para o tema **Mission Dark** (fundo escuro).
  Traços do "H" em laranja vivo, texto "H-DROP" em **branco ou cinza claro**, fundo **transparente**.
- **`logo-hdrop-light.png`** — variante para o tema **Utility Light** (fundo claro).
  Traços do "H" em laranja, texto "H-DROP" em **preto/cinza escuro**, fundo **transparente**.

O header troca automaticamente entre as duas conforme o tema ativo (CSS em [mission-control.component.scss](../../app/features/mission-control/mission-control.component.scss)).

## Dimensões recomendadas

- Altura renderizada: 40 px (desktop), 30 px (mobile).
- Entregue em **pelo menos 2×** (80 px de altura) para displays retina. Largura proporcional.
- Formato preferido: **SVG** (escalável, nítido). Se for PNG, usar 3× (~120 px de altura) com fundo transparente.
- Se fornecer SVG, renomeie para `.svg` e atualize as duas linhas `<img src="...">` em [mission-control.component.ts](../../app/features/mission-control/mission-control.component.ts).

## (Opcional) Logo só-ícone

- `logo-hdrop-icon-dark.png` / `logo-hdrop-icon-light.png` — só o "H" sem o texto, para sidebar recolhida.

## (Opcional) Splash

- `splash.png` — tela de carregamento inicial (1920×1080). Pode ser o logo centralizado sobre um fundo Critical Ocean Blue.
