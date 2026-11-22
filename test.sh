#!/usr/bin/env bash
# Batería completa de xroot-distro en Google Colab (/content).
# Uso: bash test.sh   (o: make test)
set -u
export PATH="/usr/local/bin:/root/.local/bin:$PATH"
LOG="$(dirname "$0")/test.log"
BIN="$(dirname "$0")/xroot-distro"
: > "$LOG"
PASS=0
FAIL=0

say() { printf '\n===== %s =====\n' "$*" | tee -a "$LOG"; }
run() { printf '\n$ %s\n' "$*" | tee -a "$LOG"; "$@" 2>&1 | tee -a "$LOG"; return "${PIPESTATUS[0]}"; }
check() {
  local desc="$1" rc="$2"
  if [ "$rc" -eq 0 ]; then printf 'PASS: %s\n' "$desc" | tee -a "$LOG"; PASS=$((PASS+1));
  else printf 'FAIL: %s (rc=%s)\n' "$desc" "$rc" | tee -a "$LOG"; FAIL=$((FAIL+1)); fi
}
expect_fail() {
  local desc="$1" rc="$2"
  if [ "$rc" -ne 0 ]; then printf 'PASS: %s (falló como se esperaba, rc=%s)\n' "$desc" "$rc" | tee -a "$LOG"; PASS=$((PASS+1));
  else printf 'FAIL: %s (debió fallar)\n' "$desc" | tee -a "$LOG"; FAIL=$((FAIL+1)); fi
}
contains() {
  local desc="$1" needle="$2" haystack="$3"
  if printf '%s' "$haystack" | grep -qF -- "$needle"; then printf 'PASS: %s\n' "$desc" | tee -a "$LOG"; PASS=$((PASS+1));
  else printf 'FAIL: %s (no contiene %q)\n' "$desc" "$needle" | tee -a "$LOG"; FAIL=$((FAIL+1)); fi
}

say '1. AYUDA Y FLAGS DE AYUDA'
out=$(run "$BIN" help); check 'help' $?
contains 'help muestra URL de ejemplo' 'alpine-minirootfs-3.24.2-x86_64.tar.gz' "$out"
out=$(run "$BIN" -h); check '-h' $?
out=$(run "$BIN" --help); check '--help' $?

say '2. LISTADO BASE'
out=$(run "$BIN" list); check 'list' $?
contains 'list muestra alpine' 'alpine' "$out"
out=$(run "$BIN" list --images); check 'list --images' $?

say '3. PRUEBA DE /tmp COMPARTIDO (Google Colab)'
PROBE=/tmp/xroot_colab_probe.txt
printf 'HOLA_DESDE_COLAB_TMP\n' > "$PROBE"
out=$(run "$BIN" login alpine --user root --shared-tmp -- /bin/sh -c "cat $PROBE"); check 'login --shared-tmp lee archivo del host' $?
contains 'contenido visible dentro de la distro' 'HOLA_DESDE_COLAB_TMP' "$out"
out=$(run "$BIN" login alpine --user root --shared-tmp -- /bin/sh -c "echo CREADO_DENTRO > /tmp/xroot_desde_distro.txt"); check 'login escribe en /tmp compartido' $?
if [ "$(cat /tmp/xroot_desde_distro.txt 2>/dev/null)" = 'CREADO_DENTRO' ]; then echo 'PASS: archivo creado dentro aparece en el host' | tee -a "$LOG"; PASS=$((PASS+1)); else echo 'FAIL: archivo creado dentro no aparece en el host' | tee -a "$LOG"; FAIL=$((FAIL+1)); fi
out=$(run "$BIN" login alpine --user root -- /bin/sh -c "cat $PROBE 2>&1 || echo NO_VISIBLE"); check 'login sin --shared-tmp' $?
contains 'sin --shared-tmp el archivo NO se ve' 'NO_VISIBLE' "$out"
printf 'SOLO_EN_CONTENT\n' > /content/xroot_content_probe.txt
out=$(run "$BIN" login alpine --user root --shared-tmp -- /bin/sh -c 'cat /content/xroot_content_probe.txt 2>&1 || echo CONTENT_NO_MONTADO'); check 'login con archivo de /content' $?
contains '/content no se comparte por diseño' 'CONTENT_NO_MONTADO' "$out"
rm -f /content/xroot_content_probe.txt

