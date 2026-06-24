# build.ps1 - compila main.tex -> main.pdf sem precisar de Perl/latexmk
# Uso (na pasta PC4):  .\build.ps1
# Sequencia: pdflatex -> biber -> pdflatex -> pdflatex

$ErrorActionPreference = "Stop"
$env:Path = "C:\Program Files\MiKTeX\miktex\bin\x64;" + $env:Path
Set-Location -Path $PSScriptRoot

Write-Host "[1/4] pdflatex (1a passada)..." -ForegroundColor Cyan
pdflatex -interaction=nonstopmode -file-line-error main.tex | Out-Null

Write-Host "[2/4] biber (bibliografia)..." -ForegroundColor Cyan
biber main | Out-Null

Write-Host "[3/4] pdflatex (2a passada)..." -ForegroundColor Cyan
pdflatex -interaction=nonstopmode -file-line-error main.tex | Out-Null

Write-Host "[4/4] pdflatex (3a passada)..." -ForegroundColor Cyan
pdflatex -interaction=nonstopmode -file-line-error main.tex | Out-Null

if (Test-Path main.pdf) {
    $kb = (Get-Item main.pdf).Length / 1KB
    Write-Host ("OK -> main.pdf ({0:N0} KB)" -f $kb) -ForegroundColor Green
    # avisa sobre imagens ausentes, se houver
    $miss = Select-String -Path main.log -Pattern "IMAGEM AUSENTE" -ErrorAction SilentlyContinue
    if ($miss) { Write-Host "Atencao: ha imagens [IMAGEM AUSENTE] no PDF (ver LEIA-COMPILACAO.md)." -ForegroundColor Yellow }
} else {
    Write-Host "FALHOU: main.pdf nao gerado. Veja main.log." -ForegroundColor Red
    exit 1
}
