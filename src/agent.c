#include "ash.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PROVIDERS 32
#define DEFAULT_MAX_TOKENS 1024
#define DEFAULT_TIMEOUT 60

typedef struct {
    char name[64];
    char type[16];
    char model[128];
    char url[512];
    char key_env[64];
    long max_tokens;
} AgentProvider;

static AgentProvider providers[MAX_PROVIDERS];
static int provider_total;

typedef struct {
    const char *p;
    const char *end;
} JsonCursor;

static void j_ws(JsonCursor *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static int j_expect(JsonCursor *c, char ch)
{
    j_ws(c);
    if (c->p < c->end && *c->p == ch) {
        c->p++;
        return 0;
    }
    return -1;
}

static void j_utf8(StrBuf *sb, unsigned cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    sb_append_n(sb, b, n);
}

static int j_hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9')
            v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            v |= (unsigned)(ch - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

static int j_string(JsonCursor *c, StrBuf *out)
{
    j_ws(c);
    if (c->p >= c->end || *c->p != '"')
        return -1;
    c->p++;
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch == '"')
            return 0;
        if (ch != '\\') {
            if (out)
                sb_append_char(out, (char)ch);
            continue;
        }
        if (c->p >= c->end)
            return -1;
        char esc = *c->p++;
        char lit = 0;
        switch (esc) {
        case '"':
            lit = '"';
            break;
        case '\\':
            lit = '\\';
            break;
        case '/':
            lit = '/';
            break;
        case 'b':
            lit = '\b';
            break;
        case 'f':
            lit = '\f';
            break;
        case 'n':
            lit = '\n';
            break;
        case 'r':
            lit = '\r';
            break;
        case 't':
            lit = '\t';
            break;
        case 'u': {
            unsigned cp;
            if (c->end - c->p < 4 || j_hex4(c->p, &cp))
                return -1;
            c->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && c->end - c->p >= 6 &&
                c->p[0] == '\\' && c->p[1] == 'u') {
                unsigned lo;
                if (j_hex4(c->p + 2, &lo) == 0 && lo >= 0xDC00 &&
                    lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    c->p += 6;
                }
            }
            if (out)
                j_utf8(out, cp);
            continue;
        }
        default:
            return -1;
        }
        if (out)
            sb_append_char(out, lit);
    }
    return -1;
}

static int j_value(JsonCursor *c, int depth)
{
    if (depth > 64)
        return -1;
    j_ws(c);
    if (c->p >= c->end)
        return -1;
    char ch = *c->p;
    if (ch == '"')
        return j_string(c, NULL);
    if (ch == '{') {
        c->p++;
        j_ws(c);
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return 0;
        }
        for (;;) {
            if (j_string(c, NULL))
                return -1;
            if (j_expect(c, ':'))
                return -1;
            if (j_value(c, depth + 1))
                return -1;
            j_ws(c);
            if (c->p < c->end && *c->p == ',') {
                c->p++;
                continue;
            }
            return j_expect(c, '}');
        }
    }
    if (ch == '[') {
        c->p++;
        j_ws(c);
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return 0;
        }
        for (;;) {
            if (j_value(c, depth + 1))
                return -1;
            j_ws(c);
            if (c->p < c->end && *c->p == ',') {
                c->p++;
                continue;
            }
            return j_expect(c, ']');
        }
    }
    if (ch == 't') {
        if (c->end - c->p >= 4 && memcmp(c->p, "true", 4) == 0) {
            c->p += 4;
            return 0;
        }
        return -1;
    }
    if (ch == 'f') {
        if (c->end - c->p >= 5 && memcmp(c->p, "false", 5) == 0) {
            c->p += 5;
            return 0;
        }
        return -1;
    }
    if (ch == 'n') {
        if (c->end - c->p >= 4 && memcmp(c->p, "null", 4) == 0) {
            c->p += 4;
            return 0;
        }
        return -1;
    }
    const char *start = c->p;
    while (c->p < c->end &&
           (isdigit((unsigned char)*c->p) || *c->p == '-' || *c->p == '+' ||
            *c->p == '.' || *c->p == 'e' || *c->p == 'E'))
        c->p++;
    return c->p > start ? 0 : -1;
}

