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
    bool bg; //whether or not this job is in the background
    int pgid; //pgid of the job
} JobStruct;

/**Necessary subfunctions**/
char** parse_input(char *input, const char* delim);
ProcessStruct* createProcess(char** tokens);
JobStruct* createJob(char** tokens);
void startProcess(ProcessStruct* proc, int procInput, int procOutput);

/**Helpful Macros**/
#define NOPIPE -1
#define DEBUG 1

int main(){
    char *strbuf; //buffer to hold the input string
    char** tokens; //buffer to hold the tokenized version of the input string
    signal(SIGTTOU,SIG_IGN);
    
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
                close(pipeArr[0]); //close pipe
                setpgid(getpid(),getpid()); //setup and add to prog grp

                //transfer term. control
                tcsetpgrp(STDIN_FILENO, job->pgid);

                //start process
                startProcess(job->p1,NOPIPE,pipeArr[1]); 
           }
           job->pgid = child1;

            //Exec Process 2
           pid_t child2 = fork();
           if(child2==0){
                close(pipeArr[1]); //close pipe
                setpgid(getpid(),job->pgid); //add to prog grp

                //transfer term. control
                tcsetpgrp(STDIN_FILENO, job->pgid);

                //start process
                startProcess(job->p2,pipeArr[0], NOPIPE); 
           }

           //close the pipes
           close(pipeArr[0]);
           close(pipeArr[1]);
           //in case child processes havent been scheduled yet, set their pgids
           setpgid(child1, job->pgid); 
           setpgid(child2, job->pgid);
               
           //wait on processes to finish
           int execstat1;
           int execstat2;
           waitpid((-1)*(job->pgid),&execstat1,WUNTRACED); //OH: Do we need a double wait here for the processes to finish?
           waitpid((-1)*(job->pgid),&execstat2,WUNTRACED);

           #if DEBUG == 1 //DEBUG ONLY: Check if the processes have exited properly
           int debug;
           waitpid(child1, &debug, WNOHANG|WUNTRACED);
           int debug2; 
           waitpid(child2,&debug2,WNOHANG|WUNTRACED);
           printf("%x\n",WIFEXITED(debug));
           printf("%x\n",WIFEXITED(debug2));
           #endif
           
           //transfer terminal control back to the shell
           tcsetpgrp(STDIN_FILENO, getpgid(0));
           
        } else {
            pid_t child1 = fork();
            if(child1==0){
                //setup pgid
                job->pgid = getpid();
                setpgid(getpid(),getpid());

                //transfer terminal control
                tcsetpgrp(STDIN_FILENO, getpgid(0));

                //Start the process
                startProcess(job->p1,NOPIPE,NOPIPE);
            }

            //setup pgid to wait on
           job->pgid = child1;
           setpgid(child1,job->pgid);
           tcsetpgrp(STDIN_FILENO, job->pgid); // transfer terminal control to the job
      

           int execstat;
           waitpid((-1)*(job->pgid),&execstat,WUNTRACED); //wait on the job

           #if DEBUG == 1 //DEBUG ONLY: Check if the processes have exited properly
           int debug;
           waitpid(child1, &debug, WNOHANG|WUNTRACED);
           printf("%x\n",WIFEXITED(debug));
           #endif

           //transfer terminal control back to the shell
           tcsetpgrp(STDIN_FILENO, getpgid(0));
      
        }


        //QUESTION FOR TA/PROF: IS THE FOLLOWING LINE OKAY?
        //TODO: add wait on job to finish in order to prevent weird printing errors
        //TODO: cleanup should wait on job finished
        free(strbuf);
        free(job->p1);
        free(job->p2);
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
    int i;
    job->numProcesses = 1; //every job has at least one process
    for(i = 0; tokens[i]!=0x0;i++){ //loop until null terminator
        if(strcmp(tokens[i],"|")==0 && tokens[i+1]!=NULL){ //if we find a pipe with something after it, replace it with a null and mark second as starting after it
            tokens[i] = NULL;
            secondCmd = i+1;
            job->numProcesses++;
            break; //TODO: If we have time to implement infinite pipe, get rid of this break
        }
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
 * Watch signals lecture to understand more
 * Add Support for backgrounding a job
 * Work on terminal ctrl
 * Organize code more once stuff is done
 */