#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <pty.h>
#include <signal.h>
#include <errno.h>

#define _POSIX_
#include <limits.h>

#define PATH 1024

static int _fd_in = -1;

static char *_script = NULL;
static const char *_script_cur = NULL;
static char *
_prg_full_path_guess(const char *prg)
{
   char full_path[PATH];
   if (strchr(prg, '/')) return strdup(prg);
   char *paths = strdup(getenv("PATH"));
   char *paths0 = paths;
   char *ret = NULL;
   while (paths && *paths && !ret)
     {
        char *colon = strchr(paths, ':');
        if (colon) *colon = '\0';

        sprintf(full_path, "%s/%s", paths, prg);
        ret = realpath(full_path, NULL);

        paths += strlen(paths);
        if (colon) paths++;
     }
   free(paths0);
   return ret;
}

static char *
_file_get_as_string(const char *filename)
{
   char *file_data = NULL;
   int file_size;
   FILE *fp = fopen(filename, "rb");
   if (!fp)
     {
        perror("open");
        fprintf(stderr, "Can not open file: \"%s\"", filename);
        return NULL;
     }

   fseek(fp, 0, SEEK_END);
   file_size = ftell(fp);
   if (file_size == -1)
     {
        fclose(fp);
        fprintf(stderr, "Can not ftell file: \"%s\"", filename);
        return NULL;
     }
   rewind(fp);
   file_data = (char *) calloc(1, file_size + 1);
   if (!file_data)
     {
        fclose(fp);
        fprintf(stderr, "Calloc failed");
        return NULL;
     }
   int res = fread(file_data, file_size, 1, fp);
   fclose(fp);
   if (!res)
     {
        free(file_data);
        file_data = NULL;
        fprintf(stderr, "fread failed");
     }
   return file_data;
}

static void
_script_consume()
{
   char cr = 0x0A;
   char *nl = strchr(_script_cur, '\n');
   if (nl) *nl = '\0';
   if (!strncmp(_script_cur, "TYPE ", 5))
     {
        write(_fd_in, _script_cur + 5, strlen(_script_cur + 5));
        write(_fd_in, &cr, 1);
     }
   else
     {
        fprintf(stderr, "Unrecognized line: %s\n", _script_cur);
     }
   if (nl) _script_cur = nl + 1;
   else _script_cur = NULL;
}

static void
_script_load(const char *script_name)
{
   char path[1024];
   struct stat s;
   if (strstr(script_name, "..") || strchr(script_name, '/'))
     {
        fprintf(stderr, "Script name (%s) should not contain the path\n",
              script_name);
        return;
     }
   sprintf(path, "%s/.config/zcra/scripts/%s.zcra",
         getenv("HOME"), script_name);

   if (stat(path, &s) == -1)
     {
        fprintf(stderr, "Script %s permission cannot be read\n", path);
        perror("stat");
        return;
     }

   if (s.st_mode & (S_IRWXG | S_IRWXO))
     {
        fprintf(stderr, "Script %s should be forbidden for other users (chmod 600)\n",
              path);
        return;
     }

   _script = _file_get_as_string(path);
   _script_cur = _script;
   _script_consume();
}

static void
_handle_sigint(int sig)
{
   (void) sig;
   char c = 3;
   write(_fd_in, &c, 1);
}

