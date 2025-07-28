#include "mediamonitor.h"
#include <stdlib.h>
#include <unistd.h>

/* commandline input: keep simple to one filter type
 *  $ mediamonitor -x prog -e extension -- folder1 folder2 ...
 * and args...
 */

const char USAGE[]
    = "command line invocation of mediamonitor, simple folder "
      "and file batch program handler\n Usage: mediamonitor -f "
      "folder1 folder2... -e extension1 extension2... -x prog\n";

void
print_usage ()
{
  fputs (USAGE, stdout);
}

int
main (int argc, char *argv[])
{
  char **folders = NULL;
  int num_folders = 0;
  char *prog_and_args = NULL;
  char **extensions = NULL;
  int num_extensions = 0;

  int opt;
  // all 2 require args
  while ((opt = getopt (argc, argv, "e:x:")) != -1)
    {
      switch (opt)
        {
        case 'e':
          extensions = malloc (1 * sizeof (char *));
          extensions[0] = optarg;
          num_extensions = 1;
          break;
        case 'x':
          prog_and_args = optarg;
          break;
        default:
          print_usage ();
          exit (EXIT_FAILURE);
        }
    }
  // process remaining args, all list of folders
  num_folders = argc - optind;
  folders = malloc (num_folders * sizeof (char *));
  for (int i = optind; i < argc; i++)
    {
      folders[i - optind] = argv[i];
    }

  mm_monitor *media_monitor = mm_monitor_init ();

  mm_monitor_add_folders (media_monitor, folders, num_folders);
  mm_monitor_add_filter (media_monitor, prog_and_args, extensions,
                         num_extensions);
  mm_monitor_start (media_monitor);

  // infinite loop until user quits
  char ch;
  printf ("Press 'q' to stop");
  while ((ch = getchar ()) != 'q')
    {
      if (ch == 's')
      {
        mm_monitor_print_status(media_monitor);
      }
    }

  mm_monitor_stop (media_monitor);
  mm_monitor_free (media_monitor);

  return EXIT_SUCCESS;
}
