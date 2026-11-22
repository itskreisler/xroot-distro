#!/bin/sh
# Instalador POSIX de xroot-distro desde GitHub Releases.
# Funciona con: curl -fsSL <raw>/install.sh | sh
# El instalador se sirve desde el repo; los releases solo publican los tarballs.
set -eu

REPO="__REPO__"
VERSION="${VERSION:-}"
ARCH="${ARCH:-}"
PREFIX="${PREFIX:-}"
BASE_URL="${BASE_URL:-}"
SYSTEM=0

die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Uso: sh install.sh [OPCIONES] [VERSION]

Instala xroot-distro y xd desde un release de GitHub y verifica el SHA256
del tarball contra el SHA256SUMS publicado.

Opciones:
  -v, --version VERSION   Tag del release (por defecto: el último publicado)
  -a, --arch ARCH         x86_64 | aarch64 | armv7 | i686 | riscv64
                          (por defecto: detectado con uname -m)
  -p, --prefix PREFIX     Prefijo de instalacion (por defecto: ~/.local)
  -s, --system            Instalar en /usr/local/bin (requiere root)
  -r, --repo OWNER/REPO   Repositorio GitHub (por defecto: el de este repo)
  -h, --help              Muestra esta ayuda

Variables de entorno: VERSION, ARCH, PREFIX, REPO, BASE_URL.
BASE_URL permite usar un espejo o un directorio local (file://...) en pruebas.

Ejemplos:
  curl -fsSL https://raw.githubusercontent.com/OWNER/REPO/main/install.sh | sh
  curl -fsSL .../install.sh | sudo sh -s -- --system
  sh install.sh v0.1.0
  BASE_URL=file:///tmp/dist sh install.sh -p /tmp/xroot-install
EOF
}

positional_version=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -v|--version)
      [ "$#" -ge 2 ] || die "$1 requiere VERSION"
      VERSION="$2"
      shift 2
      ;;
    -a|--arch)
      [ "$#" -ge 2 ] || die "$1 requiere ARCH"
      ARCH="$2"
      shift 2
      ;;
    -p|--prefix)
      [ "$#" -ge 2 ] || die "$1 requiere PREFIX"
      PREFIX="$2"
      shift 2
      ;;
    -s|--system)
      SYSTEM=1
      shift
      ;;
    -r|--repo)
      [ "$#" -ge 2 ] || die "$1 requiere OWNER/REPO"
      REPO="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      die "opcion desconocida: $1"
      ;;
    *)
      [ -z "$positional_version" ] || die "version duplicada: $1"
      positional_version="$1"
      shift
      ;;
  esac
done
if [ -n "$positional_version" ]; then
  [ -z "$VERSION" ] || die "version duplicada (usa solo -v o el argumento posicional)"
  VERSION="$positional_version"
fi
if [ "$#" -gt 0 ]; then
  die "argumentos inesperados: $*"
fi

# El marcador __REPO__ se resuelve al repositorio real o a GITHUB_REPOSITORY.
if [ "$REPO" = "__REPO__" ]; then
  REPO="${GITHUB_REPOSITORY:-itskreisler/xroot-distro}"
fi

case "$REPO" in
  */*) ;;
  *) die "REPO debe tener forma owner/repo: $REPO" ;;
esac

if [ "$SYSTEM" -eq 1 ]; then
  PREFIX="/usr/local"
  [ "$(id -u)" -eq 0 ] || die "--system requiere root (usa sudo sh -s -- --system)"
fi
if [ -z "$PREFIX" ]; then
  : "${HOME:?HOME no definido; usa -p/--prefix}"
  PREFIX="$HOME/.local"
fi

if [ -z "$ARCH" ]; then
  case "$(uname -m)" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    armv7*) ARCH=armv7 ;;
    i386|i486|i586|i686) ARCH=i686 ;;
    riscv64) ARCH=riscv64 ;;
    *) die "arquitectura no soportada: $(uname -m) (usa -a/--arch)" ;;
  esac
fi

# Por defecto se usa siempre el ultimo release de GitHub. Con -v/--version
# se fija un tag concreto; BASE_URL sigue permitiendo espejos o file://.
if [ -z "$BASE_URL" ]; then
  if [ -n "$VERSION" ] && [ "$VERSION" != dev ]; then
    BASE_URL="https://github.com/${REPO}/releases/download/${VERSION}"
  else
    BASE_URL="https://github.com/${REPO}/releases/latest/download"
  fi
fi
BASE_URL="${BASE_URL%/}"
ASSET="xroot-distro-linux-${ARCH}.tar.gz"

for tool in curl tar grep sha256sum install mktemp id uname; do
  command -v "$tool" >/dev/null 2>&1 || die "falta la herramienta: $tool"
done

fetch_url=""
fetch_out=""
fetch() {
  fetch_url="$1"
  fetch_out="$2"
  printf 'descargando %s\n' "$fetch_url" >&2
  curl -fsSL --retry 3 --connect-timeout 15 -o "$fetch_out" "$fetch_url" \
    || die "no se pudo descargar $fetch_url"
}

tmp="$(mktemp -d)" || die "no se pudo crear el directorio temporal"
trap 'rm -rf "$tmp"' 0
trap 'exit 130' 2
trap 'exit 143' 15

fetch "$BASE_URL/$ASSET" "$tmp/$ASSET"
fetch "$BASE_URL/SHA256SUMS" "$tmp/SHA256SUMS"
grep -q "  ${ASSET}\$" "$tmp/SHA256SUMS" \
  || die "$ASSET no aparece en SHA256SUMS"
(cd "$tmp" && grep "  ${ASSET}\$" SHA256SUMS | sha256sum -c -) \
  || die "SHA256 no coincide con el release"

mkdir -p "$tmp/pkg"
tar -xzf "$tmp/$ASSET" -C "$tmp/pkg"
[ -x "$tmp/pkg/xroot-distro" ] || die "el tarball no contiene xroot-distro"
[ -x "$tmp/pkg/xd" ] || die "el tarball no contiene xd"

mkdir -p "$PREFIX/bin"
install -m 755 "$tmp/pkg/xroot-distro" "$PREFIX/bin/xroot-distro"
install -m 755 "$tmp/pkg/xd" "$PREFIX/bin/xd"
"$PREFIX/bin/xd" help >/dev/null || die "el binario instalado no responde"

printf 'Instalado: %s/bin/xroot-distro y %s/bin/xd\n' "$PREFIX" "$PREFIX"
printf 'Prueba: %s/bin/xd help\n' "$PREFIX"
