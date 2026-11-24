#include "ash.h"
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    char *name;
    Node *body;
} Function;

static Function *functions;
static int func_count;
static int func_capacity;

void function_define(const char *name, Node *body)
{
    for (int i = 0; i < func_count; i++) {
        if (strcmp(functions[i].name, name) == 0) {
            node_free(functions[i].body);
            functions[i].body = body;
            return;
        }
    }
    if (func_count == func_capacity) {
        func_capacity = func_capacity ? func_capacity * 2 : 16;
        functions = xrealloc(functions, func_capacity * sizeof(Function));
    }
    functions[func_count].name = xstrdup(name);
    functions[func_count].body = body;
    func_count++;
}

Node *function_lookup(const char *name)
{
    for (int i = 0; i < func_count; i++)
        if (strcmp(functions[i].name, name) == 0)
            return functions[i].body;
    return NULL;
}

int function_count(void)
{
    return func_count;
}

const char *function_name(int index)
{
    return functions[index].name;
}

static int status_from(int st)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return 0;
}

static void reset_child_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    trap_reset_child();
}

static void save_fd(FdSave **saves, int *n, int fd)
{
    *saves = xrealloc(*saves, (*n + 1) * sizeof(FdSave));
    (*saves)[*n].fd = fd;
    (*saves)[*n].orig = dup(fd);
    (*n)++;
}

int apply_redirects(Redirect *redirs, int count, FdSave **saves,
                    int *save_count)
{
    fflush(NULL);
    *saves = NULL;
    *save_count = 0;
    char *target = NULL;
    for (int i = 0; i < count; i++) {
        Redirect *r = &redirs[i];
        int fd = r->fd;
        target = NULL;
        if (r->kind != REDIR_HEREDOC && r->kind != REDIR_HERESTRING) {
            target = expand_word_string(&r->target, 0);
            if (!target)
                goto fail;
        }
        if (r->kind == REDIR_HEREDOC || r->kind == REDIR_HERESTRING) {
            char *body;
            if (r->kind == REDIR_HEREDOC) {
                body = r->heredoc->quoted ? xstrdup(r->heredoc->text)
                                          : expand_heredoc(r->heredoc->text);
            } else {
                char *val = expand_word_string(&r->target, 0);
                if (!val)
                    goto fail;
                StrBuf sb;
                sb_init(&sb);
                sb_append(&sb, val);
                sb_append_char(&sb, '\n');
                free(val);
                body = sb_take(&sb);
            }
            FILE *tmp = tmpfile();
            if (!tmp) {
                free(body);
                perror("ash: tmpfile");
                goto fail;
            }
            fwrite(body, 1, strlen(body), tmp);
            rewind(tmp);
            free(body);
            save_fd(saves, save_count, fd);
            dup2(fileno(tmp), fd);
            fclose(tmp);
            continue;
        }
        if (r->kind == REDIR_DUP_IN || r->kind == REDIR_DUP_OUT) {
            if (strcmp(target, "-") == 0) {
                save_fd(saves, save_count, fd);
                close(fd);
            } else {
                int all_digits = target[0] != '\0';
                for (const char *p = target; *p; p++)
                    if (*p < '0' || *p > '9')
                        all_digits = 0;
                if (!all_digits) {
                    fprintf(stderr, "ash: %s: ambiguous redirect\n", target);
                    goto fail;
                }
                save_fd(saves, save_count, fd);
                if (dup2(atoi(target), fd) < 0) {
                    fprintf(stderr, "ash: %s: bad file descriptor\n", target);
                    goto fail;
                }
            }
            free(target);
            continue;
        }
        int flags = 0;
        switch (r->kind) {
        case REDIR_INPUT:
            flags = O_RDONLY;
            break;
        case REDIR_OUTPUT:
            flags = O_WRONLY | O_CREAT | (options.noclobber ? O_EXCL : O_TRUNC);
            break;
        case REDIR_APPEND:
        case REDIR_ALL_APPEND:
            flags = O_WRONLY | O_CREAT | O_APPEND;
            break;
        case REDIR_CLOBBER:
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            break;
        case REDIR_RDWR:
            flags = O_RDWR | O_CREAT;
            break;
        case REDIR_ALL_OUT:
            flags = O_WRONLY | O_CREAT | (options.noclobber ? O_EXCL : O_TRUNC);
            break;
        default:
            break;
        }
        int newfd = open(target, flags, 0666);
        if (newfd < 0) {
            fprintf(stderr, "ash: %s: %s\n", target, strerror(errno));
            goto fail;
        }
        if (r->kind == REDIR_ALL_OUT || r->kind == REDIR_ALL_APPEND) {
            save_fd(saves, save_count, 1);
            save_fd(saves, save_count, 2);
            dup2(newfd, 1);
            dup2(newfd, 2);
        } else {
            save_fd(saves, save_count, fd);
            dup2(newfd, fd);
        }
        close(newfd);
        free(target);
    }
    return 0;
fail:
    free(target);
    restore_redirects(*saves, *save_count);
    *saves = NULL;
    *save_count = 0;
    return -1;
}

