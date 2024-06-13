/**
 * uqfindexec.c
 * A program which allows users to run a specified command or pipeline of
 * commands on all files in a given directory.
 *
 * Written by Alvin Benny
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csse2310a3.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <locale.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>

#define BUFFER 1024 // based on ulimit -u
#define READWRITE 0666

// Structure to hold command line arguments
typedef struct {
    const char* dir;
    int recurse;
    int showHidden;
    int parallel;
    int report;
    const char* cmd;
    int dirSpecified;
} ProgParams;

// Structure which stores information for the result of processing files
typedef struct {
    int total;
    int successful;
    int failed;
    int terminated;
    int unable;
} ReportParams;

// Structure to store the the result of how files were processed
typedef struct {
    int allCmdsNormal;
    int nonZeroExit;
    int signalExit;
    int pipelineNotExec;
    int ioError;
} ReportFlags;

// Structure to store read directory information
typedef struct {
    struct dirent** fileList;
    int numOfEntries;
} DirInfo;

// Command line option arguments
const char* const dirArg = "--dir";
const char* const recurseArg = "--recurse";
const char* const showHiddenArg = "--showhidden";
const char* const parallelArg = "--parallel";
const char* const reportArg = "--report";
const char* const defaultDir = ".";
const char* const defaultCMD = "echo {}";

// Exit status values
typedef enum {
    OK = 0,
    USAGE_ERROR = 1,
    DIRECTORY_ERROR = 13,
    CMD_ERROR = 16,
    EXEC_ERROR = 99,
    SIG_INTERRUPTED = 14,
    PROCESSING_FAILED = 7,
    IO_ERROR = 98,
} ExitStatus;

// Error messages
const char* const errorUsage = "Usage: uqfindexec [--dir dir] [--recurse] "
                               "[--showhidden] [--parallel] [--report] [cmd]\n";
const char* const errorCMD = "uqfindexec: command is invalid\n";

// Variable that may change at any time
volatile bool keepRunning = true;

void handle_sig(int sig)
{
    (void)sig; // intentionally ignore and not use sig
    keepRunning = false;
}

/** display(report)
 * Prints out statistics regarding how the files were processed
 *
 * report: A structure holding information for how the files were processed
 * i.e., successful, failed, unable etc.
 */
void display_report(ReportParams* report)
{
    fprintf(stderr, "Attempted to process a total of %d files\n",
            report->total);
    fprintf(stderr, " - operations succeeded for %d files\n",
            report->successful);
    fprintf(stderr, " - processing may have failed for %d files\n",
            report->failed);
    fprintf(stderr, " - processing terminated by signal for %d files\n",
            report->terminated);
    fprintf(stderr, " - operations unable to be executed for %d files\n",
            report->unable);
}

/** sig_handler()
 * Handles interrupt signal based on whether the processes are running in
 * parallel.
 *
 * parallel: integer to indicate whether parallel mode is active
 */
void sig_handler(int parallel)
{
    // Structure to handle signals of various types
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); // initialised to 0
    if (parallel) {
        sa.sa_handler = SIG_IGN; // ignore SIGINT in parallel
    } else {
        // Handle SIGINT in sequential by pointing to handle_sig function.
        sa.sa_handler = handle_sig;
        // Change default behaviour so most system calls are
        // restarted if interrupted by signal
        sa.sa_flags = SA_RESTART;
    }
    // Action to be done based on contents of sa struct
    sigaction(SIGINT, &sa, NULL);
}

/* usage_error()
 * Prints a usage error message and exists with appropriate code
 */
void usage_error(void)
{
    fprintf(stderr, "%s", errorUsage);
    exit(USAGE_ERROR);
}

/* process_cmd_lines ()
 * Collects the input parameters for the program and assigns them to the
 * respective variables.
 *
 * argc: parameter count including program name
 * argv: array of program parameters
 * returns: a struct containing parameters with assigned values
 *
 */