say '4. LOGIN: USUARIO, --user= Y UID NUMÉRICO'
out=$(run "$BIN" login alpine --user root -- /bin/sh -c 'echo USER_FLAG; id -u; cat /etc/alpine-release'); check 'login --user root' $?
contains 'uid=0' '0' "$out"
out=$(run "$BIN" login alpine --user=root -- /bin/sh -c 'echo USER_IGUAL_OK'); check 'login --user=root' $?
contains 'flag --user=' 'USER_IGUAL_OK' "$out"
out=$(run "$BIN" login alpine --user nobody -- /bin/sh -c 'echo USER_NOMBRE; id -u'); check 'login --user nobody' $?
out=$(run "$BIN" login alpine --user 65534 -- /bin/sh -c 'echo USER_UID; id -u'); check 'login --user UID numérico' $?

say '5. SIN MONTAJES RESIDUALES TRAS LOGIN'
for m in proc sys dev etc/resolv.conf tmp; do
  if mountpoint -q "/content/.xroot-distro/alpine/$m" 2>/dev/null; then
    echo "FAIL: mount residual en $m" | tee -a "$LOG"; FAIL=$((FAIL+1));
  else
    echo "PASS: $m limpio" | tee -a "$LOG"; PASS=$((PASS+1));
  fi
done
out=$(run "$BIN" login alpine --user root --shared-tmp -- /bin/sh -c 'mount | grep -c /content/.xroot-distro || echo 0')
contains 'sin referencias al rootfs dentro del chroot' '0' "$out"

