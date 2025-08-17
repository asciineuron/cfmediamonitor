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
 *
 * kCFStringEncodingASCII vs kCFStringEncodingUTF8?
 */

static void die(char *msg)
{
	fprintf(stderr, "%s", msg);
	exit(EXIT_FAILURE);
}

void mm_filter_free(struct mm_filter *filter)
{
	CFRelease(filter->prog);
	for (CFIndex i = 0; i < CFArrayGetCount(filter->extensions); ++i) {
		CFRelease(CFArrayGetValueAtIndex(filter->extensions, i));
	}
}

void mm_filter_release_callback(CFAllocatorRef allocator, const void *value)
{
	// TODO : potential problem if held by multiple people and released? Do we need ref count?
	// shouldn't use array allocator...
	struct mm_filter *filter = (struct mm_filter *)value;
	filter->refcount--;
	if (filter->refcount == 0)
		mm_filter_free(filter);
}

const void *mm_filter_retain_callback(CFAllocatorRef allocator,
				      const void *value)
{
	// to respect cf stuff increase retain count so callers don't suddenly get data deleted
	struct mm_filter *filter = (struct mm_filter *)value;
	filter->refcount++;
	CFRetain(filter->prog);
	for (CFIndex i = 0; i < CFArrayGetCount(filter->extensions); ++i) {
		CFRetain(CFArrayGetValueAtIndex(filter->extensions, i));
	}
	return value;
}

unsigned char mm_filter_equal_callback(const void *filter1, const void *filter2)
{
	struct mm_filter *filter_a = (struct mm_filter *)filter1;
	struct mm_filter *filter_b = (struct mm_filter *)filter2;

	if (CFStringCompare(filter_a->prog, filter_b->prog, 0) !=
	    kCFCompareEqualTo)
		return 0;

	CFIndex len = CFArrayGetCount(filter_a->extensions);
	if (len != CFArrayGetCount(filter_b->extensions))
		return 0;

	for (CFIndex i = 0; i < len; i++) {
		if (CFStringCompare(
			    CFArrayGetValueAtIndex(filter_a->extensions, i),
			    CFArrayGetValueAtIndex(filter_b->extensions, i),
			    0) != kCFCompareEqualTo)
			return 0;
	}
	// prog, extensions length, and each extension all equal
	return 1;
}

// defines string repr, equality, release (free),
// retain (add from ptr so copy or clone), version
CFArrayCallBacks mm_filter_array_callbacks = {
	.copyDescription = NULL,
	.equal = &mm_filter_equal_callback,
	.release = &mm_filter_release_callback,
	.retain = &mm_filter_retain_callback,
	.version = 0
};

static bool file_has_valid_extension(const struct mm_filter *filter,
				     CFStringRef filename)
{
	const char *filestr =
		CFStringGetCStringPtr(filename, kCFStringEncodingASCII);
	// whether filename is some valid extension registered in filter
	for (CFIndex i = 0; i < CFArrayGetCount(filter->extensions); ++i) {
		char *extension = strrchr(filestr, '.');
		// if file does not have valid extension:
		if (!extension || strchr(extension, '/') != NULL)
			continue;

		CFStringRef ext_strref = CFArrayGetValueAtIndex(filter->extensions, i);
		const char *ext = CFStringGetCStringPtr(ext_strref,
							kCFStringEncodingASCII);
		if (ext) {
			if (strcmp(extension, ext) == 0)
				return true;
		} else {
			CFIndex ext_len = CFStringGetLength(ext_strref) + 1; // +1='\0'
			char *ext = malloc(ext_len);
			if (!CFStringGetCString(ext_strref, ext, ext_len, kCFStringEncodingASCII))
				die("unable to obtain extension string");
			if (strcmp(extension, ext) == 0) {
				free(ext);
				return true;
			}
			free(ext);
		}

	}
	return false;
}

int mm_monitor_filter_file(mm_monitor *monitor, CFStringRef filename)
{
	// if already seen, reject:
	if (CFArrayContainsValue(
		    monitor->processed_files,
		    CFRangeMake(0, CFArrayGetCount(monitor->processed_files)),
		    filename))
		return 0;

	for (CFIndex i = 0; i < CFArrayGetCount(monitor->filters); ++i) {
		if (file_has_valid_extension(
			    CFArrayGetValueAtIndex(monitor->filters, i),
			    filename))
			return 0;
	}
	// not valid, exclude from list
	return -1;
}