ProgParams process_cmd_line(int argc, char* argv[])
{
    ProgParams params = {defaultDir, 0, 0, 0, 0, defaultCMD, 0};
    argc--, argv++; // skip program name
    while (argc >= 1) {
        // if it's dir param then there must be a following parameter
        // parameter cannot be empty i.e., strlen > 0
        if (!strcmp(argv[0], dirArg)) {
            if (params.dir == defaultDir && argc >= 2 && strlen(argv[1]) > 0
                    && strcmp(argv[1], dirArg)) {
                params.dir = argv[1]; // assign value
                params.dirSpecified = 1;
                argc -= 2; // decrement count
                argv += 2; // increment array being traversed
            } else {
                usage_error();
            }
        } else if (!strcmp(argv[0], recurseArg)) {
            if (!params.recurse) {
                params.recurse = 1, argc--, argv++;
            } else {
                usage_error();
            }
        } else if (!strcmp(argv[0], showHiddenArg)) {
            if (!params.showHidden) {
                params.showHidden = 1, argc--, argv++;
            } else {
                usage_error();
            }
        } else if (!strcmp(argv[0], parallelArg)) {
            if (!params.parallel) {
                params.parallel = 1, argc--, argv++;
            } else {
                usage_error();
            }
        } else if (!strcmp(argv[0], reportArg)) {
            if (!params.report) {
                params.report = 1, argc--, argv++;
            } else {
                usage_error();
            }
        } else if (params.cmd == defaultCMD && strlen(argv[0]) > 0
                && strncmp(argv[0], "--", 2) != 0 && argc == 1) {
            params.cmd = argv[0];
            argc--;
            argv++;
        } else {
            usage_error();
        }
    }
    return params;
}

/* open_dictionary()
 * Checks if provided directory path can be accessed
 *
 * path: The provided path to the dictionary
 */
DIR* open_directory(const char* path)
{
    DIR* result = opendir(path);
    if (result == NULL) {
        fprintf(stderr, "uqfindexec: directory \"%s\" can not be accessed\n",
                path);
        exit(DIRECTORY_ERROR);
    }
    return result;
}

/** free_directory()
 * Frees struct containing files from directory
 *
 * processedDir: A struct containing information and contents of a directory
 */
void free_directory(DirInfo processedDir)
{
    for (int i = 0; i < processedDir.numOfEntries; i++) {
        free(processedDir.fileList[i]);
    }
    free(processedDir.fileList);
}

/** generate_path()
 * Creates a full path with the provdied directory and filename
 *
 * dir: directory path
 * filename: name of the file
 * fullPath: initialised variable to store path
 * buffer: size of the fullPath variable
 */
void generate_path(const ProgParams* paramsP, const char* filename,
        char* fullPath, int buffer)
{
    if (paramsP->dirSpecified && !paramsP->showHidden) {
        size_t dirLen = strlen(paramsP->dir);
        // If the user provides the directory ending with '/'
        if (paramsP->dir[dirLen - 1] == '/') {
            snprintf(fullPath, buffer, "%s%s", paramsP->dir, filename);
        } else {
            snprintf(fullPath, buffer, "%s/%s", paramsP->dir, filename);
        }
    } else if (paramsP->dirSpecified && paramsP->showHidden) {
        snprintf(fullPath, buffer, "%s/%s", paramsP->dir, filename);
    } else {
        snprintf(fullPath, buffer, "%s", filename);
    }
}

/* filter()
 * A custom filter function that determines whether a provided file is regular
 * or a symbolic link to a regular file.
 *
 * entry: Entry from read directory
 * paramsP: pointer to the structure containing the program parameters
 */

int filter(const struct dirent* entry, const ProgParams* paramsP)
{
    // Skip hidden files unless permitted
    if (!paramsP->showHidden && entry->d_name[0] == '.') {
        return 0;
    }

    // Check if it is a regular file
    if (entry->d_type == DT_REG) {
        return 1;
    }
    // If not, continue to check if its symbolic link to regular file

    // Construct full path to file
    char fullPath[PATH_MAX];
    generate_path(paramsP, entry->d_name, fullPath, sizeof(fullPath));

    // Obtain file status without following symbolic links
    struct stat statbuf;
    if (lstat(fullPath, &statbuf) != 0) {
        return 0; // fail to get status return 0;
    }

    // check if symbolic link
    // if it is, call stat and follow link
    // check if file is regular
    if ((S_ISLNK(statbuf.st_mode) && stat(fullPath, &statbuf) == 0
                && S_ISREG(statbuf.st_mode))) {

        return 1;
    }
    return 0;
}

/** update_report()
 * Increments paramaters in the report structure based on the flags provided
 *
 * report: A struct containing information for the processing and execution
 * of files.
 * flags: A struct containing flags that indicate the status of how
 * commands for a file executed
 */
