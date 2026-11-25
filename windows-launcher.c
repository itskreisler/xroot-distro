#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

int wmain(int argc, wchar_t **argv) {
  wchar_t *program = L"/usr/local/bin/xroot-distro";
  wchar_t *name = wcsrchr(argv[0], L'\\');
  if (!name)
    name = wcsrchr(argv[0], L'/');
  name = name ? name + 1 : argv[0];
  if (_wcsicmp(name, L"xd.exe") == 0)
    program = L"/usr/local/bin/xd";

  DWORD distro_len = GetEnvironmentVariableW(L"XROOT_DISTRO_WSL_DISTRO", NULL,
                                             0);
  wchar_t *distro = NULL;
  if (distro_len > 0) {
    distro = calloc(distro_len, sizeof *distro);
    if (!distro) {
      fputs("xroot-distro: no se pudo reservar memoria\n", stderr);
      return 1;
    }
    if (GetEnvironmentVariableW(L"XROOT_DISTRO_WSL_DISTRO", distro,
                                distro_len) == 0) {
      fputs("xroot-distro: no se pudo leer XROOT_DISTRO_WSL_DISTRO\n", stderr);
      free(distro);
      return 1;
    }
  }

  wchar_t **wsl_argv = calloc((size_t)argc + 7, sizeof *wsl_argv);
  if (!wsl_argv) {
    fputs("xroot-distro: no se pudo reservar memoria\n", stderr);
    free(distro);
    return 1;
  }

  int i = 0;
  wsl_argv[i++] = L"wsl.exe";
  if (distro && distro[0]) {
    wsl_argv[i++] = L"--distribution";
    wsl_argv[i++] = distro;
  }
  wsl_argv[i++] = L"--user";
  wsl_argv[i++] = L"root";
  wsl_argv[i++] = L"--exec";
  wsl_argv[i++] = program;
  for (int j = 1; j < argc; j++)
    wsl_argv[i++] = argv[j];
  wsl_argv[i] = NULL;

  int status = _wspawnvp(_P_WAIT, L"wsl.exe",
                         (const wchar_t *const *)wsl_argv);
  if (status == -1) {
    if (errno == ENOENT)
      fputs("xroot-distro: no se encontro wsl.exe; instala WSL2 primero\n",
            stderr);
    else
      fprintf(stderr, "xroot-distro: no se pudo iniciar WSL: errno=%d\n",
              errno);
    status = 1;
  }

  free(wsl_argv);
  free(distro);
  return status;
}
