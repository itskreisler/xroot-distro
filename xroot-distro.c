/*
 * xroot-distro — equivalente mínimo de proot-distro basado en chroot.
 * Requiere root y que la arquitectura del rootfs coincida con el host (sin
 * QEMU).
 *
 * Variables: XROOT_DISTRO_ROOT, XROOT_DISTRO_SHA256
 * Compilar: gcc -O2 -Wall -Wextra -o xroot-distro xroot-distro.c -lcurl -lz
 * -lcrypto
 */
#define _GNU_SOURCE
/* Todos los PATH_MAX se rellenan con snprintf (sin desbordamiento). */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <lzma.h>
#include <openssl/evp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define DEFAULT_ROOT "/content/.xroot-distro"
#define MARKER ".xroot-distro"
#define EXAMPLE_URL                                                            \
  "https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64/"               \
  "alpine-minirootfs-3.24.2-x86_64.tar.gz"
#define EXAMPLE_SHA                                                            \
  "c5ca053cfe1d85c5b96dff8b9bc57045f7f184a30ffb6b65776409ca90388677"
#define UBUNTU_URL                                                             \
  "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/"             \
  "ubuntu-base-24.04.5-base-amd64.tar.gz"
#define UBUNTU_SHA                                                             \
  "e77b6f10c2590cef872b33ee9f635a0e3fd1f57fb074c0e52b5c7f56147a0c86"
#define DEBIAN_ARTIFACTS                                                       \
  "https://doi-janky.infosiftr.net/job/tianon/job/debuerreotype/job/amd64/"    \
  "lastSuccessfulBuild/artifact/"
#define DEBIAN_URL DEBIAN_ARTIFACTS "bookworm/slim/rootfs.tar.xz"
#define DEBIAN_SHA                                                             \
  "bc02ac8bf3a0e42fd921f6c039a6095db8fceb893385ea31b8a9f1743bb5950d"
#define DEBIAN_FULL_URL DEBIAN_ARTIFACTS "bookworm/rootfs.tar.xz"
#define DEBIAN_FULL_SHA                                                        \
  "699df719f36b854f17010e1fb9df8b9ebc1943547f2f9a9c416f65fa63583135"
#define DEBIAN_TRIXIE_URL DEBIAN_ARTIFACTS "trixie/slim/rootfs.tar.xz"
#define DEBIAN_TRIXIE_SHA                                                      \
  "2a7cfbcffec723c0951077672930fdab95b472a02b9d17b983f4cfb3376badfd"

typedef struct {
  const char *key;
  const char *url;
  const char *sha;
  const char *name;
  const char *family;
} BuiltinImage;

/* Solo imágenes verificadas en esta bitácora (SHA fijado al build actual). */
static const BuiltinImage BUILTIN_IMAGES[] = {
    {"alpine", EXAMPLE_URL, EXAMPLE_SHA, "alpine", "alpine"},
    {"alpine:latest", EXAMPLE_URL, EXAMPLE_SHA, "alpine", "alpine"},
    {"alpine:3.24.2", EXAMPLE_URL, EXAMPLE_SHA, "alpine", "alpine"},
    {"ubuntu", UBUNTU_URL, UBUNTU_SHA, "ubuntu", "ubuntu"},
    {"ubuntu:latest", UBUNTU_URL, UBUNTU_SHA, "ubuntu", "ubuntu"},
    {"ubuntu:24.04", UBUNTU_URL, UBUNTU_SHA, "ubuntu", "ubuntu"},
    {"ubuntu:24.04.5", UBUNTU_URL, UBUNTU_SHA, "ubuntu", "ubuntu"},
    {"debian", DEBIAN_URL, DEBIAN_SHA, "debian", "debian"},
    {"debian:latest", DEBIAN_URL, DEBIAN_SHA, "debian", "debian"},
    {"debian:slim", DEBIAN_URL, DEBIAN_SHA, "debian", "debian"},
    {"debian:12", DEBIAN_URL, DEBIAN_SHA, "debian", "debian"},
    {"debian:12-slim", DEBIAN_URL, DEBIAN_SHA, "debian", "debian"},
    {"debian:bookworm", DEBIAN_URL, DEBIAN_SHA, "debian", "debian"},
    {"debian:bookworm-slim", DEBIAN_URL, DEBIAN_SHA, "debian", "debian"},
    {"debian:bookworm-full", DEBIAN_FULL_URL, DEBIAN_FULL_SHA, "debian",
     "debian"},
    {"debian:trixie", DEBIAN_TRIXIE_URL, DEBIAN_TRIXIE_SHA, "debian", "debian"},
    {"debian:trixie-slim", DEBIAN_TRIXIE_URL, DEBIAN_TRIXIE_SHA, "debian",
     "debian"},
};

static const BuiltinImage *find_builtin(const char *spec) {
  for (size_t i = 0; i < sizeof BUILTIN_IMAGES / sizeof BUILTIN_IMAGES[0]; i++)
    if (!strcmp(spec, BUILTIN_IMAGES[i].key))
      return &BUILTIN_IMAGES[i];
  return NULL;
}

static void die(const char *fmt, ...);
static void info(const char *fmt, ...);

/* ---------- arquitectura: solo chroot, sin emulación de CPU ---------- */
typedef struct {
  const char *alias;
  const char *arch;
} ArchAlias;

static const ArchAlias ARCH_ALIASES[] = {
    {"x86_64", "x86_64"},
    {"amd64", "x86_64"},
    {"linux/amd64", "x86_64"},
    {"aarch64", "aarch64"},
    {"arm64", "aarch64"},
    {"linux/arm64", "aarch64"},
    {"arm", "arm"},
    {"armv7", "arm"},
    {"armv7l", "arm"},
    {"linux/arm/v7", "arm"},
    {"i686", "i686"},
    {"i386", "i686"},
    {"x86", "i686"},
    {"linux/386", "i686"},
    {"riscv64", "riscv64"},
    {"linux/riscv64", "riscv64"},
};

static const char *normalize_arch(const char *spec) {
  for (size_t i = 0; i < sizeof ARCH_ALIASES / sizeof ARCH_ALIASES[0]; i++)
    if (!strcasecmp(spec, ARCH_ALIASES[i].alias))
      return ARCH_ALIASES[i].arch;
  die("arquitectura no soportada: %s (usa aarch64, arm, i686, riscv64, "
      "x86_64 o su equivalente docker)",
      spec);
  return NULL;
}

static const char *host_arch(void) {
  struct utsname u;
  if (uname(&u))
    die("no se pudo detectar la arquitectura del host: %s", strerror(errno));
  return normalize_arch(u.machine);
}

static const char *arch_for_distro(const char *arch, const char *family) {
  int alpine = !strcmp(family, "alpine");
  if (!strcmp(arch, "x86_64"))
    return alpine ? "x86_64" : "amd64";
  if (!strcmp(arch, "aarch64"))
    return alpine ? "aarch64" : "arm64";
  if (!strcmp(arch, "arm"))
    return alpine ? "armv7" : "armhf";
  if (!strcmp(arch, "i686"))
    return "i386";
  return arch; /* riscv64 */
}

