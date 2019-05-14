#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#define _POSIX_
#include <limits.h>

int
main(int argc, char **argv)
{
   int i, len, opt, id = -1, ret = 0, help = 0;
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

   /* Replace the current command line to hide the Exactness part */
   len = argv[argc - 1] + strlen(argv[argc - 1]) - argv[optind];
   memcpy(argv[0], argv[optind], len);
   memset(argv[0] + len, 0, _POSIX_PATH_MAX - len);

   for (i = optind; i < argc; i++)
     {
        if (i != optind)
          {
             argv[i - optind] = argv[0] + (argv[i] - argv[optind]);
          }
     }
   printf("Id: %d\n", id);

end:
   return ret;
}
