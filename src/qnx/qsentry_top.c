/*
 * qsentry-top - live terminal dashboard for qsentryd.
 * Reads /dev/qsentry (plain text) and sends commands by writing to it, so it
 * also works with any file path for testing on non-QNX hosts.
 *
 * keys: 1-4 select channel   s stuck  d drift  p spike  n noise
 *       c clear faults       r reset supervisor   q quit
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NCH 4
#define HIST 48

static const char *names[NCH] = { "speed", "accel", "steer", "yaw" };
static const char *units[NCH] = { "m/s", "m/s2", "rad", "rad/s" };
static float hist[NCH][HIST];
static int   hcount;
static struct termios saved;
static int   raw_on;

static void restore(void)
{
    if (raw_on) tcsetattr(0, TCSANOW, &saved);
    printf("\x1b[0m\x1b[?25h\n");
}

static void raw_mode(void)
{
    if (!isatty(0)) return;
    struct termios t;
    tcgetattr(0, &saved);
    t = saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    raw_on = 1;
    atexit(restore);
}

static int read_status(const char *path, char *buf, size_t sz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t n = 0;
    ssize_t r;
    while (n < sz - 1 && (r = read(fd, buf + n, sz - 1 - n)) > 0) n += (size_t)r;
    close(fd);
    buf[n] = '\0';
    return (int)n;
}

static void send_cmd(const char *path, const char *cmd)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    ssize_t r = write(fd, cmd, strlen(cmd));
    (void)r;
    close(fd);
}

/* find "key=" at line start, copy rest of line */
static int field(const char *st, const char *key, char *out, size_t sz)
{
    size_t kl = strlen(key);
    const char *p = st;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (p[i] && p[i] != '\n' && i < sz - 1) { out[i] = p[i]; i++; }
            out[i] = '\0';
            return 1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    out[0] = '\0';
    return 0;
}

static const char *spark(const float *h, int n, char *out)
{
    static const char *lv[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    float mn = 1e30f, mx = -1e30f;
    for (int i = 0; i < n; i++) { if (h[i] < mn) mn = h[i]; if (h[i] > mx) mx = h[i]; }
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        int k = (mx - mn) > 1e-9f ? (int)((h[i] - mn) / (mx - mn) * 7.0f + 0.5f) : 0;
        strcat(out, lv[k]);
    }
    return out;
}

static void bar(float p, char *out)
{
    int n = (int)(p * 10.0f + 0.5f);
    out[0] = '\0';
    for (int i = 0; i < 10; i++) strcat(out, i < n ? "█" : "·");
}