void update_report(ReportParams* report, ReportFlags* flags)
{
    if (flags->allCmdsNormal) {
        report->successful++;
    } else if (flags->signalExit) {
        report->terminated++;

    } else if (flags->nonZeroExit) {
        report->failed++;
    } else {
        report->unable++;
    }
}

/** parallel_reap()
 * A function that performs a separate child reaping process if parallel is
 * specified
 *
 * pids: An array containing process IDs of child all processes
 * report: A structure containing information for the processing and execution
 * of files i.e., count for how many files succcessfuly executed
 */
void parallel_reap(pid_t* pids, int pidC, ReportParams* report)
{
    ReportFlags flags = {1, 0, 0, 0, 0};

    for (int i = 0; i < pidC; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) != 0) {
                flags.allCmdsNormal = 0;
                if (WEXITSTATUS(status) == EXEC_ERROR) {
                    flags.ioError = 1;
                } else {
                    flags.nonZeroExit = 1;
                }
            }
        }
        if (WIFSIGNALED(status)) {
            flags.allCmdsNormal = 0;
            flags.signalExit = 1;
        }
    }
    update_report(report, &flags);
}

/** pipeline_reap()
 * A function that performs a separate child reaping process for processes in
 * a pipeline line.
 *
 * piplinePids: An array containing the pids of all child processes in the
 * pipeline.
 * pipelinePidC: The number of child process IDs for each pipeline
 * flags: A structure to indicate the processing status of a child process.
 * report: A structure containing information regarding the processing status
 * of child processes
 */

void pipeline_reap(pid_t* pipelinePids, int pipelinePidC, ReportFlags* flags,
        ReportParams* report)
{
    for (int i = 0; i < pipelinePidC; i++) {
        int status;
        waitpid(pipelinePids[i], &status, 0);
        if (WIFEXITED(status)) { // if the child exited
            if (WEXITSTATUS(status) != 0) { // check exit status is none zero
                flags->allCmdsNormal = 0;
                if (WEXITSTATUS(status) == EXEC_ERROR) {
                    flags->ioError = 1;
                } else {
                    flags->nonZeroExit = 1;
                }
            }
        }
        // Check if the child process was termianted by a signal
        if (WIFSIGNALED(status)) {
            flags->allCmdsNormal = 0;
            flags->signalExit = 1;
        }
    }
    update_report(report, flags);
}

/** determine_redirection()
 * Performs input/output redirection if specified
 */
void determine_redirection(int currentIndex, int* fd, CommandPipeline* parsed,
        const int* previousFDP, int inFD, int outFD)
{
    // Redirect if its the first process in the pipeline and the input
    // redirection file is specified
    if (currentIndex == 0 && parsed->stdinFileName) {
        dup2(inFD, STDIN_FILENO);
        close(inFD);
        // If it's not the first command, continue using the previous fd
    } else if (currentIndex != 0) {
        dup2(*previousFDP, STDIN_FILENO);
        close(*previousFDP);
    }
    // Redirect it if it's the last command and the output redirection file is
    // specified
    if (currentIndex == parsed->numCmds - 1 && parsed->stdoutFileName) {
        dup2(outFD, STDOUT_FILENO);
        close(outFD);

        // else if its not the last, continue using the previous fd
    } else if (currentIndex < parsed->numCmds - 1) {
        dup2(fd[1], STDOUT_FILENO);
        close(fd[1]);
    }
}

/** replace_placeholder()
 * substitues '{}' with the fullpath to the file
 */
char* replace_placeholder(char* command, char* fullPath)
{
    if (!command || !fullPath) {
        return NULL; // prevent operating on null pointers
    }

    // Search for occurence of placeholder
    char* placeholder = strstr(command, "{}");
    if (!placeholder) {
        // return the command if no substitution required
        // strdup because command always freed in caller regardless of whether
        // it is dynamically allocated or not.
        return strdup(command);
    }
    // Calculate new string length used pointer arithmetic
    size_t prefixLength = placeholder - command;

    //-2 for {} + length of fullpath
    size_t newLen = strlen(command) - 2 + strlen(fullPath);

    char* result = malloc(newLen + 1); //+1 for null terminator

    // Copy the part of the command before the placeholder
    // strncpy used to specify how many characters to copy up to
    strncpy(result, command, prefixLength);

    // Insert the filepath
    strcpy(result + prefixLength, fullPath);

    // Append the part of the command after the placeholder
    strcpy(result + prefixLength + strlen(fullPath), placeholder + 2);

    return result;
}

