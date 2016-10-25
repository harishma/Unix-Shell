/* 
 * tsh - A tiny shell program with job control
 * 
 * <Harishma Dayanidhi hdayanid>
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF         0   /* undefined */
#define FG            1   /* running in foreground */
#define BG            2   /* running in background */
#define ST            3   /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL   0x0   /* next token is an argument */
#define ST_INFILE   0x1   /* next token is the input file */
#define ST_OUTFILE  0x2   /* next token is the output file */


/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */
volatile sig_atomic_t fpid; /* for checking if foreground job returned */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

struct cmdline_tokens {
    int argc;               /* Number of arguments */
    char *argv[MAXARGS];    /* The arguments list */
    char *infile;           /* The input file */
    char *outfile;          /* The output file */
    enum builtins_t {       /* Indicates if argv[0] is a builtin command */
        BUILTIN_NONE,
        BUILTIN_QUIT,
        BUILTIN_JOBS,
        BUILTIN_BG,
        BUILTIN_FG} builtins;
};

/* End global variables */


/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list); 
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid); 
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *job_list, int output_fd);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/* Miscellaneous helper functions */
int builtin_command(struct cmdline_tokens*); 

/* From csapp.h */
/* Process control wrappers */
pid_t Fork(void);
void Execve(const char *filename, char *const argv[], char *const envp[]);
pid_t Wait(int *status);
pid_t Waitpid(pid_t pid, int *iptr, int options);
void Kill(pid_t pid, int signum);
unsigned int Sleep(unsigned int secs);
void Pause(void);
unsigned int Alarm(unsigned int seconds);
void Setpgid(pid_t pid, pid_t pgid);
pid_t Getpgrp();

/* Signal wrappers */
void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
void Sigemptyset(sigset_t *set);
void Sigfillset(sigset_t *set);
void Sigaddset(sigset_t *set, int signum);
void Sigdelset(sigset_t *set, int signum);
int Sigismember(const sigset_t *set, int signum);
int Sigsuspend(const sigset_t *set);

/* Sio (Signal-safe I/O) routines */
ssize_t sio_puts(char s[]);
ssize_t sio_putl(long v);
void sio_error(char s[]);

/* Sio wrappers */
ssize_t Sio_puts(char s[]);
ssize_t Sio_putl(long v);
void Sio_error(char s[]);

/*
 * main - The shell's main routine 
 */
