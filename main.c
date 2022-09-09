#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <readline/readline.h>
#include <stdbool.h>

/**DECLARATIONS**/

/**Struct Defs**/
typedef struct _procstruct{
    char** CMD; //the list of tokens to pass to execvp
    char* INPUT; //file descriptors for replacement, std macros if not
    char* OUTPUT;
    char* ERROR;
    pid_t pid; //pid of the process
    int status; //status of the process
} ProcessStruct;

//TODO: If time to implement infinite pipe, processes should be a linked list rather than distinct children of the job struct
typedef struct _jobstruct {
    char* jobcmd; //complete input command
    int numtokens; //number of tokens in the input command
    struct _jobstruct* nextJob; // pointer to the next job
    ProcessStruct* p1; //first process
    ProcessStruct* p2; //second process
    int numProcesses; //how many processes we actually have
    int visibility; //whether or not this job is in the background
    int status; //status of this job
    int pgid; //pgid of the job
} JobStruct;


/**JOB CREATION FUNCTIONS**/
char** parse_input(char *input, const char* delim); 
ProcessStruct* createProcess(char** tokens);
JobStruct* createJob(char** tokens, char* inputCmd);
void startProcess(ProcessStruct* proc, int procInput, int procOutput);

/**JOB CONTROL FUNCTIONS**/
JobStruct* stopHandler(JobStruct* fgJob, JobStruct* bgJobs);
void updateJobStatus(JobStruct* jobslist);
JobStruct* pruneJobs(JobStruct* bgStack);
void waitOnJob(JobStruct* job, JobStruct** bgStack);
void printJob(JobStruct* job, int jobNum, char* jobInd);

/**SHELL COMMAND FUNCTIONS**/
void fg_handler(JobStruct** bgStack);
void bg_handler(JobStruct** bgStack);
void jobs_handler(JobStruct* bgStack);

/**Helpful Macros**/
//Process creation macros
#define NOPIPE -1 //startProcess argument to indicate that there is no pipe in a direction

//Job control macros
#define FOREGROUND 0 //foreground job
#define BACKGROUND 1 //background job
#define RUNNING 0 //status macros
#define STOPPED 1
#define TERMINATED 2

/**DEBUG**/
void debugJobs(JobStruct* bgJobs);

/**PROGRAM STARTS HERE**/

