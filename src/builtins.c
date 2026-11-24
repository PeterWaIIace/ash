#include "ash.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *name;
    char *value;
} Alias;

static Alias *aliases;
static int alias_total;
static int alias_capacity;

void alias_set(const char *name, const char *value)
{
    for (int i = 0; i < alias_total; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            free(aliases[i].value);
            aliases[i].value = xstrdup(value);
            return;
        }
    }
    if (alias_total == alias_capacity) {
        alias_capacity = alias_capacity ? alias_capacity * 2 : 16;
        aliases = xrealloc(aliases, alias_capacity * sizeof(Alias));
    }
    aliases[alias_total].name = xstrdup(name);
    aliases[alias_total].value = xstrdup(value);
    alias_total++;
}

const char *alias_get(const char *name)
{
    for (int i = 0; i < alias_total; i++)
        if (strcmp(aliases[i].name, name) == 0)
            return aliases[i].value;
    return NULL;
}

int alias_unset(const char *name)
{
    for (int i = 0; i < alias_total; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            free(aliases[i].name);
            free(aliases[i].value);
            memmove(&aliases[i], &aliases[i + 1],
                    (alias_total - i - 1) * sizeof(Alias));
            alias_total--;
            return 0;
        }
    }
    return -1;
}

int alias_count(void)
{
    return alias_total;
}

const char *alias_name(int index)
{
    return aliases[index].name;
}

void alias_print_all(FILE *out)
{
    for (int i = 0; i < alias_total; i++)
        fprintf(out, "alias %s='%s'\n", aliases[i].name, aliases[i].value);
}

static char *trap_commands[64];
static volatile sig_atomic_t trap_pending_flags[64];

static const struct {
    const char *name;
    int num;
} sig_table[] = {
    { "HUP", SIGHUP },   { "INT", SIGINT },   { "QUIT", SIGQUIT },
    { "ILL", SIGILL },   { "TRAP", SIGTRAP }, { "ABRT", SIGABRT },
    { "BUS", SIGBUS },   { "FPE", SIGFPE },   { "KILL", SIGKILL },
    { "USR1", SIGUSR1 }, { "SEGV", SIGSEGV }, { "USR2", SIGUSR2 },
    { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM },
    { "CHLD", SIGCHLD }, { "CONT", SIGCONT }, { "STOP", SIGSTOP },
    { "TSTP", SIGTSTP }, { "TTIN", SIGTTIN }, { "TTOU", SIGTTOU },
    { "URG", SIGURG },   { "XCPU", SIGXCPU }, { "XFSZ", SIGXFSZ },
    { "VTALRM", SIGVTALRM }, { "PROF", SIGPROF }, { "WINCH", SIGWINCH },
    { "IO", SIGIO },     { "SYS", SIGSYS },
};

int trap_signal_number(const char *name)
{
    if (strcmp(name, "EXIT") == 0 || strcmp(name, "0") == 0)
        return 0;
    if (strncmp(name, "SIG", 3) == 0)
        name += 3;
    for (size_t i = 0; i < sizeof(sig_table) / sizeof(sig_table[0]); i++)
        if (strcmp(sig_table[i].name, name) == 0)
            return sig_table[i].num;
    char *end;
    long n = strtol(name, &end, 10);
    if (*end == '\0' && end != name && n >= 0 && n < 64)
        return (int)n;
    return -1;
}

const char *trap_signal_name(int signo)
{
    if (signo == 0)
        return "EXIT";
    for (size_t i = 0; i < sizeof(sig_table) / sizeof(sig_table[0]); i++)
        if (sig_table[i].num == signo)
            return sig_table[i].name;
    return "UNKNOWN";
}

static void trap_handler(int sig)
{
    if (sig >= 0 && sig < 64)
        trap_pending_flags[sig] = 1;
}

void trap_set(int signo, const char *command)
{
    if (signo < 0 || signo >= 64)
        return;
    free(trap_commands[signo]);
    trap_commands[signo] = command ? xstrdup(command) : NULL;
    if (signo == 0)
        return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    if (!command) {
        sa.sa_handler = SIG_DFL;
    } else if (command[0] == '\0') {
        sa.sa_handler = SIG_IGN;
    } else {
        sa.sa_handler = trap_handler;
    }
    sigemptyset(&sa.sa_mask);
    sigaction(signo, &sa, NULL);
}

