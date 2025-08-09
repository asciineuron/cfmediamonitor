#ifndef MEDIAMONITOR_H
#define MEDIAMONITOR_H
#include <CoreServices/CoreServices.h>
#include <pthread.h>

// strlist owns each *str
struct strlist {
        char *str;
        struct strlist *next;
};

// TODO: make list head which can always be on stack, simplify
//struct strlist {
//	struct strnode *head;
//};
//
//typedef struct strnode* strlist;

void strlist_unique_append(struct strlist *list, char *str);
const char *strlist_get(struct strlist *list, size_t idx);

struct mm_filter {
	char *prog; // all concatenated together
        struct strlist *extensions;
};
void mm_filter_free(struct mm_filter *filter);

typedef struct mm_monitor {
        // user config:
	struct strlist *folders;
	struct mm_filter *filters;
        size_t num_filters;
	struct strlist *processed_files;
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

char *mm_monitor_print_status(mm_monitor *monitor);

// TODO use to see in gui if input valid:
int mm_monitor_filter_file(mm_monitor *monitor, const char *filename);

bool mm_filter_is_valid(const struct mm_filter *filter);

#endif
