#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

#define PORT "9000"
#define BUFFER_SIZE 1024
#define TMP_FILE "/var/tmp/aesdsocketdata"

volatile sig_atomic_t keep_running = 1;

void signal_handler(int signal)
{
    keep_running = 0;
}

void *receive_write_echo(void *thread);
pthread_mutex_t file_mutex;
void *timestamp_log();

struct thread_info_s 
{
    pthread_t thread_id;
    int connected_fd;
    char client_ip[INET6_ADDRSTRLEN];
    int is_complete;
};

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
    {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

struct Node
{
    struct thread_info_s *thread_info;
    struct Node *prev;
    struct Node *next;
};

struct List
{
    int length;
    struct Node *head;
    struct Node *tail;
};

struct Node *node_init(struct thread_info_s *thread_info)
{
    struct Node *node;

    if ((node = malloc(sizeof(struct Node))) == NULL)
        return NULL;

    node->thread_info = thread_info;
    node->prev = NULL;
    node->next = NULL;

    return node;
}

struct List *list_init(struct Node *init_node)
{
    struct List *list;

    if ((list = malloc(sizeof(struct List))) == NULL)
        return NULL;
    
    list->head = init_node;
    list->tail = init_node;
    list->length = 1;
    
    return list;
}

void list_append(struct List *list, struct Node *node)
{
    node->prev = list->tail;
    node->next = NULL;

    list->tail->next = node;
    list->tail = node;
}

int main(int argc, char *argv[])
{
    openlog(argv[0], LOG_PID, LOG_USER);

    int run_daemon = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d")) != -1)
    {
        switch (opt) {
            case 'd':
                run_daemon = 1;
                syslog(LOG_INFO, "Running in daemon mode\n");
                break;
            default:
                fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
                return -1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int mutex_return = pthread_mutex_init(&file_mutex, NULL);
    if (mutex_return != 0) return -1;

    struct addrinfo hints;
    struct addrinfo *res, *rp;
    struct sockaddr_storage client_addr;
    int status;
    int yes = 1;
    int sockfd, connectfd;
    socklen_t addr_size;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((status = getaddrinfo(NULL, PORT, &hints, &res)) != 0)
    {
        syslog(LOG_ERR, "Error: Could not get addr info with status %s. Exiting\n", gai_strerror(status));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, 0);

        if (sockfd < 0)
            continue;
        
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(sockfd);
    }
    
    if (rp == NULL)
    {
        syslog(LOG_ERR, "Could not bind: %s\n", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    
    if (listen(sockfd, 5) == -1)
    {
        syslog(LOG_ERR, "Could not listen: %s\n", strerror(errno));
        return -1;
    }
    
    // fork here for daemon
    if (run_daemon)
    {
        pid_t pid = fork();
        if (pid < 0) { exit(EXIT_FAILURE); }
        if (pid > 0) { exit(EXIT_SUCCESS); }

        if (setsid() < 0) {exit(EXIT_FAILURE); }

        pid = fork();
        if (pid < 0) { exit(EXIT_FAILURE); }
        if (pid > 0) { exit(EXIT_SUCCESS); }

        umask(0);
        chdir("/");

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);

        if (devnull > 2) { close(devnull); }
    }

    pthread_t timestamp_thread;
    pthread_create(&timestamp_thread, NULL, &timestamp_log, NULL);
    
    struct List *thread_list;
    struct Node *head_dummy;

    head_dummy = node_init(NULL);
    thread_list = list_init(head_dummy);

    while(keep_running)
    {
        addr_size = sizeof(client_addr);
        connectfd = accept(sockfd, (struct sockaddr *)&client_addr, &addr_size);

        if (connectfd == -1)
        {
            if (errno == EINTR && !keep_running) { break; }

            syslog(LOG_ERR, "Could not accept connection: %s\n", strerror(errno));
            continue;
        }

        struct thread_info_s *accepted = malloc(sizeof(struct thread_info_s));
        accepted->connected_fd = connectfd;
        inet_ntop(client_addr.ss_family, get_in_addr((struct sockaddr *)&client_addr), accepted->client_ip, sizeof(accepted->client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", accepted->client_ip);
        accepted->is_complete = 0;

        pthread_create(&accepted->thread_id, NULL, &receive_write_echo, accepted);
        list_append(thread_list, node_init(accepted));    
        
        struct Node *temp = thread_list->head->next;
        while (temp != thread_list->tail)
        {
            if (temp->thread_info->is_complete)
            {
                struct Node *to_delete = temp;
                temp = temp->next;

                if (to_delete->prev)
                {
                    to_delete->prev->next = to_delete->next;
                }
                if (to_delete->next)
                {
                    to_delete->next->prev = to_delete->prev;
                }

                shutdown(to_delete->thread_info->connected_fd, SHUT_RDWR);
                pthread_join(to_delete->thread_info->thread_id, NULL);
                free(to_delete->thread_info);
                free(to_delete);
            } else {
                temp = temp->next;
            }
        }

        // temp is thread_list tail
        if (temp->thread_info->is_complete)
        {
            temp->prev->next = NULL;
            thread_list->tail = temp->prev;
            shutdown(temp->thread_info->connected_fd, SHUT_RDWR);
            pthread_join(temp->thread_info->thread_id, NULL);
            free(temp->thread_info);
            free(temp);
        }
   }

    // final clean up loop
    struct Node *temp = thread_list->head->next;
    while (temp != NULL)
    {
        struct Node *to_delete = temp;
        temp = temp->next;
    
        shutdown(to_delete->thread_info->connected_fd, SHUT_RDWR);
        pthread_join(to_delete->thread_info->thread_id, NULL);
    
        free(to_delete->thread_info);
        free(to_delete);
    }
    
    pthread_join(timestamp_thread, NULL);

    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    remove(TMP_FILE);
    pthread_mutex_destroy(&file_mutex);
    
    free(head_dummy);
    free(thread_list);

    return 0;
}

void *receive_write_echo(void *thread_s)
{
    struct thread_info_s *thread_info = (struct thread_info_s *) thread_s;
    char *buffer = malloc(BUFFER_SIZE);
    memset(buffer, 0, BUFFER_SIZE);

    ssize_t bytes_received;
    int packet_complete = 0;
    
    int fd = open(TMP_FILE, O_RDWR | O_CREAT | O_APPEND, 0664); 

    while (!packet_complete && (bytes_received = recv(thread_info->connected_fd, buffer, BUFFER_SIZE, 0)) > 0)
    {
        char *newline_at = memchr(buffer, '\n', bytes_received);

        if (newline_at != NULL)
        {
            size_t to_write = newline_at - buffer + 1;
            pthread_mutex_lock(&file_mutex);
            write(fd, buffer, to_write);
            pthread_mutex_unlock(&file_mutex);
            packet_complete = 1;
        } else {
            pthread_mutex_lock(&file_mutex);
            write(fd, buffer, bytes_received);
            pthread_mutex_unlock(&file_mutex);
        } 
    }

    if (bytes_received == -1)
        syslog(LOG_ERR, "Could not receive bytes: %s\n", strerror(errno));
    
    close(fd);
    memset(buffer, 0, BUFFER_SIZE);

    pthread_mutex_lock(&file_mutex);
    fd = open(TMP_FILE, O_RDONLY);

    while ((bytes_received = read(fd, buffer, BUFFER_SIZE)) > 0)
    {
        send(thread_info->connected_fd, buffer, bytes_received, 0);
    }
    
    close(fd);
    pthread_mutex_unlock(&file_mutex);
    
    free(buffer);
    shutdown(thread_info->connected_fd, SHUT_RDWR);
    close(thread_info->connected_fd);
    syslog(LOG_INFO, "Closed connection from %s\n", thread_info->client_ip);
    thread_info->is_complete = 1;
    
    return NULL;
}

void *timestamp_log()
{
    while (keep_running) 
    {
        sleep(10);

        time_t current_time;
        char timestamp[256];

        time(&current_time);
        
        struct tm *tm = localtime(&current_time);
        strftime(timestamp, 256, "timestamp:%x@%H:%M:%S\n", tm);
        
        pthread_mutex_lock(&file_mutex);
        int fd = open(TMP_FILE, O_RDWR | O_CREAT | O_APPEND, 0664);
        write(fd, timestamp, 256);
        close(fd);
        pthread_mutex_unlock(&file_mutex);

   }

    return NULL;
}
