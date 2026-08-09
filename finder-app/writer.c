#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

int main(int argc, char *argv[])
{
    const char *writefile;
    const char *writestr;
    FILE *fp;

    openlog("writer", 0, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Invalid number of arguments: %d", argc);
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        closelog();
        return 1;
    }

    writefile = argv[1];
    writestr = argv[2];

    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    fp = fopen(writefile, "w");
    if (fp == NULL) {
        syslog(LOG_ERR, "Error opening file %s", writefile);
        closelog();
        return 1;
    }

    if (fprintf(fp, "%s", writestr) < 0) {
        syslog(LOG_ERR, "Error writing to file %s", writefile);
        fclose(fp);
        closelog();
        return 1;
    }

    if (fclose(fp) != 0) {
        syslog(LOG_ERR, "Error closing file %s", writefile);
        closelog();
        return 1;
    }

    closelog();
    return 0;
}