const char *trap_get(int signo)
{
    if (signo < 0 || signo >= 64)
        return NULL;
    return trap_commands[signo];
}

void trap_run_pending(void)
{
    static int running;
    if (running)
        return;
    running = 1;
    for (int sig = 1; sig < 64; sig++) {
        if (trap_pending_flags[sig] && trap_commands[sig]) {
            trap_pending_flags[sig] = 0;
            exec_string(trap_commands[sig]);
        }
        trap_pending_flags[sig] = 0;
    }
    running = 0;
}

void trap_run_exit(void)
{
    if (trap_commands[0])
        exec_string(trap_commands[0]);
}

void trap_reset_child(void)
{
    for (int sig = 1; sig < 64; sig++)
        if (trap_commands[sig])
            signal(sig, SIG_DFL);
}

static int bi_cd(char **argv)
{
    const char *dir = argv[1];
    int print_new = 0;
    if (!dir) {
        dir = var_get("HOME");
        if (!dir) {
            fputs("ash: cd: HOME not set\n", stderr);
            return 1;
        }
    } else if (strcmp(dir, "-") == 0) {
        dir = var_get("OLDPWD");
        if (!dir) {
            fputs("ash: cd: OLDPWD not set\n", stderr);
            return 1;
        }
        print_new = 1;
    }
    char old[4096];
    if (!getcwd(old, sizeof(old)))
        old[0] = '\0';
    if (chdir(dir) < 0) {
        fprintf(stderr, "ash: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    char newcwd[4096];
    if (getcwd(newcwd, sizeof(newcwd))) {
        var_set("OLDPWD", old, 1);
        var_set("PWD", newcwd, 1);
        if (print_new)
            printf("%s\n", newcwd);
    }
    return 0;
}

static int bi_pwd(char **argv)
{
    (void)argv;
    char cwd[4096];
    const char *pwd = var_get("PWD");
    if (pwd) {
        printf("%s\n", pwd);
    } else if (getcwd(cwd, sizeof(cwd))) {
        printf("%s\n", cwd);
    } else {
        perror("ash: pwd");
        return 1;
    }
    return 0;
}

static int bi_exit(char **argv)
{
    int code = argv[1] ? atoi(argv[1]) : shell.last_status;
    if (shell.in_child || getpid() != shell.shell_pid)
        _exit(code);
    shell.exit_pending = 1;
    shell.exit_status = code;
    return code;
}

static void print_escaped(const char *s)
{
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case 'r': putchar('\r'); break;
            case 'a': putchar('\a'); break;
            case 'b': putchar('\b'); break;
            case 'f': putchar('\f'); break;
            case 'v': putchar('\v'); break;
            case '\\': putchar('\\'); break;
            case '0': {
                int val = 0;
                int digits = 0;
                while (p[1] >= '0' && p[1] <= '7' && digits < 3) {
                    val = val * 8 + (p[1] - '0');
                    p++;
                    digits++;
                }
                putchar(val);
                break;
            }
            default:
                putchar('\\');
                putchar(*p);
                break;
            }
            continue;
        }
        putchar(*p);
    }
}

static int bi_echo(char **argv)
{
    int newline = 1;
    int escapes = 0;
    int i = 1;
    while (argv[i] && argv[i][0] == '-' && argv[i][1]) {
        int valid = 1;
        for (const char *p = argv[i] + 1; *p; p++)
            if (!strchr("neE", *p))
                valid = 0;
        if (!valid)
            break;
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'n')
                newline = 0;
            else if (*p == 'e')
                escapes = 1;
            else if (*p == 'E')
                escapes = 0;
        }
        i++;
    }
    for (int j = i; argv[j]; j++) {
        if (j > i)
            putchar(' ');
        if (escapes)
            print_escaped(argv[j]);
        else
            fputs(argv[j], stdout);
    }
    if (newline)
        putchar('\n');
    return 0;
}