// don't need event id for timing yet
// NOTE also use this to update the tracked ones in monitor, and return those
// which are new
static CFArrayRef
event_stream_get_new_files(mm_monitor *monitor, size_t numEvents,
			   CFArrayRef eventPaths,
			   const FSEventStreamEventFlags *eventFlags)
{
	// loop through paths with 'created' flag, check if not found in
	// media_monitor list
	CFMutableArrayRef new_files = CFArrayCreateMutable(NULL, 0, NULL);

	for (int i = 0; i < numEvents; i++) {
		CFShow(CFSTR("handling:\n"));
		CFShow(CFArrayGetValueAtIndex(eventPaths, i));
		if (!(eventFlags[i] & kFSEventStreamEventFlagItemCreated)) {
			CFShow(CFSTR("not a file created event...\n"));
			continue;
		}

		if (eventFlags[i] & kFSEventStreamEventFlagItemIsFile) {
			CFStringRef path =
				CFArrayGetValueAtIndex(eventPaths, i);

			// filter filetype
			if (mm_monitor_filter_file(monitor, path))
				continue;

			CFArrayAppendValue(monitor->processed_files, path);
			CFArrayAppendValue(new_files, path);
		}
	}
	// TODO update tracked files
	return new_files;
}

// expects files to be realpath'd already
//static int apply_filter(const mm_filter *filter, char **files, int num_files)
static int apply_filter(const struct mm_filter *filter, CFArrayRef files)
{
	bool failed = false;
	for (CFIndex i = 0; i < CFArrayGetCount(files); ++i) {
		CFStringRef file = CFArrayGetValueAtIndex(files, i);
		const char *filestr =
			CFStringGetCStringPtr(file, kCFStringEncodingASCII);
		if (file_has_valid_extension(filter, file)) {
			// extension match for this file, execute:
			// TODO combine all in one swoop instead so prog files... vs prog file prog file...
			CFMutableStringRef prog_and_file =
				CFStringCreateMutable(NULL, 0);
			CFStringAppend(prog_and_file, filter->prog);
			CFStringAppend(prog_and_file, CFSTR(" "));
			CFStringAppend(prog_and_file, file);

			const char *cstr = CFStringGetCStringPtr(
				prog_and_file, kCFStringEncodingASCII);
			printf("Running: '%s'\n", cstr);
			int result = system(cstr);
			if (result) {
				fprintf(stderr,
					"program returned nonzero exit status: %d\n", result);
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

	//char **paths = eventPaths;
	CFArrayRef paths = (CFArrayRef)eventPaths;
	CFArrayRef new_files = event_stream_get_new_files(monitor, numEvents,
							  paths, eventFlags);
	CFShow(CFSTR("New files:\n"));
	for (CFIndex i = 0; i < CFArrayGetCount(new_files); ++i) {
		CFShow(CFArrayGetValueAtIndex(new_files, i));
	}
	CFShow(CFSTR("\n"));

	for (CFIndex i = 0; i < CFArrayGetCount(monitor->filters); ++i) {
		apply_filter((const struct mm_filter *)(CFArrayGetValueAtIndex(
				     monitor->filters, i)),
			     new_files);
	}

	CFRelease(new_files);
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
	if (CFArrayGetCount(monitor->folders) == 0)
		return;

	mm_monitor_stop(monitor);

	//CFMutableArrayRef paths = CFArrayCreateMutable(NULL, 0, NULL);
	//// build CFArray:
	//pthread_mutex_lock(&monitor->lock);
	//for (CFIndex i = 0; i < CFArrayGetCount(monitor->folders); ++i) {
	//	CFStringRef folder = CFArrayGetValueAtIndex(monitor->folders, i);

	//	CFArrayAppendValue(paths, folder);
	//}
	//pthread_mutex_unlock(&monitor->lock);
	//printf("paths:\n");
	//CFShow(paths);
	//CFShow(CFAllocatorGetDefault());

	CFAbsoluteTime latency = 3.0;
	// TODO track time and not just since now. First, just process things
	// incoming while currently running, compare against found file names, no
	// timestamp necessary
	// kFSEventStreamCreateFlagUseCFTypes makes eventPaths
	// CFArrayRef of CFStringRef which are released on callback exit
	// TODO add context pointer whose info points to this monitor object
	pthread_mutex_lock(&monitor->lock);
	fprintf(stderr, "int folders");
	CFShow(monitor->folders);
	FSEventStreamContext monitor_context = { .copyDescription = NULL,
						 .info = monitor,
						 .release = NULL,
						 .retain = NULL,
						 .version = 0 };
	monitor->event_stream =
		FSEventStreamCreate(NULL, &event_stream_callback, &monitor_context, monitor->folders,
				    kFSEventStreamEventIdSinceNow, latency,
				    kFSEventStreamCreateFlagFileEvents |
					    kFSEventStreamCreateFlagUseCFTypes);
	if (!monitor->event_stream)
		die("failed to create event stream");
	else
		CFShow(CFSTR("successfully created event stream"));

	pthread_mutex_unlock(&monitor->lock);

	FSEventStreamSetDispatchQueue(monitor->event_stream,
				      monitor->dispatch_queue);
	//CFRelease(paths);
	monitor->is_stream_valid = true;
}

static int num_created = 0;
const char dispatch_name[] = "com.asciineuron.mediamonitorqueue";

mm_monitor *mm_monitor_init(void)
{
	// TODO static for now, maybe eventually create in-place?
	// everything pointer type so memset ok:
	mm_monitor *monitor = malloc(sizeof(mm_monitor));
	memset(monitor, 0, sizeof(mm_monitor));

	monitor->folders =
		CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	monitor->processed_files =
		CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	monitor->filters = CFArrayCreateMutable(NULL, 0, &mm_filter_array_callbacks);

	char numbered_dispatch_name[50];
	sprintf(numbered_dispatch_name, "%s-%d", dispatch_name, num_created);
	num_created++;

	monitor->dispatch_queue = dispatch_queue_create(numbered_dispatch_name,
							DISPATCH_QUEUE_SERIAL);

	pthread_mutex_init(&monitor->lock, NULL);

	//monitor->is_running = false;
	//monitor->is_stream_valid = false;
	//mm_create_update_event_stream(monitor);

	return monitor;
}

void mm_monitor_free(mm_monitor *monitor)
{
	mm_monitor_stop(monitor);
	dispatch_release(monitor->dispatch_queue);
	pthread_mutex_lock(&monitor->lock);

	CFRelease(monitor->folders);
	CFRelease(monitor->filters);
	CFRelease(monitor->processed_files);

	pthread_mutex_unlock(&monitor->lock);
	pthread_mutex_destroy(&monitor->lock);
}

void mm_monitor_start(mm_monitor *monitor)
{
	if (!monitor || monitor->is_running)
		return;
	if (!monitor->is_stream_valid)
		mm_create_update_event_stream(monitor);

	CFShow(CFSTR("starting monitor\n"));
	if (!FSEventStreamStart(monitor->event_stream)) {
		fprintf(stderr, "failed to start event stream\n");
		exit(EXIT_FAILURE);
	}
}

void mm_monitor_pause(mm_monitor *monitor)
{
	if (!monitor->is_running)
		return;
	CFShow(CFSTR("pausing monitor\n"));
	FSEventStreamStop(monitor->event_stream);
}

void mm_monitor_update(mm_monitor *monitor)
{
	bool was_running = monitor->is_running;
	mm_create_update_event_stream(monitor);
	if (was_running)
		mm_monitor_start(monitor);
}

//static char *str_resize(char *str, int testlen, int alloc)
//{
//	if (testlen >= alloc) {
//		alloc *= 2;
//		if (!(str = realloc(str, alloc)))
//			die("realloc failure");
//	}
//	return str;
//}

//char *mm_monitor_print_status(mm_monitor *monitor)
//{
//	pthread_mutex_lock(&monitor->lock);
//	int alloc = 1000;
//	char *output = malloc(alloc);
//	int len = 0;
//	len += sprintf(output, "Watching folders:\n");
//
//	for (struct strlist *folder = monitor->folders; folder != NULL;
//	     folder = folder->next) {
//		const char *folderstr = folder->str;
//		output = str_resize(output, strlen(folderstr) + 1 + len, alloc);
//		len += snprintf(output, alloc - len, "%s\n",
//				monitor->folders[i]);
//	}
//
//	const char[] filter_header = "Filters:\nprog\nextensions";
//	if (strlen(filter_header) + len >= alloc) {
//		alloc += 1000;
//		if (!(output = realloc(output, alloc)))
//			die("realloc failure");
//	}
//	len += sprintf(output, filter_header);
//
//	for (int i = 0; i < monitor->num_filters; i++) {
//		struct mm_filter *filter = monitor->filters[i];
//		output = str_resize(output, strlen(filter.prog) + 2 + len,
//				    alloc); // 2 = \n\n
//		len += snprintf(output, alloc - len, "\n%s\n", filter->prog);
//
//		for (struct strlist *ext = filter->extensions; ext != NULL;
//		     ext = ext->next) {
//			const char *extstr = ext->str;
//			output = str_resize(output, strlen(extstr) + 1 + len,
//					    alloc);
//			len += snprintf(output, alloc - len, "%s ", extstr);
//		}
//	}
//
//	pthread_mutex_unlock(&monitor->lock);
//	output[len] = '\0';
//	return output;
//}

bool mm_filter_is_valid(const struct mm_filter *filter)
{
	// need prog to be real executable file
	// and each extension to be '.SO.METHI..NG.with.dots\0'
	struct stat prog_stat;
	const char *prog =
		CFStringGetCStringPtr(filter->prog, kCFStringEncodingASCII);
	if (!prog)
		die("CFStringGetCStringPtr fail, consider CFStringGetCString");
	if (stat(prog, &prog_stat))
		return false;
	if (!S_ISREG(prog_stat.st_mode))
		return false;
	if (!(prog_stat.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
		return false;
	// program ok

	for (CFIndex i = 0; i < CFArrayGetCount(filter->extensions); ++i) {
		const char *ext = CFStringGetCStringPtr(
			CFArrayGetValueAtIndex(filter->extensions, i),
			kCFStringEncodingASCII);
		if ((strcmp(ext, ".") == 0) || (strcmp(ext, "..") == 0))
			return false;
		if (ext[0] != '.')
			return false;
	}
	// all valid extensions

	return true;
}