static int j_descend(JsonCursor *c, const char *want)
{
    j_ws(c);
    if (c->p >= c->end)
        return -1;
    if (*c->p == '{') {
        c->p++;
        j_ws(c);
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return -1;
        }
        for (;;) {
            StrBuf key;
            sb_init(&key);
            if (j_string(c, &key)) {
                sb_free(&key);
                return -1;
            }
            int hit = strcmp(key.data, want) == 0;
            sb_free(&key);
            if (j_expect(c, ':'))
                return -1;
            if (hit)
                return 0;
            if (j_value(c, 0))
                return -1;
            j_ws(c);
            if (c->p < c->end && *c->p == ',') {
                c->p++;
                continue;
            }
            return -1;
        }
    }
    if (*c->p == '[') {
        char *endp = NULL;
        long index = strtol(want, &endp, 10);
        if (!want[0] || !endp || *endp || index < 0)
            return -1;
        c->p++;
        for (long i = 0;; i++) {
            j_ws(c);
            if (c->p >= c->end || *c->p == ']')
                return -1;
            if (i == index)
                return 0;
            if (j_value(c, 0))
                return -1;
            j_ws(c);
            if (c->p < c->end && *c->p == ',') {
                c->p++;
                continue;
            }
            return -1;
        }
    }
    return -1;
}

static int j_lookup(const char *json, const char *path, StrBuf *out)
{
    JsonCursor c;
    c.p = json;
    c.end = json + strlen(json);
    j_ws(&c);
    const char *p = path;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        char key[128];
        if (n == 0 || n >= sizeof(key))
            return -1;
        memcpy(key, p, n);
        key[n] = '\0';
        if (j_descend(&c, key))
            return -1;
        if (!slash)
            break;
        p = slash + 1;
    }
    return j_string(&c, out);
}

static int anthropic_text(const char *json, StrBuf *out)
{
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof(path), "content/%d/type", i);
        StrBuf type;
        sb_init(&type);
        int is_text = j_lookup(json, path, &type) == 0 &&
                      strcmp(type.data, "text") == 0;
        sb_free(&type);
        if (!is_text)
            continue;
        snprintf(path, sizeof(path), "content/%d/text", i);
        if (j_lookup(json, path, out) == 0)
            return 0;
    }
    return -1;
}

static void json_escape(StrBuf *out, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':
            sb_append(out, "\\\"");
            break;
        case '\\':
            sb_append(out, "\\\\");
            break;
        case '\b':
            sb_append(out, "\\b");
            break;
        case '\f':
            sb_append(out, "\\f");
            break;
        case '\n':
            sb_append(out, "\\n");
            break;
        case '\r':
            sb_append(out, "\\r");
            break;
        case '\t':
            sb_append(out, "\\t");
            break;
        default:
            if (*p < 0x20) {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", *p);
                sb_append(out, buf);
            } else {
                sb_append_char(out, (char)*p);
            }
        }
    }
}

static int normalize_type(const char *in, char *out, size_t size)
{
    if (strcmp(in, "openai") == 0 || strcmp(in, "mistral") == 0 ||
        strcmp(in, "opencode") == 0 || strcmp(in, "zen") == 0) {
        snprintf(out, size, "openai");
        return 0;
    }
    if (strcmp(in, "anthropic") == 0 || strcmp(in, "claude") == 0) {
        snprintf(out, size, "anthropic");
        return 0;
    }
    return -1;
}