static int bi_export(char **argv)
{
    if (!argv[1]) {
        var_print_exported(stdout);
        return 0;
    }
    for (int i = 1; argv[i]; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            var_set(argv[i], eq + 1, 1);
            *eq = '=';
        } else {
            var_export(argv[i]);
        }
    }
    return 0;
}

static int bi_unset(char **argv)
{
    for (int i = 1; argv[i]; i++)
        var_unset(argv[i]);
    return 0;
}

static int bi_shift(char **argv)
{
    int n = argv[1] ? atoi(argv[1]) : 1;
    if (n < 0 || n > shell.positional_count) {
        fputs("ash: shift: shift count out of range\n", stderr);
        return 1;
    }
    for (int i = 0; i < n; i++)
        free(shell.positional[i]);
    memmove(shell.positional, shell.positional + n,
            (shell.positional_count - n) * sizeof(char *));
    shell.positional_count -= n;
    return 0;
}

static int bi_true(char **argv)
{
    (void)argv;
    return 0;
}

static int bi_false(char **argv)
{
    (void)argv;
    return 1;
}

static int test_file(const char *op, const char *path)
{
    struct stat st;
    if (strcmp(op, "-e") == 0)
        return stat(path, &st) == 0;
    if (strcmp(op, "-f") == 0)
        return stat(path, &st) == 0 && S_ISREG(st.st_mode);
    if (strcmp(op, "-d") == 0)
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0)
        return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
    if (strcmp(op, "-b") == 0)
        return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
    if (strcmp(op, "-c") == 0)
        return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
    if (strcmp(op, "-p") == 0)
        return stat(path, &st) == 0 && S_ISFIFO(st.st_mode);
    if (strcmp(op, "-S") == 0)
        return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
    if (strcmp(op, "-s") == 0)
        return stat(path, &st) == 0 && st.st_size > 0;
    if (strcmp(op, "-r") == 0)
        return access(path, R_OK) == 0;
    if (strcmp(op, "-w") == 0)
        return access(path, W_OK) == 0;
    if (strcmp(op, "-x") == 0)
        return access(path, X_OK) == 0;
    if (strcmp(op, "-t") == 0)
        return isatty(atoi(path));
    return -1;
}

static int test_eval(int argc, char **argv)
{
    if (argc == 0)
        return 1;
    if (argc >= 2 && strcmp(argv[0], "!") == 0)
        return !test_eval(argc - 1, argv + 1);
    if (argc == 1)
        return argv[0][0] == '\0';
    if (argc == 2) {
        if (strcmp(argv[0], "-z") == 0)
            return argv[1][0] == '\0';
        if (strcmp(argv[0], "-n") == 0)
            return argv[1][0] != '\0';
        int r = test_file(argv[0], argv[1]);
        if (r >= 0)
            return !r;
        return 2;
    }
    if (argc == 3) {
        if (strcmp(argv[0], "(") == 0 && strcmp(argv[2], ")") == 0)
            return test_eval(1, argv + 1);
        if (strcmp(argv[1], "=") == 0 || strcmp(argv[1], "==") == 0)
            return strcmp(argv[0], argv[2]) != 0;
        if (strcmp(argv[1], "!=") == 0)
            return strcmp(argv[0], argv[2]) == 0;
        if (strcmp(argv[1], "-eq") == 0)
            return strtoll(argv[0], NULL, 10) != strtoll(argv[2], NULL, 10);
        if (strcmp(argv[1], "-ne") == 0)
            return strtoll(argv[0], NULL, 10) == strtoll(argv[2], NULL, 10);
        if (strcmp(argv[1], "-lt") == 0)
            return strtoll(argv[0], NULL, 10) >= strtoll(argv[2], NULL, 10);
        if (strcmp(argv[1], "-le") == 0)
            return strtoll(argv[0], NULL, 10) > strtoll(argv[2], NULL, 10);
        if (strcmp(argv[1], "-gt") == 0)
            return strtoll(argv[0], NULL, 10) <= strtoll(argv[2], NULL, 10);
        if (strcmp(argv[1], "-ge") == 0)
            return strtoll(argv[0], NULL, 10) < strtoll(argv[2], NULL, 10);
        if (strcmp(argv[1], "-nt") == 0) {
            struct stat a, b;
            return !(stat(argv[0], &a) == 0 && stat(argv[2], &b) == 0 &&
                     a.st_mtime > b.st_mtime);
        }
        if (strcmp(argv[1], "-ot") == 0) {
            struct stat a, b;
            return !(stat(argv[0], &a) == 0 && stat(argv[2], &b) == 0 &&
                     a.st_mtime < b.st_mtime);
        }
        return 2;
    }
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            int left = test_eval(i, argv);
            int right = test_eval(argc - i - 1, argv + i + 1);
            return !(left == 0 || right == 0);
        }
    }
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            int left = test_eval(i, argv);
            int right = test_eval(argc - i - 1, argv + i + 1);
            return !(left == 0 && right == 0);
        }
    }
    return 2;
}