int 
main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];    /* cmdline for fgets */
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
            break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(job_list);
    
    /* Initialize fpid */
    fpid = 0;


    /* Execute the shell's read/eval loop */
    while (1) {

        if (emit_prompt) {
            Sio_puts(prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { 
            /* End of file (ctrl-d) */
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }
        
        /* Remove the trailing newline */
        cmdline[strlen(cmdline)-1] = '\0';
        
        /* Evaluate the command line */
        eval(cmdline);
        
        fflush(stdout);
        fflush(stdout);
    } 
    
    exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void 
eval(char *cmdline) 
{
    int bg;              /* should the job run in bg or fg? */
    struct cmdline_tokens tok;
    pid_t pid;
    sigset_t mask_all,prev_all; 

    /* Parse command line */
    bg = parseline(cmdline, &tok); 

    if (bg == -1) /* parsing error */
        return;
    if (tok.argv[0] == NULL) /* ignore empty lines */
        return;
    
    Sigfillset(&mask_all);
    
    if (!builtin_command(&tok)) { 
    	Sigprocmask(SIG_BLOCK,&mask_all,&prev_all); /* Block ALL_SIGNALS */
    	if ((pid = Fork()) == 0) {   /* Child runs user job */
        	Sigprocmask(SIG_UNBLOCK,&mask_all,NULL); /* Unblock ALL_SIGNALS 
			in child */
			Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    		Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    		Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    		Signal(SIGTTIN, SIG_IGN);
    		Signal(SIGTTOU, SIG_IGN);

    		/* This one provides a clean way to kill the shell */
    		Signal(SIGQUIT, sigquit_handler);
			
			/* Set process group of a process to its process ID */
			Setpgid(0, 0) ;
			
			if(tok.outfile)
			{
				int fd = open(tok.outfile, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
				if(verbose)
					Sio_puts("Redirecting stdout to user given file\n");
				if(dup2(fd,1)==-1) /* Redirects stdout to outfile */
					Sio_puts("I/O redirection failed\n");
				close(fd);
			}
			if(tok.infile)
			{
				int fd = open(tok.infile, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
				if(verbose)
					Sio_puts("Redirecting stdin from user given file\n");
				if(dup2(fd,0)==-1) /* Redirects stdin to infile */
					Sio_puts("I/O redirection failed\n");
				close(fd);
			}
			Execve(tok.argv[0], tok.argv, environ);
        }

		/* Parent waits for foreground job to terminate */
		if (!bg) { /* Foreground job */
			Sigfillset(&mask_all);
			Sigprocmask(SIG_BLOCK, &mask_all, &prev_all); /* Block 
			All Signals */
			if(verbose)
				Sio_puts("Entering blocked addjob/wait region of Parent\n");
			fpid = 0;
			addjob(job_list,pid,FG,cmdline);
			sigset_t block_none;
			Sigemptyset(&block_none);
			while(!fpid)
	    		Sigsuspend(&block_none);
	    	if(verbose)
	    		Sio_puts("Exiting blocked addjob/wait region of Parent\n");
			Sigprocmask(SIG_SETMASK, &prev_all, NULL); /* Unblock All Signals */
		}
		else /* Background job */
		{
			Sigfillset(&mask_all);
			Sigprocmask(SIG_BLOCK, &mask_all, &prev_all); /* Block All Signals 
			*/
			addjob(job_list,pid,BG,cmdline);
        	sprintf(sbuf,"[%d] (%d) %s\n", pid2jid(pid), pid, cmdline);
        	Sio_puts(sbuf);
        	Sigprocmask(SIG_SETMASK, &prev_all, NULL); /* Unblock All Signals 
			*/
	    }
	}
	return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *             structure will be populated with the parsed tokens. Characters 
 *             enclosed in single or double quotes are treated as a single
 *             argument. 
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job  
 *  -1:        if cmdline is incorrectly formatted
 * 
 * Note:       The string elements of tok (e.g., argv[], infile, outfile) 
 *             are statically allocated inside parseline() and will be 
 *             overwritten the next time this function is invoked.
 */
int 
parseline(const char *cmdline, struct cmdline_tokens *tok) 
{

    static char array[MAXLINE];          /* holds local copy of command line */
    const char delims[10] = " \t\r\n";   /* argument delimiters (white-space) */
    char *buf = array;                   /* ptr that traverses command line */
    char *next;                          /* ptr to the end of the current arg */
    char *endbuf;                        /* ptr to end of cmdline string */
    int is_bg;                           /* background job? */

    int parsing_state;                   /* indicates if the next token is the
                                            input or output file */

    if (cmdline == NULL) {
        (void) fprintf(stderr, "Error: command line is NULL\n");
        return -1;
    }

    (void) strncpy(buf, cmdline, MAXLINE);
    endbuf = buf + strlen(buf);

    tok->infile = NULL;
    tok->outfile = NULL;

    /* Build the argv list */
    parsing_state = ST_NORMAL;
    tok->argc = 0;

    while (buf < endbuf) {
        /* Skip the white-spaces */
        buf += strspn (buf, delims);
        if (buf >= endbuf) break;

        /* Check for I/O redirection specifiers */
        if (*buf == '<') {
            if (tok->infile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_INFILE;
            buf++;
            continue;
        }
        if (*buf == '>') {
            if (tok->outfile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_OUTFILE;
            buf ++;
            continue;
        }

        if (*buf == '\'' || *buf == '\"') {
            /* Detect quoted tokens */
            buf++;
            next = strchr (buf, *(buf-1));
        } else {
            /* Find next delimiter */
            next = buf + strcspn (buf, delims);
        }
        
        if (next == NULL) {
            /* Returned by strchr(); this means that the closing
               quote was not found. */
            (void) fprintf (stderr, "Error: unmatched %c.\n", *(buf-1));
            return -1;
        }

        /* Terminate the token */
        *next = '\0';

        /* Record the token as either the next argument or the i/o file */
        switch (parsing_state) {
        case ST_NORMAL:
            tok->argv[tok->argc++] = buf;
            break;
        case ST_INFILE:
            tok->infile = buf;
            break;
        case ST_OUTFILE:
            tok->outfile = buf;
            break;
        default:
            (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
            return -1;
        }
        parsing_state = ST_NORMAL;

        /* Check if argv is full */
        if (tok->argc >= MAXARGS-1) break;

        buf = next + 1;
    }

    if (parsing_state != ST_NORMAL) {
        (void) fprintf(stderr,
                       "Error: must provide file name for redirection\n");
        return -1;
    }

    /* The argument list must end with a NULL pointer */
    tok->argv[tok->argc] = NULL;

    if (tok->argc == 0)  /* ignore blank line */
        return 1;

    if (!strcmp(tok->argv[0], "quit")) {                 /* quit command */
        tok->builtins = BUILTIN_QUIT;
    } else if (!strcmp(tok->argv[0], "jobs")) {          /* jobs command */
        tok->builtins = BUILTIN_JOBS;
    } else if (!strcmp(tok->argv[0], "bg")) {            /* bg command */
        tok->builtins = BUILTIN_BG;
    } else if (!strcmp(tok->argv[0], "fg")) {            /* fg command */
        tok->builtins = BUILTIN_FG;
    } else {
        tok->builtins = BUILTIN_NONE;
    }

    /* Should the job run in the background? */
    if ((is_bg = (*tok->argv[tok->argc-1] == '&')) != 0)
        tok->argv[--tok->argc] = NULL;

    return is_bg;
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The 
 *     handler reaps all available zombie children, but doesn't wait 
 *     for any other currently running children to terminate.  
 */
void 
sigchld_handler(int sig) 
{
	if(verbose)
		Sio_puts("Entering Sigchld\n");
	int olderrno = errno;
    sigset_t mask_all, prev_all;
    pid_t pid,cfpid;
    
	Sigfillset(&mask_all);
	/* Reap a zombie child */
	int status;
	/* Only reap already dead or stopped children without waiting for any */ 
    while ((pid = waitpid(-1,&status, WNOHANG|WUNTRACED)) > 0) { 
		Sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
		cfpid = fgpid(job_list);
		if(WIFEXITED(status)) /* child terminated normally */
		{
			deletejob(job_list,pid); /* Delete the child from the job list */
	    	if(verbose)
        	{
        		sprintf(sbuf,"Deleted job [%d] (%d)\n",pid2jid(pid),pid);
				Sio_puts(sbuf);
        	}
        	if(pid == cfpid)
        	{
        		if(verbose)
        		{
        			sprintf(sbuf,"%d no more the foreground process\n",pid);
					Sio_puts(sbuf);
        		}
				fpid = pid;
    		}
    	}
    	else if(WIFSIGNALED(status)) /* expected when child received SIGINT */
    	{
    		if(pid == cfpid)
        	{
        		if(verbose)
        		{
        			sprintf(sbuf,"%d no more the foreground process\n",pid);
					Sio_puts(sbuf);
        		}
				fpid = pid;
    		}
    		pid_t jid = pid2jid(pid);
			sprintf(sbuf,"Job [%d] (%d) terminated by signal 2\n",jid,pid);
			Sio_puts(sbuf);
			deletejob(job_list,pid); /* Delete the child from the job list */
			if(verbose)
    		{
        		sprintf(sbuf,"Deleted SGNLD job [%d] (%d)\n",jid,pid);
				Sio_puts(sbuf);
    		}
    	}
    	else if(WIFSTOPPED(status)) /* expected when child received SIGTSTP */
    	{
    		if(pid == cfpid)
        	{
        		if(verbose)
        		{
        			sprintf(sbuf,"%d no more the foreground process\n",pid);
					Sio_puts(sbuf);
        		}
				fpid = pid;
    		}
    		pid_t jid = pid2jid(pid);
    		struct job_t * job = getjobjid(job_list,jid);
    		job->state = ST;
			sprintf(sbuf,"Job [%d] (%d) stopped by signal 20\n",jid,cfpid);
			Sio_puts(sbuf);
    	}
       Sigprocmask(SIG_SETMASK, &prev_all, NULL);
    }
    errno = olderrno;
	if(verbose)
		Sio_puts("Exiting Sigchld\n");
    return;

}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void 
sigint_handler(int sig) 
{
	/* My modIified code */
	if(verbose)
		Sio_puts("Entering Sigint\n");
	int olderrno = errno;
    sigset_t mask_all, prev_all;
    pid_t cfpid;
    
	Sigfillset(&mask_all);
    Sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
	cfpid = fgpid(job_list);
	if(cfpid) /* Make sure there is a process present to send SIGINT to */
	{
		if(verbose)
		{
			sprintf(sbuf,"Sent SIGINT to %d\n",cfpid);
			Sio_puts(sbuf);
		}
		kill(-cfpid,SIGINT);
	}
	else
	{
		if(verbose)
			Sio_puts("Did not find FG process to send SIGINT\n");
	}
	Sigprocmask(SIG_SETMASK, &prev_all, NULL);
    errno = olderrno;
	if(verbose)
		Sio_puts("Exiting Sigint\n");
    return;	
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void 
sigtstp_handler(int sig) 
{
	if(verbose)
		Sio_puts("Entering Sigtstp\n");
	int olderrno = errno;
    sigset_t mask_all, prev_all;
    pid_t cfpid;
    
	Sigfillset(&mask_all);
    Sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
	cfpid = fgpid(job_list);
	if(cfpid) /* Make sure there is a process present to send SIGINT to */
	{
		kill(-cfpid,SIGTSTP); 
	}
	Sigprocmask(SIG_SETMASK, &prev_all, NULL);
    errno = olderrno;
	if(verbose)
		Sio_puts("Exiting Sigtstp\n");
    return;
}

/*********************
 * End signal handlers
 *********************/
 
 /************************************
 * Wrappers for Unix signal functions 
 ***********************************/

void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    if (sigprocmask(how, set, oldset) < 0)
	unix_error("Sigprocmask error");
    return;
}

void Sigemptyset(sigset_t *set)
{
    if (sigemptyset(set) < 0)
	unix_error("Sigemptyset error");
    return;
}

void Sigfillset(sigset_t *set)
{ 
    if (sigfillset(set) < 0)
	unix_error("Sigfillset error");
    return;
}

void Sigaddset(sigset_t *set, int signum)
{
    if (sigaddset(set, signum) < 0)
	unix_error("Sigaddset error");
    return;
}

void Sigdelset(sigset_t *set, int signum)
{
    if (sigdelset(set, signum) < 0)
	unix_error("Sigdelset error");
    return;
}

int Sigismember(const sigset_t *set, int signum)
{
    int rc;
    if ((rc = sigismember(set, signum)) < 0)
	unix_error("Sigismember error");
    return rc;
}

int Sigsuspend(const sigset_t *set)
{
    int rc = sigsuspend(set); /* always returns -1 */
    if (errno != EINTR)
        unix_error("Sigsuspend error");
    return rc;
}

/*********************
 * End signal handlers
 *********************/

/*********************************************
 * Wrappers for Unix process control functions
 ********************************************/

/* $begin forkwrapper */
pid_t Fork(void) 
{
    pid_t pid;

    if ((pid = fork()) < 0)
	unix_error("Fork error");
    return pid;
}
/* $end forkwrapper */

void Execve(const char *filename, char *const argv[], char *const envp[]) 
{
    if (execve(filename, argv, envp) < 0)
    {
    	printf("%s: Command not found\n", filename);
    	fflush(stdout);
    	exit(1);
    }
}

/* $begin wait */
pid_t Wait(int *status) 
{
    pid_t pid;

    if ((pid  = wait(status)) < 0)
	unix_error("Wait error");
    return pid;
}
/* $end wait */

pid_t Waitpid(pid_t pid, int *iptr, int options) 
{
    pid_t retpid;

    if ((retpid  = waitpid(pid, iptr, options)) < 0) 
		unix_error("Waitpid error");
    return(retpid);
}

/* $begin kill */
void Kill(pid_t pid, int signum) 
{
    int rc;

    if ((rc = kill(pid, signum)) < 0)
	unix_error("Kill error");
}
/* $end kill */

void Pause() 
{
    (void)pause();
    return;
}

unsigned int Sleep(unsigned int secs) 
{
    return sleep(secs);
}

unsigned int Alarm(unsigned int seconds) {
    return alarm(seconds);
}
 
void Setpgid(pid_t pid, pid_t pgid) {
    int rc;

    if ((rc = setpgid(pid, pgid)) < 0)
	unix_error("Setpgid error");
    return;
}

pid_t Getpgrp(void) {
    return getpgrp();
}

/*************************************************************
 * The Sio (Signal-safe I/O) package - simple reentrant output
 * functions that are safe for signal handlers.
 *************************************************************/

/* Private sio functions */
/* $begin sioprivate */

/* sio_reverse - Reverse a string (from K&R) */
void sio_reverse(char s[])
{
    int c, i, j;

    for (i = 0, j = strlen(s)-1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}

/* sio_ltoa - Convert long to base b string (from K&R) */
void sio_ltoa(long v, char s[], int b) 
{
    int c, i = 0;
    
    do {  
        s[i++] = ((c = (v % b)) < 10)  ?  c + '0' : c - 10 + 'a';
    } while ((v /= b) > 0);
    s[i] = '\0';
    sio_reverse(s);
}

/* sio_strlen - Return length of string (from K&R) */
size_t sio_strlen(char s[])
{
    int i = 0;

    while (s[i] != '\0')
        ++i;
    return i;
}
/* $end sioprivate */

/* Public Sio functions */
/* $begin siopublic */

ssize_t sio_puts(char s[]) /* Put string */
{
    return write(STDOUT_FILENO, s, sio_strlen(s)); //line:csapp:siostrlen
}

ssize_t sio_putl(long v) /* Put long */
{
    char s[128];
    
    sio_ltoa(v, s, 10); /* Based on K&R itoa() */  //line:csapp:sioltoa
    return sio_puts(s);
}

void sio_error(char s[]) /* Put error message and exit */
{
    sio_puts(s);
    _exit(1);                                      //line:csapp:sioexit
}
/* $end siopublic */

/*******************************
 * Wrappers for the SIO routines
 ******************************/
ssize_t Sio_putl(long v)
{
    ssize_t n;
  
    if ((n = sio_putl(v)) < 0)
	sio_error("Sio_putl error");
    return n;
}

ssize_t Sio_puts(char s[])
{
    ssize_t n;
  
    if ((n = sio_puts(s)) < 0)
	sio_error("Sio_puts error");
    return n;
}

void Sio_error(char s[])
{
    sio_error(s);
}

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void 
clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void 
initjobs(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int 
maxjid(struct job_t *job_list) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int 
addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline) 
{
	int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == 0) {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(job_list[i].cmdline, cmdline);
            if(verbose){
            	sprintf(sbuf,"Added job [%d] %d %s\n",
                       job_list[i].jid,
                       job_list[i].pid,
                       job_list[i].cmdline);
                Sio_puts(sbuf);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int 
deletejob(struct job_t *job_list, pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == pid) {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t 
fgpid(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
            return job_list[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t 
*getjobpid(struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int 
pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid) {
            return job_list[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void 
listjobs(struct job_t *job_list, int output_fd) 
{
    int i;
    char buf[MAXLINE];

    for (i = 0; i < MAXJOBS; i++) {
        memset(buf, '\0', MAXLINE);
        if (job_list[i].pid != 0) {
            sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            switch (job_list[i].state) {
            case BG:
                sprintf(buf, "Running    ");
                break;
            case FG:
                sprintf(buf, "Foreground ");
                break;
            case ST:
                sprintf(buf, "Stopped    ");
                break;
            default:
                sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
                        i, job_list[i].state);
            }
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            sprintf(buf, "%s\n", job_list[i].cmdline);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/
 
 
 /* If first arg is a builtin command, run it and return true */
 int builtin_command(struct cmdline_tokens* tok) 
{
	if (tok->builtins == BUILTIN_NONE) /* Not a builtin command */
		return 0;
    if (tok->builtins == BUILTIN_QUIT) 
		exit(0);  
	if (tok->builtins == BUILTIN_JOBS) 
	{
		/* Before listing jobs check if a redirection is requested  */
		sigset_t mask_all,prev_all;
		Sigfillset(&mask_all);
		Sigprocmask(SIG_BLOCK, &mask_all, &prev_all); /* Block All Signals */
		int oldstdoutfd = dup(1);
		if(tok->outfile)
		{
			int fd = open(tok->outfile, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
			if(dup2(fd,1)==-1) /* Redirects stdout to outfile */
				Sio_puts("I/O redirection failed\n");
			close(fd);
		}
		listjobs(job_list,STDOUT_FILENO);
		dup2(oldstdoutfd,1); /* Set default output to stdout */
		Sigprocmask(SIG_SETMASK, &prev_all, NULL); /* Unblock All Signals */
		return 1;
	}
	if (tok->builtins == BUILTIN_BG)
	{
		sigset_t mask_all,prev_all;
		Sigfillset(&mask_all);
		Sigprocmask(SIG_BLOCK, &mask_all, &prev_all); /* Block All Signals */
		if(tok->argc>2) /* Incorrectly formatted command */
			return 0; 
		int arg = (int)(tok->argv[1][1]-'0'); /* Command of the format BG %i */
		int isJid = 1;
		if(arg>maxjid(job_list))
			isJid = 0;
		struct job_t* job = NULL;
		if(isJid)
		{
			job = getjobjid(job_list,arg);
			if(!job) /* Ignore request to restart non-existent job */
				return 1; 
			pid_t pid = job->pid;
			kill(-pid,SIGCONT);
			job->state = BG;
		}
		else
		{
			kill(-arg,SIGCONT);
			job = getjobpid(job_list,arg);
			if(!job) /* Ignore request to restart non-existent job */
				return 1; 
			job->state = BG;
		}
		sprintf(sbuf,"[%d] (%d) %s\n",job->jid,job->pid,job->cmdline);
		Sio_puts(sbuf);
		Sigprocmask(SIG_SETMASK, &prev_all, NULL); /* Unblock All Signals */

		return 1;
	}
	if (tok->builtins == BUILTIN_FG)
	{
		sigset_t mask_all,prev_all;
		Sigfillset(&mask_all);
		Sigprocmask(SIG_BLOCK, &mask_all, &prev_all); /* Block All Signals */
		if(tok->argc>2) /* Incorrectly formatted command */
			return 0; 
		int arg = (tok->argv[1][1]-'0'); /* Command of the format FG %i */
		int isJid = 1;
		if(arg>maxjid(job_list))
			isJid = 0;
		if(isJid)
		{
			struct job_t* job = getjobjid(job_list,arg);
			if(!job) /* Ignore request to restart a non-existent job */
				return 1; 
			pid_t pid = job->pid;
			kill(-pid,SIGCONT);
			job->state = FG;
		}
		else
		{
			kill(-arg,SIGCONT);
			struct job_t * job = getjobpid(job_list,arg);
			if(!job) /* Ignore request to restart a non-existent job */
				return 1; 
				job->state = FG;
		}
		fpid = 0;
		sigset_t block_none;
		Sigemptyset(&block_none);
		while(!fpid)
	    	Sigsuspend(&block_none);
	    Sigprocmask(SIG_SETMASK, &prev_all, NULL); /* Unblock All Signals */

		return 1;
	}
	if (!strcmp(tok->argv[0], "&"))    /* Ignore singleton & */
		return 1;
	return 0;
}

/*
 * usage - print a help message
 */
void 
usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void 
unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void 
app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t 
*Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void 
sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