static int load_providers(void)
{
    const char *path = getenv("ASHAGENT");
    char def[PATH_MAX];
    if (!path || !*path) {
        const char *home = getenv("HOME");
        snprintf(def, sizeof(def), "%s/.ashagent",
                 home && *home ? home : ".");
        path = def;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ask: cannot read %s\n", path);
        return -1;
    }
    provider_total = 0;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int lineno = 0;
    int bad = 0;
    while ((n = getline(&line, &cap, f)) > 0) {
        lineno++;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '#')
            continue;
        char *tok[7];
        int nt = 0;
        while (*p && nt < 7) {
            tok[nt++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p)
                *p++ = '\0';
            while (*p == ' ' || *p == '\t')
                p++;
        }
        if (nt < 5 || nt > 6) {
            fprintf(stderr,
                    "ask: %s:%d: expected: name type model url key_env [max_tokens]\n",
                    path, lineno);
            bad = 1;
            continue;
        }
        if (provider_total == MAX_PROVIDERS) {
            fprintf(stderr, "ask: %s: too many providers (max %d)\n", path,
                    MAX_PROVIDERS);
            bad = 1;
            break;
        }
        if (strlen(tok[0]) >= sizeof(providers[0].name) ||
            strlen(tok[2]) >= sizeof(providers[0].model) ||
            strlen(tok[3]) >= sizeof(providers[0].url) ||
            strlen(tok[4]) >= sizeof(providers[0].key_env)) {
            fprintf(stderr, "ask: %s:%d: field too long\n", path, lineno);
            bad = 1;
            continue;
        }
        AgentProvider *ap = &providers[provider_total];
        if (normalize_type(tok[1], ap->type, sizeof(ap->type))) {
            fprintf(stderr, "ask: %s:%d: unknown type '%s'\n", path, lineno,
                    tok[1]);
            bad = 1;
            continue;
        }
        snprintf(ap->name, sizeof(ap->name), "%s", tok[0]);
        snprintf(ap->model, sizeof(ap->model), "%s", tok[2]);
        snprintf(ap->url, sizeof(ap->url), "%s", tok[3]);
        snprintf(ap->key_env, sizeof(ap->key_env), "%s", tok[4]);
        ap->max_tokens = DEFAULT_MAX_TOKENS;
        if (nt == 6) {
            char *endp = NULL;
            long v = strtol(tok[5], &endp, 10);
            if (endp == tok[5] || *endp || v <= 0 || v > 1000000) {
                fprintf(stderr, "ask: %s:%d: invalid max_tokens '%s'\n", path,
                        lineno, tok[5]);
                bad = 1;
                continue;
            }
            ap->max_tokens = v;
        }
        provider_total++;
    }
    free(line);
    fclose(f);
    return bad ? -1 : 0;
}

static AgentProvider *select_provider(const char *want_name,
                                      const char *want_model)
{
    AgentProvider *found = NULL;
    int hits = 0;
    for (int i = 0; i < provider_total; i++) {
        if (want_name && strcmp(providers[i].name, want_name) != 0)
            continue;
        if (want_model && strcmp(providers[i].model, want_model) != 0)
            continue;
        found = &providers[i];
        hits++;
    }
    if (hits == 1)
        return found;
    if (hits == 0) {
        fprintf(stderr, "ask: no provider");
        if (want_name)
            fprintf(stderr, " named '%s'", want_name);
        if (want_model)
            fprintf(stderr, " for model '%s'", want_model);
        fprintf(stderr, "\n");
        for (int i = 0; i < provider_total; i++)
            fprintf(stderr, "  %s\t%s\t%s\n", providers[i].name,
                    providers[i].type, providers[i].model);
    } else {
        fprintf(stderr, "ask: %d providers match; add -p NAME\n", hits);
    }
    return NULL;
}

static int write_fd_all(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static int make_temp(char *path, size_t size)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";
    int n = snprintf(path, size, "%s/ashagent-XXXXXX", dir);
    if (n < 0 || (size_t)n >= size) {
        fprintf(stderr, "ask: temporary path too long\n");
        return -1;
    }
    int fd = mkstemp(path);
    if (fd < 0)
        fprintf(stderr, "ask: cannot create temporary file: %s\n",
                strerror(errno));
    return fd;
}

