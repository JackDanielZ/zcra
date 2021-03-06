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
#include <fcntl.h>
#include <stdarg.h>
#include <sys/inotify.h>

#define _POSIX_
#include <limits.h>

#define PATH 1024

static int _app_fd = -1;
static int _app_pid = -1;

static char *_script = NULL;
static const char *_script_cur = NULL;
static const char *_wait_str = NULL;
static struct timeval _wait_tm;
static unsigned int _wait_fail_continue;

static const char *_log_file = NULL;
static int _log_fd = -1;

#define PRINT(...) \
   fprintf(stderr, __VA_ARGS__); \
   LOG(__VA_ARGS__);

static void
LOG(const char *fmt, ...)
{
   va_list args;
   char buf[1024];
   if (!_log_file) return;

   va_start(args, fmt);
   vsnprintf(buf, sizeof(buf), fmt, args);
   va_end(args);

   if (_log_fd == -1)
     {
        _log_fd = open(_log_file, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
        if (_log_fd == -1)
          {
             perror("log_open");
             return;
          }
     }
   if (write(_log_fd, buf, strlen(buf)) <= 0)
     {
        perror("log_write");
     }
}

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
_script_terminate()
{
   free(_script);
   _script = NULL;
   _script_cur = NULL;
   PRINT("Script terminated\n");
}
static void
_script_consume()
{
   int exit = 0;
   char cr = 0x0A;
   while (!exit && _script_cur)
     {
        char *nl = strchr(_script_cur, '\n');
        if (nl) *nl = '\0';

        while (*_script_cur == ' ') _script_cur++;

        if (*_script_cur)
          {
             if (!strncmp(_script_cur, "TYPE ", 5))
               {
                  write(_app_fd, _script_cur + 5, strlen(_script_cur + 5));
                  write(_app_fd, &cr, 1);
               }
             else if (!strncmp(_script_cur, "PASSWORD ", 9))
               {
                  char pw_path[1024], *pw, *pw_nl;
                  const char *pw_alias = _script_cur + 9;
                  struct stat s;

                  if (strstr(pw_alias, "..") || strchr(pw_alias, '/'))
                    {
                       PRINT("Password alias (%s) should not contain the path\n", pw_alias);
                       return;
                    }
                  sprintf(pw_path, "%s/.config/zcra/passwords/%s", getenv("HOME"), pw_alias);

                  if (stat(pw_path, &s) == -1)
                    {
                       PRINT("Password file %s: permission cannot be read\n", pw_path);
                       perror("stat");
                       return;
                    }

                  if (s.st_mode & (S_IRWXG | S_IRWXO))
                    {
                       PRINT("Password %s should be forbidden for other users (chmod 600)\n", pw_path);
                       return;
                    }

                  pw = _file_get_as_string(pw_path);
                  pw_nl = strchr(pw, '\n');
                  if (*pw_nl) *pw_nl = '\0';
                  write(_app_fd, pw, strlen(pw));
                  write(_app_fd, &cr, 1);
                  free(pw);
               }
             else if (!strncmp(_script_cur, "WAIT ", 5))
               {
                  _wait_str = _script_cur + 5;
                  exit = 1;
               }
             else if (!strncmp(_script_cur, "WAIT_TIME ", 10))
               {
                  char *end_time;
                  unsigned int time_us = 0, time_s = 0, time;
                  _script_cur += 10;
                  time = strtol(_script_cur, &end_time, 10);
                  if (end_time != _script_cur)
                    {
                       while (*end_time == ' ') end_time++;
                       if (!strncmp(end_time, "ms ", 3))
                         {
                            time_us = time * 1000000;
                            _script_cur = end_time + 3;
                         }
                       else if (!strncmp(end_time, "s ", 2))
                         {
                            time_s = time;
                            _script_cur = end_time + 2;
                         }
                       else
                         {
                            PRINT("Expected time in milli-seconds or seconds\n");
                            return;
                         }
                       _wait_tm.tv_sec = time_s;
                       _wait_tm.tv_usec = time_us;
                       while (*_script_cur == ' ') _script_cur++;
                       if (!strncmp(_script_cur, "CONTINUE ", 9))
                         {
                            _wait_fail_continue = 1;
                            _script_cur += 9;
                         }
                       else if (!strncmp(_script_cur, "EXIT ", 5))
                         {
                            _wait_fail_continue = 0;
                            _script_cur += 5;
                         }
                       else
                         {
                            PRINT("Expected CONTINUE or EXIT\n");
                            return;
                         }
                       while (*_script_cur == ' ') _script_cur++;
                       _wait_str = _script_cur;
                       exit = 1;
                    }
                  else
                    {
                       PRINT("Expected time\n");
                    }
               }
             else
               {
                  PRINT("Unrecognized line: %s\n", _script_cur);
               }
          }
        if (nl) _script_cur = nl + 1;
        else exit = 1;
     }
   if (!_wait_str)
     {
        _script_terminate();
     }
}

static void
_script_load(const char *script_name)
{
   char path[1024];
   struct stat s;
   if (strstr(script_name, "..") || strchr(script_name, '/'))
     {
        PRINT("Script name (%s) should not contain the path\n",
              script_name);
        return;
     }
   sprintf(path, "%s/.config/zcra/scripts/%s.zcra",
         getenv("HOME"), script_name);

   if (stat(path, &s) == -1)
     {
        PRINT("Script %s permission cannot be read\n", path);
        perror("stat");
        return;
     }

   if (s.st_mode & (S_IRWXG | S_IRWXO))
     {
        PRINT("Script %s should be forbidden for other users (chmod 600)\n",
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
   write(_app_fd, &c, 1);
//   kill(_app_pid, SIGINT);
}

int
main(int argc, char **argv)
{
   int opt, id = -1, ret = 0, help = 0;
   int udp_fd = -1;
   struct option opts[] =
     {
          { "help",  no_argument,       NULL, 'h' },
          { "id",    required_argument, NULL, 'i' },
          { "log",   required_argument, NULL, 'l' },
          { NULL,   0,                 NULL, 0   }
     };

   for (opt = 0; (opt = getopt_long(argc, argv, "hi:l:", opts, NULL)) != -1; )
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
           case 'l':
                   _log_file = optarg;
                   break;
           default:
                   help = 1;
                   break;
          }
     }

   if (optind >= argc) help = 1;

   if (help)
     {
        printf("Usage: %s [-h/--help] [-i/--id id] [-l/--log logfile] prg [prg_args]\n", argv[0]);
        printf("       -h | --help Print that help\n");
        printf("       -i | --id   Id of the instance when remote usage is needed. If not provided, no remote is possible.\n");
        printf("       -l | --log  File path where to store the log.\n");
        ret = 1;
        goto end;
     }

   struct termios old_in_t, t;
   setbuf(stdout, NULL);
   if ((_app_pid = forkpty(&_app_fd, NULL, NULL, NULL)) == 0)
     {
        execv(_prg_full_path_guess(argv[optind]), argv + optind);
     }
   else
     {
        unsigned int max_wait_len = 0, cur_wait_len = 0;
        char *wait_buf = NULL;

        /* ZCRA */
        fd_set fds;
        int nb, fd, max_fd, error = 0;
        int inot_fd = -1, log_inot_wfd = -1;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(_app_fd, &fds);
        max_fd = _app_fd;

        signal(SIGINT, _handle_sigint);

        tcgetattr(STDIN_FILENO, &old_in_t);
        t = old_in_t;
        t.c_lflag &= ~ICANON;
        t.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);

        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        t.c_oflag &= ~OPOST;
        t.c_lflag &= ~(ICANON|ISIG|ECHO);
        t.c_iflag &= ~(INLCR|IGNCR|ICRNL|IUCLC|IXON);
        tcsetattr(_app_fd, TCSANOW, &t);

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

        if (_log_file)
          {
             if (inot_fd == -1)
               {
                  inot_fd = inotify_init();
                  FD_SET(inot_fd, &fds);
                  if (inot_fd > max_fd) max_fd = inot_fd;
               }
          }
        while (error == 0)
          {
             fd_set rfds = fds;

             if (_log_file)
               {
                  if (_log_fd == -1)
                    {
                       _log_fd = open(_log_file, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
                       if (_log_fd == -1)
                         {
                            perror("log_open");
                         }
                    }
                  if (log_inot_wfd == -1)
                    {
                       log_inot_wfd = inotify_add_watch(inot_fd, _log_file,
                             IN_ATTRIB | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF);
                       if (log_inot_wfd == -1) perror("inotify_add_watch");
                    }
               }

             nb = select(max_fd + 1, &rfds, NULL, NULL,
                   _wait_tm.tv_sec || _wait_tm.tv_usec ? &_wait_tm : NULL);
             if (nb == 0)
               {
                  /* Timeout */
                  if (_wait_fail_continue == 1)
                    {
                       cur_wait_len = 0;
                       _wait_str = NULL;
                       _script_consume();
                    }
                  else
                    {
                       _script_terminate();
                    }
               }
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
                                 write(_app_fd, &c, 1);
                              }
                         }
                       else if (fd == _app_fd)
                         {
                            if (read(_app_fd, &c, 1) == 1)
                              {
                                 printf("%c", c);
                                 LOG("%c", c);
                                 if (_wait_str)
                                   {
                                      if (!max_wait_len)
                                        {
                                           max_wait_len = 16;
                                           wait_buf = malloc(max_wait_len);
                                        }
                                      if (max_wait_len == cur_wait_len + 1)
                                        {
                                           max_wait_len <<= 1;
                                           wait_buf = realloc(wait_buf, max_wait_len);
                                           LOG("Realloc wait_buf to %d bytes\n",
                                                 max_wait_len);
                                        }

                                      wait_buf[cur_wait_len] = c;
                                      cur_wait_len++;
                                      wait_buf[cur_wait_len] = '\0';
//                                      PRINT("WAIT \"%s\" in \"%s\"\n", _wait_str, wait_buf);

                                      if (strstr(wait_buf, _wait_str))
                                        {
                                           cur_wait_len = 0;
                                           _wait_str = NULL;
                                           memset(&_wait_tm, 0, sizeof(_wait_tm));
                                           _script_consume();
                                        }
                                      if (c == '\n') cur_wait_len = 0;
                                   }
                              }
                            else
                              {
                                 FD_CLR(_app_fd, &fds);
                                 close(_app_fd);
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
                            LOG("\nScript from client %s: %s\n", src_ip, buffer);
                            _script_load(buffer);
                         }
                       else if (fd == inot_fd)
                         {
                            struct inotify_event event;
                            if (read(inot_fd, &event, sizeof(event)) == sizeof(event))
                              {
                                 if (event.wd == log_inot_wfd)
                                   {
                                      close(_log_fd);
                                      inotify_rm_watch(inot_fd, log_inot_wfd);
                                      _log_fd = -1;
                                      log_inot_wfd = -1;
                                   }
                              }
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
