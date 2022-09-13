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
    struct _jobstruct* nextJob; // pointer to the next job
    ProcessStruct* p1; //first process
    ProcessStruct* p2; //second process
    int numProcesses; //how many processes we actually have
    int visibility; //whether or not this job is in the background
    int status; //status of this job
    int pgid; //pgid of the job
    int jobnum; //the jobnumber of this job
} JobStruct;


/**JOB CREATION FUNCTIONS**/
char** parse_input(char *input, const char* delim); 
ProcessStruct* createProcess(char** tokens);
JobStruct* createJob(char** tokens, char* inputCmd);
void startProcess(ProcessStruct* proc, int procInput, int procOutput);

/**JOB CONTROL FUNCTIONS**/
JobStruct* addJobToBgStack(JobStruct* job, JobStruct* bgStack);
JobStruct* stopHandler(JobStruct* fgJob, JobStruct* bgJobs);
void updateJobStatus(JobStruct* jobslist);
JobStruct* pruneJobs(JobStruct* bgStack);
void waitOnJob(JobStruct* job, JobStruct** bgStack);
void printJob(JobStruct* job, int jobNum, char* jobInd);

/**SHELL COMMAND FUNCTIONS**/
void fg_handler(JobStruct** bgStack);
void bg_handler(JobStruct** bgStack);
void jobs_handler(JobStruct** bgStack, char* indicator);

/**Helpful Macros**/
//Process creation macros
#define NOPIPE -1 //startProcess argument to indicate that there is no pipe in a direction

//Job control macros
#define FOREGROUND 0 //foreground job
#define BACKGROUND 1 //background job
#define RUNNING 0 //status macros
#define STOPPED 1
#define TERMINATED 2
#define FAILED 3

/**DEBUG**/
void debugJobs(JobStruct* bgJobs);

/**PROGRAM STARTS HERE**/

int jobNumMax = 0; //global variable to hold the maximum job number currently
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
        if(strbuf == NULL){
            printf("\n");
            exit(0);
        }
        strbufcpy = (char*) malloc(strlen(strbuf)*sizeof(char));
        strcpy(strbufcpy,strbuf);

        //Update the bg jobs stack
        updateJobStatus(bgJobs);

        tokens = parse_input(strbuf," "); //tokenize based on whitespace
        if(tokens[0]==NULL){ //handle nocmd without memleak
            bgJobs = pruneJobs(bgJobs);
            free(strbuf);
            free(tokens);
            continue;
        } else if (strcmp(tokens[0],"jobs") == 0){ //jobs command is a shell cmd w no args
            jobs_handler(&bgJobs,"+");
            bgJobs = pruneJobs(bgJobs);
            continue;
        }
        #if DEBUG == 1
        printf("PRE-PRUNE\n");
        debugJobs(bgJobs);
        #endif
        bgJobs = pruneJobs(bgJobs);
        #if DEBUG == 1
        printf("POST-PRUNE\n");
        debugJobs(bgJobs);
        #endif
        
        if (strcmp(tokens[0],"fg") == 0){ //fg command is a shell cmd w no args. It puts the most recently stopped/backgrounded process back into foreground run status
            fg_handler(&bgJobs);
            continue;
        } else if (strcmp(tokens[0],"bg") == 0){ //bg command is a shell cmd w no args
            bg_handler(&bgJobs);
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
                int err = close(pipeArr[0]); //close pipe
                if(err == -1){
                    printf("Failed to close pipe. \n"); 
                    exit(-1);
                }
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
                int err = close(pipeArr[1]); //close pipe
                if(err == -1){
                    printf("Failed to close pipe. \n"); 
                    exit(-1);
                }
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
            int err0 = close(pipeArr[0]);
            int err1 = close(pipeArr[1]);
            if(err1 == -1 || err0 == -1){
                printf("Failed to close pipe. \n");
                exit(-1);
            }

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
    jobNumMax++; //assign jobnum on creation
    job->jobnum = jobNumMax;
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
    int stdoutcpy =  dup(STDOUT_FILENO);
    int stdincpy  =  dup(STDIN_FILENO);
    int stderrcpy =  dup(STDERR_FILENO);
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
        if(inputfile == -1){
            exit(-1);
        }
        dup2(inputfile,STDIN_FILENO);
    }
    if(proc->OUTPUT != NULL){
        //redirect stdout to OUTPUT
        int outputfile = open(proc->OUTPUT,O_WRONLY|O_APPEND|O_CREAT,S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
        if(outputfile == -1){
            exit(-1);
        }
        dup2(outputfile,STDOUT_FILENO);
    }
    if(proc->ERROR != NULL){
        //redirect stderr to ERROR
        int errorfile = open(proc->ERROR,O_WRONLY|O_APPEND|O_CREAT);
        if(errorfile == -1){
            exit(-1);
        }
        dup2(errorfile,STDERR_FILENO);
    }
    //execute the process defined in cmd
    int retstat = execvp(proc->CMD[0],proc->CMD);
    
    //if execvp failed, reset all stds
    dup2(stdincpy,STDIN_FILENO);
    dup2(stdoutcpy,STDOUT_FILENO);
    dup2(stderrcpy,STDERR_FILENO);
    
    if(procOutput != NOPIPE){
        int closeStat = close(procOutput);
        if(closeStat == -1){
            exit(-1);
        }
    }
    if(procInput != NOPIPE){
        int closeStat = close(procInput);
        if(closeStat == -1){
            exit(-1);
        }
    }
    exit(-1);
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
                job->p1 = NULL;
                free(job->jobcmd);
                job->jobcmd = NULL;
                free(job);
                job = NULL;
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
                job->p1 = NULL;
                free(job->p2);
                job->p2 = NULL;
                free(job->jobcmd);
                job->jobcmd = NULL;
                free(job);
                job = NULL;
            };
        }
    } else {
        //assign jobnum to job here
        *bgStack = addJobToBgStack(job,*bgStack);
    }
}

