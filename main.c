#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <readline/readline.h>
#include <stdbool.h>


/**Struct Defs**/
typedef struct _procstruct{
    char** CMD; //the list of tokens to pass to execvp
    char* INPUT; //file descriptors for replacement, std macros if not
    char* OUTPUT;
    char* ERROR;
    pid_t pid; //pid of the process
} ProcessStruct;

//TODO: If time to implement infinite pipe, processes should be a linked list rather than distinct children of the job struct
typedef struct _jobstruct {
    struct _jobstruct* nextJob; // pointer to the next job
    ProcessStruct* p1; //first process
    ProcessStruct* p2; //second process
    int numProcesses; //how many processes we actually have
    int bg; //whether or not this job is in the background
    int status; //status of this job
    int pgid; //pgid of the job
} JobStruct;

/**Necessary subfunctions**/
char** parse_input(char *input, const char* delim);
ProcessStruct* createProcess(char** tokens);
JobStruct* createJob(char** tokens);
void startProcess(ProcessStruct* proc, int procInput, int procOutput);

/**Helpful Macros**/
//Process creation macros
#define NOPIPE -1 //startProcess argument to indicate that there is no pipe in a direction

//Job control macros
#define FOREGROUND 0 //foreground job
#define BACKGROUND 1 //background job
#define RUNNING 0 //status macros
#define STOPPED 1
#define TERMINATED 2


int main(){
    char *strbuf; //buffer to hold the input string
    char** tokens; //buffer to hold the tokenized version of the input string
    JobStruct* fjob; //pointer to the current foreground job
    JobStruct* bjobs; //pointer to the stack of background jobs

    //ignore certain signals for the main shell process
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    // signal(SIGINT,  SIG_IGN); //UNCOMMENT once Ctrl D works
    
    while(1){
        /**Create a Job to execute**/
        //read in input
        strbuf = readline("# ");
        tokens = parse_input(strbuf," "); //tokenize based on whitespace
        if(tokens[0]==NULL){ //handle nocmd without memleak
            free(strbuf);
            free(tokens);
            continue;
        }
        JobStruct* job = createJob(tokens); 
        /**Execute the Job that was created**/
        int pipeArr[2]; //create the pipe if needed, condition on job numjobs field
        if(job->numProcesses == 2){
            pipe(pipeArr); //NOTE: the right side is the write side, the left side is the read side

            //Exec Process 1
            pid_t child1 = fork();
            if(child1==0){
                //unignore signals
                signal(SIGTSTP, SIG_DFL);
                signal(SIGINT,  SIG_DFL);
                close(pipeArr[0]); //close pipe
                setpgid(getpid(),getpid()); //setup and add to prog grp

                //transfer terminal control if job is a foreground job
                if(job->bg == FOREGROUND){
                    tcsetpgrp(STDIN_FILENO, job->pgid);
                } 
                //start process
                job->status = RUNNING;
                startProcess(job->p1,NOPIPE,pipeArr[1]); 
            }
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
                if(job->bg == FOREGROUND){
                    tcsetpgrp(STDIN_FILENO, job->pgid);
                } 
                //start process
                job->status = RUNNING;
                startProcess(job->p2,pipeArr[0], NOPIPE); 
            }
            job->status = RUNNING; //set job as running

            //close the pipes
            close(pipeArr[0]);
            close(pipeArr[1]);
            //in case child processes havent been scheduled yet, set their pgids
            setpgid(child1, job->pgid); 
            setpgid(child2, job->pgid);
                
            //wait on processes to finish
            int execstat1;
            int execstat2;
            if(job->bg == FOREGROUND){
                tcsetpgrp(STDIN_FILENO, job->pgid); // transfer terminal control to the job because its foreground
                waitpid(child1,&execstat1,WUNTRACED);  //wait on the two jobs
                waitpid(child2,&execstat2,WUNTRACED);
            } else {
                waitpid(child1,&execstat1,WUNTRACED|WNOHANG); //non-blocking wait on the jobs
                waitpid(child2, &execstat2,WUNTRACED|WNOHANG);
            }


            #if DEBUG == 1 //DEBUG ONLY: Check if the processes have exited properly
            int debug;
            waitpid(child1, &debug, WNOHANG|WUNTRACED);
            int debug2; 
            waitpid(child2,&debug2,WNOHANG|WUNTRACED);
            printf("EXIT STATUS: %x\n",WIFEXITED(debug));
            printf("EXIT STATUS: %x\n",WIFEXITED(debug2));
            #endif
            //transfer terminal control back to the shell
            if(job->bg == FOREGROUND){
                tcsetpgrp(STDIN_FILENO, getpgid(0));
            }

        } else {
            pid_t child1 = fork();
            if(child1==0){
                //unignore stop and int signals
                signal(SIGTSTP, SIG_DFL);
                signal(SIGINT,  SIG_DFL);
                //setup pgid
                job->pgid = getpid();
                setpgid(getpid(),getpid());

                //transfer terminal control if job is a foreground job
                if(job->bg == FOREGROUND){
                    tcsetpgrp(STDIN_FILENO, getpgid(0));
                } 
                //Start the process
                job->status = RUNNING;
                startProcess(job->p1,NOPIPE,NOPIPE);
            }

            //setup pgid to wait on
           job->pgid = child1;
           setpgid(child1,job->pgid);
           job->status = RUNNING; //set job as running
           int execstat;
           if(job->bg == FOREGROUND){
                tcsetpgrp(STDIN_FILENO, job->pgid); // transfer terminal control to the job
                waitpid(child1,&execstat,WUNTRACED); //wait on the job
           } else {
                waitpid(child1,&execstat,WUNTRACED|WNOHANG); //non-blocking wait on the job
           }


           #if DEBUG == 1 //DEBUG ONLY: Check if the processes have exited properly
           int debug;
           waitpid(child1, &debug, WNOHANG|WUNTRACED);
           printf("EXIT STATUS: %x\n",WIFEXITED(debug));
           #endif

           //transfer terminal control back to the shell
           if(job->bg == FOREGROUND){
                tcsetpgrp(STDIN_FILENO, getpgid(0));
           }
      
        }

        //TODO: These cleanups need to get moved to wherever the jobs list gets pruned
        free(job->p1);
        free(job->p2);
        free(job);

        free(strbuf);
        free(tokens);
    }

}

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
JobStruct* createJob(char** tokens){
    JobStruct *job = (JobStruct*) malloc(sizeof(JobStruct));
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
        job->bg = BACKGROUND;
    } else {
        #if DEBUG == 1 
        printf("foreground process\n");
        #endif
        job->bg = FOREGROUND;
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
        dup2(errorfile,STDOUT_FILENO);
    }
    //execute the process defined in cmd
    execvp(proc->CMD[0],proc->CMD);
}






