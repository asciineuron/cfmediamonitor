#include "mediamonitor.h"
#include <CoreServices/CoreServices.h>
#include <dirent.h> // scandir
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// #define MM_SOCKET "/tmp/cmediamonitor.sock"

// update with realloc later...
#define MAX_FOLDERS 100
#define MAX_FILTERS 10
#define MAX_FILES 1000

typedef struct mm_filter
{
  char *prog; // all concatenated together
  char **extensions;
  int num_extensions;
} mm_filter;

typedef struct mm_monitor
{
  FSEventStreamRef event_stream;
  dispatch_queue_t dispatch_queue;
  mm_filter **filters;
  char **folders;
  char **processed_files;
  int num_folders;
  int num_filters;
  int num_processed;
} mm_monitor;

static mm_monitor media_monitor;

static void check_resize_strarr(char** arr, int *max, int *cur, int add)
{
  if (cur + add > max)
  {
    *max *= 2;
    char **temp = realloc(arr, *max);
    if (!temp)
    {
      fprintf(stderr, "realloc failure");
      exit(EXIT_FAILURE);
    }
    arr = temp;
  }
}

static void
mm_filter_free (mm_filter *filter)
{
  free (filter->prog);
  for (int i = 0; i < filter->num_extensions; i++)
    {
      free (filter->extensions[i]);
    }
}

static bool
filter_is_valid_extension (const mm_filter *filter, const char *filename)
{
  for (int j = 0; j < filter->num_extensions; j++)
    {
      char *extension = strrchr (filename, '.');
      // if file does not have valid extension:
      if (!extension || strchr (extension, '/') != NULL)
        continue;
      if (strcmp (extension, filter->extensions[j]) == 0)
        return true;
    }
  return false;
}

static int
mm_monitor_dirent_filter (const struct dirent *entry)
{
  for (int i = 0; i < media_monitor.num_filters; i++)
    {
      if (filter_is_valid_extension (media_monitor.filters[i],
                                     entry->d_name))
        return 0;
    }
  // not valid, exclude from list
  return 1;
}

// Do actual filetype filtering as well via mm_monitor_dirent_filter
static char **
this_dir_scan (char *dir)
{
  struct dirent **namelist;
  int num_entries
      = scandir (dir, &namelist, mm_monitor_dirent_filter, alphasort);
  if (num_entries == -1)
  {
    perror("scandir failure");
    exit(EXIT_FAILURE);
  }
  char **filtered_names = malloc (num_entries * sizeof (char *));
  for (int i = 0; i < num_entries; i++)
    {
      filtered_names[i] = strdup (namelist[i]->d_name);
      if (!filtered_names[i])
        {
          fprintf (stderr, "strdup failure\n");
          exit (EXIT_FAILURE);
        }
    }
  return filtered_names;
}

static char **
full_subdir_scan ()
{
  return NULL;
}

// don't need event id for timing yet
static char **
event_stream_get_new_files (int *num_new_files, size_t numEvents,
                            char **eventPaths,
                            const FSEventStreamEventFlags *eventFlags)
{
  // loop through paths with 'created' flag, check if not found in
  // media_monitor list 
  int current_max_size = numEvents;
  char **new_files = malloc (current_max_size * sizeof (char *));
  int cur_new_files = 0;

  for (int i = 0; i < numEvents; i++)
    {
      printf ("handling %s\n", eventPaths[i]);
      if (!(eventFlags[i] & kFSEventStreamEventFlagItemCreated))
        {
          printf ("not a file created event...\n");
          continue;
        }
      if (eventFlags[i] & kFSEventStreamEventFlagItemIsFile)
        {
          // add to new_files:
          check_resize_strarr (new_files, &current_max_size, &cur_new_files,
                               1);
          new_files[cur_new_files] = eventPaths[i];
          ++cur_new_files;
        }
      else
        {
          // otherwise scan the whole dir: likely not needed with
          // kFSEventStreamCreateFlagFileEvents
          char **dir_new_files = NULL;
          int dir_cur_new_files = 0;
          if (eventFlags[i] & kFSEventStreamEventFlagMustScanSubDirs)
            {
              // nftw() else could use scandir()
              printf ("subdir scan not yet implemented\n");
              // dir_new_files = full_subdir_scan ();
            }
          else
            {
              dir_new_files = this_dir_scan (eventPaths[i]);
            }

          // append to new_files, grow if necessary
          check_resize_strarr (new_files, &current_max_size, &cur_new_files,
                               dir_cur_new_files);
          // resized, now add the elems
          for (int j = 0; j < dir_cur_new_files; j++)
            {
              new_files[cur_new_files] = dir_new_files[j];
              ++cur_new_files;
            }
        }
    }

  *num_new_files = cur_new_files;
  return new_files;
}

