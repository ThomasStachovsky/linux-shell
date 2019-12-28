#include "shell.h"

typedef struct proc
{
  pid_t pid;    /* process identifier */
  int state;    /* RUNNING or STOPPED or FINISHED */
  int exitcode; /* -1 if exit status not yet received */
} proc_t;

typedef struct job
{
  pid_t pgid;            /* 0 if slot is free */
  proc_t *proc;          /* array of processes running in as a job */
  struct termios tmodes; /* saved terminal modes */
  int nproc;             /* number of processes */
  int state;             /* changes when live processes have same state */
  char *command;         /* textual representation of command line */
} job_t;

static job_t *jobs = NULL;          /* array of all jobs */
static int njobmax = 1;             /* number of slots in jobs array */
static int tty_fd = -1;             /* controlling terminal file descriptor */
static struct termios shell_tmodes; /* saved shell terminal modes */

static void sigchld_handler(int sig)
{
  int old_errno = errno;
  pid_t pid;
  int status;
  /* TODO: Change state (FINISHED, RUNNING, STOPPED) of processes and jobs.
   * Bury all children that finished saving their status in jobs. */
  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
  {
    for (int i = 0; i < njobmax; i++)
      for (int j = 0; j < jobs[i].nproc; j++)
        if (jobs[i].proc[j].pid == pid)
        {
          if (WIFCONTINUED(status))
            jobs[i].proc[j].state = RUNNING;
          else if (WIFSTOPPED(status))
            jobs[i].proc[j].state = STOPPED;
          else //FINISHED
          {
            jobs[i].proc[j].state = FINISHED;
            jobs[i].proc[j].exitcode = status;
          }

          bool is_running = false;
          bool is_stopped = false;
          for (int k = 0; k < jobs[i].nproc; k++)
            if (jobs[i].proc[k].state == RUNNING)
            {
              jobs[i].state = RUNNING;
              is_running = true;
              break;
            }

          if (!is_running)
            for (int k = 0; k < jobs[i].nproc; k++)
              if (jobs[i].proc[k].state == STOPPED)
              {
                jobs[i].state = STOPPED;
                is_stopped = true;
                break;
              }
          if (!is_running && !is_stopped)
            jobs[i].state = FINISHED;
          //break outer for
          i = njobmax;
          break;
        }
  }

  errno = old_errno;
}

/* When pipeline is done, its exitcode is fetched from the last process. */
static int exitcode(job_t *job)
{
  return job->proc[job->nproc - 1].exitcode;
}

static int allocjob(void)
{
  /* Find empty slot for background job. */
  for (int j = BG; j < njobmax; j++)
    if (jobs[j].pgid == 0)
      return j;

  /* If none found, allocate new one. */
  jobs = realloc(jobs, sizeof(job_t) * (njobmax + 1));
  memset(&jobs[njobmax], 0, sizeof(job_t));
  return njobmax++;
}

static int allocproc(int j)
{
  job_t *job = &jobs[j];
  job->proc = realloc(job->proc, sizeof(proc_t) * (job->nproc + 1));
  return job->nproc++;
}

int addjob(pid_t pgid, int bg)
{
  int j = bg ? allocjob() : FG;
  job_t *job = &jobs[j];
  /* Initial state of a job. */
  job->pgid = pgid;
  job->state = RUNNING;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
  job->tmodes = shell_tmodes;
  return j;
}

static void deljob(job_t *job)
{
  assert(job->state == FINISHED);
  free(job->command);
  free(job->proc);
  job->pgid = 0;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
}

static void movejob(int from, int to)
{
  assert(jobs[to].pgid == 0);
  memcpy(&jobs[to], &jobs[from], sizeof(job_t));
  memset(&jobs[from], 0, sizeof(job_t));
}

static void mkcommand(char **cmdp, char **argv)
{
  if (*cmdp)
    strapp(cmdp, " | ");

  for (strapp(cmdp, *argv++); *argv; argv++)
  {
    strapp(cmdp, " ");
    strapp(cmdp, *argv);
  }
}

void addproc(int j, pid_t pid, char **argv)
{
  assert(j < njobmax);
  job_t *job = &jobs[j];

  int p = allocproc(j);
  proc_t *proc = &job->proc[p];
  /* Initial state of a process. */
  proc->pid = pid;
  proc->state = RUNNING;
  proc->exitcode = -1;
  mkcommand(&job->command, argv);
}

/* Returns job's state.
 * If it's finished, delete it and return exitcode through statusp. */
int jobstate(int j, int *statusp)
{
  assert(j < njobmax);
  job_t *job = &jobs[j];
  int state = job->state;

  /* TODO: Handle case where job has finished. */
  if (state == FINISHED)
  {
    *statusp = exitcode(job);
    deljob(job);
  }
  return state;
}

char *jobcmd(int j)
{
  assert(j < njobmax);
  job_t *job = &jobs[j];
  return job->command;
}

/* Continues a job that has been stopped. If move to foreground was requested,
 * then move the job to foreground and start monitoring it. */
