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

/*
 * replace getters/setters with eg:
 * pthread_mutex_lock(monitor->lock);
 * strlist_unique_append(monitor->folders, folder);
 * pthread_mutex_unlock(monitor->lock);
 * mm_monitor_update(monitor);
 */

static void die(char *msg)
{
	fprintf(stderr, msg);
	exit(EXIT_FAILURE);
}

void strlist_unique_append(struct strlist *list, char *str)
{
	if (!list)
		return;
	while (list->next != NULL && (strcmp(str, list->str) != 0))
		list = list->next;
	if (strcmp(str, list->str) == 0) 
		return
	struct strlist *tail = malloc(sizeof(struct strlist));
	*tail = {.str = strdup(str), .next = NULL};
	list->next = tail;
}

struct strlist *strlist_push(struct strlist *list, const char *str)
{
	if (!list)
		return NULL;
	struct strlist *head = malloc(sizeof(struct strlist));
	*head = {.str = strdup(str), .next = list};
	return head;
	//if (!list)
	//	return NULL;
	//struct strlist *last = list;
	//while (last->next != NULL)
	//	last = last->next;
	//struct strlist *pushed = malloc(sizeof(struct strlist));
	//*pushed = { .str = strdup(str), .next = NULL };
	//last->next = pushed;
	//return list;
}

const char *strlist_get(struct strlist *list, size_t idx)
{
	if (!list)
		return NULL;
	while (list->next != NULL && idx > 0) {
		list = list->next;
		--idx;
	}
	if (idx != 0)
		return NULL;
	return list->str;
}

size_t strlist_find(struct strlist *list, const char *search)
{
	if (!list)
		return NULL;
	size_t idx = 0;
	while (list->next != NULL && (strcmp(list->str, search) != 0)) {
		list = list->next;
		++idx;
	}
	if (strcmp(list->str, search) == 0)
		return idx;
	return -1;
}

size_t strlist_len(struct strlist *list)
{
	if (!list)
		return 0;
	size_t len;
	while (list->next != NULL) {
		list = list->next;
		++len;
	}
	return len;
}

void strlist_free(struct strlist *list)
{
	if (!list)
		return;
	// NOTE: if on stack, can't free head, so skip first elem and leave to caller
	struct strlist *tmp = list->next;
	while (tmp != NULL) {
		struct strtmp *prev = tmp;
		tmp = tmp->next;
		free(prev->str);
		free(prev);
		prev = NULL;
	}
	list->next = NULL;
}

void mm_filter_free(struct mm_filter *filter)
{
	free(filter->prog);
	strlist_free(filter->extensions);
	//free(filter->extensions);
}

static bool filter_is_valid_extension(const struct mm_filter *filter,
				      const char *filename)
{
	for (int i = 0; i < strlist_len(filter->extensions); i++) {
		char *extension = strrchr(filename, '.');
		// if file does not have valid extension:
		if (!extension || strchr(extension, '/') != NULL)
			continue;
		if (strcmp(extension, strlist_get(filter->extensions, i)) == 0)
			return true;
	}
	return false;
}

int mm_monitor_filter_file(mm_monitor *monitor, const char *filename)
{
	// if already seen, reject:
	for (int i = 0; i < strlist_len(monitor->processed_files); i++) {
		if (strcmp(strlist_get(monitor->processed_files, i),
			   filename) == 0)
			return 0;
	}

	for (int i = 0; i < monitor->num_filters; i++) {
		if (filter_is_valid_extension(&monitor->filters[i], filename))
			return 1; // valid extension for some filter
	}
	// not valid, exclude from list
	return 0;
}

// don't need event id for timing yet
// NOTE also use this to update the tracked ones in monitor, and return those
// which are new
static struct strlist *
event_stream_get_new_files(mm_monitor *monitor, size_t numEvents,
			   char **eventPaths,
			   const FSEventStreamEventFlags *eventFlags)
{
	// loop through paths with 'created' flag, check if not found in
	// media_monitor list
	struct strlist *new_files = malloc(sizeof(struct strlist));

	for (int i = 0; i < numEvents; i++) {
		printf("handling %s\n", eventPaths[i]);
		if (!(eventFlags[i] & kFSEventStreamEventFlagItemCreated)) {
			printf("not a file created event...\n");
			continue;
		}

		if (eventFlags[i] & kFSEventStreamEventFlagItemIsFile) {
			// filter filetype
			if (!mm_monitor_filter_file(monitor, eventPaths[i]))
				continue;

			// add to processed files list:
			strlist_push(monitor->processed_files, eventPaths[i]);
			// add to new_files:
			strlist_push(new_files, eventPaths[i]);
			free(eventPaths[i]); // do we own this?
		}
	}
	// TODO update tracked files
	return new_files;
}