static int bi_test(char **argv)
{
    int argc = 0;
    while (argv[argc])
        argc++;
    if (argc > 0 && strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fputs("ash: [: missing `]'\n", stderr);
            return 2;
        }
        argv[argc - 1] = NULL;
        argc--;
    }
    int r = test_eval(argc - 1, argv + 1);
    if (r == 2) {
        fputs("ash: test: syntax error\n", stderr);
        return 2;
    }
    return r;
}

static int bi_read(char **argv)
{
    int raw = 0;
    const char *prompt = NULL;
    int i = 1;
    while (argv[i] && argv[i][0] == '-' && argv[i][1]) {
        if (strcmp(argv[i], "-r") == 0) {
            raw = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            if (!argv[i + 1]) {
                fputs("ash: read: -p: option requires an argument\n", stderr);
                return 2;
            }
            prompt = argv[++i];
        } else {
            fprintf(stderr, "ash: read: %s: invalid option\n", argv[i]);
            return 2;
        }
        i++;
    }
    if (prompt) {
        fputs(prompt, stderr);
        fflush(stderr);
    }
    clearerr(stdin);
    StrBuf line;
    sb_init(&line);
    char buf[4096];
    int got = 0;
    for (;;) {
        if (!fgets(buf, sizeof(buf), stdin))
            break;
        got = 1;
        sb_append(&line, buf);
        if (line.len > 0 && line.data[line.len - 1] == '\n') {
            if (!raw && line.len > 1 && line.data[line.len - 2] == '\\') {
                line.len -= 2;
                line.data[line.len] = '\0';
                continue;
            }
            line.len--;
            line.data[line.len] = '\0';
            break;
        }
    }
    if (!got) {
        sb_free(&line);
        return 1;
    }
    char *text = sb_take(&line);
    if (!raw) {
        char *r = text;
        char *w = text;
        while (*r) {
            if (*r == '\\' && r[1])
                r++;
            *w++ = *r++;
        }
        *w = '\0';
    }
    char **names = argv + i;
    int nnames = 0;
    while (names[nnames])
        nnames++;
    if (nnames == 0) {
        var_set("REPLY", text, 0);
        free(text);
        return 0;
    }
    const char *ifs = var_get("IFS");
    if (!ifs)
        ifs = " \t\n";
    char *p = text;
    for (int v = 0; v < nnames; v++) {
        while (*p && strchr(ifs, *p) && strchr(" \t\n", *p))
            p++;
        if (v == nnames - 1) {
            size_t len = strlen(p);
            while (len > 0 && strchr(ifs, p[len - 1]) &&
                   strchr(" \t\n", p[len - 1]))
                len--;
            p[len] = '\0';
            var_set(names[v], p, 0);
            break;
        }
        char *start = p;
        while (*p && !strchr(ifs, *p))
            p++;
        char saved = *p;
        *p = '\0';
        var_set(names[v], start, 0);
        if (saved)
            p++;
        if (saved && !strchr(" \t\n", saved)) {
            while (*p && strchr(ifs, *p) && strchr(" \t\n", *p))
                p++;
        } else {
            while (*p && strchr(ifs, *p) && strchr(" \t\n", *p))
                p++;
        }
    }
    free(text);
    return 0;
}

