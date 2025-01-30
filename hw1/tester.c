#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <getopt.h>

#define BUFFER_SIZE 4096

volatile int running = 1;

void usage(const char *prog_name)
{
  fprintf(stderr, "Usage: %s [options] conn [port]\n", prog_name);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -i                 Interactive mode (default)\n");
  fprintf(stderr, "  -s scriptfile      Script mode: read commands from scriptfile\n");
  fprintf(stderr, "  -n num_clients     Number of clients to simulate (default 1)\n");
  fprintf(stderr, "  --delay N          Delay between commands in milliseconds (default 0)\n");
  fprintf(stderr, "  conn               Connection string. If it starts with '@', Unix socket path; else IP\n");
  fprintf(stderr, "  port               Port number (required if conn is IP)\n");
}

int connect_unix_domain_socket(const char *path)
{
  int sockfd;
  struct sockaddr_un addr;

  if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("socket");
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
  {
    perror("connect");
    close(sockfd);
    return -1;
  }

  return sockfd;
}

int connect_tcp_socket(const char *ip, int port)
{
  int sockfd;
  struct sockaddr_in addr;

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
  {
    perror("socket");
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0)
  {
    perror("inet_pton");
    close(sockfd);
    return -1;
  }

  if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
  {
    perror("connect");
    close(sockfd);
    return -1;
  }

  return sockfd;
}

ssize_t send_command(int sockfd, const char *command)
{
  size_t len = strlen(command);
  ssize_t total_sent = 0;
  while (total_sent < len)
  {
    ssize_t sent = write(sockfd, command + total_sent, len - total_sent);
    if (sent <= 0)
    {
      perror("write");
      return -1;
    }
    total_sent += sent;
  }
  return total_sent;
}

void *receiver_thread(void *arg)
{
  int sockfd = *(int *)arg;
  free(arg); // Free the allocated sockfd_ptr

  char buffer[BUFFER_SIZE];

  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

  while (running)
  {
    ssize_t n = read(sockfd, buffer, sizeof(buffer) - 1);
    if (n > 0)
    {
      buffer[n] = '\0';
      printf("%s", buffer);
      fflush(stdout);
    }
    else if (n == 0)
    {
      printf("Server closed the connection\n");
      running = 0;
      break;
    }
    else
    {
      if (errno == EINTR)
      {
        continue;
      }
      perror("read");
      running = 0;
      break;
    }
  }
  return NULL;
}

void run_interactive_mode(int sockfd)
{
  pthread_t recv_thread;
  int *sockfd_ptr = malloc(sizeof(int));
  if (sockfd_ptr == NULL)
  {
    perror("malloc");
    return;
  }
  *sockfd_ptr = sockfd;

  if (pthread_create(&recv_thread, NULL, receiver_thread, sockfd_ptr) != 0)
  {
    perror("pthread_create");
    free(sockfd_ptr);
    return;
  }

  // Allow receiver thread to start
  usleep(100000); // 0.1 seconds

  char input[BUFFER_SIZE];
  while (running)
  {
    printf("> ");
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL)
    {
      // EOF or error
      break;
    }

    // Remove trailing newline if present
    size_t len = strlen(input);
    if (len > 0 && input[len - 1] == '\n')
    {
      input[len - 1] = '\0';
    }

    // Send the command to the server
    if (send_command(sockfd, input) == -1)
    {
      printf("Failed to send command.\n");
      break;
    }
    // Send newline character to denote end of command
    if (send_command(sockfd, "\n") == -1)
    {
      printf("Failed to send newline.\n");
      break;
    }

    // If the command is 'quit', we can exit
    if (strcmp(input, "quit") == 0)
    {
      running = 0;
      break;
    }
  }

  // Wait for the receiver thread to finish
  pthread_cancel(recv_thread);
  pthread_join(recv_thread, NULL);
}

typedef struct
{
  char *conn;
  int port;
  int interactive_mode;
  char *scriptfile;
  int delay_ms;
  int client_num; // For identification
} client_args_t;

