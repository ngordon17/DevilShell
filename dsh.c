#include "dsh.h"

void seize_tty(pid_t callingprocess_pgid); /* Grab control of the terminal for the calling process pgid.  */
void continue_job(job_t *j); /* resume a stopped job */
void spawn_job(job_t *j, bool fg); /* spawn a new job */
bool add_job(job_t *j); /*add job to end of job_list */
void del_job(job_t *j);
bool frees_job(job_t *j);
int mark_process_status(pid_t pid, int status);

enum {max_num_jobs = 20};
char prompt_pid[32];
job_t *job_list = NULL;
pid_t dsh_pgid;
#define MAX_ERR_MSG_LEN 256

/* frees_job iterates and invokes free on all its members - used to free built-in jobs which never get added to job_list 
 this is copied from helper.c */
bool frees_job(job_t *j) {
	if(!j)
		return true;
	free(j->commandinfo);
	process_t *p;
	for(p = j->first_process; p; p = p->next) {
		int i;
		for(i = 0; i < p->argc; i++)
			free(p->argv[i]);
		free(p->argv);
        free(p->ifile);
        free(p->ofile);
	}
	free(j);
	return true;
}

/* checks for process status changes and updates process -> stopped/completed*/
int mark_process_status (pid_t pid, int status) {
    job_t *j;
    process_t *p;
    if (pid > 0) {
        /* Update the record for the process.  */
        for (j = job_list; j; j = j->next) {
            for (p = j->first_process; p; p = p->next) {
                if (p->pid == pid) {
                    p->status = status;
                    if (WIFSTOPPED (status)) {
                        p->stopped = true;
                    }
                    else if (WIFCONTINUED(status)) {
                        p -> stopped= false;
                        p -> completed = false;
                    }
                    else {
                        p->completed = true;
                        if (WIFSIGNALED (status)) {
                            char msg[MAX_ERR_MSG_LEN];
                            sprintf(msg, "%d: Stopped by signal %d.\n", (int) pid, WTERMSIG (p->status));
                            write(STDERR_FILENO, msg, strlen(msg));
                            write(STDOUT_FILENO, msg, strlen(msg));
                        }
                    }
                    return false;
                }
            }
        }
        char msg[MAX_ERR_MSG_LEN];
        sprintf(msg, "ERROR: No child process %d.\n", pid);
        perror(msg);
        write(STDOUT_FILENO, msg, strlen(msg));
        return -1;
    }
    else if (pid == 0 || errno == ECHILD) {
        /* no processes ready to report */
        return -1;
    }
    else {
        /* other weird errors */
        char msg[MAX_ERR_MSG_LEN];
        sprintf(msg, "ERROR: Unknown error - waitpid");
        perror(msg);
        write(STDOUT_FILENO, msg, strlen(msg));
        return -1;
    }
}

/* adds a job to the end of job_list - always returns true */
bool add_job(job_t *j) {
    job_t *prev = job_list;
    job_t *current = job_list;
    if (job_list == NULL) {
        job_list = j;
        return true;
    }
    while (current != NULL) {
        prev = current;
        current = current -> next;
    }
    prev -> next = j;
    return true;;
}
/* Sets the process group id for a given job and process */
int set_child_pgid(job_t *j, process_t *p)
{
    if (j->pgid < 0) /* first child: use its pid for job pgid */
        j->pgid = p->pid;
    return(setpgid(p->pid,j->pgid));
}

/* Creates the context for a new child by setting the pid, pgid and tcsetpgrp */
void new_child(job_t *j, process_t *p, bool fg)
{
         /* establish a new process group, and put the child in
          * foreground if requested
          */

         /* Put the process into the process group and give the process
          * group the terminal, if appropriate.  This has to be done both by
          * the dsh and in the individual child processes because of
          * potential race conditions.  
          * */

         p->pid = getpid();

         /* also establish child process group in child to avoid race (if parent has not done it yet). */
         set_child_pgid(j, p);

         if(fg) {// if fg is set
             if (isatty(STDIN_FILENO)) {
             seize_tty(j->pgid); // assign the terminal
             }
         }
         /* Set the handling for job control signals back to the default. */
         signal(SIGTTOU, SIG_DFL);
}