/* verify_input()
 * Checks if the provided input file for redirection can be accessed
 */
int verify_input(CommandPipeline* parsed, ReportFlags* flags, char* fullPath,
        int currentIndex)
{
    // First check if an input file was specified and it is the first command
    if (currentIndex == 0 && parsed->stdinFileName) {
        char* inFile = parsed->stdinFileName;

        // Substitute any placeholders with the filename
        if (inFile) {
            inFile = replace_placeholder(inFile, fullPath);
        }
        int inFD = open(inFile, O_RDONLY);
        if (inFD == -1) {
            fprintf(stderr,
                    "uqfindexec: unable to open \"%s\" for reading "
                    "while "
                    "processing \"%s\"\n",
                    inFile, fullPath);
            flags->ioError = 1;
            flags->allCmdsNormal = 0;
        }
        free(inFile);
        return inFD;
    }
    return STDIN_FILENO;
}

/* verify_output()
 * Checks if the provided output file for redirection can be accessed.
 */
int verify_output(CommandPipeline* parsed, ReportFlags* flags, char* fullPath,
        int currentIndex)
{
    if (currentIndex == parsed->numCmds - 1 && parsed->stdoutFileName) {

        char* outFile = parsed->stdoutFileName;
        if (outFile) {
            outFile = replace_placeholder(outFile, fullPath);
        }
        int outFD = open(outFile, O_WRONLY | O_CREAT | O_TRUNC, READWRITE);
        if (outFD == -1) {
            fprintf(stderr,
                    "uqfindexec: cannot open \"%s\" for writing when "
                    "processing "
                    "\"%s\"\n",
                    outFile, fullPath);
            flags->ioError = 1;
            flags->allCmdsNormal = 0;
        }
        free(outFile);
        return outFD;
    }
    return STDOUT_FILENO;
}

/* free_args()
 * Frees a given dynamically allocated array
 */
void free_args(char** argv, int argC)
{
    for (int i = 0; i < argC; i++) {
        free(argv[i]);
    }
    free(argv);
}

/** execute_process()
 * Forks and executes child processes by creating pipelines and managing
 * input/output redirection.
 *
 * argv: the array of arguments for this command
 * parallel: An integer status of whether the parallel argument was specified
 * pids: A array of pids for parallel execution
 * pidC A counter for the array of pids for parallel execution
 * currentIndex: The current command index
 * fullPath: A string containing the full path to the file being processed
 * pid_t: An array of pids for pipeline execution
 * pipelinePidsC: A counter for the array of pids for pipeline execution
 */
void execute_process(char** argv, int argCount, int parallel, pid_t* pids,
        int* pidC, ReportFlags* flags, CommandPipeline* parsed, int* fd,
        int* previousFDP, int currentIndex, char* fullPath, pid_t* pipelinePids,
        int* pipelinePidsC)
{
    int inFD = verify_input(parsed, flags, fullPath, currentIndex);
    int outFD = verify_output(parsed, flags, fullPath, currentIndex);

    if (keepRunning) {
        fflush(stdout); // Flush to stdout to maintain consistency
        if (currentIndex < parsed->numCmds - 1) {
            if (pipe(fd) == -1) { // Check if pipe can be established
                perror("pipe");
                exit(EXEC_ERROR);
            }
        }
        if (!flags->ioError) { // Ensure input/output files processed correctly
            pid_t pid = fork();
            if (pid == 0) {
                determine_redirection(
                        currentIndex, fd, parsed, previousFDP, inFD, outFD);

                execvp(argv[0], argv);
                fprintf(stderr,
                        "uqfindexec: unable to execute \"%s\" when processing "
                        "\"%s\""
                        "\n",
                        argv[0], fullPath);
                flags->pipelineNotExec = 1;
                exit(EXEC_ERROR);
            } else if (pid < 0) {
                perror("fork");
                exit(EXEC_ERROR);
            } else { // parent process
                if (currentIndex != 0) {
                    close(*previousFDP);
                }
            }
            if (currentIndex < parsed->numCmds - 1) {
                // Save read end of current pipe to be input for the next
                *previousFDP = fd[0];
                close(fd[1]);
            }
            if (parallel) {
                pids[(*pidC)++] = pid;
            } else {
                pipelinePids[(*pipelinePidsC)++] = pid;
            }
            free_args(argv, argCount);
        }
    } else { // if it's not running then a interrupt signal has been triggered
        flags->signalExit = 1;
        flags->allCmdsNormal = 0;
    }
}