static const char *elf_arch_of(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  unsigned char h[20];
  size_t n = fread(h, 1, sizeof h, f);
  fclose(f);
  if (n < 20 || h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F')
    return NULL;
  uint16_t machine = (uint16_t)((unsigned)h[18] | ((unsigned)h[19] << 8U));
  switch (machine) {
  case 0x3e:
    return "x86_64";
  case 0xb7:
    return "aarch64";
  case 0x28:
    return "arm";
  case 0x03:
    return "i686";
  case 0xf3:
    return "riscv64";
  default:
    return NULL;
  }
}

static const char *elf_arch_in_root(const char *dest, const char *rel) {
  char full[PATH_MAX], resolved[PATH_MAX];
  snprintf(full, sizeof full, "%s%s", dest, rel);
  struct stat st;
  if (lstat(full, &st))
    return NULL;
  const char *path = full;
  if (S_ISLNK(st.st_mode)) {
    char link[PATH_MAX];
    ssize_t n = readlink(full, link, sizeof link - 1);
    if (n <= 0)
      return NULL;
    link[n] = '\0';
    if (link[0] == '/')
      snprintf(resolved, sizeof resolved, "%s%s", dest, link);
    else {
      char dir[PATH_MAX];
      snprintf(dir, sizeof dir, "%s", full);
      char *slash = strrchr(dir, '/');
      if (!slash)
        return NULL;
      *slash = '\0';
      snprintf(resolved, sizeof resolved, "%s/%s", dir, link);
    }
    path = resolved;
  }
  return elf_arch_of(path);
}

static const char *detect_rootfs_arch(const char *dest) {
  static const char *paths[] = {"/bin/sh",   "/bin/busybox", "/bin/dash",
                                "/bin/bash", "/bin/ls",      "/usr/bin/env"};
  for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
    const char *a = elf_arch_in_root(dest, paths[i]);
    if (a)
      return a;
  }
  return NULL;
}

static void check_login_arch(const char *dest) {
  const char *arch = detect_rootfs_arch(dest);
  if (!arch)
    return;
  const char *host = host_arch();
  if (strcmp(arch, host))
    die("arquitectura del rootfs (%s) no coincide con el host (%s): chroot "
        "no emula CPU; instale un rootfs %s o use QEMU/binfmt",
        arch, host, host);
}

static void builtin_url_for_arch(const BuiltinImage *bi, const char *arch,
                                 char *out, size_t n, const char **sha_out) {
  *sha_out = bi->sha;
  if (!strcmp(arch, host_arch())) {
    snprintf(out, n, "%s", bi->url);
    return;
  }
  const char *a = arch_for_distro(arch, bi->family);
  if (!strcmp(bi->family, "alpine"))
    snprintf(out, n,
             "https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/%s/"
             "alpine-minirootfs-3.24.2-%s.tar.gz",
             a, a);
  else if (!strcmp(bi->family, "debian"))
    snprintf(out, n,
             "https://doi-janky.infosiftr.net/job/tianon/job/debuerreotype/"
             "job/%s/lastSuccessfulBuild/artifact/bookworm/slim/"
             "rootfs.tar.xz",
             a);
  else
    snprintf(out, n,
             "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/"
             "ubuntu-base-24.04.5-base-%s.tar.gz",
             a);
  *sha_out = NULL;
  info("arquitectura %s no nativa: %s", arch, out);
  info("sin SHA256 publicado en esta tabla; chroot no podrá ejecutarlo sin "
       "QEMU/binfmt");
}

#ifndef PROGRAM_NAME
#define PROGRAM_NAME "xroot-distro"
#endif

#define ALIAS_NAME "xd"
static const char *g_root = DEFAULT_ROOT;
static const char *g_sha = NULL;

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs(PROGRAM_NAME ": ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}
static void info(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs(PROGRAM_NAME ": ", stdout);
  vfprintf(stdout, fmt, ap);
  fputc('\n', stdout);
  va_end(ap);
}
static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p)
    die("sin memoria");
  return p;
}
static char *xstrdup(const char *s) {
  char *p = strdup(s);
  if (!p)
    die("sin memoria");
  return p;
}

static int exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}
/* No seguir symlinks: /bin/sh -> /bin/busybox se resolveria contra el host. */
static int lexists(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0;
}

static char *dirname_str(const char *path) {
  char *p = xstrdup(path);
  char *slash = strrchr(p, '/');
  if (!slash) {
    free(p);
    return xstrdup(".");
  }
  if (slash == p)
    slash[1] = '\0';
  else
    *slash = '\0';
  return p;
}

static int mkdir_p(const char *path, mode_t mode) {
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp)
    return -1;
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0777) && errno != EEXIST) {
        *p = '/';
        return -1;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, mode) && errno != EEXIST)
    return -1;
  return 0;
}

static int valid_name(const char *s) {
  if (!s || !*s)
    return 0;
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') ||
        (s[0] >= '0' && s[0] <= '9')))
    return 0;
  for (const char *p = s; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
      return 0;
  return 1;
}

static void distro_dir(const char *name, char *out, size_t n) {
  snprintf(out, n, "%s/%s", g_root, name);
}
static void cache_dir(char *out, size_t n) {
  snprintf(out, n, "%s/.cache", g_root);
}

static void sha256_string(const char *s, char out[65]) {
  unsigned char d[EVP_MAX_MD_SIZE];
  unsigned int l = 0;
  EVP_Digest(s, strlen(s), d, &l, EVP_sha256(), NULL);
  for (unsigned i = 0; i < l; i++)
    sprintf(out + i * 2, "%02x", d[i]);
  out[l * 2] = '\0';
}
static void sha256_file(const char *path, char out[65]) {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1)
    die("SHA256 init falló");
  FILE *f = fopen(path, "rb");
  if (!f)
    die("no se puede leer %s", path);
  unsigned char buf[65536], digest[EVP_MAX_MD_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0)
    EVP_DigestUpdate(ctx, buf, n);
  unsigned int len = 0;
  EVP_DigestFinal_ex(ctx, digest, &len);
  fclose(f);
  EVP_MD_CTX_free(ctx);
  for (unsigned i = 0; i < len; i++)
    sprintf(out + i * 2, "%02x", digest[i]);
  out[len * 2] = '\0';
}