int main(){
    char* strbuf; //buffer to hold the input string
    char* strbufcpy; //buffer to hold a copy of the input string 
    char** tokens; //buffer to hold the tokenized version of the input string
    JobStruct* fgJob = NULL; //pointer to the current foreground job
    JobStruct* bgJobs = NULL; //pointer to the stack of background jobs

    //ignore certain signals for the main shell process
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGINT,  SIG_IGN); 
    
    while(1){
        /**Create a Job to execute**/
        //read in input, create one copy to store in the jobstruct, and one to tokenize
        strbuf = readline("# ");
        if(!strbuf){
            printf("\n");
            exit(0);
        }
        strbufcpy = (char*) malloc(strlen(strbuf)*sizeof(char));
        strcpy(strbufcpy,strbuf);

        //Update the bg jobs stack
        updateJobStatus(bgJobs);

        tokens = parse_input(strbuf," "); //tokenize based on whitespace
        if(tokens[0]==NULL){ //handle nocmd without memleak
            free(strbuf);
            free(tokens);
            continue;
        } else if (strcmp(tokens[0],"fg") == 0){ //fg command is a shell cmd w no args. It puts the most recently stopped/backgrounded process back into foreground run status
            fg_handler(&bgJobs);
            continue;
        } else if (strcmp(tokens[0],"bg") == 0){ //bg command is a shell cmd w no args
            bg_handler(&bgJobs);
            continue;
        } else if (strcmp(tokens[0],"jobs") == 0){ //jobs command is a shell cmd w no args
            //TODO: add the remaining logic for jobs command
            #if DEBUG == 1
            printf("PRE-PRUNE\n");
            debugJobs(bgJobs);
            #endif
            bgJobs = pruneJobs(bgJobs);
            jobs_handler(bgJobs);
            #if DEBUG == 1
            printf("POST-PRUNE\n");
            debugJobs(bgJobs);
            #endif
            continue;
        }

        JobStruct* job = createJob(tokens,strbufcpy);

        /**Execute the Job that was created**/
        int pipeArr[2]; //array to hold pipe
        /**Double Process Job**/
        if(job->numProcesses == 2){
            pipe(pipeArr); //create pipe

            //Exec Process 1
            pid_t child1 = fork();
            if(child1==0){
                //unignore signals
                signal(SIGTSTP, SIG_DFL);
                signal(SIGINT,  SIG_DFL);
                close(pipeArr[0]); //close pipe
                setpgid(getpid(),getpid()); //setup and add to prog grp

                //transfer terminal control if job is a foreground job
                if(job->visibility == FOREGROUND){
                    tcsetpgrp(STDIN_FILENO, job->pgid);
                }
                //start process
                startProcess(job->p1,NOPIPE,pipeArr[1]); 
            }
            //store pgid and pid of child1
            job->p1->pid = child1;
            job->pgid = child1;

            //Exec Process 2
            pid_t child2 = fork();
            if(child2==0){
                //unignore stop and int signals
                signal(SIGTSTP, SIG_DFL);
                signal(SIGINT,  SIG_DFL);
                close(pipeArr[1]); //close pipe
                setpgid(getpid(),job->pgid); //add to prog grp

                //transfer terminal control if job is a foreground job
                if(job->visibility == FOREGROUND){
                    tcsetpgrp(STDIN_FILENO, job->pgid);
                } 
                //start process
                startProcess(job->p2,pipeArr[0], NOPIPE); 
            }
            //store pid of child2
            job->p2->pid = child2;

            //close the pipes
            close(pipeArr[0]);
            close(pipeArr[1]);
            //in case child processes havent been scheduled yet, set their pgids
            setpgid(child1, job->pgid); 
            setpgid(child2, job->pgid);
            
            job->status = RUNNING; //set job as running
            job->p1->status = RUNNING;
            job->p2->status = RUNNING;

            //Job foregrounding/backgrounding and wait sequence
            waitOnJob(job,&bgJobs);
        /**Single Process Job**/
        } else {
            pid_t child1 = fork();
            if(child1==0){
                //unignore stop and int signals
                signal(SIGTSTP, SIG_DFL);
                signal(SIGINT,  SIG_DFL);
                
                //setup pgid
                setpgid(0,getpid());

                //transfer terminal control if job is a foreground job, this is here for race protection against parent thread
                if(job->visibility == FOREGROUND){
                    tcsetpgrp(STDIN_FILENO, getpgid(0));
                } 
                //Start the process
                startProcess(job->p1,NOPIPE,NOPIPE);
            }

            //store child pid
            job->p1->pid = child1;

            //setup pgid to wait on
            job->pgid = child1;
            setpgid(child1,job->pgid);

            //set job as running
            job->status = RUNNING; 
            job->p1->status = RUNNING;
            
            //Job foregrounding/backgrounding and wait sequence
            waitOnJob(job, &bgJobs);
        }
        free(strbuf);
        strbufcpy = NULL;
    }

}

/**JOB CREATION FUNCTIONS**/

/**
 * @brief Splits a given input up into tokens and returns a pointer to an array of the tokens as strings. This array is null-terminated
 * @param input 
 * @return char** 
 */
char **parse_input(char *input, const char* delim){
    char **retcmd = malloc(10000*sizeof(char*)); 
    char *saveptr;
    int index;
    char *token = strtok_r(input,delim,&saveptr);
    for(index = 0; token!=NULL; index++, token=strtok_r(NULL,delim,&saveptr)){
        retcmd[index] = token;
    }
    retcmd[index] = NULL;
    return retcmd;
};

/**
 * @brief Create a Job object by parsing an input line. It will either have one or two processes, which will be indicated by the
 * numProcesses field of the returned JobStruct
 * 
 * @param tokens 
 * @return JobStruct* 
 */
