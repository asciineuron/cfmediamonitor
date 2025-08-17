#ifndef MEDIAMONITOR_H
#define MEDIAMONITOR_H
#include <CoreServices/CoreServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>

struct mm_filter {
	CFStringRef prog;
	CFMutableArrayRef extensions; // CFString
	int refcount;
};

extern CFArrayCallBacks mm_filter_array_callbacks;

void mm_filter_free(struct mm_filter *filter);

typedef struct mm_monitor {
        // user config:
	CFMutableArrayRef folders; // of CFString
	CFMutableArrayRef filters; // of mm_filter, need to define equality and free() callbacks
	CFMutableArrayRef processed_files; // of CFString
	pthread_mutex_t lock; // for eventstream
        // internals:
	FSEventStreamRef event_stream;
	dispatch_queue_t dispatch_queue;
	bool is_running;
	bool is_stream_valid;
} mm_monitor;

// TODO later move to nonglobal instance, use socket to send event_stream data
// to listening server
mm_monitor *mm_monitor_init(void);
void mm_monitor_free(mm_monitor *monitor);
void mm_monitor_start(mm_monitor *monitor); // starts server and normal
void mm_monitor_pause(mm_monitor *monitor);
void mm_monitor_update(mm_monitor *monitor); // call after manipulating

//char *mm_monitor_print_status(mm_monitor *monitor);

// TODO use to see in gui if input valid:
int mm_monitor_filter_file(mm_monitor *monitor, CFStringRef filename);

bool mm_filter_is_valid(const struct mm_filter *filter);

#endif