static char *cache_path_for(const char *url) {
  char h[65];
  sha256_string(url, h);
  const char *base = strrchr(url, '/');
  base = base ? base + 1 : url;
  char clean[128];
  snprintf(clean, sizeof clean, "%s", base);
  char *q = strchr(clean, '?');
  if (q)
    *q = '\0';
  for (char *p = clean; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
      *p = '_';
  char cdir[PATH_MAX];
  cache_dir(cdir, sizeof cdir);
  char *out = xmalloc(PATH_MAX);
  snprintf(out, PATH_MAX, "%s/%.16s-%s", cdir, h, clean);
  return out;
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  return fwrite(ptr, size, nmemb, (FILE *)userdata);
}
static void download(const char *url, const char *path) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  CURL *c = curl_easy_init();
  if (!c)
    die("no se pudo inicializar libcurl");
  FILE *f = fopen(path, "wb");
  if (!f)
    die("no se pudo crear %s: %s", path, strerror(errno));
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
  struct curl_slist *headers =
      curl_slist_append(NULL, "User-Agent: xroot-distro/1.0");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  CURLcode rc = curl_easy_perform(c);
  fclose(f);
  if (rc != CURLE_OK) {
    unlink(path);
    die("descarga falló: %s", curl_easy_strerror(rc));
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  curl_global_cleanup();
}

/* ---------- lector de tar (.tar, .tar.gz, .tar.xz; sin comando tar) ------- */
static long long parse_octal(const char *field, size_t len) {
  long long v = 0;
  size_t i = 0;
  while (i < len && (field[i] == ' ' || field[i] == '\0'))
    i++;
  for (; i < len && field[i] >= '0' && field[i] <= '7'; i++)
    v = v * 8 + (field[i] - '0');
  return v;
}

typedef enum { ARC_GZ, ARC_XZ, ARC_PLAIN } ArcKind;
typedef struct {
  ArcKind kind;
  FILE *f;
  gzFile gz;
  lzma_stream lz;
  uint8_t in[16384];
  int in_eof;
} Arc;

static void arc_open(const char *path, Arc *a) {
  memset(a, 0, sizeof *a);
  a->f = fopen(path, "rb");
  if (!a->f)
    die("no se puede abrir %s: %s", path, strerror(errno));
  unsigned char magic[6] = {0};
  size_t n = fread(magic, 1, sizeof magic, a->f);
  if (fseek(a->f, 0, SEEK_SET))
    die("no se pudo rebobinar %s: %s", path, strerror(errno));
  if (n >= 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
    fclose(a->f);
    a->f = NULL;
    a->gz = gzopen(path, "rb");
    if (!a->gz)
      die("no se puede abrir %s como gzip", path);
    a->kind = ARC_GZ;
  } else if (n >= 6 && !memcmp(magic,
                               "\xfd"
                               "7zXZ"
                               "\x00",
                               6)) {
    if (lzma_stream_decoder(&a->lz, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
      die("no se pudo inicializar el decodificador xz");
    a->kind = ARC_XZ;
  } else {
    a->kind = ARC_PLAIN;
  }
}

/* Devuelve bytes leidos, 0 al agotar el archivo o -1 en error. */
static int arc_read(Arc *a, void *buf, size_t want) {
  if (!want)
    return 0;
  if (a->kind == ARC_GZ) {
    int n = gzread(a->gz, buf, (unsigned)want);
    return n < 0 ? -1 : n;
  }
  if (a->kind == ARC_PLAIN) {
    size_t n = fread(buf, 1, want, a->f);
    return n ? (int)n : (ferror(a->f) ? -1 : 0);
  }
  a->lz.next_out = (uint8_t *)buf;
  a->lz.avail_out = want;
  while (a->lz.avail_out > 0) {
    size_t before = a->lz.avail_out;
    if (!a->lz.avail_in && !a->in_eof) {
      a->lz.next_in = a->in;
      a->lz.avail_in = fread(a->in, 1, sizeof a->in, a->f);
      if (!a->lz.avail_in)
        a->in_eof = 1;
    }
    lzma_ret r = lzma_code(&a->lz, a->in_eof ? LZMA_FINISH : LZMA_RUN);
    if (r == LZMA_STREAM_END)
      break;
    if (r != LZMA_OK)
      return -1;
    if (a->in_eof && !a->lz.avail_in && a->lz.avail_out == before)
      break; /* fin del stream sin más salida */
  }
  return (int)(want - a->lz.avail_out);
}

static int arc_discard(Arc *a, long long n) {
  char buf[8192];
  while (n > 0) {
    size_t want = n > (long long)sizeof buf ? sizeof buf : (size_t)n;
    int got = arc_read(a, buf, want);
    if (got <= 0)
      return -1;
    n -= got;
  }
  return 0;
}

static void arc_close(Arc *a) {
  if (a->kind == ARC_GZ && a->gz)
    gzclose(a->gz);
  if (a->kind != ARC_GZ && a->f)
    fclose(a->f);
  if (a->kind == ARC_XZ)
    lzma_end(&a->lz);
}
static int safe_entry(const char *name) {
  if (!name || !*name || name[0] == '/')
    return 0;
  const char *p = name;
  while (*p) {
    while (*p == '/')
      p++;
    const char *start = p;
    while (*p && *p != '/')
      p++;
    if ((size_t)(p - start) == 2 && start[0] == '.' && start[1] == '.')
      return 0;
  }
  return 1;
}
static void extract_tar(const char *path, const char *dest) {
  Arc arc;
  arc_open(path, &arc);
  char block[512], longname[4096] = "";
  for (;;) {
    if (arc_read(&arc, block, 512) != 512)
      die("tarball truncado o inválido: %s", path);
    int zero = 1;
    for (int i = 0; i < 512; i++)
      if (block[i]) {
        zero = 0;
        break;
      }
    if (zero)
      break;

    char name[101] = {0}, prefix[156] = {0}, linkname[101] = {0};
    char modebuf[9] = {0}, sizebuf[13] = {0};
    memcpy(name, block, 100);
    memcpy(modebuf, block + 100, 8);
    memcpy(sizebuf, block + 124, 12);
    char type = block[156];
    memcpy(linkname, block + 157, 100);
    memcpy(prefix, block + 345, 155);
    long long size = parse_octal(sizebuf, 12);
    mode_t mode = (mode_t)parse_octal(modebuf, 8);
    if (!mode)
      mode = (type == '5') ? 0755 : 0644;

    char entry[4096];
    if (longname[0]) {
      snprintf(entry, sizeof entry, "%s", longname);
      longname[0] = '\0';
    } else if (prefix[0])
      snprintf(entry, sizeof entry, "%s/%s", prefix, name);
    else
      snprintf(entry, sizeof entry, "%s", name);

    if (type == 'L') {
      char *ln = xmalloc((size_t)size + 1);
      if (arc_read(&arc, ln, (size_t)size) != size)
        die("no se pudo leer nombre largo");
      ln[size] = '\0';
      snprintf(longname, sizeof longname, "%s", ln);
      free(ln);
      long long pad = (512 - (size % 512)) % 512;
      if (pad && arc_discard(&arc, pad))
        die("tarball truncado (pad)");
      continue;
    }
    if (!safe_entry(entry)) {
      if (arc_discard(&arc, size) ||
          arc_discard(&arc, (512 - (size % 512)) % 512))
        die("tarball truncado");
      continue;
    }

    char target[PATH_MAX];
    snprintf(target, sizeof target, "%s/%s", dest, entry);
    switch (type) {
    case '0':
    case '\0': {
      char *parent = dirname_str(target);
      mkdir_p(parent, 0777);
      free(parent);
      int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, mode);
      if (fd < 0)
        die("no se pudo crear %s: %s", target, strerror(errno));
      long long left = size;
      while (left > 0) {
        char data[8192];
        size_t want =
            left > (long long)sizeof data ? sizeof data : (size_t)left;
        int n = arc_read(&arc, data, want);
        if (n <= 0) {
          close(fd);
          die("tarball truncado en %s", entry);
        }
        if (write(fd, data, (size_t)n) != n) {
          close(fd);
          die("error escribiendo %s", entry);
        }
        left -= n;
      }
      close(fd);
      if (arc_discard(&arc, (512 - (size % 512)) % 512))
        die("tarball truncado (pad)");
      break;
    }
    case '5':
      if (mkdir_p(target, mode))
        die("no se pudo crear dir %s", target);
      break;
    case '2': {
      char *parent = dirname_str(target);
      mkdir_p(parent, 0777);
      free(parent);
      unlink(target);
      if (symlink(linkname, target))
        info("aviso: no se pudo crear symlink %s", entry);
      break;
    }
    case '1': {
      char *parent = dirname_str(target);
      mkdir_p(parent, 0777);
      free(parent);
      char origin[PATH_MAX];
      snprintf(origin, sizeof origin, "%s/%s", dest, linkname);
      unlink(target);
      if (link(origin, target))
        info("aviso: no se pudo crear hardlink %s", entry);
      break;
    }
    default:
      if (arc_discard(&arc, size) ||
          arc_discard(&arc, (512 - (size % 512)) % 512))
        die("tarball truncado");
      break;
    }
  }
  arc_close(&arc);
}

/* ---------- escritor de tar.gz (backup) ---------- */
static void put_octal(char *p, int len, unsigned long v) {
  snprintf(p, (size_t)len, "%0*lo", len - 1, v);
}
static void tar_header(gzFile out, const char *name, const struct stat *st,
                       char type, const char *linkname) {
  char h[512] = {0};
  put_octal(h + 100, 8, (unsigned long)st->st_mode);
  put_octal(h + 108, 8, (unsigned long)st->st_uid);
  put_octal(h + 116, 8, (unsigned long)st->st_gid);
  put_octal(h + 124, 12, (unsigned long)st->st_size);
  put_octal(h + 136, 12, (unsigned long)st->st_mtime);
  memset(h + 148, ' ', 8);
  h[156] = type;
  if (linkname)
    snprintf(h + 157, 100, "%s", linkname);
  memcpy(h + 257, "ustar\0", 6);
  memcpy(h + 263, "00", 2);
  const char *slash = strrchr(name, '/');
  if (strlen(name) > 100 && slash && strlen(slash + 1) < 100 &&
      (size_t)(slash - name) < 155) {
    snprintf(h + 345, 155, "%.*s", (int)(slash - name), name);
    snprintf(h, 100, "%s", slash + 1);
  } else
    snprintf(h, 100, "%s", name);
  unsigned sum = 0;
  for (int i = 0; i < 512; i++)
    sum += (unsigned char)h[i];
  snprintf(h + 148, 8, "%06o", sum);
  h[154] = '\0';
  h[155] = ' ';
  gzwrite(out, h, 512);
}
static void tar_walk(gzFile out, const char *root, const char *rel) {
  char dirpath[PATH_MAX];
  if (rel[0])
    snprintf(dirpath, sizeof dirpath, "%s/%s", root, rel);
  else
    snprintf(dirpath, sizeof dirpath, "%s", root);
  DIR *d = opendir(dirpath);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char full[PATH_MAX], childrel[PATH_MAX], arcname[PATH_MAX];
    snprintf(full, sizeof full, "%s/%s", dirpath, e->d_name);
    if (rel[0])
      snprintf(childrel, sizeof childrel, "%s/%s", rel, e->d_name);
    else
      snprintf(childrel, sizeof childrel, "%s", e->d_name);
    snprintf(arcname, sizeof arcname, "%s", childrel);
    struct stat st;
    if (lstat(full, &st))
      continue;
    if (S_ISDIR(st.st_mode)) {
      char withslash[PATH_MAX];
      snprintf(withslash, sizeof withslash, "%s/", arcname);
      tar_header(out, withslash, &st, '5', NULL);
      tar_walk(out, root, childrel);
    } else if (S_ISLNK(st.st_mode)) {
      char tgt[PATH_MAX];
      ssize_t n = readlink(full, tgt, sizeof tgt - 1);
      if (n < 0)
        continue;
      tgt[n] = '\0';
      tar_header(out, arcname, &st, '2', tgt);
    } else if (S_ISREG(st.st_mode)) {
      tar_header(out, arcname, &st, '0', NULL);
      FILE *f = fopen(full, "rb");
      if (f) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
          gzwrite(out, buf, (unsigned)n);
        fclose(f);
      }
      long pad = (512 - (st.st_size % 512)) % 512;
      if (pad) {
        char z[512] = {0};
        gzwrite(out, z, (unsigned)pad);
      }
    }
  }
  closedir(d);
}