/* Spawning a process with job control. fg is true if the 
 * newly-created process is to be placed in the foreground. 
 * (This implicitly puts the calling process in the background, 
 * so watch out for tty I/O after doing this.) pgid is -1 to 
 * create a new job, in which case the returned pid is also the 
 * pgid of the new job.  Else pgid specifies an existing job's 
 * pgid: this feature is used to start the second or 
 * subsequent processes in a pipeline.
 * */

void spawn_job(job_t *j, bool fg) {
    
	pid_t pid;
	process_t *p;
    int status;
    int mypipe[2];
    int end;


	for(p = j->first_process; p; p = p->next) {
        
        /* YOUR CODE HERE? */
        if (pipe(mypipe) < 0) {
            char msg[MAX_ERR_MSG_LEN];
            sprintf(msg, "ERROR: Failed to create pipe \n");
            perror(msg);
            write(STDOUT_FILENO, msg, strlen(msg));
            exit(EXIT_FAILURE);
        }
 
        /* Builtin commands are already taken care earlier */
        
        switch (pid = fork()) {
                
            case -1: /* fork failure */
                perror("ERROR: Failed to fork process\n");
                write(STDOUT_FILENO, "Failed to fork process\n", strlen("Failed to fork process\n"));
                exit(EXIT_FAILURE);
                
            case 0: /* child process  */
                p->pid = getpid();
                new_child(j, p, fg);

                /* YOUR CODE HERE?  Child-side code for new process. */
                
                /*IO redirection*/
                int fd;
                if (j -> mystdin == INPUT_FD) {
                    fd = open(p -> ifile, O_WRONLY, 0600);
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }
                if (j -> mystdout == OUTPUT_FD) {
                    fd = open(p -> ofile, O_WRONLY | O_TRUNC | O_CREAT, 0600);
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }
                
                
                //NO PIPE
                if (p == j -> first_process && p -> next == NULL) {
                    close(mypipe[0]);
                    close(mypipe[1]);
                }
                /*Pipeline*/
                //READ - check if first process in pipe
                else if (p == j -> first_process) {
                    close(mypipe[0]);
                    dup2(mypipe[1], 1);
                }
            
                //READ AND WRITE - check if process is in middle of pipe
                else if (p -> next != NULL) {
                    dup2(end, 0);
                    close(mypipe[0]);
                    dup2(mypipe[1], 1);
                }
                //WRITE - check if last process in pipe
                else {
                    dup2(end, 0);
                    close(mypipe[0]);
                }
                
                //process ready for exec, print launch to log file (stderr = dsh.log) and to stdout. 
                char msg[MAX_ERR_MSG_LEN];
                sprintf(msg, "%d (Launched): %s\n", j-> pgid, j -> commandinfo);
                write(STDERR_FILENO, msg, strlen(msg));
                write(STDOUT_FILENO, msg, strlen(msg));
                
                //process ready for exec
                execvp(p -> argv[0], p -> argv);
            
                //reached only if exec fails
                char msg2[MAX_ERR_MSG_LEN];
                sprintf(msg2, "New child should have done an exec /n");
                perror("ERROR: New child should have done an exec\n");
                write(STDOUT_FILENO, msg2, strlen(msg));
                exit(EXIT_FAILURE);  
                break;   
                
            default: /* parent */
                /* establish child process group */
                p->pid = pid;
                set_child_pgid(j, p);
                //parent side pipe configurations
                end = mypipe[0];
                close(mypipe[1]);
        }
        
        //check if process in fg and wait until process finishes or is stopped/terminated by signal
        if (fg) {
            if (!p -> next) {
                waitpid(pid, &status, WUNTRACED);
                p -> status = status;
                if (WIFSTOPPED(status)) {
                    p -> stopped = true;
                    char msg[MAX_ERR_MSG_LEN];
                    sprintf(msg, "%d: Stopped by signal %d.\n", (int) pid, WTERMSIG (p->status));
                    write(STDERR_FILENO, msg, strlen(msg));
                    write(STDOUT_FILENO, msg, strlen(msg));
                }
                else if (WIFEXITED(status) || WIFEXITED(status) == 0) {
                    p -> completed = true;
                    if (WIFSIGNALED(status)) {
                        char msg[MAX_ERR_MSG_LEN];
                        sprintf(msg,  "%d: Terminated by signal %d. \n", (int) pid, WTERMSIG(p -> status));
                        write(STDERR_FILENO, msg, strlen(msg));
                        write(STDOUT_FILENO, msg, strlen(msg));
                    }   
                }
                else {
                    p -> completed = false;
                    p -> stopped = false;
                }
            }
        }
        /* YOUR CODE HERE?  Parent-side code for new job.*/
        
        //process finished, seize tty
        if (isatty(STDIN_FILENO)) {
            seize_tty(getpid()); // assign the terminal back to dsh
        }
        
	}
}

