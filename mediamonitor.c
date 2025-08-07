#include "mediamonitor.h"
#include <CoreServices/CoreServices.h>
#include <dirent.h> // scandir
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// #define MM_SOCKET "/tmp/cmediamonitor.sock"

// update with realloc later...
#define MAX_FOLDERS 100
#define MAX_FILTERS 10
#define MAX_FILES 1000

typedef struct mm_monitor
{
  FSEventStreamRef event_stream;
  dispatch_queue_t dispatch_queue;
  mm_filter *filters;
  char **folders;
  char **processed_files;
  int num_folders;
  int num_filters;
  int num_processed;
  bool is_running;
  bool is_stream_valid;
} mm_monitor;

static void
check_resize_strarr (char **arr, int *max, int *cur, int add)
{
  if (cur + add > max)
    {
      *max *= 2;
      char **arr = realloc (arr, *max);
      if (!arr)
        {
          fprintf (stderr, "realloc failure");
          exit (EXIT_FAILURE);
        }
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
  for (int i = 0; i < filter->num_extensions; i++)
    {
      char *extension = strrchr (filename, '.');
      // if file does not have valid extension:
      if (!extension || strchr (extension, '/') != NULL)
        continue;
      if (strcmp (extension, filter->extensions[i]) == 0)
        return true;
    }
  return false;
}

int
mm_monitor_filter_file (mm_monitor *monitor, const char *filename)
{
  // if already seen, reject:
  for (int i = 0; i < monitor->num_processed; i++)
    {
      if (strcmp (monitor->processed_files[i], filename) == 0)
        return 0;
    }

  for (int i = 0; i < monitor->num_filters; i++)
    {
      if (filter_is_valid_extension (&monitor->filters[i], filename))
        return 1; // valid extension for some filter
    }
  // not valid, exclude from list
  return 0;
}

// don't need event id for timing yet
// NOTE also use this to update the tracked ones in monitor, and return those
// which are new
static char **
event_stream_get_new_files (mm_monitor *monitor, int *num_new_files,
                            size_t numEvents, char **eventPaths,
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
          // filter filetype
          if (!mm_monitor_filter_file (monitor, eventPaths[i]))
            continue;

          // add to processed files list:
          monitor->processed_files[monitor->num_processed] = strdup (eventPaths[i]);
          monitor->num_processed++;

          // add to new_files:
          new_files[cur_new_files] = eventPaths[i];
          cur_new_files++;
          if (cur_new_files >= current_max_size)
            {
              current_max_size *= 2;
              if (!(new_files = realloc (new_files, current_max_size)))
                {
                  fprintf (stderr, "realloc failure");
                  exit (EXIT_FAILURE);
                }
            }

          check_resize_strarr (new_files, &current_max_size, &cur_new_files,
                               1);
          new_files[cur_new_files] = eventPaths[i];
          ++cur_new_files;
        }
    }
  // TODO update tracked files
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
          if (!system (prog_and_file))
            {
              fprintf (stderr, "program returned nonzero exit status\n");
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
  mm_monitor *monitor = (mm_monitor *)clientCallBackInfo;

  char **paths = eventPaths;
  int num_new_files;
  char **new_files = event_stream_get_new_files (monitor, &num_new_files,
                                                 numEvents, paths, eventFlags);
  printf ("New files:\n");
  for (int i = 0; i < num_new_files; i++)
    {
      printf ("%s ", new_files[i]);
    }
  printf ("\n");

  for (int i = 0; i < monitor->num_filters; i++)
    {
      apply_filter (&monitor->filters[i], new_files, num_new_files);
    }
}

static void
mm_monitor_stop (mm_monitor *monitor)
{
  if (!monitor->event_stream)
    return;
  if (!monitor->is_running)
    return;
  FSEventStreamEventId latest_event
      = FSEventStreamGetLatestEventId (monitor->event_stream);
  printf ("Latest event: %llu\n", latest_event);
  printf ("stopping monitor\n");
  FSEventStreamStop (monitor->event_stream);
  FSEventStreamInvalidate (monitor->event_stream);
  FSEventStreamRelease (monitor->event_stream);
  monitor->is_stream_valid = false;
  monitor->is_running = false;
}

static void
mm_create_update_event_stream (mm_monitor *monitor)
{
  mm_monitor_stop (monitor);

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

  FSEventStreamSetDispatchQueue (monitor->event_stream,
                                 monitor->dispatch_queue);

  monitor->is_stream_valid = true;
}

static int num_created = 0;
const char dispatch_name[] = "com.asciineuron.mediamonitorqueue";

mm_monitor *
mm_monitor_init (void)
{
  // TODO static for now, maybe eventually create in-place?
  // everything pointer type so memset ok:
  mm_monitor *monitor = malloc (sizeof (mm_monitor));
  memset (monitor, 0, sizeof (mm_monitor));

  monitor->folders = malloc (MAX_FOLDERS * sizeof (char *));
  monitor->processed_files = malloc (MAX_FILES * sizeof (char *));
  monitor->filters = malloc (MAX_FILTERS * sizeof (mm_filter *));

  char numbered_dispatch_name[50];
  sprintf (numbered_dispatch_name, "%s-%d", dispatch_name, num_created);
  num_created++;

  monitor->dispatch_queue
      = dispatch_queue_create (numbered_dispatch_name, DISPATCH_QUEUE_SERIAL);

  mm_create_update_event_stream (monitor);
  monitor->is_running = false;

  return monitor;
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
      mm_filter_free (&monitor->filters[i]);
    }
  for (int i = 0; i < monitor->num_processed; i++)
    {
      free (monitor->processed_files[i]);
    }
}

