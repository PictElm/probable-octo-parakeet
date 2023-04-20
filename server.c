#include "server.h"

/// create and bind a local socket; exits on failure
static int bind_local_socket(char const* filename, int listen_n) {
  int sock = -1;
  struct sockaddr_un addr = {.sun_family= AF_LOCAL};

  try(sock, socket(PF_LOCAL, SOCK_STREAM, 0));

  strncpy(addr.sun_path, filename, sizeof addr.sun_path);

  int r;
  try(r, bind(sock, (struct sockaddr*)&addr, SUN_LEN(&addr)));
  try(r, listen(sock, listen_n));

  return sock;
finally:
  close(sock);
  _die();
  return -1;
}

#define IDX_SOCK 0
#define IDX_TERM 1
#define IDX_CLIS 2
#define IDX_COUNT 8

static char const* crap_name = NULL; // ZZZ: crap (storing argument pointer into static...)
static struct pollfd fds[IDX_COUNT];
static int fds_count = 0;
static pid_t cpid = 0;
/// cleanup SIGINT handler
static void cleanup(int sign) {
  for (int k = 0; k < fds_count; k++)
    close(fds[k].fd);
  if (crap_name) unlink(crap_name);
  if (sign) kill(cpid, SIGTERM);
  // TODO: send exit code to clients
  waitpid(cpid, NULL, 0);
  if (sign) _exit(0);
}

/// fork and start program on a new pty
static pid_t fork_program(char** args) {
  pid_t cpid;
  try(cpid, forkpty(&fds[IDX_TERM].fd, NULL, NULL, NULL));

  // parent (server)
  if (0 < cpid) return cpid;

  // child (program)
  if (0 != strcmp("crap", args[0]))
  try(cpid, execvp(args[0], args));

  // simpler thing for testing
  char buf[BUF_SIZE];
  int len;
  while (1) {
    try(len, read(STDIN_FILENO, buf, BUF_SIZE));
    if (0 == len--) break;
    for (int k = 0; k < len/2; k++) {
      char tmp = buf[k];
      buf[k] = buf[len-1-k];
      buf[len-1-k] = tmp;
    }
    try(len, write(STDIN_FILENO, buf, len+1));
  }
  _exit(0);

finally:
  _die();
  return -1;
}

void server(char const* name, char** args) {
  cpid = fork_program(args);

  setup_cleanup();

  fds[IDX_TERM].events = POLLIN;

  crap_name = name;
  fds[IDX_SOCK].fd = bind_local_socket(name, IDX_COUNT-IDX_CLIS);
  fds[IDX_SOCK].events = POLLIN;
  fds_count = IDX_CLIS;

  puts("server: listening");

  char buf[BUF_SIZE];
  int len;
  while (1) {
    int n;
    try(n, poll(fds, fds_count, -1));

    // notification from a client
    for (int i = IDX_CLIS; i < fds_count; i++) {
      bool remove = POLLHUP & fds[i].revents;

      // client got input
      if (!remove && POLLIN & fds[i].revents) {
        try(len, read(fds[i].fd, buf, BUF_SIZE));
        printf("<%d> (%dB)\n", fds[i].fd, len);
        remove = 0 == len;

        // program input
        if (0 != len) try(r, write(fds[IDX_TERM].fd, buf, len));
      }

      // client was closed
      if (remove) {
        printf("server: -%d\n", fds[i].fd);
        close(fds[i].fd);
        fds_count--;
        for (int j = i; j < fds_count; j++)
          fds[j] = fds[j+1];
      }
    } // for (clients)

    // progam is done
    if (POLLHUP & fds[IDX_TERM].revents) {
      puts("server: program done");
      break;
    }

    // program output (only if there are clients listening)
    // TODO: how should it behave, stay alive even if no client?
    //       discard program output? or terminate on last disconnect?
    if (POLLIN & fds[IDX_TERM].revents && IDX_CLIS < fds_count) {
      try(len, read(fds[IDX_TERM].fd, buf, BUF_SIZE));
      printf("<prog> (%dB)\n", len);

      // progam is done (eof)
      if (0 == len) {
        puts("server: program done (eof)");
        break;
      }

      // echo back to every clients
      for (int j = IDX_CLIS; j < fds_count; j++)
        try(r, write(fds[j].fd, buf, len));
    }

    // new incoming connection
    if (POLLIN & fds[IDX_SOCK].revents) {
      try(fds[fds_count].fd, accept(fds[IDX_SOCK].fd, NULL, NULL));
      printf("server: +%d\n", fds[fds_count].fd);

      // receive new size to adopt
      struct winsize ws;
      try(len, recv(fds[fds_count].fd, &ws, sizeof ws, MSG_WAITALL));
      // TODO: store sizes of clients and adopts smaller one (..?)

      // abort and close on failure
      if (sizeof ws == len) {
        printf("server: %dx%d\n", ws.ws_col, ws.ws_row);
        try(r, ioctl(fds[IDX_TERM].fd, TIOCSWINSZ, &ws));

        fds[fds_count].events = POLLIN;
        fds_count++;
      } else {
        puts("server: aborting connection");
        close(fds[fds_count].fd);
      }
    }
  } // while (poll)

finally:
  cleanup(0);
  if (errdid) _die();
}