/* ---------- metadata ---------- */
static char *read_key(const char *path, const char *prefix) {
  FILE *f = fopen(path, "r");
  if (!f)
    return NULL;
  char line[1024];
  char *result = NULL;
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, prefix, strlen(prefix))) {
      char *p = line + strlen(prefix);
      p[strcspn(p, "\r\n")] = '\0';
      result = xstrdup(p);
      break;
    }
  }
  fclose(f);
  return result;
}
static void marker_path(const char *dest, char *out, size_t n) {
  snprintf(out, n, "%s/%s", dest, MARKER);
}
static void write_marker(const char *dest, const char *name, const char *url,
                         const char *sha, const char *arch) {
  char m[PATH_MAX];
  marker_path(dest, m, sizeof m);
  time_t now = time(NULL);
  struct tm t;
  gmtime_r(&now, &t);
  char stamp[64];
  strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &t);
  FILE *f = fopen(m, "w");
  if (!f)
    die("no se pudo escribir %s", m);
  fprintf(f, "name=%s\nurl=%s\nsha256=%s\narch=%s\ninstalled=%s\n", name, url,
          sha, arch && *arch ? arch : "unknown", stamp);
  fclose(f);
  chmod(m, 0600);
}
static void marker_set_name(const char *dest, const char *name) {
  char m[PATH_MAX];
  marker_path(dest, m, sizeof m);
  char *url = read_key(m, "url="), *sha = read_key(m, "sha256="),
       *arch = read_key(m, "arch=");
  write_marker(dest, name, url ? url : "-", sha ? sha : "unknown", arch);
  free(url);
  free(sha);
  free(arch);
}

/* ---------- install / remove / rename / reset ---------- */
static const char *obtain_archive(const char *url, const char *sha) {
  char cdir[PATH_MAX];
  cache_dir(cdir, sizeof cdir);
  if (mkdir_p(cdir, 0777))
    die("no se pudo crear %s", cdir);
  char *cached = cache_path_for(url);
  if (exists(cached)) {
    if (!sha || !*sha) {
      info("usando descarga en caché: %s", cached);
      return cached;
    }
    char actual[65];
    sha256_file(cached, actual);
    if (!strcmp(actual, sha)) {
      info("usando descarga en caché: %s", cached);
      return cached;
    }
    info("caché con SHA distinto; descargando de nuevo");
    unlink(cached);
  }
  info("descargando %s", url);
  download(url, cached);
  if (sha && *sha) {
    char actual[65];
    sha256_file(cached, actual);
    if (strcmp(actual, sha)) {
      unlink(cached);
      die("SHA256 no coincide: esperado %s, obtenido %s", sha, actual);
    }
    info("SHA256 verificado: %s", actual);
  }
  info("guardado en caché: %s", cached);
  return cached;
}

/* ubuntu-base y otros rootfs mínimos no traen /tmp ni /run con permisos útiles.
 */
static void ensure_runtime_dirs(const char *dest) {
  char p[PATH_MAX];
  snprintf(p, sizeof p, "%s/tmp", dest);
  if (mkdir(p, 0777) && errno != EEXIST)
    die("no se pudo crear %s: %s", p, strerror(errno));
  if (chmod(p, 01777))
    die("no se pudo ajustar permisos de %s: %s", p, strerror(errno));
  snprintf(p, sizeof p, "%s/run", dest);
  if (mkdir(p, 0755) && errno != EEXIST)
    die("no se pudo crear %s: %s", p, strerror(errno));
  snprintf(p, sizeof p, "%s/run/lock", dest);
  if (mkdir(p, 0755) && errno != EEXIST)
    die("no se pudo crear %s: %s", p, strerror(errno));
}

static void install_from(const char *name, const char *url, const char *sha,
                         const char *arch) {
  char dest[PATH_MAX], marker[PATH_MAX];
  distro_dir(name, dest, sizeof dest);
  marker_path(dest, marker, sizeof marker);
  if (exists(marker)) {
    info("'%s' ya está instalado en %s (nada que hacer)", name, dest);
    return;
  }
  if (mkdir_p(dest, 0755))
    die("no se pudo crear %s", dest);
  const char *archive = obtain_archive(url, sha);
  info("extrayendo en %s", dest);
  extract_tar(archive, dest);
  ensure_runtime_dirs(dest);
  char sh[PATH_MAX];
  snprintf(sh, sizeof sh, "%s/bin/sh", dest);
  if (!lexists(sh))
    die("el archivo no parece un rootfs (falta /bin/sh)");
  const char *detected = detect_rootfs_arch(dest);
  if (arch && strcmp(arch, "unknown") && detected && strcmp(arch, detected))
    die("la arquitectura pedida (%s) no coincide con el rootfs (%s)", arch,
        detected);
  write_marker(dest, name, url, sha && *sha ? sha : "unknown",
               detected ? detected : (arch ? arch : "unknown"));
  info("instalado: %s -> %s (arch=%s)", name, dest,
       detected ? detected : (arch ? arch : "unknown"));
}