// expects files to be realpath'd already
static int
apply_filter (const mm_filter *filter, char **files, int num_files)
{
  for (int i = 0; i < num_files; ++i)
    {
      if (filter_is_valid_extension (filter, files[i]))
        {
          // extension match for this file, execute:
          // NOTE only on one file at a time for now
          char *prog_and_file = malloc (
              sizeof (char) * (1 + strlen (filter->prog) + strlen (files[i])));
          strcpy (prog_and_file, filter->prog);
          strcat (prog_and_file, " "); // + 1
          strcat (prog_and_file, files[i]);
          printf ("Running: %s", prog_and_file);
          if (!system (prog_and_file)) {
            fprintf(stderr, "program returned nonzero exit status\n");
          }
        }
    }
  return 0;
}

static void
event_stream_callback (ConstFSEventStreamRef streamRef,
                       void *clientCallBackInfo, size_t numEvents,
                       void *eventPaths,
                       const FSEventStreamEventFlags *eventFlags,
                       const FSEventStreamEventId *eventIds)
{
  // loop over events, filter created, and not in already-processed list of
  // media monitor
  char **paths = eventPaths;
  int num_new_files;
  char **new_files = event_stream_get_new_files (&num_new_files, numEvents,
                                                 paths, eventFlags);
  printf("New files:\n");
  for (int i = 0; i < num_new_files; i++) {
    printf("%s ", new_files[i]);
  }
  printf("\n");

  for (int i = 0; i < media_monitor.num_filters; i++)
    {
      apply_filter (media_monitor.filters[i], new_files, num_new_files);
    }
}

static void
close_event_stream (mm_monitor *monitor)
{
  if (!monitor->event_stream)
    return;
  FSEventStreamEventId latest_event
      = FSEventStreamGetLatestEventId (monitor->event_stream);
  printf ("Latest event: %llu\n", latest_event);

  FSEventStreamStop (monitor->event_stream);
  FSEventStreamInvalidate (monitor->event_stream);
  FSEventStreamRelease (monitor->event_stream);
}

static void
mm_create_update_event_stream (mm_monitor *monitor)
{
  close_event_stream (monitor);

  CFMutableArrayRef paths = CFArrayCreateMutable (NULL, 0, NULL);
  // build CFArray:
  for (int i = 0; i < monitor->num_folders; i++)
    {
      CFStringRef folder = CFStringCreateWithCString (
          kCFAllocatorDefault, monitor->folders[i], kCFStringEncodingUTF8);

      CFArrayAppendValue (paths, folder);
    }

  CFAbsoluteTime latency = 3.0;
  // TODO track time and not just since now. First, just process things
  // incoming while currently running, compare against found file names, no
  // timestamp necessary
  monitor->event_stream = FSEventStreamCreate (
      NULL, &event_stream_callback, NULL, paths, kFSEventStreamEventIdSinceNow,
      latency, kFSEventStreamCreateFlagFileEvents);
  // kFSEventStreamCreateFlagNone
  FSEventStreamSetDispatchQueue (monitor->event_stream,
                                 monitor->dispatch_queue);
}

