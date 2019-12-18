#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>

/* This program is a simple shell.  Its built-in commands are exit, cd, and status.  The shell uses the PATH variable to look
use non built-in commands.
*/
#define CLINELENGTH 2048   // Maximum command line length
#define ARGLENGTH 512      // Maximum argument count
#define DELIMITERS " \n\t" // The list of delimiters that strtok uses to break up a user's command line input

// I typically try to avoid using lots of global variables, but ended up using a few in this assigment simply because they made
// things so much easier.
char RIN[50], ROUT[50]; // character arrays to store where standard input and standard output are being redirected
// BACKGROUND keeps track of whether a process is background or foreground
// FOREGROUND is used in catchSIGSTOP to help switch the program to FOREGROUND-ONLY mode
// inTrue and outTrue are both used to help process command-line inputs from the user that have redirection
int BACKGROUND = 0, FOREGROUND = 0, inTrue = 0, outTrue = 0;
;

void userInput(char *line);          // Takes input from the user (on the command line)
char **parseArgs(char *uI, int pid); // Parses through the user's input and breaks it up into commands, arguments, redirection stuff, and then finally & (for background processes)
void biCD(char **args);              // changes directories (built-in command)
void biStatus(int cStatus);          // Gives status of processes
void catchSIGSTOP(int signo);        // Function to deal with SIGTSTP - When user inputs ctrl-z, program switches into foreground-only mode (I explain more on this later)