void restore_redirects(FdSave *saves, int save_count)
{
    fflush(NULL);
    for (int i = save_count - 1; i >= 0; i--) {
        if (saves[i].orig >= 0) {
            dup2(saves[i].orig, saves[i].fd);
            close(saves[i].orig);
        } else {
            close(saves[i].fd);
        }
    }
    free(saves);
}

static void discard_redirect_saves(FdSave *saves, int save_count)
{
    for (int i = 0; i < save_count; i++)
        if (saves[i].orig >= 0)
            close(saves[i].orig);
    free(saves);
}

static int split_assignment(const Word *w, char **name, Word *value)
{
    char *raw = word_raw_text(w);
    char *eq = strchr(raw, '=');
    if (!eq) {
        free(raw);
        return -1;
    }
    size_t namelen = eq - raw;
    *name = xmalloc(namelen + 1);
    memcpy(*name, raw, namelen);
    (*name)[namelen] = '\0';
    word_init(value);
    size_t offset = namelen;
    size_t pos = 0;
    int found = 0;
    for (int i = 0; i < w->count; i++) {
        const Piece *p = &w->items[i];
        size_t plen = strlen(p->text);
        if (!found && p->kind == PIECE_TEXT && offset < pos + plen) {
            size_t cut = offset - pos;
            if (cut + 1 < plen)
                word_add_piece(value, PIECE_TEXT, p->quoted, p->text + cut + 1);
            found = 1;
        } else if (found) {
            word_add_piece(value, p->kind, p->quoted, p->text);
        }
        pos += plen;
    }
    free(raw);
    return 0;
}

typedef struct {
    char *name;
    char *old_value;
    int had_old;
} TempAssign;

static TempAssign *apply_temp_assigns(Word *assigns, int count, int *out_count)
{
    TempAssign *saved = xmalloc((count ? count : 1) * sizeof(TempAssign));
    *out_count = 0;
    for (int i = 0; i < count; i++) {
        char *name = NULL;
        Word value;
        if (split_assignment(&assigns[i], &name, &value) < 0)
            continue;
        char *val = expand_word_string(&value, EXP_ASSIGN);
        word_free(&value);
        if (!val) {
            free(name);
            continue;
        }
        const char *old = getenv(name);
        saved[*out_count].name = name;
        saved[*out_count].old_value = old ? xstrdup(old) : NULL;
        saved[*out_count].had_old = old != NULL;
        (*out_count)++;
        setenv(name, val, 1);
        free(val);
    }
    return saved;
}

static void restore_temp_assigns(TempAssign *saved, int count)
{
    for (int i = 0; i < count; i++) {
        if (saved[i].had_old)
            setenv(saved[i].name, saved[i].old_value, 1);
        else
            unsetenv(saved[i].name);
        free(saved[i].name);
        free(saved[i].old_value);
    }
    free(saved);
}

static void word_to_raw_quoted(const Word *w, StrBuf *sb)
{
    for (int i = 0; i < w->count; i++) {
        const Piece *p = &w->items[i];
        if (p->kind == PIECE_TEXT) {
            if (p->quoted) {
                sb_append_char(sb, '\'');
                for (const char *c = p->text; *c; c++) {
                    if (*c == '\'')
                        sb_append(sb, "'\\''");
                    else
                        sb_append_char(sb, *c);
                }
                sb_append_char(sb, '\'');
            } else {
                sb_append(sb, p->text);
            }
        } else if (p->kind == PIECE_PARAM) {
            if (p->quoted)
                sb_append(sb, "\"$");
            else
                sb_append(sb, "$");
            sb_append(sb, p->text);
            if (p->quoted)
                sb_append_char(sb, '"');
        } else if (p->kind == PIECE_CMDSUB) {
            sb_append(sb, "$(");
            sb_append(sb, p->text);
            sb_append_char(sb, ')');
        } else {
            sb_append(sb, "$((");
            sb_append(sb, p->text);
            sb_append(sb, "))");
        }
    }
}