static void install_usage(void) {
  printf(
      "%s install [OPTIONS] (URL or FILE)\n\n"
      "USAGE\n\n"
      "  %s install [OPTIONS] (URL or FILE)\n\n"
      "ALIASES\n\n"
      "  add, i, in, ins\n\n"
      "DESCRIPTION\n\n"
      "  Create a chroot container from a rootfs tarball (.tar.gz or .tar.xz)\n"
      "  given as a web URL or a local file (file:// or path). Unlike "
      "proot-distro, this\n"
      "  tool does NOT pull Docker/OCI images and does NOT emulate other CPU\n"
      "  architectures: the rootfs must match the host architecture "
      "(x86_64).\n\n"
      "  Container name is derived from the archive file name. To install\n"
      "  multiple instances of the same distribution, override it with -n.\n\n"
      "OPTIONS\n\n"
      "  -h, --help                 Show this help.\n"
      "  -n, --name [NAME]          Set a custom name for the container. Must\n"
      "                             start with alphanumeric character and "
      "then\n"
      "                             may contain letters, numbers, dot, minus\n"
      "                             and underscore.\n"
      "  -a, --architecture [ARCH]  Override the target CPU architecture. "
      "Accepts\n"
      "                             native names (aarch64, arm, i686, "
      "riscv64,\n"
      "                             x86_64) or Docker platform strings\n"
      "                             (linux/arm64, linux/amd64, linux/arm/v7,\n"
      "                             linux/386, linux/riscv64). chroot does "
      "NOT\n"
      "                             emulate CPUs: foreign rootfs can be "
      "installed\n"
      "                             but not executed without QEMU/binfmt.\n"
      "  --sha256 [HASH]            Expected SHA256 (or XROOT_DISTRO_SHA256).\n"
      "                             Not present in proot-distro.\n\n"
      "EXAMPLES\n\n"
      "  $ %s install -n alpine %s\n"
      "  $ %s install -n ubuntu "
      "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/"
      "ubuntu-base-24.04.5-base-amd64.tar.gz\n"
      "  $ %s install -n debian %s\n"
      "  $ %s install local file:///tmp/rootfs.tar.gz\n\n"
      "WHERE TO FIND IMAGES\n\n"
      "  ▸ Alpine: "
      "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/\n"
      "  ▸ Ubuntu: https://cdimage.ubuntu.com/ubuntu-base/releases/\n"
      "  ▸ Debian: https://docker.debian.net/ (debuerreotype slim rootfs)\n\n"
      "RECOMMENDED IMAGES\n\n"
      "  ▸ alpine:3.24.2\n"
      "  ▸ ubuntu-base:24.04.5\n"
      "  ▸ debian:bookworm-slim\n",
      PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, EXAMPLE_URL, PROGRAM_NAME,
      PROGRAM_NAME, DEBIAN_URL, PROGRAM_NAME);
  printf("\nBUILT-IN IMAGES (short names)\n\n"
         "  ▸ alpine, alpine:latest, alpine:3.24.2\n"
         "  ▸ ubuntu, ubuntu:latest, ubuntu:24.04, ubuntu:24.04.5\n"
         "  ▸ debian, debian:bookworm-slim, debian:trixie-slim ...\n");
}

static void cmd_install(int argc, char **argv) {
  const char *name = NULL, *url = NULL, *sha = g_sha, *spec = NULL,
             *arch = NULL;
  char urlbuf[PATH_MAX];
  for (int i = 0; i < argc; i++)
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      install_usage();
      return;
    }
  for (int i = 0; i < argc; i++) {
    if (!strncmp(argv[i], "-n=", 3))
      name = argv[i] + 3;
    else if (!strcmp(argv[i], "-n") && i + 1 < argc)
      name = argv[++i];
    else if ((!strcmp(argv[i], "-a") || !strcmp(argv[i], "--architecture")) &&
             i + 1 < argc)
      arch = normalize_arch(argv[++i]);
    else if (!strncmp(argv[i], "--architecture=", 15))
      arch = normalize_arch(argv[i] + 15);
    else if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--architecture"))
      die("%s requiere [ARCH]", argv[i]);
    else if (!strncmp(argv[i], "--sha256=", 9))
      sha = argv[i] + 9;
    else if (!strcmp(argv[i], "--sha256") && i + 1 < argc)
      sha = argv[++i];
    else if (argv[i][0] == '-')
      die("opción desconocida en install: %s", argv[i]);
    else if (!spec)
      spec = argv[i];
    else
      die("argumento duplicado en install: %s", argv[i]);
  }
  if (!url && spec) {
    const BuiltinImage *bi = find_builtin(spec);
    if (bi) {
      if (arch) {
        const char *computed_sha = NULL;
        builtin_url_for_arch(bi, arch, urlbuf, sizeof urlbuf, &computed_sha);
        url = urlbuf;
        if (!sha)
          sha = computed_sha;
      } else {
        url = bi->url;
        if (!sha)
          sha = bi->sha;
      }
      if (!name)
        name = bi->name;
    } else if (!strncmp(spec, "http://", 7) || !strncmp(spec, "https://", 8) ||
               !strncmp(spec, "file://", 7) || spec[0] == '/') {
      url = spec;
    } else {
      die("imagen desconocida '%s' (usa alpine|ubuntu o una URL)", spec);
    }
  }
  if (!name)
    die("install requiere -n=<nombre> o un nombre integrado (alpine|ubuntu)");
  if (!url)
    die("install requiere <url> o un nombre integrado (alpine|ubuntu)");
  if (!valid_name(name))
    die("nombre inválido: %s", name);
  if (arch && !strcmp(arch, host_arch()) && sha && *sha &&
      !strncmp(url, "https://", 8))
    info("arquitectura destino: %s", arch);
  install_from(name, url, sha, arch);
}

static int path_mounted(const char *path) {
  FILE *f = fopen("/proc/self/mountinfo", "r");
  if (!f)
    return 0;
  char line[4096];
  int found = 0;
  while (fgets(line, sizeof line, f)) {
    char *p = line;
    for (int i = 0; i < 4; i++) {
      p = strchr(p, ' ');
      if (!p)
        break;
      p++;
    }
    if (!p)
      continue;
    char *end = strchr(p, ' ');
    if (!end)
      continue;
    *end = '\0';
    if (!strcmp(p, path)) {
      found = 1;
      break;
    }
  }
  fclose(f);
  return found;
}
static void assert_no_mounts(const char *name, const char *dest) {
  const char *subs[] = {"proc", "sys", "dev", "etc/resolv.conf", "tmp"};
  for (size_t k = 0; k < sizeof subs / sizeof subs[0]; k++) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dest, subs[k]);
    if (path_mounted(p))
      die("'%s' tiene mounts activos (%s); ciérralos antes", name, subs[k]);
  }
}
static int rmtree(const char *path) {
  struct stat st;
  if (lstat(path, &st))
    return -1;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if (!d)
      return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
        continue;
      char child[PATH_MAX];
      snprintf(child, sizeof child, "%s/%s", path, e->d_name);
      rmtree(child);
    }
    closedir(d);
    return rmdir(path);
  }
  return unlink(path);
}
static void destroy_container(const char *name) {
  char dest[PATH_MAX], marker[PATH_MAX];
  distro_dir(name, dest, sizeof dest);
  marker_path(dest, marker, sizeof marker);
  if (!exists(marker))
    die("'%s' no está instalado", name);
  assert_no_mounts(name, dest);
  if (rmtree(dest))
    die("no se pudo borrar %s", dest);
  info("eliminado: %s", name);
}

static void cmd_remove(int argc, char **argv) {
  if (argc < 1)
    die("remove requiere <contenedor> o <imagen-en-caché> (¡destruye datos!)");
  const char *arg = argv[0];
  if (!valid_name(arg))
    die("nombre inválido: %s", arg);
  char dest[PATH_MAX], marker[PATH_MAX];
  distro_dir(arg, dest, sizeof dest);
  marker_path(dest, marker, sizeof marker);
  if (exists(marker)) {
    destroy_container(arg);
    return;
  }
  char cdir[PATH_MAX], p[PATH_MAX];
  cache_dir(cdir, sizeof cdir);
  snprintf(p, sizeof p, "%s/%s", cdir, arg);
  struct stat st;
  if (lstat(p, &st) == 0 && S_ISREG(st.st_mode)) {
    if (unlink(p))
      die("no se pudo borrar %s", p);
    info("imagen en caché eliminada: %s", arg);
    return;
  }
  die("'%s' no es un contenedor ni una imagen en caché (usa 'list' / 'list "
      "--images')",
      arg);
}

