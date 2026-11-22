/*
 * discord-pipe-bridge - makes the native Linux Discord client reachable from
 * Wine apps, so Rich Presence sent by e.g. NeoPresence shows up on the real
 * profile instead of needing a stub pipe server.
 *
 * Discord on Linux listens on a unix socket ($XDG_RUNTIME_DIR/discord-ipc-0),
 * while Windows RPC clients look for the named pipe \\.\pipe\discord-ipc-0.
 * Wine's ws2_32 has no AF_UNIX (socket() fails with WSAEAFNOSUPPORT), but a
 * winegcc binary links the unix side directly, so this single process is the
 * whole bridge: it serves the pipe inside the prefix and relays every byte to
 * and from the real Discord socket.
 *
 * Build: ./build.sh
 * Run:   wine dist/discord-pipe-bridge.exe
 *
 * Environment:
 *   DISCORD_IPC_SOCKET  explicit unix socket path (default: auto-detect)
 *   BRIDGE_DEBUG=1      dump relayed bytes to the log
 *
 * The pipe is byte-for-byte transparent, so Discord's framing and handshake
 * stay untouched; one pipe client maps to one Discord connection, and the
 * bridge waits for the next client after a disconnect (NeoPresence and Discord
 * can both restart without restarting the bridge).
 */

#define _GNU_SOURCE
/* No winsock: this is the unix side of the process, and winsock.h's bind()/
 * connect()/socket() prototypes would clash with the real unix ones. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define PIPE_NAME "\\\\.\\pipe\\discord-ipc-0"
#define BUF_SIZE 8192
#define SOCKET_RETRIES 10
#define SOCKET_RETRY_MS 300
#define SOCKET_POLL_MS 200

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static int g_sock = -1;
static volatile int g_stop;
static int g_debug;

static void log_line(const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32];
    va_list ap;

    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", stamp);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void dump(const char *dir, const char *buf, int len)
{
    int i;

    if (!g_debug) return;
    fprintf(stderr, "    %s %d:", dir, len);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        fputc(c >= 32 && c < 127 ? c : '.', stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

static int connect_unix(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    if (strlen(path) >= sizeof addr.sun_path) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Native Discord, Flatpak, Snap and Vesktop all use one of these layouts. */
static int open_discord_socket(char *found, size_t found_len)
{
    static const char *templates[] = {
        "%s/discord-ipc-%d",
        "%s/app/com.discordapp.Discord/discord-ipc-%d",
        "%s/snap.discord/discord-ipc-%d",
        "%s/snap.discord-canary/discord-ipc-%d",
    };
    const char *explicit_path = getenv("DISCORD_IPC_SOCKET");
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    char base[512];
    char path[256];
    size_t t;
    int idx, pass;

    if (explicit_path && *explicit_path) {
        int fd = connect_unix(explicit_path);
        if (fd >= 0) {
            snprintf(found, found_len, "%s", explicit_path);
            return fd;
        }
        log_line("DISCORD_IPC_SOCKET=%s: %s", explicit_path, strerror(errno));
    }

    if (runtime_dir && *runtime_dir) snprintf(base, sizeof base, "%s", runtime_dir);
    else snprintf(base, sizeof base, "/run/user/%u", (unsigned)getuid());

    for (pass = 0; pass < 2; pass++) {
        int first = pass == 0 ? 0 : 1;
        int last = pass == 0 ? 0 : 9;
        for (idx = first; idx <= last; idx++) {
            for (t = 0; t < sizeof templates / sizeof templates[0]; t++) {
                int fd;
                snprintf(path, sizeof path, templates[t], base, idx);
                fd = connect_unix(path);
                if (fd >= 0) {
                    snprintf(found, found_len, "%s", path);
                    return fd;
                }
            }
        }
    }
    return -1;
}

static int write_all_pipe(const char *buf, int len)
{
    int done = 0;

    while (done < len) {
        DWORD written = 0;
        if (!WriteFile(g_pipe, buf + done, (DWORD)(len - done), &written, NULL)) return -1;
        if (written == 0) return -1;
        done += (int)written;
    }
    return 0;
}