static int wait_status(pid_t pid)
{
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 1;
    return status_from(st);
}

static int run_foreground(pid_t pid, const char *text)
{
    if (shell.interactive && shell.terminal_fd >= 0) {
        setpgid(pid, pid);
        int id = job_create(pid, text);
        job_add_pid(id, pid);
        return job_wait_foreground(id);
    }
    return wait_status(pid);
}

static int call_function(Node *body, char **argv, int argc)
{
    char **old_positional = shell.positional;
    int old_count = shell.positional_count;
    shell.positional = NULL;
    shell.positional_count = 0;
    shell_set_positional(argv + 1, argc - 1);
    var_push_scope();
    shell.function_depth++;
    shell.return_pending = 0;
    int status = exec_node(body);
    if (shell.return_pending) {
        status = shell.last_status;
        shell.return_pending = 0;
    }
    shell.function_depth--;
    var_pop_scope();
    for (int i = 0; i < shell.positional_count; i++)
        free(shell.positional[i]);
    free(shell.positional);
    shell.positional = old_positional;
    shell.positional_count = old_count;
    return status;
}

static int exec_command(Node *n)
{
    CommandNode *c = &n->as.command;
    static int alias_depth;

    if (alias_depth < 20 && c->word_count > 0 && c->words[0].count == 1 &&
        c->words[0].items[0].kind == PIECE_TEXT &&
        !c->words[0].items[0].quoted) {
        const char *alias = alias_get(c->words[0].items[0].text);
        if (alias) {
            StrBuf sb;
            sb_init(&sb);
            sb_append(&sb, alias);
            for (int i = 1; i < c->word_count; i++) {
                sb_append_char(&sb, ' ');
                word_to_raw_quoted(&c->words[i], &sb);
            }
            ParseOutput po = parse_string(sb.data);
            sb_free(&sb);
            if (po.result == PARSE_OK && po.root) {
                int ntemp = 0;
                TempAssign *temp = apply_temp_assigns(c->assigns,
                                                      c->assign_count, &ntemp);
                FdSave *saves = NULL;
                int nsaves = 0;
                int status;
                if (apply_redirects(n->redirs, n->redir_count, &saves,
                                    &nsaves) == 0) {
                    alias_depth++;
                    status = exec_node(po.root);
                    alias_depth--;
                    restore_redirects(saves, nsaves);
                } else {
                    status = 1;
                }
                restore_temp_assigns(temp, ntemp);
                node_free(po.root);
                return status;
            }
            if (po.root)
                node_free(po.root);
        }
    }

    if (c->word_count == 0) {
        int status = 0;
        for (int i = 0; i < c->assign_count; i++) {
            char *name = NULL;
            Word value;
            if (split_assignment(&c->assigns[i], &name, &value) < 0)
                continue;
            char *val = expand_word_string(&value, EXP_ASSIGN);
            word_free(&value);
            if (!val) {
                status = 1;
            } else {
                var_set(name, val, 0);
                free(val);
            }
            free(name);
        }
        FdSave *saves = NULL;
        int nsaves = 0;
        if (apply_redirects(n->redirs, n->redir_count, &saves, &nsaves) == 0)
            restore_redirects(saves, nsaves);
        else
            status = 1;
        return status;
    }

    char **argv = NULL;
    int argc = 0;
    int failed = 0;
    for (int i = 0; i < c->word_count && !failed; i++) {
        int nfields = 0;
        char **fields = expand_word_fields(&c->words[i], 0, &nfields);
        if (nfields < 0) {
            failed = 1;
            break;
        }
        for (int j = 0; j < nfields; j++) {
            argv = xrealloc(argv, (argc + 1) * sizeof(char *));
            argv[argc++] = fields[j];
        }
        free(fields);
    }
    if (failed) {
        for (int i = 0; i < argc; i++)
            free(argv[i]);
        free(argv);
        return 1;
    }
    if (argc == 0) {
        FdSave *saves = NULL;
        int nsaves = 0;
        int status = 0;
        if (apply_redirects(n->redirs, n->redir_count, &saves, &nsaves) == 0)
            restore_redirects(saves, nsaves);
        else
            status = 1;
        free(argv);
        return status;
    }
    argv = xrealloc(argv, (argc + 1) * sizeof(char *));
    argv[argc] = NULL;

    if (options.xtrace) {
        fputs("+", stderr);
        for (int i = 0; i < argc; i++) {
            fputc(' ', stderr);
            fputs(argv[i], stderr);
        }
        fputc('\n', stderr);
    }

    int ntemp = 0;
    TempAssign *temp = apply_temp_assigns(c->assigns, c->assign_count, &ntemp);
    int status;
    BuiltinFunc builtin = builtin_lookup(argv[0]);
    Node *func = function_lookup(argv[0]);

    if (builtin) {
        FdSave *saves = NULL;
        int nsaves = 0;
        if (apply_redirects(n->redirs, n->redir_count, &saves, &nsaves) < 0) {
            status = 1;
        } else {
            status = builtin(argv);
            if (strcmp(argv[0], "exec") == 0)
                discard_redirect_saves(saves, nsaves);
            else
                restore_redirects(saves, nsaves);
        }
    } else if (func) {
        FdSave *saves = NULL;
        int nsaves = 0;
        if (apply_redirects(n->redirs, n->redir_count, &saves, &nsaves) < 0) {
            status = 1;
        } else {
            status = call_function(func, argv, argc);
            restore_redirects(saves, nsaves);
        }
    } else {
        char *text = node_command_text(n);
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) {
            perror("ash: fork");
            status = 1;
        } else if (pid == 0) {
            shell.in_child = 1;
            reset_child_signals();
            FdSave *saves = NULL;
            int nsaves = 0;
            if (apply_redirects(n->redirs, n->redir_count, &saves, &nsaves) < 0)
                _exit(1);
            execvp(argv[0], argv);
            fprintf(stderr, "ash: %s: %s\n", argv[0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        } else {
            status = run_foreground(pid, text);
        }
        free(text);
    }
    restore_temp_assigns(temp, ntemp);
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
    return status;
}

static int exec_pipeline(Node *n)
{
    PipelineNode *p = &n->as.pipeline;
    if (p->stage_count == 1 && !p->negated)
        return exec_node(p->stages[0]);

    int npipes = p->stage_count - 1;
    int *fds = xmalloc(npipes * 2 * sizeof(int));
    for (int i = 0; i < npipes; i++) {
        if (pipe(fds + 2 * i) < 0) {
            perror("ash: pipe");
            free(fds);
            return 1;
        }
    }
    char *text = node_command_text(n);
    int job_id = -1;
    if (shell.interactive && shell.terminal_fd >= 0)
        job_id = job_create(0, text);
    pid_t pgid = 0;
    pid_t *pids = xmalloc(p->stage_count * sizeof(pid_t));
    int spawned = 0;
    for (int i = 0; i < p->stage_count; i++) {
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) {
            perror("ash: fork");
            break;
        }
        if (pid == 0) {
            if (job_id >= 0)
                setpgid(0, pgid ? pgid : 0);
            if (i > 0)
                dup2(fds[2 * (i - 1)], 0);
            if (i < p->stage_count - 1)
                dup2(fds[2 * i + 1], 1);
            for (int j = 0; j < npipes * 2; j++)
                close(fds[j]);
            shell.in_child = 1;
            reset_child_signals();
            int status = exec_node(p->stages[i]);
            fflush(NULL);
            _exit(shell.exit_pending ? shell.exit_status : status);
        }
        pids[spawned++] = pid;
        if (i == 0)
            pgid = pid;
        if (job_id >= 0) {
            setpgid(pid, pgid);
            job_add_pid(job_id, pid);
        }
    }
    for (int j = 0; j < npipes * 2; j++)
        close(fds[j]);
    int status = 1;
    if (job_id >= 0 && spawned == p->stage_count) {
        status = job_wait_foreground(job_id);
    } else {
        for (int i = 0; i < spawned; i++)
            status = wait_status(pids[i]);
        if (job_id >= 0)
            job_reap(0);
    }
    free(pids);
    free(fds);
    free(text);
    if (p->negated)
        status = status == 0;
    return status;
}

