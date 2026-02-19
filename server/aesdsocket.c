#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>

#define PORT "9000"
#define BUFFER_SIZE 1024
#define TMP_FILE "/var/tmp/aesdsocketdata"

volatile sig_atomic_t keep_running = 1;

void signal_handler(int signal)
{
    keep_running = 0;
}

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
    {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
    openlog(argv[0], LOG_PID, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT | SIGTERM, &sa, NULL);

    char *buffer;
    int fd;

    struct addrinfo hints;
    struct addrinfo *res, *rp;
    struct sockaddr_storage client_addr;
    int status;
    int yes=1;
    int sockfd, connectfd;
    socklen_t addr_size;
    char client_ip[INET6_ADDRSTRLEN];

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

        if (sockfd < 0) // == -1
            continue;
        
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(sockfd);  // close unsuccessful socket or bind; try next address
    }
    
    if (rp == NULL)
    {
        syslog(LOG_ERR, "Could not bind: %s\n", strerror(errno));
        return -1;
    }

    freeaddrinfo(res);
    
    if (listen(sockfd, 5) == -1)
    {
        syslog(LOG_ERR, "Could not listen: %s\n", strerror(errno));
        return -1;
    }


    while(keep_running)
    {
        addr_size = sizeof(client_addr);
        connectfd = accept(sockfd, (struct sockaddr *)&client_addr, &addr_size);

        if (connectfd == -1)
        {
            syslog(LOG_ERR, "Could not accept connection: %s\n", strerror(errno));
            continue;
        }

        inet_ntop(client_addr.ss_family, get_in_addr((struct sockaddr *)&client_addr), client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // do stuff
        buffer = malloc(BUFFER_SIZE);
        memset(buffer, 0, BUFFER_SIZE);

        fd = open(TMP_FILE, O_RDWR | O_CREAT | O_APPEND, 0664);
        if (fd == -1)
        {
            syslog(LOG_ERR, "Could not open or create file: %s\n", strerror(errno));
            return -1;
        } else { syslog(LOG_INFO, "Opened or created file %s\n", TMP_FILE); }

        ssize_t bytes_received;
        while ((bytes_received = recv(connectfd, buffer, BUFFER_SIZE, MSG_DONTWAIT)) > 0)
        {
            char *newline_at = memchr(buffer, '\n', BUFFER_SIZE);
            write(fd, buffer, (newline_at - buffer + 1));
        }

        if (bytes_received == -1)
        {
            syslog(LOG_ERR, "Could not receive bytes: %s\n", strerror(errno));
            return -1;
        }
        
        close(fd);
        memset(buffer, 0, BUFFER_SIZE);

        FILE *readfile = fopen(TMP_FILE, "r");
        if (!fgets(buffer, BUFFER_SIZE, readfile))
        {
            syslog(LOG_ERR, "Could not read from file: %s\n", strerror(errno));
            return -1;
        }
        
        //char *nullterm_at = memchr(buffer, '\0', BUFFER_SIZE);
        char *msg = "Test from server";
        int len = strlen(msg);
        send(sockfd, msg, len, MSG_DONTWAIT);
        //send(connectfd, buffer, (nullterm_at - buffer), 0);
        
        fclose(readfile);
        free(buffer);
        close(connectfd);
        syslog(LOG_INFO, "Closed connection from %s\n", client_ip);
    }

    close(fd);
    close(sockfd);
}
