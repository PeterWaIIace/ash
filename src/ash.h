#ifndef ASH_H
#define ASH_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static inline void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("ash: out of memory\n", stderr);
        exit(2);
    }
    return p;
}

static inline void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("ash: out of memory\n", stderr);
        exit(2);
    }
    return q;
}

static inline char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p) {
        fputs("ash: out of memory\n", stderr);
        exit(2);
    }
    return p;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static inline void sb_init(StrBuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static inline void sb_append_n(StrBuf *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        sb->cap = (sb->len + n + 1) * 2;
        sb->data = xrealloc(sb->data, sb->cap);
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static inline void sb_append(StrBuf *sb, const char *s)
{
    sb_append_n(sb, s, strlen(s));
}

static inline void sb_append_char(StrBuf *sb, char c)
{
    sb_append_n(sb, &c, 1);
}

static inline char *sb_take(StrBuf *sb)
{
    char *p = sb->data ? sb->data : xstrdup("");
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return p;
}

static inline void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

typedef enum {
    PIECE_TEXT,
    PIECE_PARAM,
    PIECE_CMDSUB,
    PIECE_ARITH
} PieceKind;

typedef struct {
    PieceKind kind;
    int quoted;
    char *text;
} Piece;

typedef struct {
    Piece *items;
    int count;
    int capacity;
} Word;

void word_init(Word *w);
void word_free(Word *w);
void word_add_piece(Word *w, PieceKind kind, int quoted, const char *text);
void word_append_text(Word *w, int quoted, const char *text, size_t n);
char *word_raw_text(const Word *w);
Word word_clone(const Word *w);

typedef struct {
    char *text;
    int quoted;
    int claimed;
} HeredocBody;

typedef enum {
    REDIR_INPUT,
    REDIR_OUTPUT,
    REDIR_APPEND,
    REDIR_CLOBBER,
    REDIR_RDWR,
    REDIR_HEREDOC,
    REDIR_HERESTRING,
    REDIR_DUP_IN,
    REDIR_DUP_OUT,
    REDIR_ALL_OUT,
    REDIR_ALL_APPEND
} RedirKind;

typedef struct {
    RedirKind kind;
    int fd;
    Word target;
    HeredocBody *heredoc;
} Redirect;

typedef enum {
    TOK_WORD,
    TOK_NEWLINE,
    TOK_EOF,
    TOK_SEMI,
    TOK_DSEMI,
    TOK_AMP,
    TOK_AND_IF,
    TOK_OR_IF,
    TOK_PIPE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_REDIR
} TokenKind;

typedef struct {
    TokenKind kind;
    Word word;
    RedirKind redir_kind;
    int fd;
    HeredocBody *heredoc;
} Token;

typedef struct {
    const char *input;
    size_t pos;
    int incomplete;
    char error[256];
    struct {
        char *delim;
        int strip_tabs;
        HeredocBody *body;
    } pending[32];
    int pending_count;
    HeredocBody *bodies[64];
    int body_count;
} Lexer;

void lexer_init(Lexer *lx, const char *input);
int lexer_next(Lexer *lx, Token *tok);
void lexer_free(Lexer *lx);
int lex_single_word(const char *text, Word *out);
void lex_heredoc_body(const char *body, Word *out);

typedef enum {
    NODE_COMMAND,
    NODE_PIPELINE,
    NODE_AND_OR,
    NODE_LIST,
    NODE_SUBSHELL,
    NODE_GROUP,
    NODE_IF,
    NODE_WHILE,
    NODE_FOR,
    NODE_CASE,
    NODE_FUNCDEF
} NodeKind;

typedef struct Node Node;

typedef struct {
    Word *assigns;
    int assign_count;
    Word *words;
    int word_count;
} CommandNode;

typedef struct {
    Node **stages;
    int stage_count;
    int negated;
} PipelineNode;

typedef struct {
    Node *left;
    Node *right;
    int or_op;
} AndOrNode;

typedef struct {
    Node **items;
    int *separators;
    int count;
} ListNode;

typedef struct {
    Node **conds;
    Node **bodies;
    int pair_count;
    Node *else_body;
} IfNode;

typedef struct {
    Node *cond;
    Node *body;
    int until;
} LoopNode;

typedef struct {
    char *name;
    Word *items;
    int item_count;
    int has_items;
    Node *body;
} ForNode;

typedef struct {
    Word *patterns;
    int pattern_count;
    Node *body;
} CaseArm;

typedef struct {
    Word subject;
    CaseArm *arms;
    int arm_count;
} CaseNode;

typedef struct {
    char *name;
    Node *body;
} FuncDefNode;

struct Node {
    NodeKind kind;
    Redirect *redirs;
    int redir_count;
    union {
        CommandNode command;
        PipelineNode pipeline;
        AndOrNode and_or;
        ListNode list;
        Node *body;
        IfNode if_node;
        LoopNode loop;
        ForNode for_node;
        CaseNode case_node;
        FuncDefNode funcdef;
    } as;
};

typedef enum {
    PARSE_OK,
    PARSE_INCOMPLETE,
    PARSE_ERROR
} ParseResult;

typedef struct {
    ParseResult result;
    Node *root;
    char error[256];
} ParseOutput;

ParseOutput parse_string(const char *input);
void node_free(Node *n);
Node *node_clone(const Node *n);
char *node_command_text(Node *n);

typedef struct {
    int last_status;
    pid_t shell_pid;
    const char *shell_name;
    char **positional;
    int positional_count;
    int interactive;
    int exit_pending;
    int exit_status;
    int return_pending;
    int break_pending;
    int continue_pending;
    int loop_depth;
    int function_depth;
    int condition_depth;
    pid_t last_bg_pid;
    int terminal_fd;
    int in_child;
} ShellState;

extern ShellState shell;

typedef struct {
    int errexit;
    int nounset;
    int xtrace;
    int noclobber;
    int interactive_comments;
    int ignoreeof;
    int transcript;
} ShellOptions;

extern ShellOptions options;

int option_set(const char *name, int value);
const char *option_name(int index);
int option_value(int index);
int option_count(void);

void vars_init(void);
void var_import_environ(void);
const char *var_get(const char *name);
void var_set(const char *name, const char *value, int exported);
void var_unset(const char *name);
int var_is_exported(const char *name);
void var_export(const char *name);
void var_push_scope(void);
void var_pop_scope(void);
void var_set_local(const char *name, const char *value);
void var_print_all(FILE *out);
void var_print_exported(FILE *out);
void shell_set_positional(char **args, int count);

#define EXP_ASSIGN 1
#define EXP_NOGLOB 2
#define EXP_NOSPLIT 4

char *expand_word_string(const Word *w, int flags);
char **expand_word_fields(const Word *w, int flags, int *count);
void free_fields(char **fields, int count);
char *expand_prompt(const char *fmt);
char *expand_heredoc(const char *body);
long long arith_eval(const char *expr, int *error);

typedef struct {
    int fd;
    int orig;
} FdSave;

int exec_node(Node *n);
int exec_string(const char *text);
int exec_command_string_capture(const char *text, char **output);
int source_file(const char *path);
int read_complete_command(FILE *in, char **out);
int apply_redirects(Redirect *redirs, int count, FdSave **saves, int *save_count);
void restore_redirects(FdSave *saves, int save_count);

void function_define(const char *name, Node *body);
Node *function_lookup(const char *name);
int function_count(void);
const char *function_name(int index);

typedef int (*BuiltinFunc)(char **argv);
BuiltinFunc builtin_lookup(const char *name);
int builtin_count(void);
const char *builtin_name(int index);
int agent_ask(char **argv);

void alias_set(const char *name, const char *value);
const char *alias_get(const char *name);
int alias_unset(const char *name);
int alias_count(void);
const char *alias_name(int index);
void alias_print_all(FILE *out);

void jobs_init(void);
int job_create(pid_t pgid, const char *text);
void job_add_pid(int job_id, pid_t pid);
void job_reap(int notify);
void job_print(FILE *out, int with_pids);
int job_wait_foreground(int job_id);
int job_fg(const char *spec);
int job_bg(const char *spec);
int job_wait_all(void);
int job_wait_pid(pid_t pid);
pid_t job_pgid(const char *spec);
int job_signal(const char *spec, int signo);
int job_count(void);

void trap_set(int signo, const char *command);
const char *trap_get(int signo);
void trap_run_pending(void);
void trap_run_exit(void);
void trap_reset_child(void);
int trap_signal_number(const char *name);
const char *trap_signal_name(int signo);

void lineedit_init(void);
char *lineedit_read(const char *prompt);
int hist_count(void);
const char *hist_line(int index);
void hist_clear(void);
void transcript_spawn(void);

#endif