static int bi_eval(char **argv)
{
    StrBuf sb;
    sb_init(&sb);
    for (int i = 1; argv[i]; i++) {
        if (i > 1)
            sb_append_char(&sb, ' ');
        sb_append(&sb, argv[i]);
    }
    int status = exec_string(sb.data);
    sb_free(&sb);
    return status;
}

static int bi_exec(char **argv)
{
    if (!argv[1])
        return 0;
    execvp(argv[1], argv + 1);
    fprintf(stderr, "ash: exec: %s: %s\n", argv[1], strerror(errno));
    if (!shell.interactive)
        _exit(127);
    return 127;
}

static int bi_source(char **argv)
{
    if (!argv[1]) {
        fputs("ash: source: filename argument required\n", stderr);
        return 2;
    }
    int status = 0;
    for (int i = 1; argv[i]; i++)
        status = source_file(argv[i]);
    return status;
}

static int bi_trap(char **argv)
{
    if (!argv[1]) {
        for (int sig = 0; sig < 64; sig++) {
            const char *cmd = trap_get(sig);
            if (cmd)
                printf("trap -- '%s' %s\n", cmd, trap_signal_name(sig));
        }
        return 0;
    }
    int argi = 1;
    const char *command = argv[argi];
    int reset = strcmp(command, "-") == 0;
    if (reset || argv[argi + 1])
        argi++;
    if (!argv[argi]) {
        fputs("ash: trap: usage: trap [-] [command] signal ...\n", stderr);
        return 2;
    }
    int status = 0;
    for (int i = argi; argv[i]; i++) {
        int sig = trap_signal_number(argv[i]);
        if (sig < 0) {
            fprintf(stderr, "ash: trap: %s: invalid signal\n", argv[i]);
            status = 1;
            continue;
        }
        trap_set(sig, reset ? NULL : command);
    }
    return status;
}

static int bi_alias(char **argv)
{
    if (!argv[1]) {
        alias_print_all(stdout);
        return 0;
    }
    int status = 0;
    for (int i = 1; argv[i]; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            alias_set(argv[i], eq + 1);
            *eq = '=';
        } else {
            const char *val = alias_get(argv[i]);
            if (val)
                printf("alias %s='%s'\n", argv[i], val);
            else {
                fprintf(stderr, "ash: alias: %s: not found\n", argv[i]);
                status = 1;
            }
        }
    }
    return status;
}

static int bi_unalias(char **argv)
{
    if (argv[1] && strcmp(argv[1], "-a") == 0) {
        while (alias_count() > 0)
            alias_unset(alias_name(0));
        return 0;
    }
    int status = 0;
    for (int i = 1; argv[i]; i++)
        if (alias_unset(argv[i]) < 0) {
            fprintf(stderr, "ash: unalias: %s: not found\n", argv[i]);
            status = 1;
        }
    return status;
}

static int find_in_path(const char *name, char *out, size_t out_size)
{
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            snprintf(out, out_size, "%s", name);
            return 0;
        }
        return -1;
    }
    const char *path = var_get("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = xstrdup(path);
    char *save = NULL;
    int found = -1;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir[0] ? dir : ".", name);
        if (access(full, X_OK) == 0) {
            snprintf(out, out_size, "%s", full);
            found = 0;
            break;
        }
    }
    free(copy);
    return found;
}

static int bi_type(char **argv)
{
    int status = 0;
    for (int i = 1; argv[i]; i++) {
        const char *name = argv[i];
        const char *alias = alias_get(name);
        if (alias) {
            printf("%s is an alias for %s\n", name, alias);
        } else if (builtin_lookup(name)) {
            printf("%s is a shell builtin\n", name);
        } else if (function_lookup(name)) {
            printf("%s is a function\n", name);
        } else {
            char full[4096];
            if (find_in_path(name, full, sizeof(full)) == 0)
                printf("%s is %s\n", name, full);
            else {
                fprintf(stderr, "ash: type: %s: not found\n", name);
                status = 1;
            }
        }
    }
    return status;
}