static void draw(const char *st, int sel, const char *msg)
{
    char state[32], reason[128], event[160], buf[256];
    field(st, "state", state, sizeof(state));
    field(st, "reason", reason, sizeof(reason));
    field(st, "event", event, sizeof(event));
    const char *col = !strcmp(state, "NOMINAL") ? "42;30" : !strcmp(state, "DEGRADED") ? "43;30" : "41;97";

    printf("\x1b[H\x1b[2J");
    printf("\x1b[1mqsentry\x1b[0m  on-device sensor-fault sentinel  \x1b[2m(QNX message passing + tiny neural net)\x1b[0m\n\n");
    printf("  state  \x1b[1;%sm  %-10s \x1b[0m  %s\n\n", col, state, reason);
    printf("  %-3s %-6s %10s  %-*s  %-7s %-10s\n", "", "sensor", "value", HIST, "last 0.5 s", "verdict", "fault prob");

    for (int ch = 0; ch < NCH; ch++) {
        char key[8], line[256], verdict[16] = "?";
        float v = 0, p[5] = { 0 };
        snprintf(key, sizeof(key), "ch%d", ch);
        if (field(st, key, line, sizeof(line))) {
            char *q;
            if ((q = strstr(line, "value="))) v = strtof(q + 6, NULL);
            if ((q = strstr(line, "verdict="))) sscanf(q + 8, "%15s", verdict);
            if ((q = strstr(line, "p="))) sscanf(q + 2, "%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &p[3], &p[4]);
        }
        memmove(hist[ch], hist[ch] + 1, sizeof(float) * (HIST - 1));
        hist[ch][HIST - 1] = v;
        char sp[HIST * 4 + 1], b[64];
        int n = hcount < HIST ? hcount + 1 : HIST;
        bar(1.0f - p[0], b);
        const char *vc = !strcmp(verdict, "ok") ? "32" : "1;31";
        printf(" %s%d %-6s %8.3f %-5s %s%*s  \x1b[%sm%-7s\x1b[0m %s %.2f\n",
               sel == ch ? "\x1b[7m>" : " ", ch + 1, names[ch], v, units[ch],
               spark(hist[ch] + HIST - n, n, sp), HIST - n, "", vc, verdict,
               b, 1.0f - p[0]);
        if (sel == ch) printf("\x1b[0m");
    }
    hcount++;

    printf("\n");
    if (field(st, "pipeline_us", buf, sizeof(buf)))    printf("  detection time   us  %s\n", buf);
    if (field(st, "tick_jitter_us", buf, sizeof(buf))) printf("  100 Hz jitter    us  %s\n", buf);
    char gap[32], wd[32], rx[32], smp[32];
    field(st, "sensor_gap_ms", gap, sizeof(gap));
    field(st, "watchdog_ms", wd, sizeof(wd));
    field(st, "reaction_ms", rx, sizeof(rx));
    field(st, "samples", smp, sizeof(smp));
    printf("  sensor stream    samples %s  last gap %s ms  deadline %s ms  watchdog fired at %s ms\n",
           smp, gap, wd, rx);
    printf("  last event       %s\n", event);
    printf("\n  \x1b[2mkeys: 1-4 channel  s stuck  d drift  p spike  n noise  c clear  r reset  q quit\x1b[0m\n");
    if (msg && *msg) printf("  > %s\n", msg);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *path = "/dev/qsentry";
    int once = 0, opt, period_ms = 100;
    while ((opt = getopt(argc, argv, "1f:i:")) != -1) {
        if (opt == '1') once = 1;
        else if (opt == 'f') path = optarg;
        else if (opt == 'i') period_ms = atoi(optarg);
        else { fprintf(stderr, "usage: qsentry-top [-1] [-f path] [-i ms]\n"); return 1; }
    }
    char st[4096], msg[96] = "";
    int sel = 0;
    if (!once) { raw_mode(); printf("\x1b[?25l"); }
    for (;;) {
        if (read_status(path, st, sizeof(st)) < 0) {
            fprintf(stderr, "qsentry-top: cannot read %s (is qsentryd running?)\n", path);
            return 1;
        }
        draw(st, sel, msg);
        if (once) return 0;

        fd_set rf;
        struct timeval tv = { 0, period_ms * 1000 };
        FD_ZERO(&rf);
        FD_SET(0, &rf);
        if (select(1, &rf, NULL, NULL, &tv) > 0) {
            char c;
            if (read(0, &c, 1) == 1) {
                char cmd[64];
                const char *f = NULL;
                switch (c) {
                case '1': case '2': case '3': case '4': sel = c - '1'; break;
                case 's': f = "stuck"; break;
                case 'd': f = "drift"; break;
                case 'p': f = "spike"; break;
                case 'n': f = "noise"; break;
                case 'c': send_cmd(path, "clear\n"); snprintf(msg, sizeof(msg), "clear"); break;
                case 'r': send_cmd(path, "reset\n"); snprintf(msg, sizeof(msg), "reset"); break;
                case 'q': return 0;
                default: break;
                }
                if (f) {
                    snprintf(cmd, sizeof(cmd), "inject %s %s\n", names[sel], f);
                    send_cmd(path, cmd);
                    snprintf(msg, sizeof(msg), "inject %s %s", names[sel], f);
                }
            }
        }
    }
}