/**JOB CONTROL FUNCTIONS**/
/**
 * @brief Given a job and a background job stack pointer, this function will assign a jobnum to the job, and add it to the bgstack
 * 
 * @param job 
 * @return ** void 
 */
JobStruct* addJobToBgStack(JobStruct* job, JobStruct* bgStack){
    // if(job->jobnum >= 1){
        //assign jobNum to job here
        // (jobNumMax)++;
        // job->jobnum = jobNumMax;
    // }
    job->nextJob = bgStack;
    return job;
};

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
            if(WIFEXITED(execstatus1) | WIFSIGNALED(execstatus1)){ //the job was terminated either by exit call or by signal 
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
    return addJobToBgStack(fgJob, bgJob);
}


/**
 * @brief A helper function for the pruneJobs function that reverses the stack of background jobs and prints it properly
 * @param bgStack pointer to the background jobs stack
 * @param indicator Indicator for the top-most job, should be a "+"
 */
void pruneJobs_helper(JobStruct* bgStack,char* indicator){
    JobStruct* j = bgStack;
    if(j == NULL){
        return;
    }
    if(j->nextJob!=NULL){
        pruneJobs_helper(j->nextJob,"-");
        if(j->status==TERMINATED){
            printJob(j,j->jobnum,indicator);
            free(j->p1);
            if(j->numProcesses == 2){
                free(j->p2);
            }
            free(j->jobcmd);
        }
        free(j);
        return;
    } else {
        if(j->status==TERMINATED){
            printJob(j,j->jobnum,indicator);
            free(j->p1);
            if(j->numProcesses == 2){
                free(j->p2);
            }
            free(j->jobcmd);
        }
        free(j);
        return;
    }
}

/**
 * @brief Given a list of jobs, prune the ones that are TERMINATED and return a new list without them
 * 
 * @param bgStack 
 * @return JobStruct* 
 */
JobStruct* pruneJobs(JobStruct* bgStack){
    int maxJobNum = 0; //the maxJobNum still active after terminated jobs are pruned
    JobStruct* newStack = NULL;
    JobStruct* newTrav = NULL;
    //iterate over the old list, copy non-terminated jobs into the new list
    for(JobStruct* trav = bgStack; trav!=NULL;trav= trav->nextJob){
        if(trav->status!=TERMINATED){
            //copy to new list
            if(newStack == NULL){
                newStack = (JobStruct*) malloc(sizeof(JobStruct)); //allocate node
                memcpy(newStack,trav,sizeof(JobStruct)); //copy the old node data
                newStack->nextJob = NULL;
                newTrav = newStack; //setup traverser for new list
            } else {
                newTrav->nextJob = (JobStruct*) malloc(sizeof(JobStruct)); //allocate node
                newTrav = newTrav->nextJob; //iterate
                memcpy(newTrav,trav,sizeof(JobStruct)); //copy the old node data   
                newTrav->nextJob = NULL;
            }

            if(newTrav->jobnum > maxJobNum){
                maxJobNum = newTrav->jobnum;
            }
        } 
    }
    pruneJobs_helper(bgStack,"+");
    jobNumMax = maxJobNum;
    return newStack;
}

/**SHELL COMMAND FUNCTIONS**/

/**
 * @brief This function pretty-prints a job given a jobstruct
 * 
 * @param job The pointer to this job 
 * @param jobNum This job's job number
 * @param jobInd "+" if this job would get selected by fg, "-" otherwise
 */
void printJob(JobStruct* job, int jobNum, char* jobInd){
    char status[20];
    if(job->status == RUNNING)      { memcpy(status,"Running",sizeof("Running"));}
    if(job->status == STOPPED)      { memcpy(status,"Stopped",sizeof("Stopped"));}
    if(job->status == TERMINATED)   { memcpy(status,   "Done",   sizeof("Done"));}
    printf("[%x] %s %s      %s",jobNum,jobInd,status, job->jobcmd);
    printf("\n");
}


/**
 * @brief Handler for the 'fg' command. It iterates over the bgstack, brings the most recently stopped process to the foreground, and begins running it
 * 
 * @param bgStack Pointer to the background stack
 */
void fg_handler(JobStruct** bgStack){
    if(*bgStack == NULL){
        printf("no background jobs\n");
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
 * @param bgStack Pointer to the background stack
 */
void bg_handler(JobStruct** bgStack){
    if(*bgStack == NULL){
        printf("no background jobs\n");
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
 * @param bgStack Pointer to the first node of the background stack
 * @param indicator Indicator token for the topmost job
 */
void jobs_handler(JobStruct** bgStack,char* indicator){
    JobStruct* j = *bgStack;
    if(j == NULL){
        return;
    }
    if(j->nextJob!=NULL){
        jobs_handler(&(j->nextJob),"-");
        if(j->status!=TERMINATED){
            printJob(j,j->jobnum,indicator);
        }
        return;
    } else {
        if(j->status!=TERMINATED){
            printJob(j,j->jobnum,indicator);
        }
        return;
    }
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

}

/**
 * TODO: fix the jobs printing on pruneJobs
 * 
 */