JobStruct* createJob(char** tokens, char* jobcmd){
    JobStruct *job = (JobStruct*) malloc(sizeof(JobStruct));
    job->nextJob = NULL;
    job->jobcmd = jobcmd;
    //parse the tokens for pipe here to create the job struct
    int secondCmd = -1; //index at which second cmd starts
    int i; //index of where we are in the string
    job->numProcesses = 1; //every job has at least one process
    bool pipeParseFinished = false; //whether or not we've finished parsing for pipes
    for(i = 0; tokens[i]!=0x0;i++){ //loop until null terminator
        if(strcmp(tokens[i],"|")==0 && tokens[i+1]!=NULL && !pipeParseFinished){ //if we find a pipe with something after it, replace it with a null and mark second as starting after it
            tokens[i] = NULL;
            secondCmd = i+1;
            job->numProcesses++;
            pipeParseFinished = true; //TODO: If we have time for infinite pipes, this should get removed and replaced
        }
    }

    //whether or not this job should start in the background
    if(strcmp(tokens[i-1],"&") == 0){
        #if DEBUG == 1 
        printf("background process\n");
        #endif
        tokens[i-1] = NULL;
        job->visibility = BACKGROUND;
    } else {
        #if DEBUG == 1 
        printf("foreground process\n");
        #endif
        job->visibility = FOREGROUND;
    }

    if(secondCmd == -1){
        secondCmd = i;
    }
    job->p1 = createProcess(tokens);
    if(job->numProcesses == 2){ //if we need a second process, create one, else null process
        job->p2 = createProcess(&tokens[secondCmd]);
    } else {
        job->p2 = (ProcessStruct*) NULL;
    }
    return job;
}

/**
 * @brief Create a Process object. Helper function for createJob. Will handle file redirection parsing. If there is file redirection,
 * the filenames will be in the INPUT,OUTPUT, and ERROR fields of the returned ProcessStruct. If there is no file redirection, the appropriate field will be a NULL
 * 
 * @param tokens 
 * @return ProcessStruct* 
 */
ProcessStruct* createProcess(char** tokens){
    ProcessStruct* proc = (ProcessStruct*) malloc(sizeof(ProcessStruct));
    proc->INPUT  = (char*) NULL; //default values
    proc->OUTPUT = (char*) NULL;
    proc->ERROR  = (char*) NULL;
    proc->pid = getpid();
    //parse for the actual command of the process and then the file redirectors
    for(int i = 0; tokens[i]!=0x0;i++){
        if(strcmp(tokens[i],"<")==0){
            //redirect the stdin
            proc->INPUT = tokens[i+1]; //mark the next token (the filename) as the input
            tokens[i] = NULL;
            i++;
        } else if (strcmp(tokens[i],">")==0){
            //redirect the stdout
            proc->OUTPUT = tokens[i+1]; //mark the next token (the filename) as the output
            tokens[i] = NULL;
            i++;
        } else if (strcmp(tokens[i],"2>")==0){
            //redirect the stderr
            proc->ERROR = tokens[i+1]; //mark the next token (the filename) as the error
            tokens[i] = NULL;
            i++;
        }
    }
    proc->CMD = tokens;
    return proc;
}

/**
 * @brief Start a process defined by a process struct. It takes in a ptr to the Process Struct, as well as two values representing the pipe file descriptors. However,
 * if the process struct already defines a stdin/stdout/stderr override, the pipe file descriptors will be ignored. Should be wrapped in a fork block, as this internally calls execvp
 * @param proc Pointer to the process struct
 * @param procInput Int describing pipe input, if applicable. If no pipe, this value will be null.
 * @param procOutput Int describing pipe output, if applicable. If no pipe, this value will be null.
 */