static int write_all_socket(const char *buf, int len)
{
    int done = 0;

    while (done < len) {
        ssize_t n = write(g_sock, buf + done, (size_t)(len - done));
        if (n > 0) {
            done += (int)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* Discord -> pipe. Ends when the socket closes/closes or the client is gone. */
static DWORD WINAPI socket_to_pipe(LPVOID unused)
{
    char buf[BUF_SIZE];

    (void)unused;
    while (!g_stop) {
        ssize_t n = read(g_sock, buf, sizeof buf);
        if (n > 0) {
            dump("discord->pipe", buf, (int)n);
            if (write_all_pipe(buf, (int)n) != 0) break;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n == 0) log_line("discord closed the socket");
        else log_line("socket read failed: %s", strerror(errno));
        /* Unblock the pipe side so the main loop can move on. */
        if (g_pipe != INVALID_HANDLE_VALUE) DisconnectNamedPipe(g_pipe);
        break;
    }
    return 0;
}

static void serve_client(HANDLE pipe)
{
    char path[256] = "";
    char buf[BUF_SIZE];
    HANDLE thread;
    int fd = -1;
    int attempt;

    for (attempt = 0; attempt < SOCKET_RETRIES && fd < 0 && !g_stop; attempt++) {
        if (attempt > 0) Sleep(SOCKET_RETRY_MS);
        fd = open_discord_socket(path, sizeof path);
    }
    if (fd < 0) {
        log_line("no Linux Discord socket found; dropping this client so it can retry");
        DisconnectNamedPipe(pipe);
        return;
    }

    /* Poll rather than block, so a dead socket never hangs the relay thread. */
    {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = SOCKET_POLL_MS * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }

    log_line("connected to %s", path);
    g_pipe = pipe;
    g_sock = fd;
    g_stop = 0;

    thread = CreateThread(NULL, 0, socket_to_pipe, NULL, 0, NULL);
    if (thread == NULL) {
        log_line("CreateThread failed: %lu", GetLastError());
        close(fd);
        g_sock = -1;
        DisconnectNamedPipe(pipe);
        return;
    }

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(pipe, buf, sizeof buf, &got, NULL)) break;
        if (got == 0) break;
        dump("pipe->discord", buf, (int)got);
        if (write_all_socket(buf, (int)got) != 0) {
            log_line("socket write failed: %s", strerror(errno));
            break;
        }
    }

    g_stop = 1;
    DisconnectNamedPipe(pipe);
    /* Closing the socket ends the relay thread's poll loop. */
    close(fd);
    g_sock = -1;
    WaitForSingleObject(thread, 2000);
    CloseHandle(thread);
    g_pipe = INVALID_HANDLE_VALUE;
    log_line("client disconnected; waiting for the next one");
}

int main(void)
{
    HANDLE mutex;

    g_debug = getenv("BRIDGE_DEBUG") != NULL && strcmp(getenv("BRIDGE_DEBUG"), "0") != 0;

    mutex = CreateMutexA(NULL, FALSE, "Local\\discord-pipe-bridge");
    if (mutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        log_line("bridge is already running in this prefix; nothing to do");
        return 0;
    }

    log_line("serving %s for the native Linux Discord socket", PIPE_NAME);

    for (;;) {
        HANDLE pipe = CreateNamedPipeA(PIPE_NAME,
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1, BUF_SIZE, BUF_SIZE, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            log_line("CreateNamedPipe failed: %lu", err);
            /* Someone else (another bridge, Steam, a game) owns the name. */
            if (err == ERROR_ACCESS_DENIED) {
                log_line("%s is owned by another process; exiting", PIPE_NAME);
                return 0;
            }
            Sleep(1000);
            continue;
        }

        if (!ConnectNamedPipe(pipe, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_PIPE_CONNECTED) {
                log_line("ConnectNamedPipe failed: %lu", err);
                CloseHandle(pipe);
                Sleep(500);
                continue;
            }
        }

        log_line("client connected on %s", PIPE_NAME);
        serve_client(pipe);
        CloseHandle(pipe);
    }
}
