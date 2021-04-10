#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct buffer {
    void *remote_to_local_buffer;
    int remote_to_local_len;
    bool remote_to_local_read;
    void *local_to_remote_buffer;
    int local_to_remote_len;
    bool local_to_remote_read;
} buffer_t;

typedef struct thread_args {
    bool local;
    int conn_fd;
} thread_args_t;

int server_listen(int socket_fd, int port, bool local);
int server_accept(int socket_fd, bool local);

void *read_thread(void *args);
void *write_thread(void *args);

buffer_t *buffer;
pthread_mutex_t buffer_lock;

int main(int argc, char *argv[]) {
    int err = 0;
    int conn_fd = -1, local_conn_fd = -1;
    int remote_fd, local_fd;
    buffer = malloc(sizeof(buffer_t));

    err = (remote_fd = socket(AF_INET, SOCK_STREAM, 0));
    if (err == -1) {
        perror("socket");
        printf("Failed to create a socket endpoint, err: %s\n", strerror(err));
        return err;
    }

    err = (local_fd = socket(AF_INET, SOCK_STREAM, 0));
    if (err == -1) {
        perror("socket");
        printf("Failed to create a socket endpoint, err:%s\n", strerror(err));
        return err;
    }

    if (!strcmp(argv[1], "-s")) {
        int port = atoi(argv[2]);
        int port_local = atoi(argv[3]);

        err = server_listen(remote_fd, port, false);
        if (err) {
            printf("Failed to listen on address 0.0.0.0:%d, err: %s\n", port, strerror(err));
            return err;
        }

        while (conn_fd == -1) {
            conn_fd = server_accept(remote_fd, false);
            if (conn_fd == -1)
                printf("Failed accepting connection, err: %s\n", strerror(err));
        }

        err = server_listen(local_fd, port_local, true);
        if (err) {
            printf("Failed to listen on address 0.0.0.0:%d, err: %s\n", port_local, strerror(err));
            return err;
        }

        while (local_conn_fd == -1) {
            local_conn_fd = server_accept(local_fd, true);
            if (local_conn_fd == -1)
                printf("Failed accepting connection, err: %s\n", strerror(err));
        }
    } else if (!strcmp(argv[1], "-c")) {
        char *remote_addr = argv[2];
        int remote_port = atoi(argv[3]);
        char *local_addr = argv[4];
        int local_port = atoi(argv[5]);

        struct sockaddr_in remote_sockaddr;
        struct sockaddr_in local_sockaddr;

        memset(&remote_sockaddr, '0', sizeof(remote_sockaddr));
        memset(&local_sockaddr, '0', sizeof(local_sockaddr));

        remote_sockaddr.sin_family = AF_INET;
        local_sockaddr.sin_family = AF_INET;
        remote_sockaddr.sin_port = htons(remote_port);
        local_sockaddr.sin_port = htons(local_port);

        if (inet_pton(AF_INET, remote_addr, &remote_sockaddr.sin_addr) <= 0) {
            printf("inet_pton failed\n");
            return 1;
        }
        if (inet_pton(AF_INET, local_addr, &local_sockaddr.sin_addr) <= 0) {
            printf("inet_pton failed\n");
            return 1;
        }

        if (connect(remote_fd, (struct sockaddr *)&remote_sockaddr, sizeof(remote_sockaddr)) < 0) {
            printf("connect failed\n");
            return 1;
        }
        if (connect(local_fd, (struct sockaddr *)&local_sockaddr, sizeof(local_sockaddr)) < 0) {
            printf("connect failed\n");
            return 1;
        }

        conn_fd = remote_fd;
        local_conn_fd = local_fd;
    } else {
        printf("Usage: %s [-s remote_port local_port | -c remote_addr remote_port local_addr local_port]\n", argv[0]);
        exit(1);
    }

    err = pthread_mutex_init(&buffer_lock, NULL);
    if (err != 0) {
        printf("pthread_mutex_init failed, err: %s\n", strerror(err));
        return err;
    }

    pthread_t remote_read_thread, local_read_thread, remote_write_thread, local_write_thread;

    pthread_create(&remote_read_thread, NULL, &read_thread, (void *)(&(thread_args_t){.local = false, .conn_fd = conn_fd}));
    pthread_create(&local_read_thread, NULL, &read_thread, (void *)(&(thread_args_t){.local = true, .conn_fd = local_conn_fd}));
    pthread_create(&remote_write_thread, NULL, &write_thread, (void *)(&(thread_args_t){.local = false, .conn_fd = local_conn_fd}));
    pthread_create(&local_write_thread, NULL, &write_thread, (void *)(&(thread_args_t){.local = true, .conn_fd = conn_fd}));

    pthread_join(local_read_thread, NULL);
    if (!strcmp(argv[1], "-s")) {
        for (;;) {
            pthread_cancel(remote_write_thread);

            local_conn_fd = server_accept(local_fd, true);
            if (local_conn_fd == -1) {
                printf("Failed accepting connection, err: %s\n", strerror(err));
                continue;
            }

            pthread_create(&local_read_thread, NULL, &read_thread, (void *)(&(thread_args_t){.local = true, .conn_fd = local_conn_fd}));
            pthread_create(&remote_write_thread, NULL, &write_thread, (void *)(&(thread_args_t){.local = false, .conn_fd = local_conn_fd}));

            pthread_join(local_read_thread, NULL);
        }
    }

    pthread_mutex_destroy(&buffer_lock);
}

