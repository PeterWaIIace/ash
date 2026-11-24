#include "ash.h"
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_JOBS 128
#define MAX_JOB_PIDS 256

typedef struct {
    int id;
    pid_t pgid;
    pid_t pids[MAX_JOB_PIDS];
    int states[MAX_JOB_PIDS];
    int statuses[MAX_JOB_PIDS];
    int pid_count;
    char *text;
    int notified;
    int foreground;
} Job;

static Job *job_table[MAX_JOBS];
static int job_total;
static int next_id = 1;

void jobs_init(void)
{
    job_total = 0;
    next_id = 1;
}

static Job *job_by_id(int id)
{
    for (int i = 0; i < job_total; i++)
        if (job_table[i]->id == id)
            return job_table[i];
    return NULL;
}

static void job_remove(Job *j)
{
    for (int i = 0; i < job_total; i++) {
        if (job_table[i] == j) {
            free(j->text);
            free(j);
            memmove(&job_table[i], &job_table[i + 1],
                    (job_total - i - 1) * sizeof(Job *));
            job_total--;
            return;
        }
    }
}

static int job_has_stopped(Job *j)
{
    for (int i = 0; i < j->pid_count; i++)
        if (j->states[i] == 1)
            return 1;
    return 0;
}

static int job_all_done(Job *j)
{
    for (int i = 0; i < j->pid_count; i++)
        if (j->states[i] != 2)
            return 0;
    return j->pid_count > 0;
}

int job_create(pid_t pgid, const char *text)
{
    if (job_total == MAX_JOBS)
        job_reap(0);
    if (job_total == MAX_JOBS)
        return -1;
    Job *j = xmalloc(sizeof(Job));
    memset(j, 0, sizeof(Job));
    j->id = next_id++;
    j->pgid = pgid;
    j->text = xstrdup(text);
    job_table[job_total++] = j;
    return j->id;
}

void job_add_pid(int job_id, pid_t pid)
{
    Job *j = job_by_id(job_id);
    if (!j || j->pid_count == MAX_JOB_PIDS)
        return;
    if (j->pgid == 0)
        j->pgid = pid;
    j->pids[j->pid_count] = pid;
    j->states[j->pid_count] = 0;
    j->statuses[j->pid_count] = 0;
    j->pid_count++;
}

void job_reap(int notify)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        for (int i = 0; i < job_total; i++) {
            Job *j = job_table[i];
            for (int k = 0; k < j->pid_count; k++) {
                if (j->pids[k] != pid)
                    continue;
                if (WIFSTOPPED(st)) {
                    j->states[k] = 1;
                } else if (WIFCONTINUED(st)) {
                    j->states[k] = 0;
                    j->notified = 0;
                } else {
                    j->states[k] = 2;
                    j->statuses[k] = WIFEXITED(st) ? WEXITSTATUS(st)
                                                   : 128 + WTERMSIG(st);
                }
            }
        }
    }
    if (!notify)
        return;
    for (int i = 0; i < job_total;) {
        Job *j = job_table[i];
        if (j->foreground) {
            i++;
            continue;
        }
        if (job_all_done(j) && !j->notified) {
            int last = j->statuses[j->pid_count - 1];
            if (last == 0)
                printf("[%d]  Done                    %s\n", j->id, j->text);
            else if (last > 128)
                printf("[%d]  Terminated              %s\n", j->id, j->text);
            else
                printf("[%d]  Exit %d                 %s\n", j->id, last,
                       j->text);
            fflush(stdout);
            job_remove(j);
            continue;
        }
        if (job_has_stopped(j) && !j->notified) {
            printf("[%d]+ Stopped                 %s\n", j->id, j->text);
            fflush(stdout);
            j->notified = 1;
        }
        i++;
    }
}

static Job *job_current(void)
{
    for (int i = job_total - 1; i >= 0; i--)
        if (job_has_stopped(job_table[i]))
            return job_table[i];
    if (job_total > 0)
        return job_table[job_total - 1];
    return NULL;
}

static Job *job_previous(void)
{
    Job *cur = job_current();
    for (int i = job_total - 1; i >= 0; i--)
        if (job_table[i] != cur)
            return job_table[i];
    return NULL;
}

static Job *job_resolve(const char *spec)
{
    if (!spec || !spec[0] || strcmp(spec, "%+") == 0 || strcmp(spec, "%%") == 0)
        return job_current();
    if (strcmp(spec, "%-") == 0)
        return job_previous();
    if (spec[0] == '%') {
        const char *body = spec + 1;
        char *end;
        long id = strtol(body, &end, 10);
        if (*end == '\0' && end != body)
            return job_by_id((int)id);
        for (int i = 0; i < job_total; i++)
            if (strncmp(job_table[i]->text, body, strlen(body)) == 0)
                return job_table[i];
        return NULL;
    }
    char *end;
    long id = strtol(spec, &end, 10);
    if (*end == '\0' && end != spec) {
        Job *j = job_by_id((int)id);
        if (j)
            return j;
        for (int i = 0; i < job_total; i++)
            for (int k = 0; k < job_table[i]->pid_count; k++)
                if (job_table[i]->pids[k] == (pid_t)id)
                    return job_table[i];
    }
    return NULL;
}