int main(int argc, char ***argv)
{
    // args is essentially an array of strings that stores processed/parsed input from the user
    // uInput holds the user's input (in string form)
    char **args, uInput[CLINELENGTH];
    pid_t bgProcesses[10];
    // cStatus holds data on how a foreground process exited or was terminated
    // bgStatus holds data on how a background process was exited or terminated
    // sPid is the pid of the main process
    // i is used as a counter in various parts of the program.  It is a filler all-purpose variable
    int cStatus = 0, bgStatus = 0, sPid = getpid(), i, bgCounter = 0;
    ;
    // childPid is used to start new processes and its value shows if a process was successfully started or not
    pid_t childPid;

    // This is all slightly modified code that was borrowed from Professor Brewster's lecture notes on signals (3.3).  It sets how the program handles interrupt and
    // tstp signals.  You can interrupt foreground actions with ctrl-c.  However the main process and background processes ignore ctrl-c.  Foreground and background
    // proesses ignore tstp (ctrl-z) signals.  The main process, however, switches to foreground-only mode when the user inputs the ctrl-z signal (I elaborate on this later)
    struct sigaction SIGINT_action = {0}, SIGTSTP_action = {0};

    SIGINT_action.sa_handler = SIG_IGN;
    sigfillset(&SIGINT_action.sa_mask);
    SIGINT_action.sa_flags = 0;

    SIGTSTP_action.sa_handler = catchSIGSTOP;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = 0;

    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &SIGINT_action, NULL);
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    // This is the main program loop.  It prints the user input prompt and essentially runs the entire program.
    do
    {
        // printing user prompt
        //printf("%d\n", FOREGROUND);
        //fflush(stdout);
        printf(": ");
        // flushing/clearing the output buffer, because Professor Brewster told me to
        fflush(stdout);
        // Calling userInput to get, well, the user's input
        userInput(uInput);
        fflush(stdin);
        // We have to look for $$ and expand it to the PID of the main process.  This was actually fairly obnoxious to program, and the only way I could get it to work
        // was to do it at this point of the process of processing the user's input.  Initially, it was done in the parseArgs function with the **args array, but I simply
        // couldn't get it to work.  So, I decided to just go into the unparsed user input and replace it there with strcpy and then sprintf.
        // This for-loop looks for $$ and if it finds it, it replaces the first $ with a terminating 0 (to essentially make that new end of the string).  It then
        // creates a temp string and copies uInput into it with strcpy.  sprintF is then used to copy the entire string + the PID into uInput
        for (i = 0; i < strlen(uInput); i++)
        {
            if (uInput[i] == '$')
            {
                if (uInput[i + 1] == '$')
                {
                    uInput[i] = '\0';
                    char temp[CLINELENGTH];
                    strcpy(temp, uInput);
                    sprintf(uInput, "%s%d\n", temp, sPid);
                }
            }
        }
        // Calling parseArgs to go through the user input and break it up into the command [arg1, arg2 ...] [&] format (redirection instrutions are dealt with separately
        // and aren't included in the arg array);
        args = parseArgs(uInput, sPid);
        // if the user actually typed something in, then we start processing his/her instructions
        if (args[0] != NULL)
        {
            // If the first character is a #, the shell treats it as a comment (that is, it just leaves it alone)
            char *first;
            first = args[0];
            // I the user typed in exit, we exit the program
            if (strcmp(args[0], "exit") == 0)
            {

                // Doing some clean up before exiting
                if (bgCounter > 0)
                {
                    for (i = 0; i < bgCounter; i++)
                    {
                        printf("Killing process %d\n", bgProcesses[i]);
                        fflush(stdout);
                        kill(bgProcesses[i], SIGTERM);
                    }
                }
                free(args);
                exit(0);
            }
            // If the user types in cd, we start process of changing directories
            if (strcmp(args[0], "cd") == 0)
            {
                biCD(args);
            }
            // if user types in status, we go and see how the last process ended (if no child process has been started, the value is 0)
            else if (strcmp(args[0], "status") == 0)
            {
                biStatus(cStatus);
            }
            // If the user's input isn't a comment, we start create a child process
            else if (first[0] != '#')
            {
                // If the fork fails, we exit the loop
                if ((childPid = fork()) == -1)
                {
                    perror("The fork failed!");
                    break;
                }
                // Now, if the fork succeeded, we have to start processing the user's inputs
                else if (childPid == 0)
                {
                    SIGTSTP_action.sa_handler = SIG_IGN;
                    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
                    // If foreground mode is off and the user specified that the process be run in the background, we have to do a few things.
                    if (BACKGROUND == 1)
                    {
                        // First, we print out the pid of the newly created background pid
                        childPid = getpid();
                        printf("Background pid is %d\n: ", childPid);
                        fflush(stdout);
                        // If the user didn't specify any redirection, we set standard in to "/dev/null"
                        if (inTrue == 0)
                        {
                            strcpy(RIN, "/dev/null");
                            inTrue = 1;
                        }
                        // if the user didn't specify any redirection, we set standard out to "/dev/null"
                        if (outTrue == 0)
                        {

                            strcpy(ROUT, "/dev/null");
                            outTrue = 1;
                        }
                    }
                    else
                    {
                        // If it isn't a background process, we have to adjust the way the child process responds to interrupt signals, so that foreground processes
                        // end if the user inputs ctrl-c
                        SIGINT_action.sa_handler = SIG_DFL;
                        sigaction(SIGINT, &SIGINT_action, NULL);
                    }
                    // If the user wants input redirection, we set it up.  Standard in is changed to what the user specifies.  also, this code is borrowed from
                    // Professor Brewster's lecture notes on unitx IO (3.4.)
                    if (inTrue == 1)
                    {
                        int sourceFD = open(RIN, O_RDONLY);
                        if (sourceFD == -1)
                        {
                            perror("source open()");
                            exit(1);
                        }
                        int result = dup2(sourceFD, 0);
                        if (result == -1)
                        {
                            perror("source dup2()");
                            exit(2);
                        }
                    }
                    if (outTrue == 1)
                    {
                        int targetFD = open(ROUT, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                        if (targetFD == -1)
                        {
                            perror("target open()");
                            exit(1);
                        }
                        int result = dup2(targetFD, 1);
                        if (result == -1)
                        {
                            perror("dup2");
                            exit(2);
                        }
                    }
                    // calling execvp, so we can actually act on the user's instructions.   This changes the child process (which is a clone of the main parent process)
                    // to the program specified in args.
                    if (execvp(args[0], args))
                    {
                        // If the user inputted something that isn't an actual command, we let the user know, and close the process
                        printf("This is not a valid command.\n");
                        fflush(stdout);
                        exit(1);
                    }
                    // Doing some cleanup
                    free(args);
                }
                // If the new child process is a foreground one, we need to block the main parent process until the child process is finished with its task
                if (BACKGROUND == 0)
                {
                    waitpid(childPid, &cStatus, 0);
                }
                else
                {
                    bgProcesses[bgCounter] = childPid;
                    bgCounter++;
                }
                // This loop checks to see if any background processes have finished.  If they have, it prints the info to the terminal (and uses biStatus to
                // find out how the proess ended)
                for (; (childPid = waitpid(-1, &bgStatus, WNOHANG)) > 0;)
                {
                    if (childPid > 0)
                    {
                        printf("background pid %d is done: ", childPid);
                        fflush(stdout);
                        for (i = 0; i < bgCounter; i++)
                        {
                            if (bgProcesses[i] == childPid)
                            {
                                fflush(stdout);
                                bgCounter--;
                            }
                        }
                        biStatus(bgStatus);
                    }
                }
            }
        }
        //  Necessary bookkeeping/cleanup at the end of every iteration of the main program loop
        free(args);
        inTrue = 0;
        outTrue = 0;
        BACKGROUND = 0;
    } while (1);
    // exiting this crap program
    return 1;
}
// userInput takes command-line input from the user and sticks it into line with fgets.  Initially, I tried to use getline(), but I had so many struggles with it
// that I had to experiment with other ways to get input from the user.  For whatever reason, fgets was infinitely less obnoxious to use (I got so many seg faults
// with getLine() even when I took the precautions that professor brewster suggested in his required readings on getline())
void userInput(char *line)
{
    fgets(line, CLINELENGTH, stdin);
}
// parseArgs takes the user's input and then parses through it and breaks it up into individual components that the shell can actually interpret.
char **parseArgs(char *uI, int pid)
{
    // i is used as a counter
    int i = 0;
    // temp1 is malloced and will eventually store the results of strtoking the user's input
    char **temp1 = malloc(ARGLENGTH * sizeof(char *));
    if (!temp1)
    {
        perror("Malloc failed!");
        return NULL;
    }
    // Temp1 is processed and the results are copied into temp2 (temp2 (which is going to become args) is structered to look like this: command [arg1, arg2]).
    char **temp2 = malloc(ARGLENGTH * sizeof(char *));
    if (!temp2)
    {
        perror("Malloc failed!");
        return NULL;
    }
    // We use strtok to break the user input into an array of individual strings (the delimaters are tab, space, and newline)
    temp1[i] = strtok(uI, DELIMITERS);
    i++;
    while (temp1[i] = strtok(NULL, DELIMITERS))
    {
        i++;
    }
    // Making the final value in the array a null value
    temp1[i] = NULL;
    i = 0;
    // If the first value in temp1 is null, that means the user simply hit enter (and didn't input anything), so the function ends
    if (temp1[i] == NULL)
    {
        temp2[i] = NULL;
        return temp2;
    }
    // If the user actually inputted something of consequence, we
    do
    {
        // if the user wants input redirection, we set it up so the program  will eventually redirect (this happens in the main loop) to the file (or wherever) the user
        // wants input to come from
        if (strcmp(temp1[i], "<") == 0 && inTrue == 0)
        {
            i++;
            // RIN stores a string of the name of the location that we're redirecting to
            memset(RIN, '\0', sizeof(RIN));
            strcpy(RIN, temp1[i]);
            // Need to change inTrue to true so that redirection happens
            inTrue = 1;
        }
        // if the user wants output redirection, we set it up so the program  will eventually redirect (this happens in the main loop) to the file (or wherever) the user
        // wants output to come from
        else if (strcmp(temp1[i], ">") == 0 && outTrue == 0)
        {
            i++;
            // ROUT stores a string of the name of hte location that we're redirecting to
            memset(ROUT, '\0', sizeof(ROUT));
            strcpy(ROUT, temp1[i]);
            // Need to change outTrue to true so that redirection happens
            outTrue = 1;
        }
        // If the user wants the process to run in the background, we set BACKGROUND to true (this is used by conditionals in the main program loop)
        else if (strcmp(temp1[i], "&") == 0 && !temp1[i + 1])
        {
            if (FOREGROUND == 0)
            {
                BACKGROUND = 1;
            }
            break;
        }
        // Otherwise, we stick the value into temp 2 and iterate the counter by 1
        else
        {
            temp2[i] = temp1[i];
            i++;
        }
    } while (temp1[i] != NULL);

    // If I free temp1, my program explodes and won't run properly.  If I don't free it, I get a memory leak!
    //free(temp1);
    return temp2;
}
// biCD changes to a directory that the user specifies (assuming it is valid).  The format for this has to be: cd <directory>
void biCD(char **args)
{
    // if the user types cd <directory> and the directory is valid, the program switches to that directory
    if (args[1] != NULL)
    {
        if (chdir(args[1]) == -1)
        {
            perror("You have to switch to a valid directory!");
        }
    }
    // if the user simply types cd, the program goes to the home directory
    else
    {
        chdir(getenv("HOME"));
    }
}
// biStatus determines and prints out how proceses ended.
void biStatus(int cStatus)
{
    int status;
    // If the process exited on its own, biStatus prints out information on this
    if ((WIFEXITED(cStatus) != 0))
    {
        status = WEXITSTATUS(cStatus);
        printf("Exit value %d\n", status);
        fflush(stdout);
        return;
    }
    // if the process was killed/ended by a signal, biStatus prints out information on this
    else if (WIFSIGNALED(cStatus) != 0)
    {
        status = WTERMSIG(cStatus);
        printf("Terminated by signal %d\n", status);
        fflush(stdout);
        return;
    }
    printf("Exit value 0");
}
// catchSIGSTOP tells the program how to handle tstp (ctrl-z) signals.
void catchSIGSTOP(int signo)
{
    // Foreground mode is activated (if it isn't already on).  Foreground mode makes it so, new background processes cannot be made.
    if (FOREGROUND == 0)
    {
        FOREGROUND = 1;
        char *message = "Switching to foreground only mode!\n: ";
        write(STDOUT_FILENO, message, 37);
        fflush(stdout);
    }
    // If foreground mode is already on, it is turned off.
    else
    {
        FOREGROUND = 0;
        char *message = "Leaving foreground only mode!\n: ";
        write(STDOUT_FILENO, message, 32);
        fflush(stdout);
    }
}