static int exec_background(Node *n)
{
    char *text = node_command_text(n);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("ash: fork");
        free(text);
        return 1;
    }
    if (pid == 0) {
        if (shell.interactive)
            setpgid(0, 0);
        shell.in_child = 1;
        reset_child_signals();
        int status = exec_node(n);
        fflush(NULL);
        _exit(shell.exit_pending ? shell.exit_status : status);
    }
    setpgid(pid, pid);
    int id = job_create(pid, text);
    job_add_pid(id, pid);
    printf("[%d] %d\n", id, pid);
    fflush(stdout);
    shell.last_bg_pid = pid;
    free(text);
    return 0;
}

static void check_errexit(int status)
{
    if (options.errexit && status != 0 && shell.condition_depth == 0 &&
        !shell.exit_pending) {
        shell.exit_pending = 1;
        shell.exit_status = status;
    }
}

int exec_node(Node *n)
{
    if (!n)
        return shell.last_status;
    if (shell.exit_pending || shell.return_pending || shell.break_pending ||
        shell.continue_pending)
        return shell.last_status;
    int status = 0;
    switch (n->kind) {
    case NODE_COMMAND:
        status = exec_command(n);
        break;
    case NODE_PIPELINE:
        status = exec_pipeline(n);
        break;
    case NODE_AND_OR: {
        shell.condition_depth++;
        int left = exec_node(n->as.and_or.left);
        shell.condition_depth--;
        shell.last_status = left;
        if ((!n->as.and_or.or_op && left == 0) ||
            (n->as.and_or.or_op && left != 0))
            status = exec_node(n->as.and_or.right);
        else
            status = left;
        break;
    }
    case NODE_LIST:
        for (int i = 0; i < n->as.list.count; i++) {
            if (n->as.list.separators[i] == '&') {
                exec_background(n->as.list.items[i]);
                status = 0;
            } else {
                status = exec_node(n->as.list.items[i]);
            }
            shell.last_status = status;
            check_errexit(status);
            if (shell.exit_pending || shell.return_pending ||
                shell.break_pending || shell.continue_pending)
                break;
        }
        break;
    case NODE_SUBSHELL: {
        char *text = node_command_text(n);
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) {
            perror("ash: fork");
            status = 1;
        } else if (pid == 0) {
            shell.in_child = 1;
            reset_child_signals();
            FdSave *saves = NULL;
            int nsaves = 0;
            if (apply_redirects(n->redirs, n->redir_count, &saves, &nsaves) < 0)
                _exit(1);
            int st = exec_node(n->as.body);
            fflush(NULL);
            _exit(shell.exit_pending ? shell.exit_status : st);
        } else {
            status = run_foreground(pid, text);
        }
        free(text);
        break;
    }
    case NODE_GROUP: {
        FdSave *saves = NULL;
        int nsaves = 0;
        if (apply_redirects(n->redirs, n->redir_count, &saves, &nsaves) < 0) {
            status = 1;
        } else {
            status = exec_node(n->as.body);
            restore_redirects(saves, nsaves);
        }
        break;
    }
    case NODE_IF: {
        int matched = 0;
        for (int i = 0; i < n->as.if_node.pair_count; i++) {
            shell.condition_depth++;
            int cond = exec_node(n->as.if_node.conds[i]);
            shell.condition_depth--;
            shell.last_status = cond;
            if (cond == 0) {
                status = exec_node(n->as.if_node.bodies[i]);
                matched = 1;
                break;
            }
        }
        if (!matched) {
            if (n->as.if_node.else_body)
                status = exec_node(n->as.if_node.else_body);
            else
                status = 0;
        }
        break;
    }
    case NODE_WHILE: {
        status = 0;
        for (;;) {
            shell.condition_depth++;
            int cond = exec_node(n->as.loop.cond);
            shell.condition_depth--;
            shell.last_status = cond;
            int run = n->as.loop.until ? cond != 0 : cond == 0;
            if (!run)
                break;
            shell.loop_depth++;
            status = exec_node(n->as.loop.body);
            shell.loop_depth--;
            shell.last_status = status;
            if (shell.break_pending) {
                shell.break_pending--;
                break;
            }
            if (shell.continue_pending) {
                shell.continue_pending--;
                continue;
            }
            if (shell.exit_pending || shell.return_pending)
                break;
            check_errexit(status);
        }
        break;
    }
    case NODE_FOR: {
        char **items = NULL;
        int nitems = 0;
        if (n->as.for_node.has_items) {
            for (int i = 0; i < n->as.for_node.item_count; i++) {
                int nf = 0;
                char **f = expand_word_fields(&n->as.for_node.items[i], 0, &nf);
                if (nf < 0) {
                    status = 1;
                    break;
                }
                for (int j = 0; j < nf; j++) {
                    items = xrealloc(items, (nitems + 1) * sizeof(char *));
                    items[nitems++] = f[j];
                }
                free(f);
            }
        } else {
            for (int i = 0; i < shell.positional_count; i++) {
                items = xrealloc(items, (nitems + 1) * sizeof(char *));
                items[nitems++] = xstrdup(shell.positional[i]);
            }
        }
        status = 0;
        for (int i = 0; i < nitems; i++) {
            var_set(n->as.for_node.name, items[i], 0);
            shell.loop_depth++;
            status = exec_node(n->as.for_node.body);
            shell.loop_depth--;
            shell.last_status = status;
            if (shell.break_pending) {
                shell.break_pending--;
                break;
            }
            if (shell.continue_pending) {
                shell.continue_pending--;
                continue;
            }
            if (shell.exit_pending || shell.return_pending)
                break;
            check_errexit(status);
        }
        for (int i = 0; i < nitems; i++)
            free(items[i]);
        free(items);
        break;
    }
    case NODE_CASE: {
        char *subject = expand_word_string(&n->as.case_node.subject, 0);
        status = 0;
        if (subject) {
            int matched = 0;
            for (int i = 0; i < n->as.case_node.arm_count && !matched; i++) {
                CaseArm *arm = &n->as.case_node.arms[i];
                for (int j = 0; j < arm->pattern_count; j++) {
                    char *pat = expand_word_string(&arm->patterns[j],
                                                   EXP_NOGLOB | EXP_NOSPLIT);
                    if (!pat)
                        continue;
                    int hit = fnmatch(pat, subject, 0) == 0;
                    free(pat);
                    if (hit) {
                        if (arm->body)
                            status = exec_node(arm->body);
                        matched = 1;
                        break;
                    }
                }
            }
            free(subject);
        } else {
            status = 1;
        }
        break;
    }
    case NODE_FUNCDEF:
        function_define(n->as.funcdef.name, node_clone(n->as.funcdef.body));
        status = 0;
        break;
    }
    shell.last_status = status;
    check_errexit(status);
    return status;
}

