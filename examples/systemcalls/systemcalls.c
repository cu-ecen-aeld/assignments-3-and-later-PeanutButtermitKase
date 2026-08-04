#include "systemcalls.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{

    if (cmd == NULL) {
        return false;
    }

    int status = system(cmd);

    if (status == -1) {
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    // return true;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    if (count < 1) {
        return false;
    }
    char *command[count + 1];
    va_list args;
    va_start(args, count);
    for (int index = 0; index < count; index++) {
        command[index] = va_arg(args, char *);
    }
    va_end(args);
    command[count] = NULL;
    if (command[0] == NULL) {
        return false;
    }
    fflush(NULL);
    pid_t child_pid = fork();
    if (child_pid == -1) {
        return false;
    }
    if (child_pid == 0) {
        execv(command[0], command);
        _exit(EXIT_FAILURE);
    }
    int status;
    pid_t wait_result;
    do {
        wait_result = waitpid(child_pid, &status, 0);
    } while (wait_result == -1 && errno == EINTR);
    if (wait_result == -1) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    if (outputfile == NULL || count < 1) {
        return false;
    }
    char *command[count + 1];
    va_list args;
    va_start(args, count);
    for (int index = 0; index < count; index++) {
        command[index] = va_arg(args, char *);
    }
    va_end(args);
    command[count] = NULL;
    if (command[0] == NULL) {
        return false;
    }
    fflush(NULL);
    pid_t child_pid = fork();
    if (child_pid == -1) {
        return false;
    }
    if (child_pid == 0) {
        int output_fd = open(
            outputfile,
            O_WRONLY | O_CREAT | O_TRUNC,
            0644
        );
        if (output_fd == -1) {
            _exit(EXIT_FAILURE);
        }
        if (dup2(output_fd, STDOUT_FILENO) == -1) {
            close(output_fd);
            _exit(EXIT_FAILURE);
        }
        close(output_fd);
        execv(command[0], command);
        _exit(EXIT_FAILURE);
    }
    int status;
    pid_t wait_result;
    do {
        wait_result = waitpid(child_pid, &status, 0);
    } while (wait_result == -1 && errno == EINTR);

    if (wait_result == -1) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