static int ask_provider(const AgentProvider *ap, const char *prompt)
{
    const char *key = getenv(ap->key_env);
    if (!key || !*key) {
        fprintf(stderr, "ask: %s: set %s\n", ap->name, ap->key_env);
        return 1;
    }
    if (strchr(key, '\n') || strchr(key, '\r')) {
        fprintf(stderr, "ask: %s: %s contains a newline\n", ap->name,
                ap->key_env);
        return 1;
    }
    char url[600];
    size_t base = strlen(ap->url);
    while (base > 0 && ap->url[base - 1] == '/')
        base--;
    const char *suffix = strcmp(ap->type, "anthropic") == 0 ? "/messages"
                                                            : "/chat/completions";
    int un = snprintf(url, sizeof(url), "%.*s%s", (int)base, ap->url, suffix);
    if (un < 0 || (size_t)un >= sizeof(url)) {
        fprintf(stderr, "ask: %s: url too long\n", ap->name);
        return 1;
    }

    StrBuf body;
    sb_init(&body);
    sb_append(&body, "{\"model\":\"");
    json_escape(&body, ap->model);
    sb_append(&body, "\",");
    if (strcmp(ap->type, "anthropic") == 0) {
        char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "\"max_tokens\":%ld,", ap->max_tokens);
        sb_append(&body, nbuf);
    }
    sb_append(&body, "\"messages\":[{\"role\":\"user\",\"content\":\"");
    json_escape(&body, prompt);
    sb_append(&body, "\"}]}");

    StrBuf headers;
    sb_init(&headers);
    if (strcmp(ap->type, "anthropic") == 0) {
        sb_append(&headers, "x-api-key: ");
        sb_append(&headers, key);
        sb_append(&headers, "\nanthropic-version: 2023-06-01\n");
    } else {
        sb_append(&headers, "Authorization: Bearer ");
        sb_append(&headers, key);
        sb_append(&headers, "\n");
    }
    sb_append(&headers, "Content-Type: application/json\n");

    char hdr_path[PATH_MAX] = "";
    char body_path[PATH_MAX] = "";
    char resp_path[PATH_MAX] = "";
    int hdr_fd = make_temp(hdr_path, sizeof(hdr_path));
    int body_fd = -1;
    int status = 1;
    StrBuf resp;
    sb_init(&resp);
    if (hdr_fd < 0)
        goto done;
    body_fd = make_temp(body_path, sizeof(body_path));
    if (body_fd < 0)
        goto done;
    int resp_fd = make_temp(resp_path, sizeof(resp_path));
    if (resp_fd < 0)
        goto done;
    close(resp_fd);
    if (write_fd_all(hdr_fd, headers.data, headers.len) ||
        write_fd_all(body_fd, body.data, body.len))
        goto done;
    close(hdr_fd);
    hdr_fd = -1;
    close(body_fd);
    body_fd = -1;

    const char *timeout_env = getenv("ASHAGENT_TIMEOUT");
    long timeout = DEFAULT_TIMEOUT;
    if (timeout_env && *timeout_env) {
        char *endp = NULL;
        long v = strtol(timeout_env, &endp, 10);
        if (endp != timeout_env && !*endp && v >= 1 && v <= 600)
            timeout = v;
    }
    char timeout_buf[16];
    snprintf(timeout_buf, sizeof(timeout_buf), "%ld", timeout);
    char hdr_arg[PATH_MAX + 2];
    char body_arg[PATH_MAX + 2];
    snprintf(hdr_arg, sizeof(hdr_arg), "@%s", hdr_path);
    snprintf(body_arg, sizeof(body_arg), "@%s", body_path);

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "ask: fork failed: %s\n", strerror(errno));
        goto done;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        char *args[20];
        int i = 0;
        args[i++] = "curl";
        args[i++] = "-sS";
        args[i++] = "--connect-timeout";
        args[i++] = "10";
        args[i++] = "--max-time";
        args[i++] = timeout_buf;
        args[i++] = "-X";
        args[i++] = "POST";
        args[i++] = "-H";
        args[i++] = hdr_arg;
        args[i++] = "--data-binary";
        args[i++] = body_arg;
        args[i++] = "-o";
        args[i++] = resp_path;
        args[i++] = url;
        args[i] = NULL;
        execvp("curl", args);
        _exit(127);
    }
    int wst = 0;
    while (waitpid(pid, &wst, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "ask: wait failed: %s\n", strerror(errno));
            goto done;
        }
    }
    int code = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128 + WTERMSIG(wst);

    FILE *rf = fopen(resp_path, "r");
    if (rf) {
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), rf)) > 0)
            sb_append_n(&resp, buf, got);
        fclose(rf);
    }
    if (code == 127) {
        fprintf(stderr, "ask: cannot run curl (is it installed?)\n");
        goto done;
    }
    if (resp.len == 0) {
        fprintf(stderr, "ask: %s: request failed (curl exit %d)\n", ap->name,
                code);
        goto done;
    }

    StrBuf text;
    sb_init(&text);
    int ok;
    if (strcmp(ap->type, "anthropic") == 0)
        ok = anthropic_text(resp.data, &text) == 0;
    else
        ok = j_lookup(resp.data, "choices/0/message/content", &text) == 0;
    if (!ok) {
        StrBuf err;
        sb_init(&err);
        if (j_lookup(resp.data, "error/message", &err) == 0 && err.len > 0)
            fprintf(stderr, "ask: %s: %s\n", ap->name, err.data);
        else
            fprintf(stderr, "ask: %s: unexpected response (curl exit %d)\n",
                    ap->name, code);
        sb_free(&err);
        sb_free(&text);
        goto done;
    }
    fwrite(text.data, 1, text.len, stdout);
    if (text.len == 0 || text.data[text.len - 1] != '\n')
        fputc('\n', stdout);
    fflush(stdout);
    sb_free(&text);
    status = 0;

