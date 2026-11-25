# JStarsReborn - compilacion automatica en Windows (issue #7: Ivy Bridge / Sandy Bridge).
#
# Que hace:
#   1. Comprueba Python 3.11+.
#   2. Descarga llvm-mingw 20261020 UCRT x86_64 (~182 MB, solo la primera vez).
#   3. Compila el runtime (tools/windows_build/build_runtime.py, servicios de host, sin GPU).
#   4. Ejecuta --selftest-host y --cpu-features.
#
# Uso: doble clic en COMPILAR_WINDOWS.bat, o en PowerShell:
#   powershell -ExecutionPolicy Bypass -File COMPILAR_WINDOWS.ps1
#
# Salida: C:\builds\jstars-isa-fix-001\jstars-boot-native.exe
# El .exe solo prueba servicios de host (--selftest-host). No es el juego completo:
# el runtime GPU/launcher se sigue compilando con el cross-build Linux documentado
# en docs/BUILD_RELEASE.md. Sirve para verificar que el fix compila y pasa los
# vectores ISA (MOVBE + BMI1) en tu PC.

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$LlvmVersion = '20261020'
$LlvmZipName = "llvm-mingw-$LlvmVersion-ucrt-x86_64.zip"
$LlvmUrl = "https://github.com/mstorsjo/llvm-mingw/releases/download/$LlvmVersion/$LlvmZipName"
$CacheDir = Join-Path $env:USERPROFILE 'builds\_toolchains'
$LlvmDir  = Join-Path $CacheDir "llvm-mingw-$LlvmVersion-ucrt-x86_64"
$LlvmBin  = Join-Path $LlvmDir 'bin'
$OutDir   = 'C:\builds\jstars-isa-fix-001'

Write-Host '== JStarsReborn: compilacion Windows (fix issue #7) ==' -ForegroundColor Cyan
Write-Host "Repo: $RepoRoot"

# 1. Python (se parsea `python --version` para evitar problemas de comillas en PowerShell)
try { $pyout = (& python --version 2>&1 | Out-String).Trim() } catch { throw 'No se encontro python en el PATH. Instala Python 3.11+ (marca "Add to PATH") y reintenta.' }
Write-Host "Python: $pyout"
if ($pyout -notmatch 'Python\s+(\d+)\.(\d+)') { throw "No se pudo leer la version de Python (salida: $pyout)." }
$pyMajor, $pyMinor = [int]$Matches[1], [int]$Matches[2]
if ($pyMajor -lt 3 -or ($pyMajor -eq 3 -and $pyMinor -lt 11)) { throw "Python $pyMajor.$pyMinor demasiado viejo; se necesita 3.11+." }

# 2. Toolchain
if (-not (Test-Path (Join-Path $LlvmBin 'clang.exe'))) {
    Write-Host "Descargando llvm-mingw $LlvmVersion (~182 MB, solo la primera vez)..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
    $zip = Join-Path $CacheDir $LlvmZipName
    if (-not (Test-Path $zip)) {
        Invoke-WebRequest -Uri $LlvmUrl -OutFile $zip -UseBasicParsing
    }
    Write-Host 'Extrayendo toolchain (tarda 1-2 min)...'
    if (Test-Path $LlvmDir) { Remove-Item -Recurse -Force $LlvmDir }
    Expand-Archive -Path $zip -DestinationPath $CacheDir -Force
} else {
    Write-Host "Toolchain OK: $LlvmBin"
}

if (-not (Test-Path (Join-Path $LlvmBin 'clang++.exe'))) { throw "--llvm-bin incompleto: falta clang++.exe en $LlvmBin" }

# 3. Compilar (el script exige un directorio de salida NUEVO o vacio, fuera del arbol fuente)
if (Test-Path $OutDir) {
    Write-Host "El directorio de salida ya existe: $OutDir" -ForegroundColor Yellow
    $ts = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutDir = "C:\builds\jstars-isa-fix-$ts"
    Write-Host "Usando en su lugar: $OutDir"
}
Write-Host "Compilando en $OutDir ..."
& python (Join-Path $RepoRoot 'tools\windows_build\build_runtime.py') `
    --llvm-bin $LlvmBin `
    --out-dir $OutDir
if ($LASTEXITCODE -ne 0) { throw "Fallo la compilacion. Mira $OutDir\compile.log y $OutDir\link.log" }

$exe = Join-Path $OutDir 'jstars-boot-native.exe'
Write-Host "Compilado OK: $exe" -ForegroundColor Green
Get-FileHash $exe -Algorithm SHA256 | Format-List

# 4. Self-test + CPU features
Write-Host '== --cpu-features (tu CPU) =='
& $exe --cpu-features
Write-Host '== --selftest-host =='
& $exe --selftest-host --log (Join-Path $OutDir 'selftest-host.log')
if ($LASTEXITCODE -ne 0) { throw "El selftest fallo. Mira selftest-host.log" }
Write-Host ''
Write-Host 'TODO OK. Binario y logs en:' -ForegroundColor Green
Write-Host "  $OutDir"
Write-Host 'Para el juego completo (launcher + runtime GPU) usa el cross-build Linux de docs/BUILD_RELEASE.md.'