void job_print(FILE *out, int with_pids)
{
    Job *cur = job_current();
    Job *prev = job_previous();
    for (int i = 0; i < job_total; i++) {
        Job *j = job_table[i];
        char marker = ' ';
        if (j == cur)
            marker = '+';
        else if (j == prev)
            marker = '-';
        const char *state = job_has_stopped(j) ? "Stopped" : "Running";
        if (job_all_done(j))
            state = "Done";
        if (with_pids)
            fprintf(out, "[%d]%c %d %s                 %s\n", j->id, marker,
                    (int)j->pgid, state, j->text);
        else
            fprintf(out, "[%d]%c %s                 %s\n", j->id, marker,
                    state, j->text);
    }
}

int job_wait_foreground(int job_id)
{
    Job *j = job_by_id(job_id);
    if (!j)
        return 1;
    j->foreground = 1;
    if (shell.interactive && shell.terminal_fd >= 0)
        tcsetpgrp(shell.terminal_fd, j->pgid);
    int last_status = 0;
    int stopped = 0;
    int stop_sig = 0;
    for (int i = 0; i < j->pid_count; i++) {
        int st = 0;
        pid_t rc;
        while ((rc = waitpid(j->pids[i], &st, WUNTRACED)) < 0)
            if (errno != EINTR)
                break;
        if (rc < 0) {
            j->states[i] = 2;
            continue;
        }
        if (WIFSTOPPED(st)) {
            j->states[i] = 1;
            stopped = 1;
            stop_sig = WSTOPSIG(st);
        } else {
            j->states[i] = 2;
            j->statuses[i] = WIFEXITED(st) ? WEXITSTATUS(st)
                                           : 128 + WTERMSIG(st);
            last_status = j->statuses[i];
        }
    }
    if (shell.interactive && shell.terminal_fd >= 0)
        tcsetpgrp(shell.terminal_fd, getpgrp());
    j->foreground = 0;
    if (stopped) {
        printf("[%d]+ Stopped                 %s\n", j->id, j->text);
        fflush(stdout);
        j->notified = 1;
        return 128 + stop_sig;
    }
    job_remove(j);
    return last_status;
}

int job_fg(const char *spec)
{
    Job *j = job_resolve(spec);
    if (!j) {
        fputs("ash: fg: no current job\n", stderr);
        return 1;
    }
    if (job_has_stopped(j)) {
        killpg(j->pgid, SIGCONT);
        for (int i = 0; i < j->pid_count; i++)
            if (j->states[i] == 1)
                j->states[i] = 0;
    }
    printf("%s\n", j->text);
    fflush(stdout);
    return job_wait_foreground(j->id);
}

int job_bg(const char *spec)
{
    Job *j = job_resolve(spec);
    if (!j) {
        fputs("ash: bg: no current job\n", stderr);
        return 1;
    }
    killpg(j->pgid, SIGCONT);
    for (int i = 0; i < j->pid_count; i++)
        if (j->states[i] == 1)
            j->states[i] = 0;
    j->notified = 0;
    printf("[%d] %s &\n", j->id, j->text);
    fflush(stdout);
    return 0;
}

int job_wait_all(void)
{
    int status = 0;
    for (int i = 0; i < job_total;) {
        Job *j = job_table[i];
        if (j->foreground) {
            i++;
            continue;
        }
        int has_running = 0;
        for (int k = 0; k < j->pid_count; k++)
            if (j->states[k] == 0)
                has_running = 1;
        if (!has_running) {
            if (job_has_stopped(j))
                status = 128 + SIGTSTP;
            else if (j->pid_count > 0)
                status = j->statuses[j->pid_count - 1];
            if (job_all_done(j)) {
                job_remove(j);
                continue;
            }
            i++;
            continue;
        }
        for (int k = 0; k < j->pid_count; k++) {
            if (j->states[k] == 2)
                continue;
            int st;
            while (waitpid(j->pids[k], &st, WUNTRACED) < 0)
                if (errno != EINTR)
                    break;
            if (WIFSTOPPED(st)) {
                j->states[k] = 1;
                status = 128 + WSTOPSIG(st);
            } else {
                j->states[k] = 2;
                j->statuses[k] = WIFEXITED(st) ? WEXITSTATUS(st)
                                               : 128 + WTERMSIG(st);
                status = j->statuses[k];
            }
        }
        if (job_all_done(j)) {
            job_remove(j);
            continue;
        }
        i++;
    }
    return status;
}

int job_wait_pid(pid_t pid)
{
    for (int i = 0; i < job_total; i++) {
        Job *j = job_table[i];
        for (int k = 0; k < j->pid_count; k++) {
            if (j->pids[k] != pid)
                continue;
            if (j->states[k] == 2)
                return j->statuses[k];
            if (j->states[k] == 1)
                return 128 + SIGTSTP;
            int st;
            while (waitpid(pid, &st, WUNTRACED) < 0)
                if (errno != EINTR)
                    return 127;
            if (WIFSTOPPED(st)) {
                j->states[k] = 1;
                return 128 + WSTOPSIG(st);
            }
            j->states[k] = 2;
            j->statuses[k] = WIFEXITED(st) ? WEXITSTATUS(st)
                                           : 128 + WTERMSIG(st);
            return j->statuses[k];
        }
    }
    int st;
    if (waitpid(pid, &st, 0) < 0)
        return 127;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

pid_t job_pgid(const char *spec)
{
    Job *j = job_resolve(spec);
    return j ? j->pgid : -1;
}

int job_signal(const char *spec, int signo)
{
    Job *j = job_resolve(spec);
    if (!j) {
        fprintf(stderr, "ash: kill: %s: no such job\n", spec);
        return 1;
    }
    if (killpg(j->pgid, signo) < 0) {
        perror("ash: kill");
        return 1;
    }
    return 0;
}

int job_count(void)
{
    return job_total;
}