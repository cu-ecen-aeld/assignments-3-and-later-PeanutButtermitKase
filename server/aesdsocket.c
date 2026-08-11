#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define RECV_BUFFER_SIZE 1024
#define FILE_BUFFER_SIZE 1024

static volatile sig_atomic_t terminate_requested = 0;
static int server_fd = -1;
static int client_fd = -1;

static void signal_handler(int signo)
{
    (void)signo;

    terminate_requested = 1;

    /*
     * Interrupt blocking socket operations.
     * The descriptors are closed later during normal cleanup.
     */
    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
    }

    if (client_fd != -1) {
        shutdown(client_fd, SHUT_RDWR);
    }
}

static int send_all(int fd, const char *buffer, size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t sent = send(fd,
                            buffer + total_sent,
                            length - total_sent,
                            0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            syslog(LOG_ERR, "send failed: %s", strerror(errno));
            return -1;
        }

        if (sent == 0) {
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}

static int send_file_to_client(int fd)
{
    int file_fd;
    char buffer[FILE_BUFFER_SIZE];

    file_fd = open(DATA_FILE, O_RDONLY);

    if (file_fd < 0) {
        syslog(LOG_ERR,
               "Unable to open %s for reading: %s",
               DATA_FILE,
               strerror(errno));
        return -1;
    }

    while (1) {
        ssize_t bytes_read = read(file_fd,
                                  buffer,
                                  sizeof(buffer));

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            syslog(LOG_ERR,
                   "read failed for %s: %s",
                   DATA_FILE,
                   strerror(errno));

            close(file_fd);
            return -1;
        }

        if (bytes_read == 0) {
            break;
        }

        if (send_all(fd,
                     buffer,
                     (size_t)bytes_read) != 0) {
            close(file_fd);
            return -1;
        }
    }

    close(file_fd);
    return 0;
}

static int append_packet_to_file(const char *data, size_t length)
{
    int fd;
    size_t total_written = 0;

    fd = open(DATA_FILE,
              O_WRONLY | O_CREAT | O_APPEND,
              0644);

    if (fd < 0) {
        syslog(LOG_ERR,
               "Unable to open %s: %s",
               DATA_FILE,
               strerror(errno));
        return -1;
    }

    while (total_written < length) {
        ssize_t written = write(fd,
                                data + total_written,
                                length - total_written);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            syslog(LOG_ERR,
                   "write failed: %s",
                   strerror(errno));

            close(fd);
            return -1;
        }

        total_written += (size_t)written;
    }

    close(fd);
    return 0;
}

static int daemonize_process(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        syslog(LOG_ERR,
               "fork failed: %s",
               strerror(errno));
        return -1;
    }

    if (pid > 0) {
        /*
         * Parent exits successfully.
         */
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        syslog(LOG_ERR,
               "setsid failed: %s",
               strerror(errno));
        return -1;
    }

    if (chdir("/") != 0) {
        syslog(LOG_ERR,
               "chdir failed: %s",
               strerror(errno));
        return -1;
    }

    int null_fd = open("/dev/null", O_RDWR);

    if (null_fd >= 0) {
        if (dup2(null_fd, STDIN_FILENO) < 0) {
            syslog(LOG_ERR, "dup2 stdin failed");
        }

        if (dup2(null_fd, STDOUT_FILENO) < 0) {
            syslog(LOG_ERR, "dup2 stdout failed");
        }

        if (dup2(null_fd, STDERR_FILENO) < 0) {
            syslog(LOG_ERR, "dup2 stderr failed");
        }

        if (null_fd > STDERR_FILENO) {
            close(null_fd);
        }
    }

    return 0;
}

static int handle_client(int fd)
{
    char recv_buffer[RECV_BUFFER_SIZE];
    char *packet = NULL;
    size_t packet_length = 0;

    while (!terminate_requested) {

        ssize_t bytes_received =
            recv(fd, recv_buffer, sizeof(recv_buffer), 0);

        if (bytes_received < 0) {

            if (errno == EINTR) {
                continue;
            }

            syslog(LOG_ERR,
                   "recv failed: %s",
                   strerror(errno));

            free(packet);
            return -1;
        }

        if (bytes_received == 0) {
            break;
        }

        size_t new_length =
            packet_length + (size_t)bytes_received;

        char *new_packet =
            realloc(packet, new_length);

        if (new_packet == NULL) {

            syslog(LOG_ERR,
                   "realloc failed while receiving packet");

            free(packet);
            return -1;
        }

        packet = new_packet;

        memcpy(packet + packet_length,
               recv_buffer,
               (size_t)bytes_received);

        packet_length = new_length;

        /*
         * A packet is complete when a newline is received.
         */
        char *newline =
            memchr(packet, '\n', packet_length);

        if (newline != NULL) {

            size_t complete_length =
                (size_t)(newline - packet) + 1;

            if (append_packet_to_file(packet,
                                      complete_length) != 0) {
                free(packet);
                return -1;
            }

            /*
             * Return the entire accumulated data file.
             */
            if (send_file_to_client(fd) != 0) {
                free(packet);
                return -1;
            }

            /*
             * One newline-terminated packet per client connection.
             * Return so main() closes the client socket and accepts
             * the next connection.
             */
            free(packet);
            return 0;
        }
    }

    free(packet);
    return 0;
}