int server_listen(int socket_fd, int port, bool local) {
    int err = 0;

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    err = bind(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (err == -1) {
        perror("bind");
        printf("Failed to bind socket to address, err: %s\n", strerror(err));
        return err;
    }

    err = listen(socket_fd, 1);
    if (err == -1) {
        perror("listen");
        printf("Failed to put socket in passive mode, err: %s\n", strerror(err));
        return err;
    }
}

int server_accept(int socket_fd, bool local) {
    int err = 0;
    int conn_fd;
    socklen_t client_len;
    struct sockaddr_in client_addr;

    client_len = sizeof(client_addr);

    err = (conn_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &client_len));
    if (err == -1) {
        perror("accept");
        printf("Failed accepting a connection, err: %s\n", strerror(err));
        return err;
    }

    if (!local)
        printf("Remote end connected!\n");
    else
        printf("Local end connected!\n");

    return conn_fd;
}

void *read_thread(void *args) {
    thread_args_t *arguments = (thread_args_t *)args;
    char *read_buffer[1024];

    for (;;) {
        int recv_resp = recv(arguments->conn_fd, read_buffer, 1024, 0);

        if (recv_resp == 0) {
            if (!arguments->local)
                printf("Remote port connection has been closed\n");
            else
                printf("Local port connection has been closed\n");
            return NULL;
        }

        if (recv_resp == -1) {
            printf("recv failed, err: %s\n", strerror(recv_resp));
            continue;
        }

        for (;;) {
            if (!arguments->local && buffer->remote_to_local_read)
                break;
            if (arguments->local && buffer->local_to_remote_read)
                break;
            sleep(1);
        }

        pthread_mutex_lock(&buffer_lock);
        if (!arguments->local) {
            buffer->remote_to_local_buffer = read_buffer;
            buffer->remote_to_local_len = recv_resp;
            buffer->remote_to_local_read = false;
        } else {
            buffer->local_to_remote_buffer = read_buffer;
            buffer->local_to_remote_len = recv_resp;
            buffer->local_to_remote_read = false;
        }
        pthread_mutex_unlock(&buffer_lock);
    }
}

void *write_thread(void *args) {
    thread_args_t *arguments = (thread_args_t *)args;

    for (;;) {
        if (!arguments->local && buffer->remote_to_local_read) {
            sleep(1);
            continue;
        }
        if (arguments->local && buffer->local_to_remote_read) {
            sleep(1);
            continue;
        }

        int send_resp;
        int sent = 0;

        pthread_mutex_lock(&buffer_lock);
        while ((!arguments->local && sent != buffer->remote_to_local_len) || (arguments->local && sent != buffer->local_to_remote_len)) {
            if (!arguments->local)
                send_resp = send(arguments->conn_fd, buffer->remote_to_local_buffer + sent, buffer->remote_to_local_len - sent, 0);
            else
                send_resp = send(arguments->conn_fd, buffer->local_to_remote_buffer + sent, buffer->local_to_remote_len - sent, 0);

            if (send_resp == -1) {
                printf("send failed, err: %s\n", strerror(send_resp));
                continue;
            }

            sent += send_resp;
        }

        if (!arguments->local)
            buffer->remote_to_local_read = true;
        else
            buffer->local_to_remote_read = true;
        pthread_mutex_unlock(&buffer_lock);
    }
}
