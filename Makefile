# Equivalente mínimo de proot-distro: binario C basado en chroot (requiere root, sin QEMU).
# Uso: make build && make list && make login
# Argumentos extra de login: make login ARGS='--isolated -e FOO=bar -- /bin/sh'

NAME     ?= alpine
NEWNAME  ?= alpine2
ARCHIVE  ?= alpine-backup.tar.gz
ARGS     ?=
VERSION  ?= dev
REPO     ?= __REPO__
ARCH     ?= x86_64
DIST     := dist
ASSET    := xroot-distro-linux-$(ARCH).tar.gz

URL      ?= https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64/alpine-minirootfs-3.24.2-x86_64.tar.gz
SHA256   ?= c5ca053cfe1d85c5b96dff8b9bc57045f7f184a30ffb6b65776409ca90388677

BIN      := xroot-distro
ALIAS    := xd
SRC      := xroot-distro.c
PREFIX   ?= /usr/local
CC       ?= gcc
WINDOWS_CC ?= x86_64-w64-mingw32-gcc
CFLAGS   ?= -O2 -Wall -Wextra
LDLIBS   := -lcurl -lz -llzma -lcrypto

export XROOT_DISTRO_SHA256 := $(SHA256)

.PHONY: all binaries windows-launchers build list images login remove rename reset backup restore clear-cache tool clean help lint test release

all: binaries

binaries: $(BIN) $(ALIAS) ## Compila ambos binarios (sin instalar rootfs)

windows-launchers: ## Compila los lanzadores Windows para WSL2 (requiere MinGW-w64)
	$(WINDOWS_CC) -municode -static -O2 -Wall -Wextra -o xroot-distro.exe windows-launcher.c
	cp xroot-distro.exe xd.exe

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(ALIAS): $(SRC)
	$(CC) $(CFLAGS) -DPROGRAM_NAME='"xd"' -o $@ $< $(LDLIBS)

build: $(BIN) $(ALIAS) ## Compila el binario e instala el rootfs indicado
	./$(BIN) install -n=$(NAME) $(URL)

list: $(BIN) ## Lista contenedores
	./$(BIN) list

images: $(BIN) ## Lista descargas en caché
	./$(BIN) list --images

login: $(BIN) ## Entra como root con /tmp compartido (ARGS= permite añadir flags)
	./$(BIN) login $(NAME) --user root --shared-tmp $(ARGS)

remove: $(BIN) ## Borra contenedor o imagen en caché (¡destruye datos!)
	./$(BIN) remove $(NAME)

rename: $(BIN) ## Renombra el contenedor NAME a NEWNAME
	./$(BIN) rename $(NAME) $(NEWNAME)

reset: $(BIN) ## Reinstala desde su URL original (¡destruye datos!)
	./$(BIN) reset $(NAME)

backup: $(BIN) ## Guarda el contenedor como TAR.gz
	./$(BIN) backup $(NAME) $(ARCHIVE)

restore: $(BIN) ## Restaura desde un TAR.gz (¡destruye datos!)
	./$(BIN) restore $(NAME) $(ARCHIVE)

clear-cache: $(BIN) ## Borra descargas en caché
	./$(BIN) clear-cache

tool: $(BIN) $(ALIAS) ## Instala ambos binarios en $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)
	install -m 755 $(ALIAS) $(PREFIX)/bin/$(ALIAS)

clean: ## Borra solo el binario compilado
	rm -f $(BIN) $(ALIAS) xroot-distro.exe xd.exe

help: $(BIN) ## Muestra la ayuda
	@./$(BIN) help

lint: $(BIN) ## Formato y análisis estático (requiere clang-format/clang-tidy en PATH)
	clang-format --dry-run --Werror $(SRC)
	clang-tidy $(SRC) -- -I/usr/include/x86_64-linux-gnu

test: $(BIN) $(ALIAS) ## Batería completa de pruebas (requiere root)
	bash test.sh

release: binaries ## Prepara artefactos para GitHub Releases en dist/
	@if [ "$(REPO)" = "__REPO__" ]; then \
		echo 'ERROR: define REPO=owner/repo (o pasalo como variable de entorno)'; \
		exit 1; \
	fi
	rm -rf $(DIST)
	mkdir -p $(DIST)/pkg $(DIST)/src
	cp $(BIN) $(ALIAS) $(DIST)/pkg/
	tar -czf $(DIST)/$(ASSET) -C $(DIST)/pkg $(BIN) $(ALIAS)
	cp Makefile xroot-distro.c windows-launcher.c install-wsl.ps1 test.sh README.md .clang-tidy $(DIST)/src/
	tar -czf $(DIST)/xroot-distro-src-$(VERSION).tar.gz -C $(DIST)/src .
	cd $(DIST) && sha256sum $(ASSET) xroot-distro-src-$(VERSION).tar.gz > SHA256SUMS
	rm -rf $(DIST)/pkg $(DIST)/src
	@echo 'Release preparada en $(DIST)/'
