#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define RECV_BUFFER_SIZE 1024
#define FILE_BUFFER_SIZE 1024
#define TIMESTAMP_INTERVAL_SECONDS 10


/*
 * Structure used to track each client worker thread.
 *
 * The main thread owns the linked list itself.
 * Worker threads only update their own completion state.
 */
struct worker_node {
    pthread_t thread_id;
    int client_fd;
    char client_ip[INET6_ADDRSTRLEN];
    bool complete;

    struct worker_node *next;
};


/*
 * Global termination flag.
 *
 * sig_atomic_t is appropriate because this variable is modified
 * by the signal handler.
 */
static volatile sig_atomic_t terminate_requested = 0;


/*
 * Listening socket.
 *
 * The signal handler shuts this socket down so a blocking accept()
 * wakes up when SIGINT or SIGTERM is received.
 */
static int server_fd = -1;


/*
 * Head of our singly linked list containing worker threads.
 *
 * Only the main thread modifies this linked list.
 */
static struct worker_node *worker_head = NULL;


/*
 * Protects access to /var/tmp/aesdsocketdata.
 *
 * Both client worker threads and the timestamp thread must use
 * this mutex before modifying the file.
 */
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;


/*
 * Protects state shared between workers and the main thread,
 * particularly:
 *
 *     complete
 *     client_fd
 */
static pthread_mutex_t worker_state_mutex =
    PTHREAD_MUTEX_INITIALIZER;


/*
 * Used to wake the timestamp thread immediately during shutdown.
 */
static pthread_mutex_t timer_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t timer_condition =
    PTHREAD_COND_INITIALIZER;


/*
 * Signal handler for SIGINT and SIGTERM.
 */
static void signal_handler(int signo)
{
    (void)signo;

    terminate_requested = 1;

    /*
     * Interrupt blocking accept().
     *
     * Descriptor cleanup is performed later by main().
     */
    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
    }
}


/*
 * Send all bytes in a buffer.
 *
 * send() is not guaranteed to transmit the complete buffer in
 * a single call, therefore continue until everything is sent.
 */
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

            syslog(LOG_ERR,
                   "send failed: %s",
                   strerror(errno));

            return -1;
        }

        if (sent == 0) {
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}


/*
 * Send the complete contents of DATA_FILE to one client.
 *
 * A fixed-size buffer is used instead of reading the entire file
 * into RAM.
 */
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