int exec_string(const char *text)
{
    ParseOutput po = parse_string(text);
    if (po.result == PARSE_ERROR) {
        fprintf(stderr, "ash: %s\n", po.error);
        shell.last_status = 2;
        return 2;
    }
    if (po.result == PARSE_INCOMPLETE) {
        fputs("ash: unexpected end of file\n", stderr);
        shell.last_status = 2;
        return 2;
    }
    int status = exec_node(po.root);
    node_free(po.root);
    trap_run_pending();
    return status;
}

int exec_command_string_capture(const char *text, char **output)
{
    int fds[2];
    if (pipe(fds) < 0) {
        perror("ash: pipe");
        *output = xstrdup("");
        return 1;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("ash: fork");
        close(fds[0]);
        close(fds[1]);
        *output = xstrdup("");
        return 1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        shell.in_child = 1;
        reset_child_signals();
        int status = exec_string(text);
        fflush(NULL);
        _exit(shell.exit_pending ? shell.exit_status : status);
    }
    close(fds[1]);
    StrBuf sb;
    sb_init(&sb);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        sb_append_n(&sb, buf, n);
    close(fds[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            break;
    shell.last_status = status_from(st);
    while (sb.len > 0 && sb.data[sb.len - 1] == '\n') {
        sb.len--;
        sb.data[sb.len] = '\0';
    }
    *output = sb_take(&sb);
    return shell.last_status;
}

int read_complete_command(FILE *in, char **out)
{
    StrBuf acc;
    sb_init(&acc);
    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        ssize_t n = getline(&line, &cap, in);
        if (n < 0) {
            free(line);
            if (acc.len == 0) {
                sb_free(&acc);
                return 0;
            }
            ParseOutput po = parse_string(acc.data);
            if (po.result == PARSE_OK) {
                node_free(po.root);
                *out = sb_take(&acc);
                return 1;
            }
            fputs("ash: unexpected end of file\n", stderr);
            sb_free(&acc);
            return -1;
        }
        sb_append(&acc, line);
        ParseOutput po = parse_string(acc.data);
        if (po.result == PARSE_OK) {
            node_free(po.root);
            free(line);
            *out = sb_take(&acc);
            return 1;
        }
        if (po.result == PARSE_ERROR) {
            fprintf(stderr, "ash: %s\n", po.error);
            free(line);
            sb_free(&acc);
            return -1;
        }
    }
}

int source_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ash: %s: %s\n", path, strerror(errno));
        return 1;
    }
    char *cmd;
    int status = 0;
    int rc;
    while ((rc = read_complete_command(f, &cmd)) == 1) {
        status = exec_string(cmd);
        free(cmd);
        if (shell.exit_pending || shell.return_pending)
            break;
    }
    fclose(f);
    return rc < 0 ? 1 : status;
}