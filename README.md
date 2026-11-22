<div align="center">

# xroot-distro

**Contenedores `chroot` ligeros en C — alternativa mínima a `proot-distro`, sin QEMU ni imágenes OCI.**

[![Release](https://img.shields.io/github/v/release/itskreisler/xroot-distro?style=flat-square)](https://github.com/itskreisler/xroot-distro/releases)
[![CI / release](https://github.com/itskreisler/xroot-distro/actions/workflows/release.yml/badge.svg)](https://github.com/itskreisler/xroot-distro/actions/workflows/release.yml)
[![Checks](https://img.shields.io/badge/tests-200%20passing-brightgreen?style=flat-square)](test.sh)
[![Stars](https://img.shields.io/github/stars/itskreisler/xroot-distro?style=flat-square)](https://github.com/itskreisler/xroot-distro/stargazers)

[Inicio rápido](#-inicio-rápido) · [Características](#-características) · [Imágenes](#-imágenes) · [Uso](#-uso) · [Tests](#-tests) · [Releases](#-releases)

</div>

---

## 💡 Concepto

> Un rootfs es solo un directorio. Si ya eres root y la arquitectura coincide, no necesitas proot, QEMU ni un demonio: basta con `mount --bind` y `chroot`.

`xroot-distro` gestiona rootfs de Alpine, Ubuntu y Debian como contenedores ligeros. Todo en un binario C de ~65 KB, sin dependencias de ejecución más que las librerías del sistema.

> [!WARNING]
> Requiere **root** y que la arquitectura del rootfs sea **la misma del host** (`x86_64`). No hay emulación de CPU.

## ✨ Características

| Característica | Detalle |
|:---|:---|
| **Binario único** | `xroot-distro` y su alias corto `xd`, construidos desde la misma fuente C. |
| **Sin QEMU** | `chroot` nativo; detecta y rechaza rootfs de otra arquitectura. |
| **Descarga segura** | libcurl + SHA256 con OpenSSL antes de extraer; caché reutilizable. |
| **Formatos** | `.tar`, `.tar.gz` (zlib) y `.tar.xz` (liblzma) sin invocar `tar`. |
| **Imágenes integradas** | Alpine 3.24, Ubuntu 24.04 y Debian 12/13 (`slim` y `full`), con hashes fijados. |
| **Aislamiento opcional** | `--isolated`, `-e KEY=VALUE`, `-w DIR` y `/tmp` compartido bajo demanda. |
| **Ciclo de vida completo** | `install`, `list`, `login`, `rename`, `reset`, `backup`, `restore`, `remove`. |
| **Batería de pruebas** | **200 checks** que cubren mounts, flags, SHA, backup/restore e intérpretes. |

## 🚀 Inicio rápido

Instalación por defecto en `~/.local/bin` (no necesita `sudo`):

```sh
curl -fsSL https://raw.githubusercontent.com/itskreisler/xroot-distro/main/install.sh | sh
```

El instalador descarga el último release, verifica `SHA256SUMS` e instala ambos binarios. Añade `~/.local/bin` a tu `PATH` si aún no está:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Crea y entra en un contenedor Debian slim:

```sh
xd i -n debian debian
xd login debian --user root -- /bin/sh -c 'cat /etc/os-release'
```

> [!TIP]
> ¿Sin rootfs local? El binario descarga la imagen integrada y valida su SHA256 automáticamente.

<details>
<summary><b>Instalar en el binario del sistema</b> (<code>/usr/local/bin</code>)</summary>

```sh
curl -fsSL https://raw.githubusercontent.com/itskreisler/xroot-distro/main/install.sh | sudo sh -s -- --system
```

El flag `--system` (o `-s`) sustituye el destino por `/usr/local/bin` y exige root. También puedes elegir otro prefijo con `-p/--prefix`, un tag concreto con `-v/--version` o una arquitectura con `-a/--arch`.

</details>

## 📦 Instalación

| Método | Comando | Destino |
|:---|:---|:---|
| Script ( usuario ) | `curl … \| sh` | `~/.local/bin` |
| Script ( sistema ) | `curl … \| sudo sh -s -- --system` | `/usr/local/bin` |
| Script ( prefijo ) | `sh install.sh -p /opt/xroot` | `/opt/xroot/bin` |
| Desde fuente | `make binaries && sudo make tool` | `/usr/local/bin` |

Dependencias para compilar: `gcc`, `make`, `libcurl4-openssl-dev`, `zlib1g-dev`, `liblzma-dev`, `libssl-dev`. Para lint: `clang-format`, `clang-tidy` y `shellcheck`.

## 🧩 Imágenes

| Short name | Rootfs | Tamaño |
|:---|:---|:---|
| `alpine`, `alpine:3.24.2` | Alpine minirootfs 3.24.2 | ~3,6 MB |
| `ubuntu`, `ubuntu:24.04.5` | Ubuntu Base 24.04.5 | ~29 MB |
| `debian`, `debian:bookworm-slim` | Debian 12 slim (`.tar.xz`) | ~17 MB |
| `debian:bookworm-full` | Debian 12 completa | ~31 MB |
| `debian:trixie-slim` | Debian 13 slim | ~19 MB |

Cualquier otra URL `http(s)://`, `file://` o ruta local funciona con `install`, siempre que sea un tarball de la arquitectura del host.

## 🛠️ Uso

```bash
xd install -n alpine https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64/alpine-minirootfs-3.24.2-x86_64.tar.gz
xd i -n debian debian
xd list
xd login alpine --user root --shared-tmp -- /bin/sh
xd login alpine --isolated -e FOO=bar -w /tmp -- /bin/sh
xd backup alpine alpine-backup.tar.gz
xd rename alpine viejo
xd reset alpine
xd remove viejo
xd clear-cache
```

Ejemplo con `cowsay` desde PyPI (no disponible en `apk` de Alpine 3.24):

```bash
xd login alpine --user root -- /bin/sh -c \
  'python3 -m venv /opt/cowsay-venv && /opt/cowsay-venv/bin/pip install cowsay && /opt/cowsay-venv/bin/cowsay -t "Hola"'
```

## 🧪 Tests

```bash
make binaries        # compila xroot-distro y xd
make lint            # clang-format + clang-tidy
shellcheck install.sh test.sh
sudo make test       # batería completa (requiere root): PASS=200 FAIL=0
```

La batería prepara rootfs aislados en `/tmp` y `/content`, verifica los SHA integrados, prueba binds, `backup`/`restore`/`reset`, intérpretes (`sh`, `bash`, `fish`, `python3`, `busybox`) y la instalación nativa de Debian desde `.tar.xz`.

## 🏗️ Releases

Hay **un único workflow** (`.github/workflows/release.yml`) que prepara las herramientas, ejecuta lint y tests, verifica los hashes en **cada commit** y publica el release. Se dispara con cada push a `main`, con tags `v*` o manualmente desde **Actions**:

```bash
git commit -m "fix: something"
git push                      # verifica hash + tests, sin publicar
git tag v0.1.0 && git push origin v0.1.0   # publica ese tag
```

Reglas del pipeline:

| Regla | Comportamiento |
|:---|:---|
| **Hash en cada commit** | Tras los tests se genera `dist/`, se valida `sha256sum -c SHA256SUMS` y se instala con `file://` para comprobar la verificación del instalador. |
| **`[skip release]`** | Si el commit (o los commits desde el tag anterior) contienen `[skip release]`, se omiten artefactos y publicación, pero se siguen ejecutando build, lint, tests y hash. |
| **Versión automática** | Al publicar desde **Actions** sin `version`, se toma el último release y se incrementa el PATCH (`v0.1.0` → `v0.1.1`), se crea el tag y se publica. Con tag push se usa el tag; con `version` se respeta el valor dado. |

Artefactos publicados (el instalador **no** se publica, se sirve desde el repo):

```text
dist/xroot-distro-linux-x86_64.tar.gz   # xroot-distro + xd
dist/xroot-distro-src-v0.1.0.tar.gz     # fuentes
dist/SHA256SUMS                         # los dos tarballs
```

`install.sh` usa por defecto `https://github.com/itskreisler/xroot-distro/releases/latest/download`, así que siempre instala el último release sin consultar la API de GitHub. Con `-v v0.1.0` se fija un tag concreto.

Para generarlos localmente y probar el instalador contra `dist/`:

```bash
make release REPO=owner/repo VERSION=v0.1.0
BASE_URL="file://$PWD/dist" sh install.sh -p /tmp/xroot-install
```

También puedes disparar el workflow manualmente desde la pestaña **Actions** (`workflow_dispatch`).

## 📁 Estructura

```text
xroot-distro/
├── .github/workflows/release.yml  # único pipeline: tools + test + release
├── .clang-tidy                    # análisis estático
├── Makefile                       # targets build/test/lint/release/...
├── README.md
├── install.sh                     # instalador POSIX (curl | sh)
├── test.sh                        # 200 checks
├── xroot-distro.c                 # descarga, SHA, tar/tar.gz/tar.xz, chroot
├── xroot-distro                   # binario compilado
└── xd                             # alias corto
```

## 🤝 Contribuir

1. Haz fork y crea una rama (`feature/nombre`).
2. Ejecuta `make binaries && make lint && sudo make test`.
3. Abre un PR describiendo el cambio y sus pruebas.

---

La bitácora completa y los apéndices con el contenido literal están en [`/content/README.md`](https://github.com/itskreisler/xroot-distro).
