/* iexec.c — run a GUI program on the active console desktop from a session-0
 * SMB-exec (netexec) context.  The agent-LESS console-session launcher.
 *
 *   nxc smb <ip> -u Administrator -p '' --exec-method smbexec \
 *       -x 'C:\probe\iexec.exe <command line>'
 *
 * launches <command line> in the *interactive* logon session on WinSta0\Default,
 * visible on the (virtual) desktop, so a screenshot tool — OR the host's QMP
 * `screendump` — captures the real screen and GUI apps appear where they can be
 * seen.  No persistent agent: each call is a one-shot SMB-exec.
 *
 * Works on XP (active console = session 0) AND Win7+ (console = session 1+):
 * WTSGetActiveConsoleSessionId picks the console session, WTSQueryUserToken grabs
 * the logged-in user's token, CreateProcessAsUser drops the child onto that
 * session's interactive desktop.
 *
 * REQUIRES: caller is SYSTEM (WTSQueryUserToken needs SeTcbPrivilege) and a user is
 * logged in at the console (autologon).  If netexec runs us as plain Administrator,
 * WTSQueryUserToken fails 1314 (privilege not held) -> message says so.  Hence the
 * `--exec-method smbexec` requirement (LocalSystem), not the default exec method.
 *
 * Build (32-bit, XP subsystem) — via the slopstudio flake's mingw32 cross:
 *   i686-w64-mingw32-gcc iexec.c -o iexec.exe -lwtsapi32 -luserenv -O2 -s -mconsole \
 *     -D_WIN32_WINNT=0x0501 -Wl,--major-subsystem-version=5,--minor-subsystem-version=1
 *   (or just `make` in this dir inside `nix develop`).
 *
 * Usage: iexec [-w<secs>] <command line...>
 *   -w<secs>  wait up to <secs> for the child to exit, then return its exit code
 *             (use for screenshot tools so the image is fully written before fetch);
 *             omit for persistent GUI apps (launcher, installer) - fire and forget.
 *
 * Provenance: vendored from ../retro-hardware/projects/xp-remote-probe/xp/iexec.c
 * (validated agent-less on real XP boxes).  Kept here so the slopstudio XP capture
 * harness is self-contained and builds from this repo's flake.
 */
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define LOGFILE "C:\\probe\\out\\iexec.log"

static void logln(const char *fmt, ...) {
    FILE *f = fopen(LOGFILE, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

int main(void) {
    /* Reconstruct the raw command line that follows our own program token (and an
       optional -w<secs> flag).  Passing the verbatim tail to CreateProcessAsUser
       keeps argument quoting intact. */
    char *cl = GetCommandLineA();
    char *p = cl;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p == '"') p++; }
    else { while (*p && *p != ' ' && *p != '\t') p++; }
    while (*p == ' ' || *p == '\t') p++;

    int waitsecs = 0;
    if (p[0] == '-' && p[1] == 'w') {
        waitsecs = atoi(p + 2);
        while (*p && *p != ' ' && *p != '\t') p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (!*p) { printf("iexec: no command given\n"); return 2; }

    DWORD sid = WTSGetActiveConsoleSessionId();
    if (sid == (DWORD)-1) { printf("iexec: no active console session\n"); logln("no console session"); return 3; }

    HANDLE hTok = NULL;
    if (!WTSQueryUserToken(sid, &hTok)) {
        DWORD e = GetLastError();
        printf("iexec: WTSQueryUserToken(sid=%lu) failed %lu (need SYSTEM + a logged-in console user)\n",
               (unsigned long)sid, (unsigned long)e);
        logln("WTSQueryUserToken failed %lu sid=%lu", (unsigned long)e, (unsigned long)sid);
        return 4;
    }

    HANDLE hDup = NULL;
    if (!DuplicateTokenEx(hTok, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hDup)) {
        printf("iexec: DuplicateTokenEx failed %lu\n", (unsigned long)GetLastError());
        CloseHandle(hTok); return 5;
    }

    LPVOID env = NULL;
    BOOL haveEnv = CreateEnvironmentBlock(&env, hDup, FALSE);  /* best-effort user env */

    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.lpDesktop = (LPSTR)"WinSta0\\Default";
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    char *cmd = _strdup(p);  /* CreateProcessAsUser may modify lpCommandLine -> writable */
    DWORD flags = haveEnv ? CREATE_UNICODE_ENVIRONMENT : 0;

    BOOL ok = CreateProcessAsUserA(hDup, NULL, cmd, NULL, NULL, FALSE,
                                   flags, haveEnv ? env : NULL, NULL, &si, &pi);
    if (!ok) {
        DWORD e = GetLastError();
        printf("iexec: CreateProcessAsUser failed %lu cmd=[%s]\n", (unsigned long)e, cmd);
        logln("CreateProcessAsUser failed %lu cmd=[%s]", (unsigned long)e, cmd);
        if (haveEnv) DestroyEnvironmentBlock(env);
        CloseHandle(hDup); CloseHandle(hTok); return 6;
    }

    printf("iexec: launched pid=%lu on console session %lu\n",
           (unsigned long)pi.dwProcessId, (unsigned long)sid);
    logln("launched sid=%lu pid=%lu wait=%d cmd=[%s]",
          (unsigned long)sid, (unsigned long)pi.dwProcessId, waitsecs, cmd);

    DWORD code = 0;
    if (waitsecs > 0) {
        WaitForSingleObject(pi.hProcess, (DWORD)waitsecs * 1000);
        GetExitCodeProcess(pi.hProcess, &code);
    }

    if (haveEnv) DestroyEnvironmentBlock(env);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    CloseHandle(hDup); CloseHandle(hTok);
    return (int)code;
}