void
mm_monitor_start (mm_monitor *monitor)
{
  if (monitor->is_running)
    return;
  if (!monitor->is_stream_valid)
    mm_create_update_event_stream (monitor);

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
  if (!monitor->is_running)
    return;
  printf ("pausing monitor\n");
  FSEventStreamStop (monitor->event_stream);
}

void
mm_monitor_add_folders (mm_monitor *monitor, char const *const *folders, int num_folders)
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

void mm_monitor_change_folder (mm_monitor *monitor, const char *new_folder, int folder_idx)
{
  if (folder_idx >= monitor->num_folders) {
    fprintf(stderr, "folder index %d is out of range for monitor", folder_idx);
    exit(EXIT_FAILURE);
  }
  if (strcmp(new_folder, monitor->folders[folder_idx]) == 0) {
    return;
  }
  free(monitor->folders[folder_idx]);
  monitor->folders[folder_idx] = strdup(new_folder);
}

void
mm_monitor_remove_folders (mm_monitor *monitor, char **folders, int num_folder)
{
  // find idx of all that match, remove those and flatten, shift left
  
}

void
mm_monitor_add_filter (mm_monitor *monitor, char *prog_and_args,
                       char **extensions, int num_extensions)
{
  mm_monitor_stop (monitor);

  bool found = false;
  for (int i = 0; i < monitor->num_filters; i++)
    {
      if (strcmp (monitor->filters[i].prog, prog_and_args) == 0)
        {
          found = true;
          break;
        }
    }
  if (!found)
    {
      // monitor->filters[monitor->num_filters] = malloc (sizeof (mm_filter
      // *));
      // TODO check realloc here
      monitor->filters[monitor->num_filters].prog = strdup (prog_and_args);
      monitor->filters[monitor->num_filters].extensions
          = malloc (num_extensions * sizeof (char *));
      for (int i = 0; i < num_extensions; i++)
        {
          monitor->filters[monitor->num_filters].extensions[i]
              = strdup (extensions[i]);
        }
      monitor->num_filters++;
    }

  mm_create_update_event_stream (monitor);
  mm_monitor_start (monitor);
}

void mm_monitor_add_filters(mm_monitor *monitor, mm_filter *filter, int num_filters)
{
  // skipping checking equality allows adding multiple defaults then editing, maybe we don't want this?
}

void mm_monitor_change_filter (mm_monitor *monitor, mm_filter *new_filter, int filter_idx)
{
  if (filter_idx >= monitor->num_filters)
  {
    fprintf(stderr, "folder index %d is out of range for monitor", filter_idx);
    exit(EXIT_FAILURE);
  }
  // skip verifying equality since easier to just re-add
  mm_filter_free(&monitor->filters[filter_idx]); 
  // filter is all lightweight pointers, not resources, so copy ok, transferring ownership here, hence no const
  monitor->filters[filter_idx] = *new_filter;
}

void
mm_monitor_remove_filter (mm_monitor *monitor, char *prog_and_args)
{
}

void
mm_monitor_print_status (mm_monitor *monitor)
{
  printf ("Watching folders:\n");
  for (int i = 0; i < monitor->num_folders; i++)
    {
      printf ("%s ", monitor->folders[i]);
    }
  printf ("Filters:\n");
  for (int i = 0; i < monitor->num_filters; i++)
    {
      printf ("%s ", monitor->filters[i].prog);
    }
}

mm_filter const *
mm_monitor_get_filters (mm_monitor *monitor, size_t *num_filters)
{
  *num_filters = monitor->num_filters;
  return monitor->filters;
}

char const *const *
mm_monitor_get_folders (mm_monitor *monitor, size_t *num_folders)
{
  *num_folders = monitor->num_folders;
  return monitor->folders;
}

char const *const *
mm_monitor_get_processed_files (mm_monitor *monitor, size_t *num_processed)
{
  *num_processed = monitor->num_processed;
  return monitor->processed_files;
}

bool
mm_filter_is_valid (const mm_filter *filter)
{
  // need prog to be real executable file
  // and each extension to be '.SO.METHI..NG.with.dots\0'
  struct stat prog_stat;
  if (stat (filter->prog, &prog_stat))
    return false;
  if (!S_ISREG (prog_stat.st_mode))
    return false;
  if (!(prog_stat.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
    return false;
  // program ok

  for (int i = 0; i < filter->num_extensions; i++)
    {
      if ((strcmp (filter->extensions[i], ".") == 0)
          || (strcmp (filter->extensions[i], "..") == 0))
        return false;
      if (filter->extensions[i][0] != '.')
        return false;
    }
  // all valid extensions

  return true;
}