/*
 * Append exactly 'length' bytes to the socket data file.
 *
 * The caller is responsible for locking file_mutex.
 */
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

        ssize_t written =
            write(fd,
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


/*
 * Append the Assignment 6 timestamp.
 *
 * Example:
 *
 * timestamp:Thu, 20 Aug 2026 12:30:42 +0200
 */
static int append_timestamp(void)
{
    char timestamp[128];
    time_t current_time;
    struct tm local_time;

    current_time = time(NULL);

    if (current_time == (time_t)-1) {
        syslog(LOG_ERR, "time() failed");
        return -1;
    }

    if (localtime_r(&current_time, &local_time) == NULL) {
        syslog(LOG_ERR, "localtime_r() failed");
        return -1;
    }

    size_t length =
        strftime(timestamp,
                 sizeof(timestamp),
                 "timestamp:%a, %d %b %Y %T %z\n",
                 &local_time);

    if (length == 0) {
        syslog(LOG_ERR, "strftime() failed");
        return -1;
    }

    pthread_mutex_lock(&file_mutex);

    int result =
        append_packet_to_file(timestamp, length);

    pthread_mutex_unlock(&file_mutex);

    return result;
}


/*
 * Dedicated timestamp thread.
 *
 * Every 10 seconds it appends a timestamp to DATA_FILE.
 *
 * A condition variable is used instead of simply sleep(10), which
 * allows the main thread to wake this thread immediately during
 * shutdown.
 */
static void *timestamp_thread_function(void *arg)
{
    (void)arg;

    while (!terminate_requested) {

        struct timespec wake_time;

        if (clock_gettime(CLOCK_REALTIME,
                          &wake_time) != 0) {

            syslog(LOG_ERR,
                   "clock_gettime failed: %s",
                   strerror(errno));

            break;
        }

        wake_time.tv_sec += TIMESTAMP_INTERVAL_SECONDS;

        pthread_mutex_lock(&timer_mutex);

        int wait_result = 0;

        while (!terminate_requested &&
               wait_result != ETIMEDOUT) {

            wait_result =
                pthread_cond_timedwait(&timer_condition,
                                       &timer_mutex,
                                       &wake_time);
        }

        pthread_mutex_unlock(&timer_mutex);

        if (terminate_requested) {
            break;
        }

        if (wait_result == ETIMEDOUT) {

            if (append_timestamp() != 0) {
                syslog(LOG_ERR,
                       "Unable to append timestamp");
            }
        }
    }

    return NULL;
}


/*
 * Handle one connected socket.
 *
 * This function executes inside a worker thread.
 */
static int handle_client(int fd)
{
    char recv_buffer[RECV_BUFFER_SIZE];
    char *packet = NULL;
    size_t packet_length = 0;

    while (!terminate_requested) {

        ssize_t bytes_received =
            recv(fd,
                 recv_buffer,
                 sizeof(recv_buffer),
                 0);

        if (bytes_received < 0) {

            if (errno == EINTR) {
                continue;
            }

            /*
             * During shutdown the socket is intentionally interrupted.
             */
            if (!terminate_requested) {
                syslog(LOG_ERR,
                       "recv failed: %s",
                       strerror(errno));
            }

            free(packet);
            return -1;
        }

        /*
         * Client closed connection.
         */
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
         * A packet is complete once '\n' has been received.
         */
        char *newline =
            memchr(packet,
                   '\n',
                   packet_length);

        if (newline != NULL) {

            size_t complete_length =
                (size_t)(newline - packet) + 1;

            /*
             * Assignment 6:
             *
             * The write AND file transmission are protected by
             * the same mutex.
             *
             * This prevents another connection or timestamp from
             * modifying the file between our write and our response.
             */
            pthread_mutex_lock(&file_mutex);

            if (append_packet_to_file(packet,
                                      complete_length) != 0) {

                pthread_mutex_unlock(&file_mutex);

                free(packet);
                return -1;
            }

            if (send_file_to_client(fd) != 0) {

                pthread_mutex_unlock(&file_mutex);

                free(packet);
                return -1;
            }

            pthread_mutex_unlock(&file_mutex);

            free(packet);

            return 0;
        }
    }

    free(packet);

    return 0;
}


/*
 * Worker thread entry point.
 */
static void *client_thread_function(void *arg)
{
    struct worker_node *worker =
        (struct worker_node *)arg;

    int fd = worker->client_fd;

    handle_client(fd);

    /*
     * Protect socket state while closing.
     */
    pthread_mutex_lock(&worker_state_mutex);

    if (worker->client_fd != -1) {

        shutdown(worker->client_fd, SHUT_RDWR);
        close(worker->client_fd);

        worker->client_fd = -1;
    }

    worker->complete = true;

    pthread_mutex_unlock(&worker_state_mutex);

    syslog(LOG_INFO,
           "Closed connection from %s",
           worker->client_ip);

    return NULL;
}


/*
 * Join and free worker threads which have completed.
 *
 * The linked list itself is only manipulated by main().
 */
static void reap_completed_workers(void)
{
    struct worker_node **current =
        &worker_head;

    while (*current != NULL) {

        struct worker_node *worker =
            *current;

        pthread_mutex_lock(&worker_state_mutex);

        bool complete =
            worker->complete;

        pthread_mutex_unlock(&worker_state_mutex);

        if (complete) {

            /*
             * Remove from list first.
             */
            *current =
                worker->next;

            /*
             * Assignment requirement:
             * do not use detached threads.
             */
            pthread_join(worker->thread_id,
                         NULL);

            free(worker);

        } else {

            current =
                &worker->next;
        }
    }
}


/*
 * Ask all still-running client threads to leave recv()/send().
 */
static void shutdown_active_workers(void)
{
    struct worker_node *worker =
        worker_head;

    pthread_mutex_lock(&worker_state_mutex);

    while (worker != NULL) {

        if (worker->client_fd != -1) {

            shutdown(worker->client_fd,
                     SHUT_RDWR);
        }

        worker =
            worker->next;
    }

    pthread_mutex_unlock(&worker_state_mutex);
}


/*
 * Join and release every remaining worker.
 *
 * Used during program shutdown.
 */
static void join_all_workers(void)
{
    while (worker_head != NULL) {

        struct worker_node *worker =
            worker_head;

        worker_head =
            worker->next;

        pthread_join(worker->thread_id,
                     NULL);

        free(worker);
    }
}


/*
 * Convert the process to daemon mode.
 *
 * This is intentionally performed before creating any threads.
 */
static int daemonize_process(void)
{
    pid_t pid =
        fork();

    if (pid < 0) {

        syslog(LOG_ERR,
               "fork failed: %s",
               strerror(errno));

        return -1;
    }

    if (pid > 0) {

        /*
         * Parent exits.
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

    int null_fd =
        open("/dev/null",
             O_RDWR);

    if (null_fd >= 0) {

        if (dup2(null_fd, STDIN_FILENO) < 0) {
            syslog(LOG_ERR,
                   "dup2 stdin failed");
        }

        if (dup2(null_fd, STDOUT_FILENO) < 0) {
            syslog(LOG_ERR,
                   "dup2 stdout failed");
        }

        if (dup2(null_fd, STDERR_FILENO) < 0) {
            syslog(LOG_ERR,
                   "dup2 stderr failed");
        }

        if (null_fd > STDERR_FILENO) {
            close(null_fd);
        }
    }

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

    pthread_t timestamp_thread;
    bool timestamp_thread_started = false;


    /*
     * Parse command line.
     */
    if (argc == 2 &&
        strcmp(argv[1], "-d") == 0) {

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
     * Remove stale data from an abnormal previous run.
     */
    unlink(DATA_FILE);


    /*
     * SIGPIPE should not terminate the complete server if one
     * client disconnects while data is being sent.
     */
    signal(SIGPIPE, SIG_IGN);


    /*
     * Install graceful shutdown handlers.
     */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);


    memset(&hints,
           0,
           sizeof(hints));

    hints.ai_family =
        AF_UNSPEC;

    hints.ai_socktype =
        SOCK_STREAM;

    hints.ai_flags =
        AI_PASSIVE;


    result =
        getaddrinfo(NULL,
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
     * Try each available address until bind succeeds.
     */
    for (entry = server_info;
         entry != NULL;
         entry = entry->ai_next) {

        server_fd =
            socket(entry->ai_family,
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
     * Assignment 5 requirement:
     *
     * fork only after successfully binding.
     *
     * Assignment 6 consideration:
     *
     * Fork BEFORE starting any pthreads.
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


    /*
     * Start the timestamp thread only after daemonization.
     */
    result =
        pthread_create(&timestamp_thread,
                       NULL,
                       timestamp_thread_function,
                       NULL);

    if (result != 0) {

        syslog(LOG_ERR,
               "pthread_create timestamp failed: %s",
               strerror(result));

        close(server_fd);

        server_fd = -1;

        unlink(DATA_FILE);

        closelog();

        return -1;
    }

    timestamp_thread_started = true;


    /*
     * Main accept loop.
     */
    while (!terminate_requested) {

        struct sockaddr_storage client_addr;

        socklen_t client_addr_len =
            sizeof(client_addr);

        char client_ip[INET6_ADDRSTRLEN];

        int accepted_fd =
            accept(server_fd,
                   (struct sockaddr *)&client_addr,
                   &client_addr_len);


        if (accepted_fd < 0) {

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


        void *addr_ptr =
            NULL;


        if (client_addr.ss_family == AF_INET) {

            struct sockaddr_in *ipv4 =
                (struct sockaddr_in *)&client_addr;

            addr_ptr =
                &ipv4->sin_addr;

        } else if (client_addr.ss_family == AF_INET6) {

            struct sockaddr_in6 *ipv6 =
                (struct sockaddr_in6 *)&client_addr;

            addr_ptr =
                &ipv6->sin6_addr;
        }


        if (addr_ptr != NULL) {

            if (inet_ntop(client_addr.ss_family,
                          addr_ptr,
                          client_ip,
                          sizeof(client_ip)) == NULL) {

                strcpy(client_ip,
                       "unknown");
            }

        } else {

            strcpy(client_ip,
                   "unknown");
        }


        syslog(LOG_INFO,
               "Accepted connection from %s",
               client_ip);


        /*
         * Allocate tracking data for this connection.
         */
        struct worker_node *worker =
            calloc(1,
                   sizeof(*worker));

        if (worker == NULL) {

            syslog(LOG_ERR,
                   "Unable to allocate worker");

            shutdown(accepted_fd,
                     SHUT_RDWR);

            close(accepted_fd);

            continue;
        }


        worker->client_fd =
            accepted_fd;

        worker->complete =
            false;

        worker->next =
            NULL;

        strncpy(worker->client_ip,
                client_ip,
                sizeof(worker->client_ip) - 1);

        worker->client_ip[
            sizeof(worker->client_ip) - 1
        ] = '\0';


        /*
         * Create one thread for this connection.
         */
        result =
            pthread_create(&worker->thread_id,
                           NULL,
                           client_thread_function,
                           worker);


        if (result != 0) {

            syslog(LOG_ERR,
                   "pthread_create client failed: %s",
                   strerror(result));

            shutdown(accepted_fd,
                     SHUT_RDWR);

            close(accepted_fd);

            free(worker);

            continue;
        }


        /*
         * Add worker to our singly linked list.
         *
         * The list itself is touched only by main().
         */
        worker->next =
            worker_head;

        worker_head =
            worker;


        /*
         * Clean up any threads which completed previously.
         *
         * This follows the assignment recommendation:
         * keep accept() blocking and reap completed threads after
         * starting the next worker.
         */
        reap_completed_workers();
    }


    /*
     * ---------------------------
     * Graceful shutdown sequence
     * ---------------------------
     */

    if (terminate_requested) {

        syslog(LOG_INFO,
               "Caught signal, exiting");
    }


    /*
     * Stop timestamp thread immediately instead of waiting
     * for its next 10-second timeout.
     */
    pthread_mutex_lock(&timer_mutex);

    pthread_cond_broadcast(&timer_condition);

    pthread_mutex_unlock(&timer_mutex);


    /*
     * Request termination of any worker blocked in recv()/send().
     */
    shutdown_active_workers();


    /*
     * Wait for every worker to terminate.
     */
    join_all_workers();


    /*
     * Wait for timestamp worker.
     */
    if (timestamp_thread_started) {

        pthread_join(timestamp_thread,
                     NULL);
    }


    /*
     * Close server socket.
     */
    if (server_fd != -1) {

        shutdown(server_fd,
                 SHUT_RDWR);

        close(server_fd);

        server_fd = -1;
    }


    /*
     * Required Assignment 5/6 cleanup.
     */
    unlink(DATA_FILE);


    pthread_mutex_destroy(&file_mutex);

    pthread_mutex_destroy(&worker_state_mutex);

    pthread_mutex_destroy(&timer_mutex);

    pthread_cond_destroy(&timer_condition);


    closelog();

    return 0;
}