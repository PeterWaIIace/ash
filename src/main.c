#include "ash.h"
#include <errno.h>
#include <signal.h>
#include <unistd.h>

static void run_interactive(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setpgid(0, 0);
    shell.terminal_fd = STDIN_FILENO;
    tcsetpgrp(shell.terminal_fd, getpgrp());

    lineedit_init();

    const char *home = var_get("HOME");
    if (home) {
        StrBuf sb;
        sb_init(&sb);
        sb_append(&sb, home);
        sb_append(&sb, "/.ashrc");
        if (access(sb.data, R_OK) == 0)
            source_file(sb.data);
        sb_free(&sb);
    }

    if (options.transcript)
        transcript_spawn();

    int eof_count = 0;
    for (;;) {
        trap_run_pending();
        job_reap(1);
        if (shell.exit_pending)
            break;
        const char *ps1 = var_get("PS1");
        char *prompt = expand_prompt(ps1 ? ps1 : "%n@%m %~ %# ");
        char *line = lineedit_read(prompt);
        free(prompt);
        if (!line) {
            eof_count++;
            if (options.ignoreeof && eof_count < 10) {
                printf("ash: use `exit' to leave the shell\n");
                continue;
            }
            printf("\n");
            break;
        }
        eof_count = 0;
        StrBuf cmd;
        sb_init(&cmd);
        sb_append(&cmd, line);
        free(line);
        int syntax_error = 0;
        for (;;) {
            ParseOutput po = parse_string(cmd.data);
            if (po.result == PARSE_OK) {
                node_free(po.root);
                break;
            }
            if (po.result == PARSE_ERROR) {
                fprintf(stderr, "ash: %s\n", po.error);
                shell.last_status = 2;
                syntax_error = 1;
                break;
            }
            const char *ps2 = var_get("PS2");
            char *more = lineedit_read(ps2 ? ps2 : "> ");
            if (!more) {
                syntax_error = 1;
                break;
            }
            sb_append_char(&cmd, '\n');
            sb_append(&cmd, more);
            free(more);
        }
        if (!syntax_error)
            exec_string(cmd.data);
        sb_free(&cmd);
    }
}

int main(int argc, char **argv)
{
    memset(&shell, 0, sizeof(shell));
    shell.shell_pid = getpid();
    shell.shell_name = "ash";
    shell.terminal_fd = -1;
    vars_init();
    var_import_environ();
    if (!var_get("PS1"))
        var_set("PS1",
                "%K{blue}%F{white} %n@%m %F{blue}%K{green}%P%F{black} "
                "%~%g %F{green}%K{default}%P%f%(?..%F{red})%C%f ", 0);
    if (!var_get("RPS1"))
        var_set("RPS1", "%(?..%F{red}%?%f)", 0);
    if (!var_get("PS2"))
        var_set("PS2", "> ", 0);
    if (!var_get("IFS"))
        var_set("IFS", " \t\n", 0);
    jobs_init();

    const char *c_arg = NULL;
    const char *script = NULL;
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "-c") == 0) {
        if (argi + 1 >= argc) {
            fputs("ash: -c: option requires an argument\n", stderr);
            return 2;
        }
        c_arg = argv[argi + 1];
        argi += 2;
        if (argi < argc) {
            shell.shell_name = argv[argi];
            argi++;
        }
        shell_set_positional(argv + argi, argc - argi);
    } else if (argi < argc && argv[argi][0] != '-') {
        script = argv[argi];
        shell.shell_name = script;
        shell_set_positional(argv + argi + 1, argc - argi - 1);
    }

    if (c_arg) {
        int status = exec_string(c_arg);
        trap_run_exit();
        return shell.exit_pending ? shell.exit_status : status;
    }

    shell.interactive = !script && isatty(STDIN_FILENO);
    if (shell.interactive) {
        run_interactive();
        trap_run_exit();
        return shell.exit_pending ? shell.exit_status : shell.last_status;
    }

    FILE *in = stdin;
    if (script) {
        in = fopen(script, "r");
        if (!in) {
            fprintf(stderr, "ash: %s: %s\n", script, strerror(errno));
            return 127;
        }
    }
    char *cmd;
    int rc;
    while ((rc = read_complete_command(in, &cmd)) == 1) {
        exec_string(cmd);
        free(cmd);
        if (shell.exit_pending)
            break;
    }
    if (script)
        fclose(in);
    trap_run_exit();
    return shell.exit_pending ? shell.exit_status : shell.last_status;
}