void execute_cmd(char* filename, const ProgParams* paramsP, pid_t* pids,
        int* pidC, ReportFlags* flags, ReportParams* report)
{
    // Parse the commmand
    CommandPipeline* parsed = parse_pipeline_string(paramsP->cmd);

    if (parsed == NULL) {
        fprintf(stderr, errorCMD);
        exit(CMD_ERROR);
    }
    int previousFD = -1; // initialising previousFD
    int fd[2]; // Create array to store FDs
    pid_t pipelinePids[BUFFER]; // Array to store PIDs for pipeline
    int pipelinePidsC = 0;

    for (int i = 0; i < parsed->numCmds; i++) {
        char** command = parsed->cmdArray[i];
        int argCount = 0;
        char fullPath[PATH_MAX];
        // Combine directory path with filename for fullpath to file
        generate_path(paramsP, filename, fullPath, sizeof(fullPath));

        // Count number of arguments for this command
        for (int c = 0; command[c] != NULL; c++) {
            argCount++;
        }

        char** argv = malloc((argCount + 1) * sizeof(char*)); //+1 for null

        for (int j = 0; j < argCount; j++) {
            argv[j] = replace_placeholder(command[j], fullPath);
        }

        argv[argCount] = NULL; // NULL terminate cmd array

        execute_process(argv, argCount, paramsP->parallel, pids, pidC, flags,
                parsed, fd, &previousFD, i, fullPath, pipelinePids,
                &pipelinePidsC);

        // reap pipeline for last command
        if (!paramsP->parallel && i == parsed->numCmds - 1) {
            pipeline_reap(pipelinePids, pipelinePidsC, flags, report);
        }
    }
    free_pipeline(parsed);
}

DirInfo read_dir(const ProgParams* paramsP, ReportParams* report)
{
    setlocale(LC_COLLATE, "en_AU"); // Set the collation local for comparsion
    pid_t pids[BUFFER]; // initialise variable to store pids for parallel
    int pidC = 0;
    DirInfo readDir = {NULL, 0}; // initialise struct to hold directory files
    readDir.numOfEntries
            = scandir(paramsP->dir, &readDir.fileList, NULL, alphasort);

    for (int i = 0; i < readDir.numOfEntries; i++) {
        // Run custom filter function to check if file is regular or
        // symbolic link to regular
        if (filter(readDir.fileList[i], paramsP)) {
            ReportFlags flags = {1, 0, 0, 0, 0};
            report->total++;
            // execute
            execute_cmd(readDir.fileList[i]->d_name, paramsP, pids, &pidC,
                    &flags, report);
        }
    }

    if (paramsP->parallel) {
        parallel_reap(pids, pidC, report);
    }
    return readDir;
}

/** determine_exit()
 * Returns the exit status to be used when the program ends based on the
 * nature of how the files executed
 *
 * report: A struct containing information regarding the processing and
 * execution of files
 * exitStatus: A pointer to an integer to store the value
 * of the exit status
 */

void determine_exit(ReportParams* report, int* exitStatus)
{
    if (report->unable > 0) {
        *exitStatus = PROCESSING_FAILED;
    } else if (report->terminated) {
        if (!keepRunning) {
            *exitStatus = SIG_INTERRUPTED;
        } else {
            *exitStatus = OK;
        }

    } else {
        *exitStatus = OK;
    }
}

int main(int argc, char* argv[])
{
    ProgParams params = process_cmd_line(argc, argv);

    sig_handler(params.parallel); // setup interrupt signal handler

    ReportParams report = {0, 0, 0, 0, 0};

    DIR* openDir = open_directory(params.dir); // Check directory is accessible
    closedir(openDir);

    // Retrieve files in directory
    DirInfo processedDir = read_dir(&params, &report);
    free_directory(processedDir);

    if (params.report) {
        display_report(&report);
    }

    int exitStatus = OK;
    determine_exit(&report, &exitStatus);
    exit(exitStatus);
}