// expects files to be realpath'd already
//static int apply_filter(const mm_filter *filter, char **files, int num_files)
static int apply_filter(const struct mm_filter *filter, const struct strlist *files)
{
	bool failed = false;
	for (struct strlist *file = files; file != NULL; file = file->next) {
		const char *filestr = file->str;
		if (filter_is_valid_extension(filter, filestr)) {
			// extension match for this file, execute:
			// NOTE only on one file at a time for now
			char *prog_and_file = malloc(
				sizeof(char) *
				(1 + strlen(filter->prog) + strlen(filestr)));
			strcpy(prog_and_file, filter->prog);
			strcat(prog_and_file, " "); // + 1
			strcat(prog_and_file, filestr);
			printf("Running: %s", prog_and_file);
			if (!system(prog_and_file)) {
				fprintf(stderr,
					"program returned nonzero exit status\n");
				failed = true;
			}
			free(prog_and_file);
		}
	}
	return failed ? -1 : 0;
}

static void event_stream_callback(ConstFSEventStreamRef streamRef,
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
	struct strlist *new_files = event_stream_get_new_files(monitor, numEvents, paths,
						      eventFlags);
	printf("New files:\n");
	for (int i = 0; i < num_new_files; i++) {
		printf("%s ", new_files[i]);
	}
	printf("\n");

	for (int i = 0; i < monitor->num_filters; i++) {
		apply_filter(&monitor->filters[i], new_files, num_new_files);
	}

	strlist_free(new_files);
	// need to free heap head separately
	free(new_files);
}

static void mm_monitor_stop(mm_monitor *monitor)
{
	if (!monitor->event_stream)
		return;
	if (!monitor->is_running || !monitor->is_stream_valid)
		return;
	FSEventStreamEventId latest_event =
		FSEventStreamGetLatestEventId(monitor->event_stream);
	printf("Latest event: %llu\n", latest_event);
	printf("stopping monitor\n");
	FSEventStreamStop(monitor->event_stream);
	FSEventStreamInvalidate(monitor->event_stream);
	FSEventStreamRelease(monitor->event_stream);
	monitor->is_stream_valid = false;
	monitor->is_running = false;
}

static void mm_create_update_event_stream(mm_monitor *monitor)
{
	mm_monitor_stop(monitor);

	CFMutableArrayRef paths = CFArrayCreateMutable(NULL, 0, NULL);
	// build CFArray:
	pthread_mutex_lock(&monitor->lock);
	for (int i = 0; i < monitor->num_folders; i++) {
		CFStringRef folder = CFStringCreateWithCString(
			kCFAllocatorDefault, monitor->folders[i],
			kCFStringEncodingUTF8);

		CFArrayAppendValue(paths, folder);
	}
	pthread_mutex_unlock(&monitor->lock);

	CFAbsoluteTime latency = 3.0;
	// TODO track time and not just since now. First, just process things
	// incoming while currently running, compare against found file names, no
	// timestamp necessary
	monitor->event_stream =
		FSEventStreamCreate(NULL, &event_stream_callback, NULL, paths,
				    kFSEventStreamEventIdSinceNow, latency,
				    kFSEventStreamCreateFlagFileEvents);

	FSEventStreamSetDispatchQueue(monitor->event_stream,
				      monitor->dispatch_queue);

	monitor->is_stream_valid = true;
}


void mm_monitor_update(mm_monitor *monitor)
{
	mm_create_update_event_stream(monitor);
}

static int num_created = 0;
const char dispatch_name[] = "com.asciineuron.mediamonitorqueue";

mm_monitor *mm_monitor_init(void)
{
	// TODO static for now, maybe eventually create in-place?
	// everything pointer type so memset ok:
	mm_monitor *monitor = malloc(sizeof(mm_monitor));
	memset(monitor, 0, sizeof(mm_monitor));

	monitor->folders = malloc(MAX_FOLDERS * sizeof(char *));
	monitor->processed_files = malloc(MAX_FILES * sizeof(char *));
	monitor->filters = malloc(MAX_FILTERS * sizeof(struct mm_filter *));

	char numbered_dispatch_name[50];
	sprintf(numbered_dispatch_name, "%s-%d", dispatch_name, num_created);
	num_created++;

	monitor->dispatch_queue = dispatch_queue_create(numbered_dispatch_name,
							DISPATCH_QUEUE_SERIAL);

	pthread_mutex_init(monitor->lock, NULL);

	mm_create_update_event_stream(monitor);
	monitor->is_running = false;

	return monitor;
}

