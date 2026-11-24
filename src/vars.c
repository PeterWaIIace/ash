#include "ash.h"

ShellState shell;
ShellOptions options = { 0, 0, 0, 0, 1, 0, 1 };

extern char **environ;

typedef struct {
    char *name;
    char *value;
    int exported;
} Var;

static Var *vars;
static int var_count;
static int var_capacity;

typedef struct {
    char *name;
    char *old_value;
    int old_exported;
    int had_old;
} Undo;

static Undo *undo_stack;
static int undo_count;
static int undo_capacity;

static int *scope_marks;
static int scope_count;
static int scope_capacity;

static int var_find(const char *name)
{
    for (int i = 0; i < var_count; i++)
        if (strcmp(vars[i].name, name) == 0)
            return i;
    return -1;
}

void vars_init(void)
{
    vars = NULL;
    var_count = 0;
    var_capacity = 0;
}

void var_import_environ(void)
{
    for (char **e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        char *name = xmalloc(eq - *e + 1);
        memcpy(name, *e, eq - *e);
        name[eq - *e] = '\0';
        var_set(name, eq + 1, 1);
        free(name);
    }
}

const char *var_get(const char *name)
{
    int i = var_find(name);
    return i >= 0 ? vars[i].value : NULL;
}

void var_set(const char *name, const char *value, int exported)
{
    int i = var_find(name);
    if (i >= 0) {
        free(vars[i].value);
        vars[i].value = xstrdup(value);
        if (exported)
            vars[i].exported = 1;
        if (vars[i].exported)
            setenv(name, value, 1);
        return;
    }
    if (var_count == var_capacity) {
        var_capacity = var_capacity ? var_capacity * 2 : 32;
        vars = xrealloc(vars, var_capacity * sizeof(Var));
    }
    vars[var_count].name = xstrdup(name);
    vars[var_count].value = xstrdup(value);
    vars[var_count].exported = exported;
    var_count++;
    if (exported)
        setenv(name, value, 1);
}

void var_unset(const char *name)
{
    int i = var_find(name);
    if (i < 0)
        return;
    free(vars[i].name);
    free(vars[i].value);
    memmove(&vars[i], &vars[i + 1], (var_count - i - 1) * sizeof(Var));
    var_count--;
    unsetenv(name);
}

int var_is_exported(const char *name)
{
    int i = var_find(name);
    return i >= 0 && vars[i].exported;
}

void var_export(const char *name)
{
    int i = var_find(name);
    if (i < 0) {
        var_set(name, "", 1);
        return;
    }
    vars[i].exported = 1;
    setenv(name, vars[i].value, 1);
}

void var_push_scope(void)
{
    if (scope_count == scope_capacity) {
        scope_capacity = scope_capacity ? scope_capacity * 2 : 16;
        scope_marks = xrealloc(scope_marks, scope_capacity * sizeof(int));
    }
    scope_marks[scope_count++] = undo_count;
}

void var_pop_scope(void)
{
    if (scope_count == 0)
        return;
    int mark = scope_marks[--scope_count];
    while (undo_count > mark) {
        Undo *u = &undo_stack[--undo_count];
        if (u->had_old)
            var_set(u->name, u->old_value, u->old_exported);
        else
            var_unset(u->name);
        free(u->name);
        free(u->old_value);
    }
}

void var_set_local(const char *name, const char *value)
{
    if (undo_count == undo_capacity) {
        undo_capacity = undo_capacity ? undo_capacity * 2 : 32;
        undo_stack = xrealloc(undo_stack, undo_capacity * sizeof(Undo));
    }
    Undo *u = &undo_stack[undo_count++];
    u->name = xstrdup(name);
    int i = var_find(name);
    u->had_old = i >= 0;
    u->old_value = i >= 0 ? xstrdup(vars[i].value) : NULL;
    u->old_exported = i >= 0 ? vars[i].exported : 0;
    var_set(name, value, 0);
    i = var_find(name);
    if (i >= 0 && vars[i].exported) {
        vars[i].exported = 0;
        unsetenv(name);
    }
}

void var_print_all(FILE *out)
{
    for (int i = 0; i < var_count; i++)
        fprintf(out, "%s=%s\n", vars[i].name, vars[i].value);
}

void var_print_exported(FILE *out)
{
    for (int i = 0; i < var_count; i++)
        if (vars[i].exported)
            fprintf(out, "export %s=%s\n", vars[i].name, vars[i].value);
}

void shell_set_positional(char **args, int count)
{
    for (int i = 0; i < shell.positional_count; i++)
        free(shell.positional[i]);
    free(shell.positional);
    shell.positional = NULL;
    shell.positional_count = count;
    if (count > 0) {
        shell.positional = xmalloc(count * sizeof(char *));
        for (int i = 0; i < count; i++)
            shell.positional[i] = xstrdup(args[i]);
    }
}

typedef struct {
    const char *name;
    int *flag;
} OptionEntry;

static OptionEntry option_table[] = {
    { "errexit", &options.errexit },
    { "nounset", &options.nounset },
    { "xtrace", &options.xtrace },
    { "noclobber", &options.noclobber },
    { "interactivecomments", &options.interactive_comments },
    { "interactive_comments", &options.interactive_comments },
    { "ignoreeof", &options.ignoreeof },
    { "transcript", &options.transcript },
};

int option_set(const char *name, int value)
{
    for (size_t i = 0; i < sizeof(option_table) / sizeof(option_table[0]); i++) {
        if (strcmp(option_table[i].name, name) == 0) {
            *option_table[i].flag = value;
            return 0;
        }
    }
    return -1;
}

int option_count(void)
{
    return 7;
}

const char *option_name(int index)
{
    static const char *names[] = {
        "errexit", "nounset", "xtrace", "noclobber",
        "interactive_comments", "ignoreeof", "transcript"
    };
    return names[index];
}

int option_value(int index)
{
    int *flags[] = {
        &options.errexit, &options.nounset, &options.xtrace,
        &options.noclobber, &options.interactive_comments,
        &options.ignoreeof, &options.transcript
    };
    return *flags[index];
}