int
main(int argc, char **argv)
{
   int opt, id = -1, ret = 0, help = 0;
   int pipe_out[2], udp_fd = -1;
   struct option opts[] =
     {
          { "help", no_argument,       NULL, 'h' },
          { "id",   required_argument, NULL, 'i' },
          { NULL,   0,                 NULL, 0   }
     };

   for (opt = 0; (opt = getopt_long(argc, argv, "hi:", opts, NULL)) != -1; )
     {
        switch (opt)
          {
           case 0: break;
           case 'h':
                   help = 1;
                   break;
           case 'i':
                   id = strtol(optarg, NULL, 10);
                   break;
           default:
                   help = 1;
                   break;
          }
     }

   if (optind >= argc) help = 1;

   if (help)
     {
        printf("Usage: %s [-h/--help] [-i/--id] prg [prg_args]\n", argv[0]);
        printf("       -h | --help Print that help\n");
        printf("       -i | --id   Id of the instance when remote usage is needed. If not provided, no remote is possible.\n");
        ret = 1;
        goto end;
     }

   pipe(pipe_out);

   if (forkpty(&_fd_in, NULL, NULL, NULL) == 0)
     {
        /* Child */
        close(pipe_out[0]);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_out[1]);

        execv(_prg_full_path_guess(argv[optind]), argv + optind);
     }
   else
     {
        /* ZCRA */
        fd_set fds, rfds;
        int nb, fd, max_fd, error = 0;
        struct termios old_in_t, t;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(pipe_out[0], &fds);
        max_fd = pipe_out[0];

        signal(SIGINT, _handle_sigint);
        close(pipe_out[1]);

        tcgetattr(STDIN_FILENO, &old_in_t);
        t = old_in_t;
        t.c_lflag &= ~ICANON;
        t.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);

        tcgetattr(pipe_out[0], &t);
        t.c_lflag &= ~ICANON;
        t.c_lflag &= ~ECHO;
        tcsetattr(pipe_out[0], TCSANOW, &t);

        /* UDP initialization */
        if (id >= 0)
          {
             struct sockaddr_in servaddr;
             servaddr.sin_family = AF_INET;
             servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
             servaddr.sin_port = htons(40000 + id);
             /* create UDP socket */
             udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
             /* binding server addr structure to udp socket */
             if (bind(udp_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) != 0)
               {
                  perror("bind");
                  error = 1;
               }
             FD_SET(udp_fd, &fds);
             if (udp_fd > max_fd) max_fd = udp_fd;
          }

        while (error == 0)
          {
             rfds = fds;
             nb = select(max_fd + 1, &rfds, NULL, NULL, NULL);
             if (nb == -1)
               {
                  if (errno != EINTR) {
                       perror("select");
                       error = 1;
                  }
                  continue;
               }

             for (fd = 0; fd < max_fd + 1 && nb; fd++)
               {
                  if (FD_ISSET(fd, &rfds))
                    {
                       char c;
                       nb--;
                       if (fd == STDIN_FILENO)
                         {
                            if (read(STDIN_FILENO, &c, 1) == 1)
                              {
//                                 fprintf(stderr, "%d - Received from stdin: %c (%.2X)\n", getpid(), c, c);
                                 write(_fd_in, &c, 1);
                              }
                         }
                       else if (fd == pipe_out[0])
                         {
                            if (read(pipe_out[0], &c, 1) == 1)
                              {
//                                 fprintf(stderr, "%d - new data %c\n", getpid(), c);
                                 printf("%c", c);
                              }
                            else
                              {
                                 FD_CLR(pipe_out[0], &fds);
                                 close(pipe_out[0]);
                                 error = 1;
                              }
                         }
                       else if (fd == udp_fd)
                         {
                            struct sockaddr_in cliaddr;
                            char *src_ip;
                            socklen_t len = sizeof(cliaddr);
                            char buffer[100];
                            char *nl;
                            recvfrom(udp_fd, buffer, sizeof(buffer), 0,
                                  (struct sockaddr*)&cliaddr, &len);
                            nl = strchr(buffer, '\n');
                            if (nl) *nl = '\0';
                            src_ip = inet_ntoa(cliaddr.sin_addr);
                            printf("\nScript from client %s: %s\n",
                                  src_ip, buffer);
                            _script_load(buffer);
                         }
                    }
               }
          }
        if (error != 0)
          {
             tcsetattr(STDIN_FILENO, TCSANOW, &old_in_t);
             ret = 1;
          }
     }

end:
   return ret;
}
