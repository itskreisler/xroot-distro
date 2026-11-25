param(
    [string]$Distro
)

$ErrorActionPreference = 'Stop'
$package = Join-Path $PSScriptRoot 'xroot-distro-linux-x86_64.tar.gz'
$checksums = Join-Path $PSScriptRoot 'SHA256SUMS'
if (-not (Test-Path -LiteralPath $package)) {
    throw "No se encuentra el binario Linux del paquete: $package"
}
if (-not (Test-Path -LiteralPath $checksums)) {
    throw "No se encuentra SHA256SUMS en el paquete: $checksums"
}

$checksumLine = Get-Content -LiteralPath $checksums |
    Where-Object { $_ -match '^[0-9a-fA-F]{64}\s+\*?xroot-distro-linux-x86_64\.tar\.gz$' } |
    Select-Object -First 1
if (-not $checksumLine) {
    throw 'SHA256SUMS no contiene el hash del binario Linux.'
}
$expectedHash = ($checksumLine -split '\s+')[0].ToLowerInvariant()
$actualHash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $expectedHash) {
    throw 'El SHA256 del binario Linux no coincide con SHA256SUMS.'
}

$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    throw 'WSL2 no está instalado. Instálalo con: wsl --install'
}

$distroArgs = @()
if ($Distro) {
    $distroArgs = @('--distribution', $Distro)
}

$linuxPackage = (& wsl.exe @distroArgs --exec wslpath -a $package | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or -not $linuxPackage) {
    throw 'No se pudo acceder al paquete desde WSL. Comprueba que la distro esté instalada.'
}

function Quote-ShellArgument([string]$Value) {
    $escapedQuote = "'" + '"' + "'" + '"' + "'"
    return "'" + $Value.Replace("'", $escapedQuote) + "'"
}

$quotedPackage = Quote-ShellArgument $linuxPackage
$installCommand = @"
set -eu
grep -qi 'microsoft.*WSL2' /proc/version || {
  echo 'Se requiere una distribución ejecutándose en WSL2.' >&2
  exit 1
}
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y libcurl4-openssl-dev libssl-dev zlib1g-dev liblzma-dev
tmp=`$(mktemp -d)
trap 'rm -rf "`$tmp"' EXIT
tar -xzf $quotedPackage -C "`$tmp"
install -m 755 "`$tmp/xroot-distro" /usr/local/bin/xroot-distro
install -m 755 "`$tmp/xd" /usr/local/bin/xd
/usr/local/bin/xd help >/dev/null
"@
$installCommand = $installCommand.Replace("`r`n", "`n")

& wsl.exe @distroArgs --user root --exec sh -lc $installCommand
if ($LASTEXITCODE -ne 0) {
    throw 'Falló la instalación del binario Linux en WSL.'
}

$installDir = Join-Path $env:LOCALAPPDATA 'xroot-distro\bin'
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'xroot-distro.exe') -Destination $installDir
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'xd.exe') -Destination $installDir

if ($Distro) {
    [Environment]::SetEnvironmentVariable('XROOT_DISTRO_WSL_DISTRO', $Distro, 'User')
    $env:XROOT_DISTRO_WSL_DISTRO = $Distro
} else {
    [Environment]::SetEnvironmentVariable('XROOT_DISTRO_WSL_DISTRO', $null, 'User')
    Remove-Item Env:XROOT_DISTRO_WSL_DISTRO -ErrorAction SilentlyContinue
}

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$pathEntries = @($userPath -split ';' | Where-Object { $_ })
if ($pathEntries -notcontains $installDir) {
    [Environment]::SetEnvironmentVariable(
        'Path',
        (($pathEntries + $installDir) -join ';'),
        'User'
    )
}
$env:Path = "$installDir;$env:Path"

Write-Host 'Instalación completada. Abre una terminal nueva y ejecuta: xd help'