int main(int argc, char *argv[])
{
    struct addrinfo hints;
    struct addrinfo *server_info = NULL;
    struct addrinfo *entry;

    bool daemon_mode = false;

    int result;
    int yes = 1;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    } else if (argc != 1) {
        fprintf(stderr,
                "Usage: %s [-d]\n",
                argv[0]);
        return -1;
    }

    openlog("aesdsocket",
            LOG_PID,
            LOG_USER);

    /*
     * Install SIGINT and SIGTERM handlers.
     */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    result = getaddrinfo(NULL,
                         PORT,
                         &hints,
                         &server_info);

    if (result != 0) {
        syslog(LOG_ERR,
               "getaddrinfo failed: %s",
               gai_strerror(result));

        closelog();
        return -1;
    }

    /*
     * Try available addresses until bind succeeds.
     */
    for (entry = server_info;
         entry != NULL;
         entry = entry->ai_next) {

        server_fd = socket(entry->ai_family,
                           entry->ai_socktype,
                           entry->ai_protocol);

        if (server_fd < 0) {
            continue;
        }

        if (setsockopt(server_fd,
                       SOL_SOCKET,
                       SO_REUSEADDR,
                       &yes,
                       sizeof(yes)) != 0) {
            syslog(LOG_ERR,
                   "setsockopt failed: %s",
                   strerror(errno));

            close(server_fd);
            server_fd = -1;
            continue;
        }

        if (bind(server_fd,
                 entry->ai_addr,
                 entry->ai_addrlen) == 0) {
            break;
        }

        close(server_fd);
        server_fd = -1;
    }

    freeaddrinfo(server_info);

    if (server_fd < 0) {
        syslog(LOG_ERR,
               "Could not bind to port %s",
               PORT);

        closelog();
        return -1;
    }

    /*
     * Requirement:
     * fork only after successfully binding to port 9000.
     */
    if (daemon_mode) {
        if (daemonize_process() != 0) {
            close(server_fd);
            server_fd = -1;

            closelog();
            return -1;
        }
    }

    if (listen(server_fd, 10) != 0) {
        syslog(LOG_ERR,
               "listen failed: %s",
               strerror(errno));

        close(server_fd);
        server_fd = -1;

        closelog();
        return -1;
    }

    while (!terminate_requested) {
        struct sockaddr_storage client_addr;
        socklen_t client_addr_len =
            sizeof(client_addr);

        char client_ip[INET6_ADDRSTRLEN];

        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_addr,
                           &client_addr_len);

        if (client_fd < 0) {
            if (terminate_requested) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            syslog(LOG_ERR,
                   "accept failed: %s",
                   strerror(errno));

            continue;
        }

        void *addr_ptr = NULL;

        if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in *ipv4 =
                (struct sockaddr_in *)&client_addr;

            addr_ptr = &ipv4->sin_addr;
        } else if (client_addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *ipv6 =
                (struct sockaddr_in6 *)&client_addr;

            addr_ptr = &ipv6->sin6_addr;
        }

        if (addr_ptr != NULL) {
            if (inet_ntop(client_addr.ss_family,
                          addr_ptr,
                          client_ip,
                          sizeof(client_ip)) == NULL) {
                strcpy(client_ip, "unknown");
            }
        } else {
            strcpy(client_ip, "unknown");
        }

        syslog(LOG_INFO,
               "Accepted connection from %s",
               client_ip);

        handle_client(client_fd);

        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        client_fd = -1;

        syslog(LOG_INFO,
               "Closed connection from %s",
               client_ip);
    }

    /*
     * Graceful shutdown.
     */
    if (client_fd != -1) {
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        client_fd = -1;
    }

    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
        server_fd = -1;
    }

    unlink(DATA_FILE);

    syslog(LOG_INFO,
           "Caught signal, exiting");

    closelog();

    return 0;
}