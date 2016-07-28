/*
 * - socket for backend program
 * - listening socket for clients
 * - list of sockets for active connections
 *   non-blocking
 *
 * - everything from backend socket is fan out to active connections
 *   assume we can always write. if it fails, kill client
 * - commmands from any active connection is passed straight to backend
 *
 * - if backend program exits, we will exit, too
 *
 * - when reading from clients, buffer lines
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#define MAXCONN 200
#define MAXEVENTS 100
#define MAXLEN 255
#define MAXCLIENTS 32

struct EchoEvent {
  int fd;
  char data[MAXLEN];
  int length;
  int offset;
};

void usage() {
  puts("Usage: tcpmpx [-h] -p <port> -c <command>");
  puts("FIXME: What does this tool do?");
  puts("  -p <int>         port number to listen on");
  puts("  -c <command>     the command to run");
  puts("  -h               this help\n");
}

int sopen(const char *program) {
  int fds[2];
  pid_t pid;

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
    return -1;

  switch(pid = fork()) {
    case -1:
      close(fds[0]);
      close(fds[1]);
      return -1;
    case 0:
      close(fds[0]);
      dup2(fds[1], 0);
      dup2(fds[1], 1);
      close(fds[1]);
      execl("/bin/sh", "sh", "-c", program, NULL);
      _exit(127);
    default:
      close(fds[1]);
      return fds[0];
  }
}

void modify_epoll(int fd_epoll, int operation, int fd, uint32_t events, void* data) {
  struct epoll_event server_listen_event = {
    .events   = events,
    .data.ptr = data
  };

  if (-1 == epoll_ctl(fd_epoll, operation, fd, &server_listen_event)) {
    perror("epoll_ctl() failed");
    exit(EXIT_FAILURE);
  }
}

int handleIn(int fd_command, void* ptr) {
  struct EchoEvent* echoEvent = ptr;

  int n = read(echoEvent->fd, echoEvent->data, MAXLEN);

  if (0 == n) {
    printf("Client closed connection.\n");
    return 0;
  } else if (-1 == n) {
    return 0;
  } else {
    ssize_t l;
    l = write(fd_command, echoEvent->data, n);
    printf("wrote %zd\n", l);
    return 1;
  }
}

void set_nonblock(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, NULL);
  flags |= O_NONBLOCK;
  fcntl(fd, F_SETFL, flags);
}

int client_insert(int *clients, int len, int client) {
  for (int i = 0; i < len; i++)
    if (clients[i] == 0) {
      clients[i] = client;
      return 1;
    }

  return 0;
}

void client_remove(int *clients, int len, int client) {
  for (int i = 0; i < len; i++)
    if (clients[i] == client)
      clients[i] = 0;
}

int main(int argc, char** argv) {
  struct sockaddr_in6 server_addr = {0};
  char *command = NULL;
  int c;

  opterr = 0;

  server_addr.sin6_family = AF_INET6;
  server_addr.sin6_addr = in6addr_any;

  while ((c = getopt(argc, argv, "p:c:h")) != -1)
    switch (c) {
      case 'p':
        server_addr.sin6_port = htons(atoi(optarg));
        break;
      case 'c':
        command = optarg;
        break;
      case 'h':
        usage();
        exit(EXIT_SUCCESS);
        break;
      default:
        fprintf(stderr, "Invalid parameter %c ignored.\n", c);
    }

  if (server_addr.sin6_port == 0) {
    fprintf(stderr, "No port given.\n");
    exit(EXIT_FAILURE);
  }

  if (command == NULL) {
    fprintf(stderr, "No command given.\n");
    exit(EXIT_FAILURE);
  }

  if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
    perror(0);
    exit(EXIT_FAILURE);
  }

  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    perror(0);
    exit(EXIT_FAILURE);
  }

  int fd_server;

  struct sockaddr_in clientaddr;
  socklen_t clientlen = sizeof(clientaddr);

  fd_server = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd_server == -1) {
    perror("create socket failed");
    exit(EXIT_FAILURE);
  }

  int optval = 1;
  setsockopt(fd_server, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));

  if (bind(fd_server, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
    perror("bind() failed");
    exit(EXIT_FAILURE);
  }

  if (listen(fd_server, MAXCONN) == -1) {
    perror("listen() failed");
    exit(EXIT_FAILURE);
  }

  int fd_epoll = epoll_create(MAXCONN);
  if (fd_epoll == -1) {
    perror("epoll_create() failed");
    exit(EXIT_FAILURE);
  }

  int *fds_clients;

  fds_clients = calloc(MAXCLIENTS, sizeof(int));

  int fd_command;
  fd_command = sopen(command);

  modify_epoll(fd_epoll, EPOLL_CTL_ADD, fd_server, EPOLLIN, &fd_server);
  modify_epoll(fd_epoll, EPOLL_CTL_ADD, fd_command, EPOLLIN, &fd_command);

  struct epoll_event *events = calloc(MAXEVENTS, sizeof(struct epoll_event));
  while (1) {
    int n = epoll_wait(fd_epoll, events, MAXEVENTS, -1);

    if (-1 == n) {
      perror("epoll_wait()");
    }

    int i;
    for (i = 0; i < n; i++) {
      if (events[i].data.ptr == &fd_command) {
        if (events[i].events & EPOLLHUP || events[i].events & EPOLLERR) {
          fprintf(stderr, "command died\n");
          close(fd_command);
          exit(EXIT_FAILURE);
        }

        char buffer[1024];
        ssize_t l;
        l = read(fd_command, buffer, sizeof(buffer) - 1);
        buffer[l>0?l:0] = 0;
        printf("read %zd bytes: %s\n", l, buffer);

        for (int i = 0; i < MAXCLIENTS; i++)
          if (fds_clients[i])
            write(fds_clients[i], buffer, l);
      } else if (events[i].data.ptr == &fd_server) {
        if (events[i].events & EPOLLHUP || events[i].events & EPOLLERR) {
          close(fd_server);
          exit(EXIT_FAILURE);
        }

        int connfd = accept(fd_server, (struct sockaddr*)&clientaddr, &clientlen);

        if (-1 == connfd) {
          perror("accept() failed");
          exit(EXIT_FAILURE);
        } else {
          if (!client_insert(fds_clients, MAXCLIENTS, connfd)) {
              close(connfd);
              fprintf(stderr, "Too many clients. Connection rejected.\n");
              continue;
          }

          printf("Accepted connection.\n");

          set_nonblock(connfd);

          printf("Adding a read event\n");

          struct EchoEvent* echoEvent = calloc(1, sizeof(struct EchoEvent));

          echoEvent->fd = connfd;

          modify_epoll(fd_epoll, EPOLL_CTL_ADD, echoEvent->fd, EPOLLIN, echoEvent);
        }
      } else {
        if (events[i].events & EPOLLHUP || events[i].events & EPOLLERR) {
          struct EchoEvent* echoEvent = (struct EchoEvent*) events[i].data.ptr;
          printf("\nClosing connection socket\n");
          client_remove(fds_clients, MAXCLIENTS, echoEvent->fd);
          close(echoEvent->fd);
          free(echoEvent);
        } else if (events[i].events == EPOLLIN) {
          struct EchoEvent* echoEvent = (struct EchoEvent*) events[i].data.ptr;
          if (!handleIn(fd_command, echoEvent)) {
            client_remove(fds_clients, MAXCLIENTS, echoEvent->fd);
            close(echoEvent->fd);
            free(echoEvent);
          }
        }
      }
    }
  }

  free(events);
  exit(EXIT_SUCCESS);
}