/**
 * TODO:
 * Add CTRL+D to quit the shell
 * Add linked list of jobs in order to implement a list of them
 * Get CTRL D working
 */

/**
 * JOBSNOTES:
 * Jobs commands prints exited jobs as "Done", and then afterwards prunes finished jobs
 * job needs a "visibility" or "position" field that indicates fg/bg. 
 * job needs a "status field" that indicates running/stopped/terminated
 * 
 * Start a foreground job (no ampersand at end of cmd) -> job should go into fg job node -> job should be marked as running and fg.
 * Start a background job (ampersand at end of cmd) -> job should go at top of bg job stack -> job should be marked as running and bg.
 * Stop foreground job (ctrl Z) -> job should go at the top of bg job stack -> job should be marked as stopped and bg.
 * bg cmd -> iterate over bg job stack until stopped job found -> Send SIGCONT using kill syscall -> print process in jobs format to stdout -> run in background. Job should be marked running. Don't wait on job
 * fg cmd -> bring most recent stopped OR background process to foreground -> print process in jobs format to stdout -> run in foreground, job should be marked running/fg, and put into the fg job ptr. Wait on job also.
 * 
 * STRATEGY:
 * At end of shell proc while loop, we need to iterate over the jobs (bg stack and fg job) to update their statuses. 
 * This can be done with a for loop, and waitpid (combined with macros). This for loop should go over the entire bg job stack and update their running statuses
 * Note: waitpid won't update status int, if the process has not changed status. 
 * Note: Job status is conditional upon both processes in a piped job.
 * 'jobs' command should print all the jobs in the bg list in the standard format. This can be done by iterating over the list of processes and printing them out. It should also prune the bg jobs list after printing
 * When a job finishes in the background, it should be printed the next time the while loop comes around. This can be done by running similar logic to the jobs command at the top of the while loop but it should only print finished jobs. It should also prune the jobs list after printing
 * 
 * FUNCTIONS:
 * pruneJobs() : trim the exited jobs from the bg jobs list : applies to bg jobs only
 * 
 * printFinishedJobs() : print the finished jobs from the bg jobs list : applies to bg jobs only
 * 
 * updateJobStatus() : iterate over the given linked list represented by a head ptr, and use the 
 * waitpid to update the job statuses of each. Make sure to initialize status int to -1, and 
 * if it doesn't change, don't update the process status : applies to bg jobs only (i think)
 * 
 * fg_handler() : foreground a job by switching the top bg job into the foreground job ptr. It should then also wait on this job to finish, and transfer terminal control back to it. 
 * It should then be waited on in the same way that a normal fg job is
 * 
 * bg_handler() : start running the most recently stopped job in the background. : applies to bg jobs only
 * 
 * NOTE: We need to have a blocking wait on the foreground job provided that the foreground job is not null. 
 * If it is null, we either have no jobs, or everything is in bg, meaning we can take a new input
 * 
 * NOTE: When waiting on an fg job, if waitpid exits, and shows the job was stopped, that job needs to move to the top of th bg job stack. 
 * 
 * NOTE: Need to restructure the current execution thread such that we skip the job creation and execution stage and go straight to waiting on the fg job if fg/bg/jobs is run
 * 
 */