mm_monitor *
mm_monitor_init ()
{
  // TODO static for now, maybe eventually create in-place?
  // everything pointer type so memset ok:
  memset (&media_monitor, 0, sizeof (media_monitor));

  media_monitor.folders = malloc (MAX_FOLDERS * sizeof (char *));
  media_monitor.processed_files = malloc (MAX_FILES * sizeof (char *));
  media_monitor.filters = malloc (MAX_FILTERS * sizeof (mm_filter *));

  media_monitor.dispatch_queue = dispatch_queue_create (
      "com.asciineuron.mediamonitorqueue", DISPATCH_QUEUE_SERIAL);

  mm_create_update_event_stream (&media_monitor);

  return &media_monitor;
}

void
mm_monitor_free (mm_monitor *monitor)
{
  mm_monitor_stop (monitor);
  dispatch_release (monitor->dispatch_queue);
  for (int i = 0; i < monitor->num_folders; i++)
    {
      free (monitor->folders[i]);
    }
  for (int i = 0; i < monitor->num_filters; i++)
    {
      mm_filter_free (monitor->filters[i]);
    }
  for (int i = 0; i < monitor->num_processed; i++)
    {
      free (monitor->processed_files[i]);
    }
}

void
mm_monitor_start (mm_monitor *monitor)
{
  // ensure stopped first:
  mm_monitor_pause (monitor);
  printf ("starting monitor\n");
  if (!FSEventStreamStart (monitor->event_stream))
    {
      fprintf (stderr, "failed to start event stream\n");
      exit (EXIT_FAILURE);
    }
}

void
mm_monitor_pause (mm_monitor *monitor)
{
  printf ("pausing monitor\n");
  FSEventStreamStop (monitor->event_stream);
}

void
mm_monitor_stop (mm_monitor *monitor)
{
  printf ("stopping monitor\n");
  FSEventStreamStop (monitor->event_stream);
  FSEventStreamInvalidate (monitor->event_stream);
  FSEventStreamRelease (monitor->event_stream);
}

void
mm_monitor_add_folders (mm_monitor *monitor, char **folders, int num_folders)
{
  mm_monitor_stop (monitor);

  for (int i = 0; i < num_folders; i++)
    {
      bool found = false;
      for (int j = 0; j < monitor->num_folders; j++)
        {
          if (strcmp (folders[i], monitor->folders[j]) == 0)
            {
              found = true;
              break;
            }
        }
      if (!found)
        {
          // TODO resize later
          monitor->folders[monitor->num_folders] = strdup (folders[i]);
          monitor->num_folders++;
        }
    }

  mm_create_update_event_stream (monitor);
  mm_monitor_start (monitor);
}

void
mm_monitor_remove_folders (mm_monitor *monitor, char **folders, int num_folder)
{
}

void
mm_monitor_add_filter (mm_monitor *monitor, char *prog_and_args,
                       char **extensions, int num_extensions)
{
  mm_monitor_stop (monitor);

  bool found = false;
  for (int i = 0; i < monitor->num_filters; i++)
    {
      if (strcmp (monitor->filters[i]->prog, prog_and_args) == 0)
        {
          found = true;
          break;
        }
    }
  if (!found)
    {
      monitor->filters[monitor->num_filters] = malloc (sizeof (mm_filter *));
      monitor->filters[monitor->num_filters]->prog = strdup (prog_and_args);
      monitor->filters[monitor->num_filters]->extensions = malloc (num_extensions * sizeof (char*));
      for (int i = 0; i < num_extensions; i++)
        {
          monitor->filters[monitor->num_filters]->extensions[i] = strdup (extensions[i]);
        }
      monitor->num_filters++;
    }

  mm_create_update_event_stream (monitor);
  mm_monitor_start (monitor);
}

void
mm_monitor_remove_filter (mm_monitor *monitor, char *prog_and_args)
{
}

void mm_monitor_print_status(mm_monitor *monitor)
{
  printf("Watching folders:\n");
  for (int i = 0; i < monitor->num_folders; i++)
  {
    printf("%s ", monitor->folders[i]);
  }
  printf("Filters:\n");
  for (int i = 0; i < monitor->num_filters; i++) {
    printf("%s ", monitor->filters[i]->prog);
  }
}