done:
    if (hdr_fd >= 0)
        close(hdr_fd);
    if (body_fd >= 0)
        close(body_fd);
    sb_free(&body);
    sb_free(&headers);
    sb_free(&resp);
    if (hdr_path[0])
        unlink(hdr_path);
    if (body_path[0])
        unlink(body_path);
    if (resp_path[0])
        unlink(resp_path);
    return status;
}

static void print_usage(FILE *out)
{
    fprintf(out,
            "usage: ask -m MODEL [-p NAME] PROMPT...\n"
            "       ask -p NAME PROMPT...\n"
            "       ask PROVIDER/MODEL PROMPT...\n"
            "       ask -l\n"
            "config: ~/.ashagent (or $ASHAGENT)\n");
}

int agent_ask(char **argv)
{
    const char *want_name = NULL;
    const char *want_model = NULL;
    int list = 0;
    int i = 1;
    for (; argv[i]; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            list = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            if (!argv[i + 1]) {
                print_usage(stderr);
                return 2;
            }
            want_name = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0) {
            if (!argv[i + 1]) {
                print_usage(stderr);
                return 2;
            }
            want_model = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            break;
        }
    }
    if (load_providers() < 0)
        return 1;
    if (list) {
        for (int k = 0; k < provider_total; k++)
            printf("%s\t%s\t%s\t%s\n", providers[k].name, providers[k].type,
                   providers[k].model, providers[k].url);
        return 0;
    }
    if (!want_model && !want_name && argv[i]) {
        const char *slash = strchr(argv[i], '/');
        if (slash && slash != argv[i]) {
            size_t n = (size_t)(slash - argv[i]);
            char name[64];
            if (n < sizeof(name)) {
                memcpy(name, argv[i], n);
                name[n] = '\0';
                for (int k = 0; k < provider_total; k++) {
                    if (strcmp(providers[k].name, name) == 0) {
                        want_name = providers[k].name;
                        want_model = slash + 1;
                        i++;
                        break;
                    }
                }
            }
        }
    }
    if (!want_model && !want_name) {
        print_usage(stderr);
        return 2;
    }
    AgentProvider *ap = select_provider(want_name, want_model);
    if (!ap)
        return 1;
    StrBuf prompt;
    sb_init(&prompt);
    for (; argv[i]; i++) {
        if (prompt.len)
            sb_append_char(&prompt, ' ');
        sb_append(&prompt, argv[i]);
    }
    if (prompt.len == 0) {
        fprintf(stderr, "ask: missing prompt\n");
        sb_free(&prompt);
        return 2;
    }
    int status = ask_provider(ap, prompt.data);
    sb_free(&prompt);
    return status;
}