static int bi_printf(char **argv)
{
    if (!argv[1]) {
        fputs("ash: printf: usage: printf format [arguments]\n", stderr);
        return 2;
    }
    const char *fmt = argv[1];
    int argi = 2;
    int nargs = 0;
    while (argv[argi + nargs])
        nargs++;
    int used = 0;
    do {
        int used_before = used;
        for (const char *p = fmt; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case '\\': putchar('\\'); break;
                case 'a': putchar('\a'); break;
                case 'b': putchar('\b'); break;
                case 'f': putchar('\f'); break;
                case 'v': putchar('\v'); break;
                default:
                    putchar('\\');
                    putchar(*p);
                    break;
                }
                continue;
            }
            if (*p != '%') {
                putchar(*p);
                continue;
            }
            p++;
            if (*p == '%') {
                putchar('%');
                continue;
            }
            int left = 0;
            int zero = 0;
            while (*p == '-' || *p == '0') {
                if (*p == '-')
                    left = 1;
                else
                    zero = 1;
                p++;
            }
            int width = 0;
            while (isdigit((unsigned char)*p)) {
                width = width * 10 + (*p - '0');
                p++;
            }
            int precision = -1;
            if (*p == '.') {
                p++;
                precision = 0;
                while (isdigit((unsigned char)*p)) {
                    precision = precision * 10 + (*p - '0');
                    p++;
                }
            }
            const char *arg = argv[argi + used] ? argv[argi + used] : "";
            if (used < nargs)
                used++;
            char tmp[512];
            const char *out = tmp;
            switch (*p) {
            case 's':
                if (precision >= 0 && (int)strlen(arg) > precision) {
                    if (precision > (int)sizeof(tmp) - 1)
                        precision = (int)sizeof(tmp) - 1;
                    memcpy(tmp, arg, precision);
                    tmp[precision] = '\0';
                } else {
                    snprintf(tmp, sizeof(tmp), "%s", arg);
                }
                break;
            case 'd':
            case 'i':
                snprintf(tmp, sizeof(tmp), "%lld", strtoll(arg, NULL, 10));
                break;
            case 'u':
                snprintf(tmp, sizeof(tmp), "%llu",
                         (unsigned long long)strtoll(arg, NULL, 10));
                break;
            case 'x':
                snprintf(tmp, sizeof(tmp), "%llx",
                         (unsigned long long)strtoll(arg, NULL, 10));
                break;
            case 'X':
                snprintf(tmp, sizeof(tmp), "%llX",
                         (unsigned long long)strtoll(arg, NULL, 10));
                break;
            case 'o':
                snprintf(tmp, sizeof(tmp), "%llo",
                         (unsigned long long)strtoll(arg, NULL, 10));
                break;
            case 'c':
                tmp[0] = arg[0];
                tmp[1] = '\0';
                break;
            case '\0':
                p--;
                tmp[0] = '\0';
                break;
            default:
                snprintf(tmp, sizeof(tmp), "%%%c", *p);
                break;
            }
            int len = (int)strlen(out);
            int pad = width > len ? width - len : 0;
            if (!left)
                for (int k = 0; k < pad; k++)
                    putchar(zero && (*p == 'd' || *p == 'i') ? '0' : ' ');
            fputs(out, stdout);
            if (left)
                for (int k = 0; k < pad; k++)
                    putchar(' ');
        }
        if (used == used_before)
            break;
    } while (used < nargs);
    return 0;
}

static int bi_local(char **argv)
{
    if (shell.function_depth == 0) {
        fputs("ash: local: can only be used in a function\n", stderr);
        return 1;
    }
    for (int i = 1; argv[i]; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            var_set_local(argv[i], eq + 1);
            *eq = '=';
        } else {
            var_set_local(argv[i], "");
        }
    }
    return 0;
}

static int bi_return(char **argv)
{
    if (shell.function_depth == 0) {
        fputs("ash: return: can only `return' from a function\n", stderr);
        return 1;
    }
    int status = argv[1] ? atoi(argv[1]) : shell.last_status;
    shell.return_pending = 1;
    shell.last_status = status;
    return status;
}