say '6. INSTALL CON -n= Y -n (SEPARADO) DESDE file:// EN ROOT AISLADO'
ALTROOT=/tmp/xroot-colab-root
rm -rf "$ALTROOT"
# Reutilizamos un backup válido como rootfs para no depender de la red.
[ -f /tmp/alpine-test.tar.gz ] || run "$BIN" backup alpine /tmp/alpine-test.tar.gz
out=$(env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" install -n=desdeigual "file:///tmp/alpine-test.tar.gz" 2>&1 | tee -a "$LOG"); check 'install -n=<nombre> file://' $?
contains 'root alternativo usado' 'desdeigual' "$out"
out=$(env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" install -n desdeespacio "file:///tmp/alpine-test.tar.gz" 2>&1 | tee -a "$LOG"); check 'install -n <nombre> (separado)' $?
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" login desdeigual --user root -- /bin/sh -c 'ls -ld /tmp'); check 'login desdeigual (root alternativo)' $?
contains '/tmp con permisos 1777 tras install' 'drwxrwxrwt' "$out"
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" list); check 'list con XROOT_DISTRO_ROOT' $?
contains 'lista ambos contenedores' 'desdeespacio' "$out"
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" list --images); check 'list --images en root alternativo' $?

say '7. SHA256: VERIFICACIÓN CORRECTA Y RECHAZO'
out=$(run env XROOT_DISTRO_SHA256=0000000000000000000000000000000000000000000000000000000000000000 XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" install -n=shamalo "file:///tmp/alpine-test.tar.gz" 2>&1); rc=$?
expect_fail 'SHA256 incorrecto rechaza la instalación' "$rc"
contains 'mensaje de SHA no coincide' 'SHA256 no coincide' "$out"
if grep -q 'shamalo' <(env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" list); then
  echo 'FAIL: contenedor shamalo quedó instalado' | tee -a "$LOG"; FAIL=$((FAIL+1));
else
  echo 'PASS: no se instaló nada con SHA inválido' | tee -a "$LOG"; PASS=$((PASS+1));
fi

say '8. LOGIN EN ROOT ALTERNATIVO Y BIND DE /tmp'
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" login desdeigual --user root --shared-tmp -- /bin/sh -c "echo ROOT_ALT_OK; cat $PROBE; cat /etc/alpine-release"); check 'login en XROOT_DISTRO_ROOT' $?
contains 'root alternativo funciona' 'ROOT_ALT_OK' "$out"
contains 'tmp compartido en root alternativo' 'HOLA_DESDE_COLAB_TMP' "$out"

say '9. RENAME (contenedor del root alternativo)'
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" rename desdeespacio renombrado); check 'rename' $?
if grep -q '^name=renombrado$' "$ALTROOT/renombrado/.xroot-distro"; then
  echo 'PASS: marcador actualizado tras rename' | tee -a "$LOG"; PASS=$((PASS+1));
else
  echo 'FAIL: marcador no actualizado' | tee -a "$LOG"; FAIL=$((FAIL+1));
fi

say '10. BACKUP -> REMOVE -> RESTORE (contenedor del root alternativo)'
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" backup renombrado /tmp/xroot-renombrado.tar.gz); check 'backup' $?
if [ -s /tmp/xroot-renombrado.tar.gz ]; then
  echo 'PASS: backup no vacío' | tee -a "$LOG"
  PASS=$((PASS+1))
else
  echo 'FAIL: backup vacío' | tee -a "$LOG"
  FAIL=$((FAIL+1))
fi
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" remove renombrado); check 'remove contenedor' $?
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" restore recuperado /tmp/xroot-renombrado.tar.gz); check 'restore con otro nombre' $?
if grep -q '^name=recuperado$' "$ALTROOT/recuperado/.xroot-distro"; then
  echo 'PASS: restore renombra el marcador' | tee -a "$LOG"; PASS=$((PASS+1));
else
  echo 'FAIL: restore no renombró el marcador' | tee -a "$LOG"; FAIL=$((FAIL+1));
fi

say '11. RESET: DESTRUYE DATOS Y REINSTALA'
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" login desdeigual --user root -- /bin/sh -c 'echo effemero > /root/xroot_debora.txt'); check 'crear archivo dentro del contenedor' $?
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" login desdeigual --user root -- /bin/sh -c 'cat /root/xroot_debora.txt'); contains 'archivo existe antes del reset' 'effemero' "$out"
out=$(run env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" reset desdeigual); check 'reset' $?
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" login desdeigual --user root -- /bin/sh -c 'cat /root/xroot_debora.txt 2>&1 || echo BORRADO'); contains 'reset borra los datos' 'BORRADO' "$out"

say '12. REMOVE DE IMAGEN EN CACHÉ Y CLEAR-CACHE'
IMG=$(env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" list --images | awk 'NF {print $1; exit}')
if [ -n "$IMG" ]; then
  out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" remove "$IMG"); check 'remove imagen en caché' $?
else
  echo 'FAIL: no se encontró imagen en caché' | tee -a "$LOG"; FAIL=$((FAIL+1));
fi
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" clear-cache); check 'clear-cache' $?
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" list --images); contains 'caché vacía tras clear-cache' 'caché vacía' "$out"

say '13. ERRORES ESPERADOS (validación)'
out=$(run "$BIN" install 2>&1); expect_fail 'install sin argumentos' $?
out=$(run "$BIN" install -n=sinurl 2>&1); expect_fail 'install sin URL' $?
out=$(run "$BIN" install sinnombre https://ejemplo.invalid/x.tar.gz 2>&1); expect_fail 'install sin -n' $?
out=$(run "$BIN" login inexistente 2>&1); expect_fail 'login de contenedor inexistente' $?
out=$(run "$BIN" remove inexistente 2>&1); expect_fail 'remove de contenedor inexistente' $?
out=$(run "$BIN" rename noexiste otro 2>&1); expect_fail 'rename de contenedor inexistente' $?
out=$(run "$BIN" reset noexiste 2>&1); expect_fail 'reset de contenedor inexistente' $?
out=$(run "$BIN" backup noexiste 2>&1); expect_fail 'backup de contenedor inexistente' $?
out=$(run "$BIN" restore noexiste /tmp/noexiste.tar.gz 2>&1); expect_fail 'restore sin archivo' $?
out=$(run "$BIN" comando_invalido 2>&1); expect_fail 'comando desconocido' $?
out=$(run "$BIN" install -n='../malo' https://ejemplo.invalid/x.tar.gz 2>&1); expect_fail 'nombre con ../ rechazado' $?


say '15. FLAGS NUEVOS DE LOGIN'
out=$(run "$BIN" login alpine -u root:root -- /bin/sh -c 'echo USER_GRUPO; id -u; id -g'); check 'login -u name:group' $?
contains 'name:group resuelve' 'USER_GRUPO' "$out"
out=$(run "$BIN" login alpine --user 65534:65534 -- /bin/sh -c 'echo UID_GID; id -u; id -g'); check 'login --user uid:gid' $?
contains 'uid:gid resuelve' '65534' "$out"
out=$(run "$BIN" login alpine -u nobody:nogroup -- /bin/sh -c 'echo GRUPO_NOMBRE; id -g'); check 'login -u name:group con nombres' $?
contains 'grupo por nombre' '65533' "$out"
# shellcheck disable=SC2016  # $PWD y $PRUEBA se expanden DENTRO del rootfs
out=$(run "$BIN" login alpine -b /content:/mnt/host -w /tmp -e PRUEBA=xroot -- /bin/sh -c 'echo "PWD=$PWD ENV=$PRUEBA"; ls /mnt/host/README.md'); check 'login -b/-w/-e' $?
contains 'work-dir aplicado' 'PWD=/tmp' "$out"
contains 'env aplicada' 'ENV=xroot' "$out"
contains 'bind personalizado' '/mnt/host/README.md' "$out"
out=$(run "$BIN" login alpine --get-cmd); check 'login --get-cmd' $?
contains 'get-cmd imprime chroot' 'chroot ' "$out"
out=$(run "$BIN" login alpine --isolated --get-cmd); check 'login --isolated --get-cmd' $?
if printf '%s' "$out" | grep -q 'resolv.conf'; then
  echo 'FAIL: --isolated aún monta resolv.conf' | tee -a "$LOG"; FAIL=$((FAIL+1));
else
  echo 'PASS: --isolated omite resolv.conf' | tee -a "$LOG"; PASS=$((PASS+1));
fi
out=$(run "$BIN" login alpine --minimal --get-cmd); check 'login --minimal --get-cmd' $?
for m in proc sys dev etc/resolv.conf tmp; do
  if mountpoint -q "/content/.xroot-distro/alpine/$m" 2>/dev/null; then
    echo "FAIL: get-cmd dejó mount en $m" | tee -a "$LOG"
    FAIL=$((FAIL+1))
  else
    echo "PASS: get-cmd sin mounts en $m" | tee -a "$LOG"
    PASS=$((PASS+1))
  fi
done
out=$(run "$BIN" login bogus -u root:rootnosuch 2>&1); expect_fail 'usuario/grupo inexistente rechazado' $?
out=$(run "$BIN" login bogus -b ruta_sin_dos_puntos 2>&1); expect_fail 'bind sin SRC:DEST rechazado' $?

say '16. APK ADD + COWSAY DESDE pip y uv pip'
if [ -x /content/.xroot-distro/alpine/opt/cowsay-venv/bin/cowsay ]; then
  echo 'PASS: cowsay venv ya instalado' | tee -a "$LOG"; PASS=$((PASS+1));
else
  out=$(run "$BIN" login alpine --user root -- /bin/sh -c 'apk add --no-cache python3 py3-pip && python3 -m venv /opt/cowsay-venv && /opt/cowsay-venv/bin/pip install --no-cache-dir cowsay'); check 'apk add + pip install cowsay' $?
fi
out=$(run "$BIN" login alpine --user root -- /bin/sh -c '/opt/cowsay-venv/bin/cowsay -t "Hola desde xroot-distro en Google Colab"'); check 'cowsay vía pip' $?
contains 'cowsay dibuja al texto' 'Hola desde xroot-distro' "$out"
contains 'cowsay dibuja la vaca' '^__^' "$out"
if [ -x /content/.xroot-distro/alpine/opt/uv-venv/bin/cowsay ]; then
  echo 'PASS: cowsay uv venv ya instalado' | tee -a "$LOG"; PASS=$((PASS+1));
else
  out=$(run "$BIN" login alpine --user root -- /bin/sh -c 'apk add --no-cache uv && uv venv /opt/uv-venv && uv pip install --python /opt/uv-venv/bin/python cowsay'); check 'uv pip install cowsay' $?
fi
out=$(run "$BIN" login alpine --user root -- /bin/sh -c '/opt/uv-venv/bin/cowsay -t "uv pip también funciona"'); check 'cowsay vía uv pip' $?
contains 'uv pip cowsay dibuja' 'uv pip también funciona' "$out"

say '17. REPOSITORIOS APK Y VERSIÓN DE apk'
out=$(run "$BIN" login alpine --user root -- /bin/sh -c 'cat /etc/apk/repositories'); check 'cat /etc/apk/repositories' $?
contains 'repositorio main' 'https://dl-cdn.alpinelinux.org/alpine/v3.24/main' "$out"
contains 'repositorio community' 'https://dl-cdn.alpinelinux.org/alpine/v3.24/community' "$out"
out=$(run "$BIN" login alpine --user root -- /bin/sh -c 'apk --version'); check 'apk --version' $?
contains 'apk-tools presente' 'apk-tools' "$out"
contains 'apk compilado para x86_64' 'compiled for x86_64' "$out"
out=$(run "$BIN" login alpine --user root -- /bin/sh -c 'apk --print-arch'); check 'apk --print-arch' $?
contains 'arquitectura x86_64' 'x86_64' "$out"

say '18. BINARIO CORTO xd, install -h E IMÁGENES INTEGRADAS'
DIR="$(dirname "$0")"
[ -x "$DIR/xd" ]; check 'binario corto xd existe' $?
out=$(run "$DIR/xd" help); check 'xd help' $?
contains 'xd help usa el nombre xd' 'Uso: xd' "$out"
out=$(run "$BIN" install -h); check 'install -h' $?
contains 'install -h muestra alias' 'add, i, in, ins' "$out"
contains 'install -h muestra secciones' 'WHERE TO FIND IMAGES' "$out"
contains 'install -h lista imágenes integradas' 'BUILT-IN IMAGES' "$out"
XDBUILT=/tmp/xroot-xd-builtin-test
rm -rf "$XDBUILT"; mkdir -p "$XDBUILT/.cache"
cp /content/.xroot-distro/.cache/*alpine-minirootfs-3.24.2* /content/.xroot-distro/.cache/*ubuntu-base-24.04.5* "$XDBUILT/.cache/" 2>/dev/null || true
out=$(run env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$XDBUILT" "$DIR/xd" i alpine); check 'xd i alpine (URL integrada, sin -n)' $?
out=$(sha256sum "$XDBUILT/.cache/"*alpine-minirootfs-3.24.2* | cut -d' ' -f1); [ "$out" = c5ca053cfe1d85c5b96dff8b9bc57045f7f184a30ffb6b65776409ca90388677 ]; check 'SHA256 integrado de Alpine coincide' $?
out=$(run env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$XDBUILT" "$DIR/xd" add ubuntu:24.04); check 'xd add ubuntu:24.04 (URL integrada)' $?
out=$(sha256sum "$XDBUILT/.cache/"*ubuntu-base-24.04.5* | cut -d' ' -f1); [ "$out" = e77b6f10c2590cef872b33ee9f635a0e3fd1f57fb074c0e52b5c7f56147a0c86 ]; check 'SHA256 integrado de Ubuntu coincide' $?
out=$(run env XROOT_DISTRO_ROOT="$XDBUILT" "$DIR/xd" list); check 'list con imágenes integradas' $?
contains 'nombre automático alpine' 'alpine' "$out"
contains 'nombre automático ubuntu' 'ubuntu' "$out"
sed -i '/^arch=/d' "$XDBUILT/alpine/.xroot-distro"
out=$(run env XROOT_DISTRO_ROOT="$XDBUILT" "$DIR/xd" list); check 'list rellena arch en marker antiguo' $?
contains 'list detecta x86_64 en marker sin arch' 'arch: x86_64' "$out"
grep -q '^arch=x86_64$' "$XDBUILT/alpine/.xroot-distro"; check 'marker antiguo reparado' $?
out=$(run env XROOT_DISTRO_ROOT="$XDBUILT" "$DIR/xd" i distro-inexistente); expect_fail 'imagen desconocida falla' $?
contains 'error claro para imagen desconocida' 'imagen desconocida' "$out"
out=$(run env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$XDBUILT" "$DIR/xd" i alpine); check 'xd i alpine idempotente' $?
contains 'reinstalar no destruye' 'ya está instalado' "$out"
rm -rf "$XDBUILT"

say '19. FLAG -a/--architecture (SIN EMULACIÓN DE CPU)'
out=$(run "$DIR/xd" install -h); check 'install -h con -a' $?
contains 'install -h documenta -a' '-a, --architecture' "$out"
contains 'install -h documenta platform strings' 'linux/arm/v7' "$out"
XDARCH=/tmp/xroot-xd-arch-test
rm -rf "$XDARCH"; mkdir -p "$XDARCH/.cache"
cp /content/.xroot-distro/.cache/*alpine-minirootfs-3.24.2* "$XDARCH/.cache/" 2>/dev/null || true
out=$(run env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$XDARCH" "$DIR/xd" i -a linux/amd64 alpine); check 'xd i -a linux/amd64 alpine' $?
contains 'arquitectura detectada al instalar' 'arch=x86_64' "$out"
out=$(run env XROOT_DISTRO_ROOT="$XDARCH" "$DIR/xd" list); check 'list con arquitectura' $?
contains 'list muestra arch x86_64' 'arch: x86_64' "$out"
out=$(run env XROOT_DISTRO_ROOT="$XDARCH" "$DIR/xd" i -a mips64 -n=fail alpine); expect_fail 'arquitectura inválida falla' $?
contains 'error de arquitectura no soportada' 'arquitectura no soportada' "$out"
python3 - <<'PYARCH'
import io, tarfile
hdr = bytearray(20)
hdr[0:4] = b'\x7fELF'; hdr[4] = 2; hdr[5] = 1; hdr[6] = 1
hdr[16:18] = (2).to_bytes(2, 'little'); hdr[18:20] = (0xb7).to_bytes(2, 'little')
with tarfile.open('/tmp/xd-fake-aarch64.tar.gz', 'w:gz') as tf:
    info = tarfile.TarInfo('bin/sh'); info.size = len(hdr); info.mode = 0o755
    tf.addfile(info, io.BytesIO(bytes(hdr)))
PYARCH
check 'rootfs aarch64 sintético creado' $?
out=$(run env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$XDARCH" "$DIR/xd" i -a aarch64 -n=fake-arm file:///tmp/xd-fake-aarch64.tar.gz); check 'xd i -a aarch64 file:// (solo instala)' $?
contains 'rootfs aarch64 registrado' 'arch=aarch64' "$out"
out=$(run env XROOT_DISTRO_ROOT="$XDARCH" "$DIR/xd" login fake-arm --user root -- /bin/true); expect_fail 'login rechaza rootfs de otra arquitectura' $?
contains 'explica que chroot no emula CPU' 'no coincide con el host' "$out"
rm -rf "$XDARCH" /tmp/xd-fake-aarch64.tar.gz

say '20. INTÉRPRETES: -- <binario> y -- /ruta/absoluta'
out=$(run "$BIN" help); check 'help con formas de --' $?
contains 'help documenta -- <binario>' '-- <binario>' "$out"
contains 'help documenta -- /ruta/absoluta' '-- /ruta/absoluta' "$out"
contains 'help menciona fish' 'fish -c' "$out"
if [ -x /content/.xroot-distro/alpine/bin/bash ] && [ -x /content/.xroot-distro/alpine/usr/bin/fish ]; then
  echo 'PASS: bash y fish ya instalados en alpine' | tee -a "$LOG"; PASS=$((PASS+1));
else
  out=$(run "$BIN" login alpine --user root -- /bin/sh -c 'apk add --no-cache bash fish >/dev/null 2>&1'); check 'apk add bash fish en alpine' $?
fi
out=$(run "$BIN" login alpine --user root -- sh -c 'echo SH_PATH_OK'); check 'sh por PATH (nombre)' $?
contains 'sh por PATH responde' 'SH_PATH_OK' "$out"
out=$(run "$BIN" login alpine --user root -- /bin/sh -c 'echo SH_ABS_OK'); check '/bin/sh (ruta absoluta)' $?
contains '/bin/sh responde' 'SH_ABS_OK' "$out"
out=$(run "$BIN" login alpine --user root -- bash -c 'echo BASH_PATH_OK'); check 'bash por PATH (nombre)' $?
contains 'bash por PATH responde' 'BASH_PATH_OK' "$out"
out=$(run "$BIN" login alpine --user root -- /bin/bash -c 'echo BASH_ABS_OK'); check '/bin/bash (ruta absoluta)' $?
contains '/bin/bash responde' 'BASH_ABS_OK' "$out"
out=$(run "$BIN" login alpine --user root -- fish -c "echo FISH_PATH_OK; echo \$FISH_VERSION"); check 'fish por PATH (nombre)' $?
contains 'fish por PATH responde' 'FISH_PATH_OK' "$out"
out=$(run "$BIN" login alpine --user root -- /usr/bin/fish -c 'echo FISH_ABS_OK'); check '/usr/bin/fish (ruta absoluta)' $?
contains '/usr/bin/fish responde' 'FISH_ABS_OK' "$out"
out=$(run "$BIN" login alpine --user root -- python3 -c 'print("PY_PATH_OK")'); check 'python3 por PATH (nombre)' $?
contains 'python3 por PATH responde' 'PY_PATH_OK' "$out"
out=$(run "$BIN" login alpine --user root -- /usr/bin/python3 -c 'print("PY_ABS_OK")'); check '/usr/bin/python3 (ruta absoluta)' $?
contains '/usr/bin/python3 responde' 'PY_ABS_OK' "$out"
out=$(run "$BIN" login alpine --user root -- busybox sh -c 'echo BUSYBOX_OK'); check 'busybox con subcomando' $?
contains 'busybox responde' 'BUSYBOX_OK' "$out"
INTPROBE=/tmp/xroot_int_probe.txt
rm -f "$INTPROBE"
out=$(run "$BIN" login alpine --user root --shared-tmp -- sh -c "echo SHARED_PATH > $INTPROBE"); check 'sh por PATH escribe en /tmp compartido' $?
contains 'contenido escrito por sh visible en host' 'SHARED_PATH' "$(cat "$INTPROBE" 2>/dev/null)"
rm -f "$INTPROBE"
out=$(run "$BIN" login alpine --user root --shared-tmp -- /bin/sh -c "echo SHARED_ABS > $INTPROBE"); check '/bin/sh escribe en /tmp compartido' $?
contains 'contenido escrito por /bin/sh visible en host' 'SHARED_ABS' "$(cat "$INTPROBE" 2>/dev/null)"
rm -f "$INTPROBE"

say '21. FASTFETCH EN CONTENEDORES NUEVOS (ALPINE Y UBUNTU)'
FFROOT=/tmp/xroot-ff-test
FF_DEB=https://github.com/fastfetch-cli/fastfetch/releases/download/2.71.0/fastfetch-linux-amd64.deb
FF_SHA=6dc0b71c03e58f9f981a6b3c62b4fa3fb30b9d5ee9fa783302c91ea4c2a3600e
rm -rf "$FFROOT"; mkdir -p "$FFROOT/.cache"
cp /content/.xroot-distro/.cache/*alpine-minirootfs-3.24.2* "$FFROOT/.cache/" 2>/dev/null || true
cp /content/.xroot-distro/.cache/*ubuntu-base-24.04.5* "$FFROOT/.cache/" 2>/dev/null || true
out=$(run env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$FFROOT" "$DIR/xd" i -n=ff-test-alpine alpine); check 'contenedor Alpine nuevo para fastfetch' $?
out=$(sha256sum "$FFROOT/.cache/"*alpine-minirootfs-3.24.2* | cut -d' ' -f1); [ "$out" = c5ca053cfe1d85c5b96dff8b9bc57045f7f184a30ffb6b65776409ca90388677 ]; check 'SHA256 integrado del Alpine nuevo' $?
out=$(run env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$FFROOT" "$DIR/xd" i -n=ff-test-ubuntu ubuntu); check 'contenedor Ubuntu nuevo para fastfetch' $?
out=$(sha256sum "$FFROOT/.cache/"*ubuntu-base-24.04.5* | cut -d' ' -f1); [ "$out" = e77b6f10c2590cef872b33ee9f635a0e3fd1f57fb074c0e52b5c7f56147a0c86 ]; check 'SHA256 integrado del Ubuntu nuevo' $?
out=$(run env XROOT_DISTRO_ROOT="$FFROOT" "$DIR/xd" login ff-test-alpine --user root -- /bin/sh -c 'apk add --no-cache fastfetch >/dev/null 2>&1; echo apk_rc=$?'); check 'apk add fastfetch en Alpine nuevo' $?
contains 'apk de Alpine devuelve 0' 'apk_rc=0' "$out"
out=$(run env XROOT_DISTRO_ROOT="$FFROOT" "$DIR/xd" login ff-test-alpine --user root -- /usr/bin/fastfetch --version); check 'fastfetch --version en Alpine' $?
contains 'fastfetch version en Alpine' 'fastfetch 2.' "$out"
out=$(run env XROOT_DISTRO_ROOT="$FFROOT" "$DIR/xd" login ff-test-alpine --user root -- /usr/bin/fastfetch --logo none); check 'fastfetch en Alpine nuevo' $?
contains 'fastfetch detecta Alpine' 'Alpine Linux v3.24' "$out"
contains 'fastfetch detecta Kernel desde /proc' 'Kernel:' "$out"
out=$(run env XROOT_DISTRO_ROOT="$FFROOT" "$DIR/xd" login ff-test-ubuntu --user root -- /bin/sh -c 'apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq --no-install-recommends ca-certificates curl >/dev/null 2>&1; echo deps_rc=$?'); check 'dependencias curl en Ubuntu nuevo' $?
contains 'apt de Ubuntu devuelve 0' 'deps_rc=0' "$out"
out=$(run env XROOT_DISTRO_ROOT="$FFROOT" "$DIR/xd" login ff-test-ubuntu --user root -- /bin/sh -c "curl -fsSL -o /tmp/fastfetch.deb '$FF_DEB' && echo sha=\$(sha256sum /tmp/fastfetch.deb | cut -d' ' -f1) && dpkg -i /tmp/fastfetch.deb >/dev/null 2>&1 && echo dpkg_rc=\$?"); check 'descargar e instalar fastfetch .deb en Ubuntu' $?
contains 'SHA256 del .deb oficial' "$FF_SHA" "$out"
contains 'dpkg devuelve 0' 'dpkg_rc=0' "$out"
out=$(run env XROOT_DISTRO_ROOT="$FFROOT" "$DIR/xd" login ff-test-ubuntu --user root -- /usr/bin/fastfetch --version); check 'fastfetch --version en Ubuntu' $?
contains 'fastfetch version en Ubuntu' 'fastfetch 2.' "$out"
out=$(run env XROOT_DISTRO_ROOT="$FFROOT" "$DIR/xd" login ff-test-ubuntu --user root -- /usr/bin/fastfetch --logo none); check 'fastfetch en Ubuntu nuevo' $?
contains 'fastfetch detecta Ubuntu' 'Ubuntu 24.04.5 LTS' "$out"
contains 'fastfetch detecta Kernel desde /proc' 'Kernel:' "$out"
rm -rf "$FFROOT"

say '22. SOPORTE .tar.xz E IMAGEN INTEGRADA DE DEBIAN'
XZROOT=/tmp/xroot-xz-test
DEB_URL=https://doi-janky.infosiftr.net/job/tianon/job/debuerreotype/job/amd64/lastSuccessfulBuild/artifact/bookworm/slim/rootfs.tar.xz
DEB_SHA=bc02ac8bf3a0e42fd921f6c039a6095db8fceb893385ea31b8a9f1743bb5950d
rm -rf "$XZROOT"; mkdir -p "$XZROOT/.cache"
CACHED_XZ=$(DEB_URL="$DEB_URL" python3 - <<'PYXZ'
import hashlib, os
u = os.environ['DEB_URL']
print(hashlib.sha256(u.encode()).hexdigest()[:16] + '-rootfs.tar.xz')
PYXZ
)
if [ -f "/content/.xroot-distro/.cache/$CACHED_XZ" ]; then
  cp "/content/.xroot-distro/.cache/$CACHED_XZ" "$XZROOT/.cache/"
else
  out=$(run curl -fsSL --retry 2 --max-time 180 -o "$XZROOT/.cache/$CACHED_XZ" "$DEB_URL"); check 'descarga del rootfs Debian xz' $?
fi
out=$(sha256sum "$XZROOT/.cache/$CACHED_XZ" | cut -d' ' -f1); [ "$out" = "$DEB_SHA" ]; check 'SHA256 del rootfs Debian xz' $?
out=$(run env -u XROOT_DISTRO_SHA256 XROOT_DISTRO_ROOT="$XZROOT" "$DIR/xd" i -n=debian-xz-test debian); check 'xd i debian (tar.xz nativo)' $?
out=$(run env XROOT_DISTRO_ROOT="$XZROOT" "$DIR/xd" list); check 'list con Debian' $?
contains 'list detecta Debian' 'Debian GNU/Linux 12 (bookworm)' "$out"
contains 'list detecta arch x86_64' 'arch: x86_64' "$out"
out=$(run env XROOT_DISTRO_ROOT="$XZROOT" "$DIR/xd" login debian-xz-test --user root -- /bin/sh -c 'cat /etc/os-release'); check 'login en Debian desde xz' $?
contains 'login muestra Debian' 'Debian GNU/Linux 12 (bookworm)' "$out"
out=$(run "$DIR/xd" install -h); contains 'install -h documenta .tar.xz' '.tar.xz' "$out"
contains 'install -h lista debian:bookworm-slim' 'debian:bookworm-slim' "$out"
rm -rf "$XZROOT"

say '14. LIMPIEZA Y VERIFICACIÓN FINAL'
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" remove desdeigual); check 'remove desdeigual' $?
out=$(run env XROOT_DISTRO_ROOT="$ALTROOT" "$BIN" remove recuperado); check 'remove recuperado' $?
out=$(run "$BIN" list); contains 'alpine principal intacto' 'alpine' "$out"
out=$(run "$BIN" login alpine --user root --shared-tmp -- /bin/sh -c "echo FINAL_OK; cat $PROBE"); check 'login final' $?
contains 'probe de Colab sigue visible' 'HOLA_DESDE_COLAB_TMP' "$out"
rm -rf "$ALTROOT" "$PROBE" /tmp/xroot_desde_distro.txt /tmp/xroot-renombrado.tar.gz

printf '\n===== RESUMEN: PASS=%s FAIL=%s =====\n' "$PASS" "$FAIL" | tee -a "$LOG"
exit "$FAIL"