void *client_thread(void *arg)
{
  client_args_t *args = (client_args_t *)arg;

  int sockfd = -1;
  if (args->conn[0] == '@')
  {
    // Unix domain socket
    printf("Client %d: Connecting to Unix domain socket at '%s'\n", args->client_num, args->conn + 1);
    sockfd = connect_unix_domain_socket(args->conn + 1);
  }
  else
  {
    // TCP socket
    printf("Client %d: Connecting to TCP socket at %s:%d\n", args->client_num, args->conn, args->port);
    sockfd = connect_tcp_socket(args->conn, args->port);
  }

  if (sockfd == -1)
  {
    fprintf(stderr, "Client %d: Failed to connect to the server\n", args->client_num);
    pthread_exit(NULL);
  }

  // Start receiver thread
  int *sockfd_ptr = malloc(sizeof(int));
  if (sockfd_ptr == NULL)
  {
    perror("malloc");
    close(sockfd);
    pthread_exit(NULL);
  }
  *sockfd_ptr = sockfd;

  pthread_t recv_thread;
  if (pthread_create(&recv_thread, NULL, receiver_thread, sockfd_ptr) != 0)
  {
    perror("pthread_create");
    close(sockfd);
    free(sockfd_ptr);
    pthread_exit(NULL);
  }

  // Allow receiver thread to start
  usleep(100000); // 0.1 seconds

  if (args->interactive_mode)
  {
    if (args->client_num == 0)
    {
      printf("Client %d: Running in interactive mode\n", args->client_num);
      run_interactive_mode(sockfd);
    }
    else
    {
      // Other clients do nothing in interactive mode
      printf("Client %d: Interactive mode is only for single client\n", args->client_num);
    }
  }
  else
  {
    // Script mode
    printf("Client %d: Running script '%s'\n", args->client_num, args->scriptfile);
    FILE *script_fp = fopen(args->scriptfile, "r");
    if (script_fp == NULL)
    {
      perror("fopen");
      close(sockfd);
      pthread_cancel(recv_thread);
      pthread_join(recv_thread, NULL);
      pthread_exit(NULL);
    }

    char line[BUFFER_SIZE];
    while (running && fgets(line, sizeof(line), script_fp) != NULL)
    {
      // Remove trailing newline
      size_t len = strlen(line);
      if (len > 0 && line[len - 1] == '\n')
      {
        line[len - 1] = '\0';
      }

      // Send the command to the server
      if (send_command(sockfd, line) == -1)
      {
        printf("Client %d: Failed to send command.\n", args->client_num);
        break;
      }
      // Send newline character to denote end of command
      if (send_command(sockfd, "\n") == -1)
      {
        printf("Client %d: Failed to send newline.\n", args->client_num);
        break;
      }

      // If the command is 'quit', we can exit
      if (strcmp(line, "quit") == 0)
      {
        running = 0;
        break;
      }

      // Delay between commands
      if (args->delay_ms > 0)
      {
        usleep(args->delay_ms * 1000);
      }
    }

    usleep(1000000); // 1 second
    fclose(script_fp);
  }

  // Wait for the receiver thread to finish
  pthread_cancel(recv_thread);
  pthread_join(recv_thread, NULL);

  // Close the connection
  close(sockfd);

  pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
  int num_clients = 1;
  char *scriptfile = NULL;
  int interactive_mode = 1;
  int delay_ms = 0;

  // Parse command-line options
  int opt;
  static struct option long_options[] = {
      {"delay", required_argument, 0, 0},
      {0, 0, 0, 0}};
  int option_index = 0;

  while ((opt = getopt_long(argc, argv, "is:n:", long_options, &option_index)) != -1)
  {
    switch (opt)
    {
    case 'i':
      interactive_mode = 1;
      break;
    case 's':
      interactive_mode = 0;
      scriptfile = optarg;
      break;
    case 'n':
      num_clients = atoi(optarg);
      if (num_clients <= 0)
      {
        fprintf(stderr, "Invalid number of clients: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;
    case 0:
      if (strcmp(long_options[option_index].name, "delay") == 0)
      {
        delay_ms = atoi(optarg);
        if (delay_ms < 0)
        {
          fprintf(stderr, "Invalid delay value: %s\n", optarg);
          exit(EXIT_FAILURE);
        }
      }
      break;
    default:
      usage(argv[0]);
      exit(EXIT_FAILURE);
    }
  }

  if (optind >= argc)
  {
    usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  // Get connection info
  char *conn = argv[optind];
  int port = 0;
  char *ip = NULL;
  char *unix_path = NULL;
  if (conn[0] == '@')
  {
    // Unix domain socket
    unix_path = conn + 1; // Skip '@'
  }
  else
  {
    // TCP socket
    ip = conn;
    if (optind + 1 >= argc)
    {
      usage(argv[0]);
      exit(EXIT_FAILURE);
    }
    port = atoi(argv[optind + 1]);
    if (port <= 0 || port > 65535)
    {
      fprintf(stderr, "Invalid port number: %s\n", argv[optind + 1]);
      exit(EXIT_FAILURE);
    }
  }

  // Start clients
  pthread_t *threads = malloc(sizeof(pthread_t) * num_clients);
  if (threads == NULL)
  {
    perror("malloc");
    exit(EXIT_FAILURE);
  }

  client_args_t *client_args = malloc(sizeof(client_args_t) * num_clients);
  if (client_args == NULL)
  {
    perror("malloc");
    free(threads);
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < num_clients; i++)
  {
    client_args[i].conn = conn;
    client_args[i].port = port;
    client_args[i].interactive_mode = interactive_mode;
    client_args[i].scriptfile = scriptfile;
    client_args[i].delay_ms = delay_ms;
    client_args[i].client_num = i;

    if (pthread_create(&threads[i], NULL, client_thread, &client_args[i]) != 0)
    {
      perror("pthread_create");
      exit(EXIT_FAILURE);
    }
  }

  // Wait for clients to finish
  for (int i = 0; i < num_clients; i++)
  {
    pthread_join(threads[i], NULL);
  }

  free(threads);
  free(client_args);
  return 0;
}