static int bi_break(char **argv)
{
    if (shell.loop_depth == 0) {
        fputs("ash: break: only meaningful in a loop\n", stderr);
        return 1;
    }
    int n = argv[1] ? atoi(argv[1]) : 1;
    if (n < 1)
        n = 1;
    if (n > shell.loop_depth)
        n = shell.loop_depth;
    shell.break_pending = n;
    return 0;
}

static int bi_continue(char **argv)
{
    if (shell.loop_depth == 0) {
        fputs("ash: continue: only meaningful in a loop\n", stderr);
        return 1;
    }
    int n = argv[1] ? atoi(argv[1]) : 1;
    if (n < 1)
        n = 1;
    if (n > shell.loop_depth)
        n = shell.loop_depth;
    shell.continue_pending = n;
    return 0;
}

static int bi_jobs(char **argv)
{
    int with_pids = argv[1] && strcmp(argv[1], "-l") == 0;
    job_reap(0);
    job_print(stdout, with_pids);
    return 0;
}

static int bi_fg(char **argv)
{
    return job_fg(argv[1]);
}

static int bi_bg(char **argv)
{
    return job_bg(argv[1]);
}

static int bi_kill(char **argv)
{
    int argi = 1;
    int signo = SIGTERM;
    if (argv[argi] && strcmp(argv[argi], "-l") == 0) {
        for (size_t i = 0;
             i < sizeof(sig_table) / sizeof(sig_table[0]); i++)
            printf("%2d) SIG%-10s", sig_table[i].num, sig_table[i].name);
        printf("\n");
        return 0;
    }
    if (argv[argi] && argv[argi][0] == '-' && argv[argi][1]) {
        const char *spec = argv[argi] + 1;
        if (isdigit((unsigned char)spec[0])) {
            signo = atoi(spec);
        } else {
            signo = trap_signal_number(spec);
            if (signo < 0) {
                fprintf(stderr, "ash: kill: %s: invalid signal\n", spec);
                return 1;
            }
        }
        argi++;
    }
    if (!argv[argi]) {
        fputs("ash: kill: usage: kill [-sig] pid | %job ...\n", stderr);
        return 2;
    }
    int status = 0;
    for (int i = argi; argv[i]; i++) {
        if (argv[i][0] == '%') {
            if (job_signal(argv[i], signo) != 0)
                status = 1;
        } else {
            pid_t pid = (pid_t)strtol(argv[i], NULL, 10);
            if (kill(pid, signo) < 0) {
                fprintf(stderr, "ash: kill: (%s) - %s\n", argv[i],
                        strerror(errno));
                status = 1;
            }
        }
    }
    return status;
}

static int bi_wait(char **argv)
{
    if (!argv[1])
        return job_wait_all();
    int status = 0;
    for (int i = 1; argv[i]; i++) {
        if (argv[i][0] == '%') {
            pid_t pgid = job_pgid(argv[i]);
            if (pgid < 0) {
                fprintf(stderr, "ash: wait: %s: no such job\n", argv[i]);
                status = 127;
                continue;
            }
            status = job_wait_pid(pgid);
        } else {
            status = job_wait_pid((pid_t)strtol(argv[i], NULL, 10));
        }
    }
    return status;
}

static int bi_umask(char **argv)
{
    if (!argv[1]) {
        mode_t cur = umask(0);
        umask(cur);
        printf("%04o\n", (unsigned)cur);
        return 0;
    }
    char *end;
    long mode = strtol(argv[1], &end, 8);
    if (*end != '\0' || mode < 0 || mode > 0777) {
        fprintf(stderr, "ash: umask: %s: invalid octal number\n", argv[1]);
        return 1;
    }
    umask((mode_t)mode);
    return 0;
}

