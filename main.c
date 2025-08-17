#include "mediamonitor.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

/* commandline input: keep simple to one filter type
 *  $ mediamonitor -x prog -e extension -- folder1 folder2 ...
 * and args...
 */

const char USAGE[] = "command line invocation of mediamonitor, simple folder "
		     "and file batch program handler\n Usage: mediamonitor -f "
		     "folder1 folder2... -e extension1 extension2... -x prog\n";

void print_usage()
{
	fputs(USAGE, stdout);
}

int main(int argc, char *argv[])
{
	CFStringRef prog;
	CFMutableArrayRef extensions = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFMutableArrayRef filters = CFArrayCreateMutable(NULL, 1, &mm_filter_array_callbacks);

	int opt;
	// all 2 require args
	while ((opt = getopt(argc, argv, "e:x:h")) != -1) {
		switch (opt) {
		case 'e':
			CFArrayAppendValue(extensions,
					   CFStringCreateWithCString(
						   NULL, optarg,
						   kCFStringEncodingASCII));
			break;
		case 'x':
			prog = CFStringCreateWithCString(
				NULL, optarg, kCFStringEncodingASCII);
			break;
		case 'h':
		default:
			print_usage();
			exit(EXIT_FAILURE);
		}
	}

	// process remaining args, all list of folders
	int num_folders = argc - optind;
	CFMutableArrayRef folders =
		CFArrayCreateMutable(NULL, num_folders, &kCFTypeArrayCallBacks);
	for (int i = optind; i < argc; i++) {
		char full_path[PATH_MAX];
		realpath(argv[i], full_path);

		CFStringRef folder = CFStringCreateWithCString(
			NULL, full_path, kCFStringEncodingASCII);
		CFArrayAppendValue(folders, folder);
	}

	struct mm_filter filter = { .prog = prog, .extensions = extensions };
	CFArrayAppendValue(filters, &filter);

	mm_monitor *monitor = mm_monitor_init();
	monitor->folders = folders;
	fprintf(stderr, "ext folders");
	CFShow(monitor->folders);
	monitor->filters = filters;

	mm_monitor_start(monitor);

	// infinite loop until user quits
	char ch;
	printf("Press 'q' to stop");
	while ((ch = getchar()) != 'q') {
		//if (ch == 's') {
		//	mm_monitor_print_status(monitor);
		//}
	}

	mm_monitor_free(monitor);

	return EXIT_SUCCESS;
}