void startProcess(ProcessStruct* proc, int procInput, int procOutput){
    if(procInput != NOPIPE){
        //redirect pipe output to stdin
        dup2(procInput,STDIN_FILENO);
    }
    if(procOutput != NOPIPE){
        //redirect stdout to pipe
        dup2(procOutput,STDOUT_FILENO);
    }
    if(proc->INPUT != NULL){
        //redirect stdin to INPUT 
        int inputfile = open(proc->INPUT,O_RDONLY);
        dup2(inputfile,STDIN_FILENO);
    }
    if(proc->OUTPUT != NULL){
        //redirect stdout to OUTPUT
        int outputfile = open(proc->OUTPUT,O_WRONLY|O_APPEND|O_CREAT,S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
        dup2(outputfile,STDOUT_FILENO);
    }
    if(proc->ERROR != NULL){
        //redirect stderr to ERROR
        int errorfile = open(proc->ERROR,O_WRONLY|O_APPEND|O_CREAT);
        dup2(errorfile,STDERR_FILENO);
    }
    //execute the process defined in cmd
    execvp(proc->CMD[0],proc->CMD);
}

/**
 * @brief This function is a helper to wait on jobs. It will transfer terminal control 
 * and blocking wait if the job is a fg job, or it will add the job to the bg stack. It also handles stop signaling. 
 * TODO: this function needs to be implemented and tested
 * @param job 
 */
void waitOnJob(JobStruct* job, JobStruct** bgStack){
    int execstat1;
    int execstat2;
    if(job->visibility == FOREGROUND){
        if(job->numProcesses == 1){
            //foreground wait and finish routine
            tcsetpgrp(STDIN_FILENO, job->pgid); // transfer terminal control to the job
            waitpid(job->p1->pid,&execstat1,WUNTRACED); //wait on the job
            tcsetpgrp(STDIN_FILENO, getpgid(0)); //transfer terminal control back to the shell
            //if the process was stopped by ctrl+z, move the job to the background in the stopped state
            if(!(WIFEXITED(execstat1) || WIFSIGNALED(execstat1))){
                *bgStack = stopHandler(job,*bgStack);
            }else{
                free(job->p1);
                free(job->jobcmd);
                free(job);
            };
        } else {
            tcsetpgrp(STDIN_FILENO, job->pgid); // transfer terminal control to the job because its foreground
            waitpid(job->p1->pid,&execstat1,WUNTRACED);  //wait on the two jobs
            waitpid(job->p2->pid,&execstat2,WUNTRACED);
            tcsetpgrp(STDIN_FILENO, getpgid(0)); //transfer terminal control back to the shell

            //if the process was stopped by ctrl+z, move the job to the background in the stopped state
            if(
                (!(WIFEXITED(execstat2) || WIFSIGNALED(execstat2))) //if the second job didn't normally exit or got signal 
                ||
                (!(WIFEXITED(execstat1) || WIFSIGNALED(execstat1))) //if the first job didn't normally exit or got signal
            ){
                *bgStack = stopHandler(job,*bgStack);
            }else{
                free(job->p1);
                free(job->p2);
                free(job->jobcmd);
                free(job);
            };
        }
    } else {
        //background add to bgstack
        job->nextJob = *bgStack; //add job to the bg stack
        *bgStack = job;
    }
}

/**JOB CONTROL FUNCTIONS**/

/**
 * @brief Updates a list of job statuses, extracting process statuses, and populating the job struct for every job in the linked list
 * 
 * @param jobslist the list of jobs to update values for
 */
void updateJobStatus(JobStruct* jobslist){
    //iterate over the list of jobs
    int execstatus1 = -2;
    int execstatus2 = -2;
    for(;jobslist!=NULL;jobslist=jobslist->nextJob, execstatus1=-2, execstatus2=-2){
        if(jobslist->numProcesses == 1){ //single process job
            int stat = waitpid(jobslist->p1->pid,&execstatus1,WNOHANG|WUNTRACED|WCONTINUED); // get status of the process into execstatus1
            if(execstatus1 == -2){ //nothing has changed for this PID
                continue;
            }
            if(WIFEXITED(execstatus1) | WIFSIGNALED(execstatus1)){ //the job was terminated either by exit call or by signal TODO: NEVER GETTING TO THIS POINT
                jobslist->p1->status = TERMINATED;
            } else if (WIFSTOPPED(execstatus1)){ //the job was stopped by a signal
                jobslist->p1->status = STOPPED;
            } else if (WIFCONTINUED(execstatus1)){ //the job was restarted by a signal
                jobslist->p1->status = RUNNING;
            }
            jobslist->status = jobslist->p1->status;

        } else {//double process job
            waitpid(jobslist->p1->pid,&execstatus1,WNOHANG|WUNTRACED|WCONTINUED); // get status of the process into execstatus1
            waitpid(jobslist->p2->pid,&execstatus2,WNOHANG|WUNTRACED|WCONTINUED); // get status of the process into execstatus2

            if(execstatus1 != -2){
                if(WIFEXITED(execstatus1) | WIFSIGNALED(execstatus1)){ //the job was terminated either by exit call or by signal
                    jobslist->p1->status = TERMINATED;
                } else if (WIFSTOPPED(execstatus1)){ //the job was stopped by a signal
                    jobslist->p1->status = STOPPED;
                } else if (WIFCONTINUED(execstatus1)){ //the job was restarted by a signal
                    jobslist->p1->status = RUNNING;
                }   
            }
            
            if(execstatus2 != -2){
                if(WIFEXITED(execstatus2) | WIFSIGNALED(execstatus2)){ //the job was terminated either by exit call or by signal
                    jobslist->p2->status = TERMINATED;
                } else if (WIFSTOPPED(execstatus2)){ //the job was stopped by a signal
                    jobslist->p2->status = STOPPED;
                } else if (WIFCONTINUED(execstatus2)){ //the job was restarted by a signal
                    jobslist->p2->status = RUNNING;
                }
            }

            if(jobslist->p1->status == RUNNING | jobslist->p2->status == RUNNING){
                jobslist->status = RUNNING;
            } else if (jobslist->p1->status == STOPPED | jobslist->p2->status == STOPPED){
                jobslist->status = STOPPED;
            } else {
                jobslist->status = TERMINATED;
            }
        }
    }
    return;
}

/**
 * @brief Set the status of a foreground job to stopped, and add it to the top of a background jobs stack
 * 
 * @param fgJob The foreground job to be marked as stopped
 * @param bgJob The Background jobs stack to add the newly stopped background jo
 */
JobStruct* stopHandler(JobStruct* fgJob, JobStruct* bgJob){
    fgJob->visibility = BACKGROUND;
    fgJob->status = STOPPED;
    fgJob->p1->status = STOPPED;
    if(fgJob->numProcesses==2){
        fgJob->p2->status = STOPPED;
    }
    fgJob->nextJob = bgJob;
    return fgJob;
}

/**
 * @brief Given a list of jobs, prune the ones that are TERMINATED and return a new list without them
 * 
 * @param bgStack 
 * @return JobStruct* 
 */
JobStruct* pruneJobs(JobStruct* bgStack){
    JobStruct* headptr = bgStack;
    if(headptr == NULL){
        return headptr;
    }
    if(headptr->status == TERMINATED){
        JobStruct* ret = headptr->nextJob;
        free(headptr->p1);
        if(headptr->numProcesses==2){
            free(headptr->p2);
        }
        free(headptr);
        return ret;
    }
    //iterate over stack with fast/slow ptrs, headptr is slow, trav is fast
    JobStruct* slow = headptr;
    for(JobStruct* trav = headptr->nextJob; trav != NULL; slow = trav, trav = trav->nextJob){
        if(trav->status == TERMINATED){
            slow->nextJob = trav->nextJob;
            free(trav->p1);
            free(trav->jobcmd);
            if(trav->numProcesses == 2){
                free(trav->p2);
            }
            free(trav);
        }
    }
    return headptr;

}

/**SHELL COMMAND FUNCTIONS**/

/**
 * @brief Handler for the 'fg' command. It iterates over the bgstack, brings the most recently stopped process to the foreground, and begins running it
 * 
 * @param bgStack 
 */
void fg_handler(JobStruct** bgStack){
    if(*bgStack == NULL){
        #if DEBUG == 1
        printf("no background jobs\n");
        #endif
        return;
    }
    JobStruct* job = NULL;
    JobStruct* prevJob = NULL;
    for(JobStruct* nodeptr = *bgStack; nodeptr != NULL; prevJob = nodeptr, nodeptr = nodeptr->nextJob){
        //iterate over the LL
        if(nodeptr->status != TERMINATED){ //stopped or already running
            nodeptr->status = RUNNING; //set to running
            nodeptr->visibility = FOREGROUND; //pull to foreground
            if(prevJob != NULL){
                prevJob->nextJob = nodeptr->nextJob; //remove the node from the bg stack
            } else {
                //nodeptr is pointing to headptr
                *bgStack = nodeptr->nextJob;
            }
            
            job = nodeptr;
            break;
        }
    }
    if(job != NULL){
        if(job->p1->status!=TERMINATED){
            job->p1->status = RUNNING;
        }
        kill(job->p1->pid, SIGCONT);
        
        if(job->numProcesses == 2){
            if(job->p2->status!=TERMINATED){
                job->p2->status = RUNNING;
            }
            kill(job->p2->pid,SIGCONT);
        }

        waitOnJob(job, bgStack);
    }
}

/**
 * @brief Handler for the 'bg' command. It iterates over the bgstack, and begins running the most recently stopped process in the backgroun
 * 
 * @param bgStack 
 */
void bg_handler(JobStruct** bgStack){
    if(*bgStack == NULL){
        #if DEBUG == 1
        printf("no background jobs\n");
        #endif
        return;
    }
    JobStruct* job = NULL;
    JobStruct* prevJob = NULL;
    for(JobStruct* nodeptr = *bgStack; nodeptr != NULL; prevJob = nodeptr, nodeptr = nodeptr->nextJob){
        //iterate over the LL and find the first stopped job to start
        if(nodeptr->status == STOPPED){
            nodeptr->status = RUNNING;
            nodeptr->visibility = BACKGROUND;
            if(prevJob != NULL){
                prevJob->nextJob = nodeptr->nextJob; //remove the node from the bg stack
            } else {
                //nodeptr is pointing to a stopped headptr
                *bgStack = nodeptr->nextJob;
            }
            job = nodeptr;
            break;
        }
    }
    if(job != NULL){
        if(job->p1->status!=TERMINATED){
            job->p1->status = RUNNING;
        }
        kill(job->p1->pid, SIGCONT);
        
        if(job->numProcesses == 2){
            if(job->p2->status!=TERMINATED){
                job->p2->status = RUNNING;
            }
            kill(job->p2->pid,SIGCONT);
        }

        waitOnJob(job, bgStack);
    }
    
    return;
}

/**
 * @brief Handler for the 'jobs command. It iterates over the bgstack, and prints out all of the jobs and their current statuses in the required format.
 * 
 * @param bgStack 
 */
void jobs_handler(JobStruct* bgStack){
    char* jobInd = "+"; //flag to mark the job that fg would pick
    int jobNum = 1;
    for(JobStruct* trav = bgStack; trav!=NULL;trav = trav->nextJob, ++jobNum, jobInd = "-"){
        printJob(trav,jobNum,jobInd);
    }
}

/**
 * @brief This function pretty-prints a job given a jobstruct
 * 
 * @param job The pointer to this job 
 * @param jobNum This job's job number
 * @param jobInd "+" if this job would get selected by fg, "-" otherwise
 */
void printJob(JobStruct* job, int jobNum, char* jobInd){
    char status[20];
    if(job->status == RUNNING){ memcpy(status,"Running",sizeof("Running"));}
    if(job->status == STOPPED){ memcpy(status,"Stopped",sizeof("Stopped"));}
    printf("[%x] %s %s      %s",jobNum,jobInd,status, job->jobcmd);
    printf("\n");
}

/**Debug utilities**/
void debugJobs(JobStruct* bgJobs){
    int cnt = 0;
    int cntTerm = 0;
    int cntStp = 0;
    int cntRun = 0;
    for(JobStruct* head = bgJobs;head!=NULL;head=head->nextJob){
        cnt++;
        switch(head->status){
            case RUNNING:
                cntRun++;
                break;
            case TERMINATED:
                cntTerm++;
                break;
            case STOPPED:
                cntStp++;
                break;
        }
    }
    printf("Count of bg jobs:               %i\n",cnt);
    printf("Count of running bg jobs:       %i\n",cntRun);
    printf("Count of terminated bg jobs:    %i\n",cntTerm);
    printf("Count of stopped bg jobs:       %i\n",cntStp);
};



/*
TODO: Get jobs working - sort of? Need to refine and finish this off
TODO: Jobnumber feature addition
TODO: Job Done print asynchronously -> put it in updateJobs
TODO: Error handling - pipe improperly closing, file open issues, error output redirection
TODO: Delete files that get created if invalid cmd w redirection comes in


BUGS:
- WaitonJobs is freeing the jobs if they terminate. This is called within UpdateJobs. 
  This means that when the jobs command is run, all of the terminated jobs have already been freed. 
  This should only happen after they've been printed to console or jobs has been called.
- Add pretty-printing for terminated bg jobs 
- Restarting jobs is causing other stopped jobs to get lost
*/