static void cmd_rename(int argc, char **argv) {
  if (argc < 2)
    die("rename requiere <viejo> <nuevo>");
  const char *old = argv[0], *neu = argv[1];
  if (!valid_name(old) || !valid_name(neu))
    die("nombre inválido");
  char od[PATH_MAX], nd[PATH_MAX], om[PATH_MAX], nm[PATH_MAX];
  distro_dir(old, od, sizeof od);
  distro_dir(neu, nd, sizeof nd);
  marker_path(od, om, sizeof om);
  marker_path(nd, nm, sizeof nm);
  if (!exists(om))
    die("'%s' no está instalado", old);
  if (exists(nm))
    die("'%s' ya existe", neu);
  assert_no_mounts(old, od);
  if (rename(od, nd))
    die("no se pudo renombrar: %s", strerror(errno));
  marker_set_name(nd, neu);
  info("renombrado: %s -> %s", old, neu);
}

static void cmd_reset(int argc, char **argv) {
  if (argc < 1)
    die("reset requiere <nombre> (¡destruye datos!)");
  const char *name = argv[0];
  if (!valid_name(name))
    die("nombre inválido: %s", name);
  char dest[PATH_MAX], marker[PATH_MAX];
  distro_dir(name, dest, sizeof dest);
  marker_path(dest, marker, sizeof marker);
  char *url = read_key(marker, "url="), *sha = read_key(marker, "sha256=");
  if (!url)
    die("'%s' no está instalado", name);
  info("reinstituyendo '%s' desde %s", name, url);
  destroy_container(name);
  install_from(name, url, sha && strcmp(sha, "unknown") ? sha : NULL, NULL);
  free(url);
  free(sha);
}

static void cmd_backup(int argc, char **argv) {
  if (argc < 1)
    die("backup requiere <nombre> [archivo.tar.gz]");
  const char *name = argv[0];
  if (!valid_name(name))
    die("nombre inválido: %s", name);
  char dest[PATH_MAX], marker[PATH_MAX];
  distro_dir(name, dest, sizeof dest);
  marker_path(dest, marker, sizeof marker);
  if (!exists(marker))
    die("'%s' no está instalado", name);
  char out[PATH_MAX];
  if (argc >= 2)
    snprintf(out, sizeof out, "%s", argv[1]);
  else {
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", &t);
    snprintf(out, sizeof out, "%s-%s.tar.gz", name, stamp);
  }
  gzFile gz = gzopen(out, "wb9");
  if (!gz)
    die("no se puede crear %s", out);
  info("guardando backup de '%s' en %s", name, out);
  tar_walk(gz, dest, "");
  char zeros[1024] = {0};
  gzwrite(gz, zeros, sizeof zeros);
  gzclose(gz);
  info("backup listo: %s", out);
}

static void cmd_restore(int argc, char **argv) {
  if (argc < 2)
    die("restore requiere <nombre> <archivo.tar.gz> (¡destruye datos!)");
  const char *name = argv[0], *archive = argv[1];
  if (!valid_name(name))
    die("nombre inválido: %s", name);
  if (!exists(archive))
    die("no existe %s", archive);
  char dest[PATH_MAX], marker[PATH_MAX];
  distro_dir(name, dest, sizeof dest);
  marker_path(dest, marker, sizeof marker);
  if (exists(marker))
    destroy_container(name);
  if (mkdir_p(dest, 0755))
    die("no se pudo crear %s", dest);
  info("restaurando '%s' desde %s", name, archive);
  extract_tar(archive, dest);
  ensure_runtime_dirs(dest);
  char sh[PATH_MAX];
  snprintf(sh, sizeof sh, "%s/bin/sh", dest);
  if (!lexists(sh))
    die("el archivo no parece un backup válido");
  if (!exists(marker)) {
    char *arch = xstrdup(archive);
    char url[PATH_MAX];
    snprintf(url, sizeof url, "restore:%s", arch);
    const char *detected = detect_rootfs_arch(dest);
    write_marker(dest, name, url, "unknown", detected ? detected : "unknown");
    free(arch);
  } else {
    marker_set_name(dest, name); /* el backup tenia el nombre original */
  }
  info("restaurado: %s -> %s", name, dest);
}

static void cmd_clear_cache(void) {
  char cdir[PATH_MAX];
  cache_dir(cdir, sizeof cdir);
  DIR *d = opendir(cdir);
  if (!d) {
    info("caché vacía: %s", cdir);
    return;
  }
  struct dirent *e;
  int n = 0;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", cdir, e->d_name);
    struct stat st;
    if (lstat(p, &st) == 0 && S_ISREG(st.st_mode) && unlink(p) == 0)
      n++;
  }
  closedir(d);
  info("caché limpia: %d descarga(s) eliminada(s)", n);
}

/* ---------- list / login ---------- */
static void list_images(void) {
  char cdir[PATH_MAX];
  cache_dir(cdir, sizeof cdir);
  DIR *d = opendir(cdir);
  if (!d) {
    info("caché vacía: %s", cdir);
    return;
  }
  struct dirent *e;
  int found = 0;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", cdir, e->d_name);
    struct stat st;
    if (lstat(p, &st) || !S_ISREG(st.st_mode))
      continue;
    found = 1;
    printf("%-58s %10lld bytes\n", e->d_name, (long long)st.st_size);
  }
  closedir(d);
  if (!found)
    info("caché vacía: %s", cdir);
}
static void cmd_list(int argc, char **argv) {
  if (argc > 0 && !strcmp(argv[0], "--images")) {
    list_images();
    return;
  }
  DIR *d = opendir(g_root);
  if (!d) {
    info("no hay contenedores en %s", g_root);
    return;
  }
  struct dirent *e;
  int found = 0;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char base[PATH_MAX], marker[PATH_MAX];
    snprintf(base, sizeof base, "%s/%s", g_root, e->d_name);
    marker_path(base, marker, sizeof marker);
    if (!exists(marker))
      continue;
    found = 1;
    char *url = read_key(marker, "url="),
         *installed = read_key(marker, "installed="),
         *arch = read_key(marker, "arch=");
    /* Markers anteriores a -a no tenían arquitectura: se detecta y se rellena.
     */
    if (!arch || !strcmp(arch, "unknown")) {
      const char *detected = detect_rootfs_arch(base);
      if (detected) {
        char *name = read_key(marker, "name="),
             *sha = read_key(marker, "sha256=");
        write_marker(base, name ? name : e->d_name, url ? url : "-",
                     sha ? sha : "unknown", detected);
        free(name);
        free(sha);
        free(arch);
        arch = xstrdup(detected);
      }
    }
    char release[256] = "desconocida";
    char osr[PATH_MAX];
    snprintf(osr, sizeof osr, "%s/etc/os-release", base);
    char *pretty = read_key(osr, "PRETTY_NAME=");
    if (pretty) {
      char *q = strchr(pretty, '"');
      if (q) {
        char *end = strrchr(q + 1, '"');
        if (end)
          *end = '\0';
        snprintf(release, sizeof release, "%s", q + 1);
      } else
        snprintf(release, sizeof release, "%s", pretty);
      free(pretty);
    } else {
      char alpine[PATH_MAX];
      snprintf(alpine, sizeof alpine, "%s/etc/alpine-release", base);
      char *v = read_key(alpine, "");
      if (v) {
        snprintf(release, sizeof release, "Alpine Linux %s", v);
        free(v);
      }
    }
    printf("%-16s %-30s %s\n", e->d_name, release, base);
    printf("                 url: %s\n", url ? url : "-");
    printf("                 arch: %s\n", arch ? arch : "unknown");
    printf("                 instalado: %s\n", installed ? installed : "-");
    free(url);
    free(installed);
    free(arch);
  }
  closedir(d);
  if (!found)
    info("no hay contenedores en %s", g_root);
}

