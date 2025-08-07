#ifndef MEDIAMONITOR_H
#define MEDIAMONITOR_H
#include <CoreServices/CoreServices.h>

typedef struct mm_filter
{
  char *prog; // all concatenated together
  char **extensions;
  int num_extensions;
} mm_filter;

typedef struct mm_monitor mm_monitor;

// TODO later move to nonglobal instance, use socket to send event_stream data
// to listening server
mm_monitor *mm_monitor_init (void);
void mm_monitor_free (mm_monitor *monitor);
void mm_monitor_start (mm_monitor *monitor); // starts server and normal
void mm_monitor_pause (mm_monitor *monitor);

void mm_monitor_add_folders (mm_monitor *monitor, char const *const *folders,
                             int num_folders);
void mm_monitor_remove_folders (mm_monitor *monitor, char **folders,
                                int num_folders);
void mm_monitor_change_folder (mm_monitor *monitor, const char *new_folder,
                               int folder_idx);

void mm_monitor_add_filter (mm_monitor *monitor, char *prog_and_args,
                            char **extensions, int num_extensions);
void mm_monitor_add_filters (mm_monitor *monitor, mm_filter *filter,
                             int num_filters);
void mm_monitor_remove_filter (mm_monitor *monitor, char *prog_and_args);
void mm_monitor_change_filter (mm_monitor *monitor, mm_filter *new_filter,
                               int filter_idx);

void mm_monitor_print_status (mm_monitor *monitor);
// TODO use to see in gui if input valid:
int mm_monitor_filter_file (mm_monitor *monitor, const char *filename);

char const *const *mm_monitor_get_folders (mm_monitor *monitor,
                                           size_t *num_folders);
mm_filter const *mm_monitor_get_filters (mm_monitor *monitor,
                                         size_t *num_filters);
// TODO update to track if actually processed or skipped, or combine with
// filter_file()
char const *const *mm_monitor_get_processed_files (mm_monitor *monitor,
                                                   size_t *num_processed);
bool mm_filter_is_valid (const mm_filter *filter);

#endif