static int bi_set(char **argv)
{
    if (!argv[1]) {
        var_print_all(stdout);
        return 0;
    }
    if (strcmp(argv[1], "--") == 0) {
        int count = 0;
        while (argv[2 + count])
            count++;
        shell_set_positional(argv + 2, count);
        return 0;
    }
    if (argv[1][0] != '-' && argv[1][0] != '+') {
        shell_set_positional(argv + 1, 1);
        return 0;
    }
    int enable = argv[1][0] == '-';
    int argi = 1;
    while (argv[argi] && (argv[argi][0] == '-' || argv[argi][0] == '+')) {
        const char *flags = argv[argi] + 1;
        if (flags[0] == 'o') {
            if (argv[argi + 1]) {
                if (option_set(argv[argi + 1], enable) < 0) {
                    fprintf(stderr, "ash: set: %s: invalid option name\n",
                            argv[argi + 1]);
                    return 2;
                }
                argi++;
            } else {
                for (int i = 0; i < option_count(); i++)
                    printf("%s %s\n", option_name(i),
                           option_value(i) ? "on" : "off");
            }
        } else {
            for (const char *p = flags; *p; p++) {
                const char *name = NULL;
                if (*p == 'e')
                    name = "errexit";
                else if (*p == 'u')
                    name = "nounset";
                else if (*p == 'x')
                    name = "xtrace";
                else if (*p == 'C')
                    name = "noclobber";
                if (name)
                    option_set(name, enable);
            }
        }
        argi++;
    }
    return 0;
}

static int bi_setopt(char **argv)
{
    int status = 0;
    for (int i = 1; argv[i]; i++) {
        if (option_set(argv[i], 1) == 0)
            continue;
        const char *negated = NULL;
        if (strncmp(argv[i], "no_", 3) == 0)
            negated = argv[i] + 3;
        else if (strncmp(argv[i], "no", 2) == 0)
            negated = argv[i] + 2;
        if (negated && option_set(negated, 0) == 0)
            continue;
        fprintf(stderr, "ash: setopt: %s: no such option\n", argv[i]);
        status = 1;
    }
    return status;
}

static int bi_unsetopt(char **argv)
{
    int status = 0;
    for (int i = 1; argv[i]; i++) {
        if (option_set(argv[i], 0) < 0) {
            fprintf(stderr, "ash: unsetopt: %s: no such option\n", argv[i]);
            status = 1;
        }
    }
    return status;
}

static int bi_history(char **argv)
{
    if (argv[1] && strcmp(argv[1], "-c") == 0) {
        hist_clear();
        return 0;
    }
    for (int i = 0; i < hist_count(); i++)
        printf("%5d  %s\n", i + 1, hist_line(i));
    return 0;
}

static int bi_help(char **argv)
{
    (void)argv;
    printf("ash builtins:");
    for (int i = 0; i < builtin_count(); i++)
        printf(" %s", builtin_name(i));
    printf("\n");
    return 0;
}

typedef struct {
    const char *name;
    BuiltinFunc func;
} BuiltinEntry;

static BuiltinEntry builtin_table[] = {
    { ":", bi_true },
    { ".", bi_source },
    { "[", bi_test },
    { "alias", bi_alias },
    { "ask", agent_ask },
    { "bg", bi_bg },
    { "break", bi_break },
    { "cd", bi_cd },
    { "continue", bi_continue },
    { "echo", bi_echo },
    { "eval", bi_eval },
    { "exec", bi_exec },
    { "exit", bi_exit },
    { "export", bi_export },
    { "false", bi_false },
    { "fg", bi_fg },
    { "help", bi_help },
    { "history", bi_history },
    { "jobs", bi_jobs },
    { "kill", bi_kill },
    { "local", bi_local },
    { "printf", bi_printf },
    { "pwd", bi_pwd },
    { "read", bi_read },
    { "return", bi_return },
    { "set", bi_set },
    { "setopt", bi_setopt },
    { "shift", bi_shift },
    { "source", bi_source },
    { "test", bi_test },
    { "trap", bi_trap },
    { "true", bi_true },
    { "type", bi_type },
    { "umask", bi_umask },
    { "unalias", bi_unalias },
    { "unset", bi_unset },
    { "unsetopt", bi_unsetopt },
    { "wait", bi_wait },
};

BuiltinFunc builtin_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof(builtin_table) / sizeof(builtin_table[0]); i++)
        if (strcmp(builtin_table[i].name, name) == 0)
            return builtin_table[i].func;
    return NULL;
}

int builtin_count(void)
{
    return (int)(sizeof(builtin_table) / sizeof(builtin_table[0]));
}

const char *builtin_name(int index)
{
    return builtin_table[index].name;
}