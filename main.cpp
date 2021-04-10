#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <mutex>
#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#else
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")
#pragma warning(disable:4996)
#endif

typedef struct buffer {
    void* remote_to_local_buffer;
    int remote_to_local_len;
    bool remote_to_local_read;
    void* local_to_remote_buffer;
    int local_to_remote_len;
    bool local_to_remote_read;
} buffer_t;

typedef struct thread_args {
    bool local;
    int conn_fd;
} thread_args_t;

int server_listen(int socket_fd, int port, bool local);
int server_accept(int socket_fd, bool local);

void* read_thread(bool local, int conn_fd);
void* write_thread(bool local, int conn_fd);

buffer_t* buffer;
std::mutex buffer_lock;

int main(int argc, char* argv[]) {
    int err = 0;
    int conn_fd = -1, local_conn_fd = -1;
    int remote_fd, local_fd;
    buffer = (buffer_t*)malloc(sizeof(buffer_t));
    buffer->remote_to_local_read = true;
    buffer->local_to_remote_read = true;

#ifdef WIN32
    WSADATA wsaData;

    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        printf("WSAStartup failed: %d\n", result);
        return 1;
    }
#endif

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
    }
    else if (!strcmp(argv[1], "-c")) {
        char* remote_addr = argv[2];
        int remote_port = atoi(argv[3]);
        char* local_addr = argv[4];
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

        if (connect(remote_fd, (struct sockaddr*)&remote_sockaddr, sizeof(remote_sockaddr)) < 0) {
            printf("connect failed\n");
            return 1;
        }
        if (connect(local_fd, (struct sockaddr*)&local_sockaddr, sizeof(local_sockaddr)) < 0) {
            printf("connect failed\n");
            return 1;
        }

        conn_fd = remote_fd;
        local_conn_fd = local_fd;
    }
    else {
        printf("Usage: %s [-s remote_port local_port | -c remote_addr remote_port local_addr local_port]\n", argv[0]);
        exit(1);
    }

    std::thread remote_read_thread(read_thread, false, conn_fd);
    std::thread local_read_thread(read_thread, true, local_conn_fd);
    std::thread remote_write_thread(write_thread, false, local_conn_fd);
    std::thread local_write_thread(write_thread, true, conn_fd);

    local_read_thread.join();
    if (!strcmp(argv[1], "-s")) {
        for (;;) {
#ifndef _WIN32
            pthread_cancel(remote_read_thread.native_handle());
            pthread_cancel(remote_write_thread.native_handle());
            pthread_cancel(local_write_thread.native_handle());
#else
            TerminateThread(remote_read_thread.native_handle(), 0);
            TerminateThread(remote_write_thread.native_handle(), 0);
            TerminateThread(local_write_thread.native_handle(), 0);
#endif
            remote_read_thread.join();
            remote_write_thread.join();
            local_write_thread.join();

            buffer->local_to_remote_read = true;
            buffer->remote_to_local_read = true;

            local_conn_fd = server_accept(local_fd, true);
            if (local_conn_fd == -1) {
                printf("Failed accepting connection, err: %s\n", strerror(err));
                continue;
            }

            local_read_thread = std::thread(read_thread, true, local_conn_fd);
            remote_write_thread = std::thread(write_thread, false, local_conn_fd);

            local_read_thread.join();
        }
    }
}

int server_listen(int socket_fd, int port, bool local) {
    int err = 0;

    struct sockaddr_in server_addr = { 0 };
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    err = bind(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
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

    return 0;
}

int server_accept(int socket_fd, bool local) {
    int err = 0;
    int conn_fd;
    socklen_t client_len;
    struct sockaddr_in client_addr;

    client_len = sizeof(client_addr);

    err = (conn_fd = accept(socket_fd, (struct sockaddr*)&client_addr, &client_len));
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

void* read_thread(bool local, int conn_fd) {
    char* read_buffer[10240];

    for (;;) {
        if (!local && !buffer->remote_to_local_read) {
#ifndef _WIN32
            sleep(1);
#else
            Sleep(1);
#endif
            continue;
        }
        if (local && !buffer->local_to_remote_read) {
#ifndef _WIN32
            sleep(1);
#else
            Sleep(1);
#endif
            continue;
        }

        int recv_resp = recv(conn_fd, (char *)read_buffer, 10240, 0);

        if (recv_resp == 0) {
            if (!local)
                printf("Remote port connection has been closed\n");
            else
                printf("Local port connection has been closed\n");
            return NULL;
        }

        if (recv_resp == -1) {
            perror("recv");
            printf("recv failed, err: %s\n", strerror(recv_resp));
            continue;
        }

        for (;;) {
            if (!local && buffer->remote_to_local_read)
                break;
            if (local && buffer->local_to_remote_read)
                break;
#ifndef _WIN32
            sleep(1);
#else
            Sleep(1);
#endif
        }

        buffer_lock.lock();
        if (!local) {
            buffer->remote_to_local_buffer = read_buffer;
            buffer->remote_to_local_len = recv_resp;
            buffer->remote_to_local_read = false;
        }
        else {
            buffer->local_to_remote_buffer = read_buffer;
            buffer->local_to_remote_len = recv_resp;
            buffer->local_to_remote_read = false;
        }
        buffer_lock.unlock();
    }
}

void* write_thread(bool local, int conn_fd) {
    for (;;) {
        if (!local && buffer->remote_to_local_read) {
#ifndef _WIN32
            sleep(1);
#else
            Sleep(1);
#endif
            continue;
        }
        if (local && buffer->local_to_remote_read) {
#ifndef _WIN32
            sleep(1);
#else
            Sleep(1);
#endif
            continue;
        }

        int send_resp;
        int sent = 0;

        buffer_lock.lock();
        while ((!local && sent != buffer->remote_to_local_len) || (local && sent != buffer->local_to_remote_len)) {
            if (!local)
                send_resp = send(conn_fd, ((char*)(buffer->remote_to_local_buffer)) + sent, buffer->remote_to_local_len - sent, 0);
            else
                send_resp = send(conn_fd, ((char*)(buffer->local_to_remote_buffer)) + sent, buffer->local_to_remote_len - sent, 0);

            if (send_resp == -1) {
                printf("send failed, err: %s\n", strerror(send_resp));
                continue;
            }

            sent += send_resp;
        }

        if (!local)
            buffer->remote_to_local_read = true;
        else
            buffer->local_to_remote_read = true;
        buffer_lock.unlock();
    }
}
