#ifndef MEDIAMONITOR_H
#define MEDIAMONITOR_H
#include <CoreServices/CoreServices.h>

typedef struct mm_filter mm_filter;
typedef struct mm_monitor mm_monitor;

// TODO later move to nonglobal instance, use socket to send event_stream data
// to listening server
mm_monitor *mm_monitor_init ();
void mm_monitor_free (mm_monitor *monitor);
void mm_monitor_start (mm_monitor *monitor); // starts server and normal
void mm_monitor_pause (mm_monitor *monitor);
void mm_monitor_add_folders (mm_monitor *monitor, char **folders,
                             int num_folders);
void mm_monitor_remove_folders (mm_monitor *monitor, char **folders,
                                int num_folders);
void mm_monitor_add_filter (mm_monitor *monitor, char *prog_and_args,
                            char **extensions, int num_extensions);
void mm_monitor_remove_filter (mm_monitor *monitor, char *prog_and_args);
void mm_monitor_print_status (mm_monitor *monitor);
// TODO use to see in gui if input valid:
int mm_monitor_filter_file (mm_monitor *monitor, const char *filename);

#endif