typedef struct {
  char *src;
  char *target;
} Mount;
static int path_mounted(const char *path); /* ya definida arriba */
static void bind_path(const char *src, const char *target, Mount *mounts,
                      int *nm) {
  if (path_mounted(target))
    return; /* no apilar mounts en logins repetidos */
  struct stat st;
  if (stat(src, &st))
    return;
  char *parent = dirname_str(target);
  mkdir_p(parent, 0777);
  free(parent);
  if (S_ISDIR(st.st_mode))
    mkdir_p(target, 0777);
  else if (!exists(target)) {
    int fd = open(target, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0)
      close(fd);
  }
  if (mount(src, target, NULL, MS_BIND, NULL) == 0) {
    mounts[*nm].src = xstrdup(src);
    mounts[*nm].target = xstrdup(target);
    (*nm)++;
  } else
    info("aviso: no se pudo montar %s -> %s: %s", src, target, strerror(errno));
}
static int all_digits(const char *s) {
  if (!s || !*s)
    return 0;
  for (const char *p = s; *p; p++)
    if (*p < '0' || *p > '9')
      return 0;
  return 1;
}

/* Acepta 'name', 'uid', 'name:group' y 'uid:gid' ( igual que proot-distro). */
static void resolve_user(const char *dest, const char *spec, uid_t *uid,
                         gid_t *gid, char *home, size_t homelen) {
  char user[256] = {0}, group[256] = {0};
  const char *colon = strchr(spec, ':');
  if (colon) {
    size_t n = (size_t)(colon - spec);
    if (n >= sizeof user)
      die("usuario demasiado largo: %s", spec);
    memcpy(user, spec, n);
    snprintf(group, sizeof group, "%s", colon + 1);
    if (!group[0])
      die("grupo vacío en %s", spec);
  } else {
    snprintf(user, sizeof user, "%s", spec);
  }
  *uid = 0;
  *gid = 0;
  snprintf(home, homelen, "/root");

  char passwd[PATH_MAX];
  snprintf(passwd, sizeof passwd, "%s/etc/passwd", dest);
  int found = 0;
  FILE *f = fopen(passwd, "r");
  if (f) {
    char line[1024];
    while (fgets(line, sizeof line, f)) {
      char name[256] = {0}, pw[256] = {0}, h[512] = {0}, sh[256] = {0},
           gecos[256] = {0};
      unsigned int u = 0, g = 0;
      sscanf(line, "%255[^:]:%255[^:]:%u:%u:%255[^:]:%511[^:]:%255[^:\n]", name,
             pw, &u, &g, gecos, h, sh);
      if (!strcmp(name, user) ||
          (all_digits(user) && (unsigned int)atoi(user) == u)) {
        *uid = (uid_t)u;
        *gid = (gid_t)g;
        snprintf(home, homelen, "%s", h[0] ? h : "/");
        found = 1;
        break;
      }
    }
    fclose(f);
  }
  if (!found) {
    if (all_digits(user)) {
      *uid = (uid_t)atoi(user);
      *gid = *uid; /* proot-distro: uid numérico sin entrada en passwd usa gid =
                      uid */
    } else {
      die("usuario '%s' no existe en %s", user, passwd);
    }
  }

  if (group[0]) {
    char gpath[PATH_MAX];
    snprintf(gpath, sizeof gpath, "%s/etc/group", dest);
    int gfound = 0;
    FILE *gf = fopen(gpath, "r");
    if (gf) {
      char line[1024];
      while (fgets(line, sizeof line, f)) {
        char gname[256] = {0}, gpw[256] = {0};
        unsigned int g = 0;
        sscanf(line, "%255[^:]:%255[^:]:%u", gname, gpw, &g);
        if (!strcmp(gname, group) ||
            (all_digits(group) && (unsigned int)atoi(group) == g)) {
          *gid = (gid_t)g;
          gfound = 1;
          break;
        }
      }
      fclose(gf);
    }
    if (!gfound && all_digits(group))
      *gid = (gid_t)atoi(group);
    else if (!gfound)
      die("grupo '%s' no existe en %s", group, gpath);
  }
}

static void validate_bind(const char *spec) {
  const char *sep = strchr(spec, ':');
  if (!sep || sep == spec || sep[1] != '/')
    die("bind inválido (SRC:DEST): %s", spec);
  const char *p = spec;
  while (*p) {
    while (*p == '/')
      p++;
    const char *inicio = p;
    while (*p && *p != '/')
      p++;
    if ((size_t)(p - inicio) == 2 && inicio[0] == '.' && inicio[1] == '.')
      die("bind con '..' no permitido: %s", spec);
  }
}

static void print_cmd(const char *dest, Mount *mounts, int nm, const char *user,
                      const char *workdir, char **envs, int nenv, int cmd_argc,
                      char **cmd_argv) {
  for (int j = 0; j < nm; j++)
    printf("mount --bind %s %s\n", mounts[j].src, mounts[j].target);
  printf("chroot %s /usr/bin/env", dest);
  for (int k = 0; k < nenv; k++)
    printf(" '%s'", envs[k]);
  printf(" USER='%s'", user);
  printf(" /bin/sh -c 'cd %s && ", workdir ? workdir : "/");
  if (cmd_argc > 0) {
    for (int k = 0; k < cmd_argc; k++)
      printf("%s'%s'", k ? " " : "", cmd_argv[k]);
  } else {
    printf("exec /bin/sh -l");
  }
  printf("'\n");
}