bool resumejob(int j, int bg, sigset_t *mask)
{
  if (j < 0)
  {
    for (j = njobmax - 1; j > 0 && jobs[j].state == FINISHED; j--)
      continue;
  }

  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;

  /* TODO: Continue stopped job. Possibly move job to foreground slot. */
  //przyjme konwencje, ze jezeli job jest running, ale ma jakies stopped procesy, to wznawiam te procesy
  int sendmsg = 2; // 0 if all processes running, 1 if some but not all processes stopped, 2 if no processes running (job stopped)
  if (jobs[j].state == RUNNING)
    sendmsg = 0;
  if (sendmsg == 0)
    for (int i = 0; i < jobs[j].nproc; i++)
      if (jobs[j].proc[i].state == STOPPED)
        sendmsg = 1;
  assert(jobs[j].pgid > 1);
  Kill(-jobs[j].pgid, SIGCONT);
  if (jobs[j].state == STOPPED)
    Sigsuspend(mask);
  if (sendmsg == 2)
    msg("[%d] continue '%s'\n", j, jobs[j].command);
  else if (sendmsg == 1)
    msg("[%d] continue '%s' (some processes were already running)\n", j, jobs[j].command);
  if (bg == FG)
  {
    movejob(j, FG);
    Tcsetattr(tty_fd, TCSANOW, &jobs[FG].tmodes);
    monitorjob(mask);
  }
  return true;
}

/* Kill the job by sending it a SIGTERM. */
bool killjob(int j)
{
  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;
  debug("[%d] killing '%s'\n", j, jobs[j].command);

  /* TODO: I love the smell of napalm in the morning. */
  for (int i = 0; i < jobs[j].nproc; i++)
  {
    Kill(jobs[j].proc[i].pid, SIGTERM);
    if (jobs[j].proc[i].state == STOPPED)
      Kill(jobs[j].proc[i].pid, SIGCONT); //na wypadek gdyby proces byl zatrzymany
  }
  return true;
}

/* Report state of requested background jobs. Clean up finished jobs. */
void watchjobs(int which)
{
  for (int j = BG; j < njobmax; j++)
  {
    if (jobs[j].pgid == 0)
      continue;

    /* TODO: Report job number, state, command and exit code or signal. */
    if (jobs[j].state != which && which != ALL)
      continue;

    if (jobs[j].state == RUNNING)
      msg("[%d] running '%s'\n", j, jobs[j].command);
    else if (jobs[j].state == STOPPED)
      msg("[%d] suspended '%s'\n", j, jobs[j].command);
    else //FINISHED
    {
      int wstatus = exitcode(&jobs[j]);

      msg("[%d] ", j);
      if (WIFEXITED(wstatus))
      {
        msg("exited '%s', status=%d\n", jobs[j].command, WEXITSTATUS(wstatus));
      }
      else if (WIFSIGNALED(wstatus))
      {
        msg("killed '%s' by signal %d\n", jobs[j].command, WTERMSIG(wstatus));
      }
      else
        msg("'%s' unidentified termination\n", jobs[j].command);
      deljob(&jobs[j]);
    }
  }
}

/* Monitor job execution. If it gets stopped move it to background.
 * When a job has finished or has been stopped move shell to foreground. */
int monitorjob(sigset_t *mask)
{
  int exitcode, state;

  /* TODO: Following code requires use of Tcsetpgrp of tty_fd. */
  Tcsetpgrp(tty_fd, jobs[FG].pgid);
  /*Ponizej znajduje sie moje (troche brzydkie) rozwiazanie problemu dla przypadkow, w ktorych 
  program po wybudzeniu zdazy sie(albo ktos inny go) uspic, gdzie inaczej moglibysmy sie zapetlic, uruchamiajac np.:
  
  #include <signal.h>

  int main()
  {
    while(1)
      raise(SIGSTOP);

    return 0;
  }
  */
  Kill(-jobs[FG].pgid, SIGCONT);
  int tries = 0;
  while (jobs[FG].state == STOPPED && tries++ <= 128) //128 to arbitralna liczba
  {
    Kill(-jobs[FG].pgid, SIGCONT);
    Sigsuspend(mask);
  }
  while (true)
  {
    state = jobstate(FG, &exitcode);
    if (state == RUNNING)
      Sigsuspend(mask);
    else
      break;
  }

  if (state == STOPPED)
  {
    int new_bg_job = addjob(0, BG);
    movejob(FG, new_bg_job);
    Tcgetattr(tty_fd, &jobs[new_bg_job].tmodes);
    msg("[%d] suspended '%s'\n", new_bg_job, jobs[new_bg_job].command);
  }
  Tcsetpgrp(tty_fd, getpgrp());
  Tcsetattr(tty_fd, TCSANOW, &shell_tmodes);
  return exitcode;
}

/* Called just at the beginning of shell's life. */
void initjobs(void)
{
  Signal(SIGCHLD, sigchld_handler);
  jobs = calloc(sizeof(job_t), 1);

  /* Assume we're running in interactive mode, so move us to foreground.
   * Duplicate terminal fd, but do not leak it to subprocesses that execve. */
  assert(isatty(STDIN_FILENO));
  tty_fd = Dup(STDIN_FILENO);
  fcntl(tty_fd, F_SETFD, FD_CLOEXEC);

  /* Take control of the terminal. */
  Tcsetpgrp(tty_fd, getpgrp());

  /* Save default terminal attributes for the shell. */
  Tcgetattr(tty_fd, &shell_tmodes);
}

/* Called just before the shell finishes. */
void shutdownjobs(void)
{
  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Kill remaining jobs and wait for them to finish. */

  for (int i = 0; i < njobmax; i++)
    if (jobs[i].state != FINISHED)
    {
      Kill(jobs[i].pgid, SIGTERM);
      Kill(jobs[i].pgid, SIGCONT);
    }
  while (true)
  {
    bool still_running = false; //nadal jest jakies niekonczone zadanie
    for (int i = 0; i < njobmax; i++)
      if (jobs[i].state != FINISHED)
      {
        still_running = true;
        break;
      }
    if (still_running == true)
      Sigsuspend(&mask);
    else
      break;
  }
  watchjobs(FINISHED);

  Sigprocmask(SIG_SETMASK, &mask, NULL);

  Close(tty_fd);
}