void mm_monitor_free(mm_monitor *monitor)
{
	mm_monitor_stop(monitor);
	dispatch_release(monitor->dispatch_queue);
	pthread_mutex_lock(&monitor->lock);
	for (int i = 0; i < monitor->num_folders; i++) {
		free(monitor->folders[i]);
	}
	for (int i = 0; i < monitor->num_filters; i++) {
		mm_filter_free(&monitor->filters[i]);
	}
	for (int i = 0; i < monitor->num_processed; i++) {
		free(monitor->processed_files[i]);
	}
	pthread_mutex_unlock(&monitor->lock);
	pthread_mutex_destroy(&monitor->lock);
}

void mm_monitor_start(mm_monitor *monitor)
{
	if (!monitor || monitor->is_running)
		return;
	if (!monitor->is_stream_valid)
		mm_create_update_event_stream(monitor);

	printf("starting monitor\n");
	if (!FSEventStreamStart(monitor->event_stream)) {
		fprintf(stderr, "failed to start event stream\n");
		exit(EXIT_FAILURE);
	}
}

void mm_monitor_pause(mm_monitor *monitor)
{
	if (!monitor->is_running)
		return;
	printf("pausing monitor\n");
	FSEventStreamStop(monitor->event_stream);
}

void mm_monitor_update(mm_monitor *monitor)
{
	bool was_running = monitor->is_running;
	mm_create_update_event_stream(monitor);
	if (was_running)
		mm_monitor_start(monitor);
}

static char *str_resize(char *str, int testlen, int alloc)
{
	if (testlen >= alloc) {
		alloc *= 2;
		if (!(str = realloc(str, alloc)))
			die("realloc failure");
	}
	return str;
}

char *mm_monitor_print_status(mm_monitor *monitor)
{
	pthread_mutex_lock(&monitor->lock);
	int alloc = 1000;
	char *output = malloc(alloc);
	int len = 0;
	len += sprintf(output, "Watching folders:\n");

	for (struct strlist *folder = monitor->folders; folder != NULL;
	     folder = folder->next) {
		const char *folderstr = folder->str;
		output = str_resize(output, strlen(folderstr) + 1 + len, alloc);
		len += snprintf(output, alloc - len, "%s\n",
				monitor->folders[i]);
	}

	const char[] filter_header = "Filters:\nprog\nextensions";
	if (strlen(filter_header) + len >= alloc) {
		alloc += 1000;
		if (!(output = realloc(output, alloc)))
			die("realloc failure");
	}
	len += sprintf(output, filter_header);

	for (int i = 0; i < monitor->num_filters; i++) {
		struct mm_filter *filter = monitor->filters[i];
		output = str_resize(output, strlen(filter.prog) + 2 + len,
				    alloc); // 2 = \n\n
		len += snprintf(output, alloc - len, "\n%s\n", filter->prog);

		for (struct strlist *ext = filter->extensions; ext != NULL;
		     ext = ext->next) {
			const char *extstr = ext->str;
			output = str_resize(output, strlen(extstr) + 1 + len,
					    alloc);
			len += snprintf(output, alloc - len, "%s ", extstr);
		}
	}

	pthread_mutex_unlock(&monitor->lock);
	output[len] = '\0';
	return output;
}

bool mm_filter_is_valid(const struct mm_filter *filter)
{
	// need prog to be real executable file
	// and each extension to be '.SO.METHI..NG.with.dots\0'
	struct stat prog_stat;
	if (stat(filter->prog, &prog_stat))
		return false;
	if (!S_ISREG(prog_stat.st_mode))
		return false;
	if (!(prog_stat.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
		return false;
	// program ok

	for (int i = 0; i < filter->num_extensions; i++) {
		if ((strcmp(filter->extensions[i], ".") == 0) ||
		    (strcmp(filter->extensions[i], "..") == 0))
			return false;
		if (filter->extensions[i][0] != '.')
			return false;
	}
	// all valid extensions

	return true;
}