static int cmd_login(int argc, char **argv) {
  const char *name = NULL, *user = "root", *workdir = NULL;
  int shared_tmp = 0, isolated = 0, get_cmd = 0, i = 0;
  char **binds = (char **)xmalloc((size_t)(argc + 1) * sizeof(char *));
  char **envs = (char **)xmalloc((size_t)(argc + 1) * sizeof(char *));
  int nbinds = 0, nenv = 0;

  for (; i < argc; i++) {
    if (!strncmp(argv[i], "--user=", 7))
      user = argv[i] + 7;
    else if ((!strcmp(argv[i], "-u") || !strcmp(argv[i], "--user")) &&
             i + 1 < argc)
      user = argv[++i];
    else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--bind")) {
      if (++i >= argc)
        die("%s requiere SRC:DEST", argv[i - 1]);
      validate_bind(argv[i]);
      binds[nbinds++] = argv[i];
    } else if (!strncmp(argv[i], "--bind=", 7)) {
      validate_bind(argv[i] + 7);
      binds[nbinds++] = argv[i] + 7;
    } else if ((!strcmp(argv[i], "-w") || !strcmp(argv[i], "--work-dir")) &&
               i + 1 < argc) {
      workdir = argv[++i];
      if (workdir[0] != '/')
        die("work-dir debe ser ruta absoluta: %s", workdir);
    } else if (!strncmp(argv[i], "--work-dir=", 11)) {
      workdir = argv[i] + 11;
      if (workdir[0] != '/')
        die("work-dir debe ser ruta absoluta: %s", workdir);
    } else if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--env")) {
      if (++i >= argc)
        die("%s requiere VAR=VALOR", argv[i - 1]);
      envs[nenv++] = argv[i];
    } else if (!strncmp(argv[i], "--env=", 6)) {
      envs[nenv++] = argv[i] + 6;
    } else if (!strcmp(argv[i], "--isolated") ||
               !strcmp(argv[i], "--minimal")) {
      isolated = 1; /* en chroot ya no hay extensiones proot que desactivar */
    } else if (!strcmp(argv[i], "--get-cmd")) {
      get_cmd = 1;
    } else if (!strcmp(argv[i], "--shared-tmp")) {
      shared_tmp =
          1; /* tiene prioridad sobre --isolated, como en proot-distro */
    } else if (!strcmp(argv[i], "--")) {
      i++;
      break;
    } else if (argv[i][0] == '-') {
      die("opción desconocida: %s", argv[i]);
    } else if (!name) {
      name = argv[i];
    } else {
      die("nombre duplicado: %s", argv[i]);
    }
  }
  if (!name)
    die("login requiere <nombre>");
  if (!valid_name(name))
    die("nombre inválido: %s", name);
  char dest[PATH_MAX], marker[PATH_MAX];
  distro_dir(name, dest, sizeof dest);
  marker_path(dest, marker, sizeof marker);
  if (!exists(marker))
    die("'%s' no está instalado (usa 'list')", name);
  check_login_arch(dest);

  uid_t uid;
  gid_t gid;
  char home[PATH_MAX];
  resolve_user(dest, user, &uid, &gid, home, sizeof home);

  Mount *mounts = (Mount *)xmalloc((size_t)(nbinds + 8) * sizeof(Mount));
  int nm = 0;
  char target[PATH_MAX];
  snprintf(target, sizeof target, "%s/proc", dest);
  bind_path("/proc", target, mounts, &nm);
  snprintf(target, sizeof target, "%s/sys", dest);
  bind_path("/sys", target, mounts, &nm);
  snprintf(target, sizeof target, "%s/dev", dest);
  bind_path("/dev", target, mounts, &nm);
  if (!isolated) {
    snprintf(target, sizeof target, "%s/etc/resolv.conf", dest);
    bind_path("/etc/resolv.conf", target, mounts, &nm);
  }
  if (shared_tmp) {
    snprintf(target, sizeof target, "%s/tmp", dest);
    bind_path("/tmp", target, mounts, &nm);
  }
  for (int b = 0; b < nbinds; b++) {
    const char *sep = strchr(binds[b], ':');
    size_t n = (size_t)(sep - binds[b]);
    char src[PATH_MAX];
    snprintf(src, sizeof src, "%.*s", (int)n, binds[b]);
    snprintf(target, sizeof target, "%s%s", dest, sep + 1);
    bind_path(src, target, mounts, &nm);
  }

  if (get_cmd) {
    print_cmd(dest, mounts, nm, user, workdir, envs, nenv, argc - i, &argv[i]);
    for (int j = nm - 1; j >= 0; j--)
      umount(mounts[j].target);
    for (int j = 0; j < nm; j++) {
      free(mounts[j].src);
      free(mounts[j].target);
    }
    free((void *)mounts);
    free((void *)binds);
    free((void *)envs);
    return 0;
  }

  info("entrando a '%s' (usuario=%s, work_dir=%s, shared_tmp=%d, isolated=%d)",
       name, user, workdir ? workdir : "/", shared_tmp, isolated);
  pid_t pid = fork();
  if (pid < 0)
    die("fork falló: %s", strerror(errno));
  if (pid == 0) {
    if (chroot(dest) || chdir(workdir ? workdir : "/"))
      _exit(125);
    gid_t glist[1] = {gid};
    (void)setgroups(1, glist);
    if (setgid(gid) || setuid(uid))
      _exit(125);
    for (int k = 0; k < nenv; k++)
      putenv(envs[k]);
    if (uid != 0) {
      setenv("HOME", home, 1);
      setenv("USER", user, 1);
      setenv("LOGNAME", user, 1);
    }
    char *shell_argv[] = {(char *)"/bin/sh", (char *)"-l", NULL};
    char **av = (i < argc) ? &argv[i] : shell_argv;
    execvp(av[0], av);
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  for (int j = nm - 1; j >= 0; j--) {
    umount(mounts[j].target);
    free(mounts[j].src);
    free(mounts[j].target);
  }
  free((void *)mounts);
  free((void *)binds);
  free((void *)envs);
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 1;
}

static void usage(void) {
  printf(
      "Uso: %s <comando> [opciones]\n\n"
      "Gestión de contenedores:\n"
      "  install -n=<nombre> <url>       Descarga y extrae un rootfs .tar.gz\n"
      "    alias: add, i, in, ins; -a, --architecture [ARCH]; ver '%s install "
      "-h'\n"
      "  list                            Lista los contenedores instalados\n"
      "  list --images                   Lista las descargas en caché\n"
      "  login <nombre> [opciones] [-- <comando>...]\n"
      "    -u, --user [USER]             'name', 'name:group', 'uid' o "
      "'uid:gid'\n"
      "    --isolated, --minimal         Sin resolv.conf ni /tmp (solo /proc, "
      "/sys, /dev)\n"
      "    --shared-tmp                  Bind de /tmp del host (tiene "
      "prioridad)\n"
      "    -b, --bind SRC:DEST           Bind extra (repetible)\n"
      "    -w, --work-dir PATH           Directorio inicial dentro del rootfs\n"
      "    -e, --env VAR=VALOR           Variable de entorno (repetible)\n"
      "    --get-cmd                     Imprime los comandos y sale sin "
      "ejecutar\n"
      "    El comando tras '--' se resuelve DENTRO del rootfs:\n"
      "      -- <binario>                nombre: se busca en el PATH del "
      "contenedor\n"
      "      -- /ruta/absoluta           ruta absoluta dentro del rootfs\n"
      "    Ejemplos: -- sh -c 'echo hola' | -- /bin/sh -c 'echo hola' | "
      "-- fish -c 'echo hola'\n"
      "    Sirve para cualquier intérprete instalado: bash, fish, python3, "
      "etc.\n"
      "  remove <nombre>                 Borra un contenedor o una imagen en "
      "caché. ¡Destruye datos!\n"
      "  rename <viejo> <nuevo>          Renombra un contenedor\n"
      "  reset <nombre>                  Reinstala desde su URL original. "
      "¡Destruye datos!\n"
      "  backup <nombre> [archivo]       Guarda el contenedor como TAR.gz\n"
      "  restore <nombre> <archivo>      Restaura desde un TAR.gz. ¡Destruye "
      "datos!\n"
      "  clear-cache                     Borra las descargas en caché\n"
      "  help                            Muestra esta ayuda\n\n"
      "Variables: XROOT_DISTRO_ROOT (por defecto %s), XROOT_DISTRO_SHA256\n"
      "Binario corto: '%s' (mismo comportamiento, nombre más breve)\n\n"
      "Ejemplo (Alpine 3.24.2 x86_64):\n"
      "  make build NAME=alpine URL='%s' SHA256='%s'\n",
      PROGRAM_NAME, PROGRAM_NAME, DEFAULT_ROOT, ALIAS_NAME, EXAMPLE_URL,
      EXAMPLE_SHA);
}

int main(int argc, char **argv) {
  const char *env = getenv("XROOT_DISTRO_ROOT");
  if (env && *env)
    g_root = env;
  env = getenv("XROOT_DISTRO_SHA256");
  if (env && *env)
    g_sha = env;
  if (argc < 2) {
    usage();
    return 0;
  }
  const char *cmd = argv[1];
  int rest = argc - 2;
  char **args = &argv[2];
  if (!strcmp(cmd, "install") || !strcmp(cmd, "add") || !strcmp(cmd, "i") ||
      !strcmp(cmd, "in") || !strcmp(cmd, "ins")) {
    cmd_install(rest, args);
    return 0;
  }
  if (!strcmp(cmd, "list")) {
    cmd_list(rest, args);
    return 0;
  }
  if (!strcmp(cmd, "login")) {
    return cmd_login(rest, args);
  }
  if (!strcmp(cmd, "remove")) {
    cmd_remove(rest, args);
    return 0;
  }
  if (!strcmp(cmd, "rename")) {
    cmd_rename(rest, args);
    return 0;
  }
  if (!strcmp(cmd, "reset")) {
    cmd_reset(rest, args);
    return 0;
  }
  if (!strcmp(cmd, "backup")) {
    cmd_backup(rest, args);
    return 0;
  }
  if (!strcmp(cmd, "restore")) {
    cmd_restore(rest, args);
    return 0;
  }
  if (!strcmp(cmd, "clear-cache")) {
    cmd_clear_cache();
    return 0;
  }
  if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
    usage();
    return 0;
  }
  die("comando desconocido: %s (usa 'help')", cmd);
  return 1;
}