/* Sends SIGCONT signal to wake up the blocked job */
void continue_job(job_t *j) {
    if(kill(-j->pgid, SIGCONT) < 0) {
        char msg[MAX_ERR_MSG_LEN];
        sprintf(msg,  "ERROR: kill(SIGCONT) \n");
        perror(msg);
        write(STDOUT_FILENO, msg, strlen(msg));
    }
}

/* modified delete_job function */
void del_job(job_t *j) {
	if(!j || !job_list)
		return;
    
	job_t *first_job_itr = job_list;
    
	if(first_job_itr == j) {	/* header */
        if ( j -> next != NULL) {
            job_list= j -> next;
        }
        else {
            job_list= NULL;
        }
		frees_job(j);
        /* reinitialize for later purposes */
		return;
	}
    
	/* not header */
	while(first_job_itr->next != NULL) {
		if(first_job_itr->next == j) {
			first_job_itr->next = first_job_itr->next->next;
			frees_job(j);
			return;
		}
		first_job_itr = first_job_itr->next;
	}
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 * it immediately.  
 */
bool builtin_cmd(job_t *last_job, int argc, char **argv) {

	    /* check whether the cmd is a built in command
        */

        if (!strcmp(argv[0], "quit")) {
            /* Your code here */
           while (job_list != NULL) {
                delete_job(job_list, job_list);
                job_list = job_list -> next;
            }
            exit(EXIT_SUCCESS);
            return true;
        }
        else if (!strcmp("jobs", argv[0])) {
            /* Your code here */
            job_t *head = job_list;
            job_t *jobs_to_delete[max_num_jobs] = {NULL};
    
            pid_t pid;
            int status;
            
            /* update statuses of all processes */
            do {
                pid = waitpid(WAIT_ANY, &status, WCONTINUED | WNOHANG | WUNTRACED);
            } while (!mark_process_status(pid, status));
        
            /* print status update messages to log (dsh.log = stderr) and to stdout */
            int index = 0;
            while (head != NULL) {
                char stat[MAX_ERR_MSG_LEN];
                if (job_is_completed(head)) {
                    sprintf(stat, "%d (Completed): %s\n", head -> pgid, head -> commandinfo);
                    jobs_to_delete[index] = head;
                }
                else if (job_is_stopped(head)) {
                    sprintf(stat, "%d (Stopped): %s\n", head -> pgid, head -> commandinfo);
                }
                else {
                    sprintf(stat, "%d (Running): %s\n", head -> pgid, head -> commandinfo);
                }
                write(STDERR_FILENO, stat, strlen(stat));
                write(STDOUT_FILENO, stat, strlen(stat));
                index++;
                head = head -> next;
            }
            
            /* delete completed/terminates jobs */
            int i;
            for (i = 0; i < max_num_jobs; i++) {
                if (jobs_to_delete[i] != NULL) {
                    del_job(jobs_to_delete[i]);
                    jobs_to_delete[i] = NULL;
                }
            }
            //free built in command - jobs
            frees_job(last_job);
            return true;
        }
        else if (!strcmp("cd", argv[0])) {
            /* Your code here */
            char *directory = argv[1];
            if (chdir(directory) < 0) {
                perror("ERROR: Chdir error");
            }
            //free built in command - cd
            frees_job(last_job);
            return true;
        }
        else if (!strcmp("bg", argv[0])) {
            /* Your code here */
            if (argv[1] == NULL) {
                char msg[MAX_ERR_MSG_LEN];
                sprintf(msg, "Forgot pgid for bg (job)\n");
                perror("ERROR: Forgot pgid for bg (job) \n");
                write(STDOUT_FILENO, msg, strlen(msg));
                return true;
            }
            //find process that we want to put in background, i.e. process with pid = argv[1]
            job_t *current = job_list;
            while (current != NULL) {
                if (current -> pgid == atoi(argv[1])) {
                    current -> bg = true;
                    continue_job(current);
                    break;
                }
                current = current -> next;
            }
            //free built in command - bg
            frees_job(last_job);
            return true;
        }
        else if (!strcmp("fg", argv[0])) {
            /* Your code here */
          
            int intpgid;
            if (argv[1] != NULL) {
               
                intpgid = atoi(argv[1]);
            }
            else {
                char msg[MAX_ERR_MSG_LEN];
                sprintf(msg, "Forgot pgid for bg (job)\n");
                perror("ERROR: Forgot pgid for bg (job) \n");
                write(STDOUT_FILENO, msg, strlen(msg));
                return true;
            }
            
            job_t *head = job_list;
            //find job given by argument argv[1] which we want to move to fg
            while (head != NULL) {
                if (head -> pgid == intpgid) {
                    int status;
                    pid_t pid;
                    if (job_is_stopped(head)) {
                        char msg[MAX_ERR_MSG_LEN];
                        sprintf(msg,  "Continuing\n");
                        write(STDOUT_FILENO, msg, strlen(msg));
                        continue_job(head);
                    }
                    //job siezes tty
                    if (isatty(STDIN_FILENO)) {
                    seize_tty(head -> pgid);
                    }
                    //update job status to determine when complete or stopped
                    do {
                        pid = waitpid(WAIT_ANY, &status, WUNTRACED);
                    } while (!mark_process_status(pid, status) && !job_is_stopped(head) && !job_is_completed(head));
                    //dsh siezes tty
                    if (isatty(STDIN_FILENO)) {
                    seize_tty(dsh_pgid);
                    }
                }
                head = head -> next;
            }
            //free built in job fg
            frees_job(last_job);
            return true;
        }
        return false;       /* not a builtin command */
}

/* Build prompt messaage */
char* promptmsg()  {
    /* Modify this to include pid */
    pid_t pid;
    pid = getpid();
    sprintf(prompt_pid, "dsh_%d$", pid);
	return prompt_pid;
}


int main() {

	init_dsh();
    dsh_pgid = getpid();
	DEBUG("Successfully initialized\n");
    
    //open up file for logging (dsh.log)
    int dsh_log = open ("dsh.log", O_TRUNC | O_CREAT | O_WRONLY, 0666);
    //printing to stderr now prints to dsh.log since we want all error messages to be logged in dsh.log
    dup2(dsh_log, 2);
    
	while(1) {
        job_t *j = NULL;
		if(!(j = readcmdline(promptmsg()))) {
			if (feof(stdin)) { /* End of file (ctrl-d) */
				fflush(stdout);
				printf("\n");
				exit(EXIT_SUCCESS);
           		}
			continue; /* NOOP; user entered return or spaces with return */
		}
    
        /* Only for debugging purposes to show parser output; turn off in the
         * final code */
        if(PRINT_INFO) {
            print_job(j);
        }
        
        /* Your code goes here */
        job_t *current = j;
        bool flag = false;
        //loop through jobs since command line can contain ; and give multiple jobs...
        while (current != NULL && current -> first_process -> argc != 0 /*check for comment*/) {
            //check if job is builtin
            if (!builtin_cmd(current, current -> first_process -> argc, current -> first_process -> argv)) {
                //check if jobs have already been added, add_job always returns true...
                if (!flag) {
                    flag = add_job(j);
                }
                //job in background
                if (j -> bg) {
                    spawn_job(current, false);
                }
                //job in foreground
                else {
                    spawn_job(current, true);
                }
            }
            current = current -> next;  
        }
        
        /* You need to loop through jobs list since a command line can contain ;*/
        /* Check for built-in commands */
        /* If not built-in */
            /* If job j runs in foreground */
            /* spawn_job(j,true) */
            /* else */
            /* spawn_job(j,false) */
    }
}
