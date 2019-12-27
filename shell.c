#include <readline/readline.h>
#include <readline/history.h>

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static sigjmp_buf loop_env;

static void sigint_handler(int sig)
{
  siglongjmp(loop_env, sig);
}

/* Rewrite closed file descriptors to -1,
 * to make sure we don't attempt do close them twice. */
static void MaybeClose(int *fdp)
{
  if (*fdp < 0)
    return;
  Close(*fdp);
  *fdp = -1;
}

/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp)
{
  token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
  int n = 0;           /* number of tokens after redirections are removed */

  for (int i = 0; i < ntokens; i++)
  {
    /* TODO: Handle tokens and open files as requested. */
    mode = token[i];
    if (mode == NULL)
      return i;
    if (mode == T_INPUT || mode == T_OUTPUT)
    {
      assert(i + 1 < ntokens && token[i + 1] != NULL && "redir operator without a filename");
      assert(token[i + 1] >= (token_t)10 && "bad syntax: another operator just after the first one");
      if (mode == T_INPUT)
      {
        MaybeClose(inputp);
        *inputp = Open(token[i + 1], O_RDONLY, 0);
      }
      else //mode == T_OUTPUT
      {
        MaybeClose(outputp);
        *outputp = Open(token[i + 1], O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP);
      }
      for (int j = i + 2; j < ntokens; j++)
        token[j - 2] = token[j];
      ntokens -= 2;
      i--;
    }
  }
  n = ntokens;
  token[n] = NULL;
  return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t *token, int ntokens, bool bg)
{
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);

  if (!bg)
  {
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;
  }

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start a subprocess, create a job and monitor it. */

  pid_t pid = Fork();

  if (pid == 0) //child
  {
    sigprocmask(SIG_SETMASK, &mask, NULL);
    if (input != -1)
      dup2(input, 0);
    if (output != -1)
      dup2(output, 1);
    setpgid(0, 0);
    Signal(SIGINT, SIG_DFL);
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    if (builtin_command(token) >= 0)
      exit(0);
    //assert to check whether the tokens vector is a vector of strings and not shell operators
    for (int i = 0; i < ntokens; i++)
      assert((token[i] == NULL || token[i] >= (token_t)10) && "shell operator in argv of an external command"); //10==(max of value of shell operator)+1
    external_command(token);
  }
  //else // parent
  setpgid(pid, pid);
  MaybeClose(&input);
  MaybeClose(&output);
  int job_id = addjob(pid, bg);
  addproc(job_id, pid, token);
  if (!bg)
    exitcode = monitorjob(&mask);

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens)
{
  ntokens = do_redir(token, ntokens, &input, &output);

  if (ntokens == 0)
    app_error("ERROR: Command line is not well formed!");

  /* TODO: Start a subprocess and make sure it's moved to a process group. */
  pid_t pid = Fork();
  if (pid == 0) //child
  {
    sigprocmask(SIG_SETMASK, mask, NULL);
    if (input != -1)
      dup2(input, 0);
    if (output != -1)
      dup2(output, 1);
    setpgid(0, pgid);
    Signal(SIGINT, SIG_DFL);
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    if (builtin_command(token) >= 0)
      exit(0);
    //assert to check whether the tokens vector is a vector of strings and not shell operators
    for (int i = 0; i < ntokens; i++)
      assert((token[i] == NULL || token[i] >= (token_t)10) && "shell operator in argv of an external command"); //10==(max of value of shell operator)+1
    external_command(token);
  }
  if (pgid == 0)
    setpgid(pid, pid);
  else
    setpgid(pid, pgid);
  MaybeClose(&input);
  MaybeClose(&output);
  return pid;
}

static void mkpipe(int *readp, int *writep)
{
  int fds[2];
  Pipe(fds);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *readp = fds[0];
  *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t *token, int ntokens, bool bg)
{
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  mkpipe(&next_input, &output);

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start pipeline subprocesses, create a job and monitor it.
   * Remember to close unused pipe ends! */

  int latest_t_pipe = -1;
  int flags;

  flags = fcntl(output, F_GETFD);
  fcntl(output, F_SETFD, flags & ~FD_CLOEXEC);

  for (int i = 0; i < ntokens; i++)
  {
    if (token[i] == T_PIPE)
    {
      assert(i + 1 < ntokens && token[i + 1] >= (token_t)10 && "bad syntax: operator or end of command after pipe symbol");
      if (pgid == 0)
      {
        pid = do_stage(pgid, &mask, input, output, token, i);
        pgid = pid;
        latest_t_pipe = i;
        job = addjob(pgid, bg);
        token[i] = NULL;
        addproc(job, pid, token);
      }
      else
      {
        pid = do_stage(pgid, &mask, input, output, &token[latest_t_pipe + 1], i - latest_t_pipe - 1);
        addproc(job, pid, &token[latest_t_pipe + 1]);
        latest_t_pipe = i;
        token[i] = NULL;
      }
      //Zamykam deskryptory potokow w do_stage, bo obsluguje przypadek, ze sa one zastapione przez deskrytory otwarte w do_redir,
      //a procedura do_stage nie daje mi mozliwosci zwrocenia nowych(tj. otwartych w do_redir) deskryptorow spowrotem do procedury do_pipeline
      flags = fcntl(next_input, F_GETFD);
      fcntl(next_input, F_SETFD, flags & ~FD_CLOEXEC);
      input = next_input;
      mkpipe(&next_input, &output);
      flags = fcntl(output, F_GETFD);
      fcntl(output, F_SETFD, flags & ~FD_CLOEXEC);
    }
  }
  //wykonujemy koncowa czesc polecenia tj. te po ostatnim znaku '|'
  MaybeClose(&output);
  MaybeClose(&next_input);
  pid = do_stage(pgid, &mask, input, output, &token[latest_t_pipe + 1], ntokens - latest_t_pipe - 1);
  addproc(job, pid, &token[latest_t_pipe + 1]);
  if (!bg)
    exitcode = monitorjob(&mask);

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens)
{
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline)
{
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB)
  {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0)
  {
    if (is_pipeline(token, ntokens))
    {
      do_pipeline(token, ntokens, bg);
    }
    else
    {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

int main(int argc, char *argv[])
{
  rl_initialize();

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  initjobs();

  Signal(SIGINT, sigint_handler);
  Signal(SIGTSTP, SIG_IGN);
  Signal(SIGTTIN, SIG_IGN);
  Signal(SIGTTOU, SIG_IGN);

  char *line;
  while (true)
  {
    if (!sigsetjmp(loop_env, 1))
    {
      line = readline("# ");
    }
    else
    {
      msg("\n");
      continue;
    }

    if (line == NULL)
      break;

    if (strlen(line))
    {
      add_history(line);
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}
