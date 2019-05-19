#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

#define _POSIX_
#include <limits.h>

#define PATH 1024

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

int
main(int argc, char **argv)
{
   int opt, id = -1, ret = 0, help = 0;
   int pipe_in[2], pipe_out[2];
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

   if (help)
     {
        printf("Usage: %s [-h/--help] [-i/--id] prg [prg_args]\n", argv[0]);
        printf("       -h | --help Print that help\n");
        printf("       -i | --id   Id of the instance when remote usage is needed. If not provided, no remote is possible.\n");
        ret = 1;
        goto end;
     }

   pipe(pipe_in);
   pipe(pipe_out);

   if (fork() == 0)
     {
        /* Child */
        dup2(pipe_in[0], STDIN_FILENO);
        close(pipe_in[0]);
        close(pipe_in[1]);

        close(pipe_out[0]);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_out[1]);

        execv(_prg_full_path_guess(argv[optind]), argv + optind);
     }
   else
     {
        /* ZCRA */
        fd_set fds;
        int nb, fd, max_fd;
        struct termios t;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(pipe_out[0], &fds);
        max_fd = pipe_out[0];

        close(pipe_in[0]);
        close(pipe_out[1]);

        tcgetattr(STDIN_FILENO, &t);
        t.c_lflag &= ~ICANON;
        t.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);

        tcgetattr(pipe_out[0], &t);
        t.c_lflag &= ~ICANON;
        t.c_lflag &= ~ECHO;
        tcsetattr(pipe_out[0], TCSANOW, &t);

        while(1)
          {
             nb = select(max_fd + 1, &fds, NULL, NULL, NULL);
             for (fd = 0; fd < max_fd + 1 && nb; fd++)
               {
                  if (FD_ISSET(fd, &fds))
                    {
                       char c;
                       nb--;
                       if (fd == STDIN_FILENO)
                         {
                            if (read(STDIN_FILENO, &c, 1) == 1)
                              {
                                 fprintf(stderr, "%d - Received from stdin: %c\n", getpid(), c);
                                 write(pipe_in[1], &c, 1);
                              }
                         }
                       else if (fd == pipe_out[0])
                         {
                            if (read(pipe_out[0], &c, 1) == 1)
                              {
                                 fprintf(stderr, "%d - new data %c\n", getpid(), c);
                                 printf("%c", c);
                              }
                            else
                              {
                                 FD_CLR(pipe_out[0], &fds);
                                 close(pipe_out[0]);
                              }
                         }
                    }
               }
          }
     }
   printf("Id: %d\n", id);
   printf("APP: %s\n", _prg_full_path_guess(argv[0]));

end:
   return ret;
}
