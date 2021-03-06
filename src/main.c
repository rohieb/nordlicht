#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <string.h>
#include <popt.h>
#include "nordlicht.h"
#include "version.h"

void print_error(char *message, ...) {
    fprintf(stderr, "nordlicht: ");
    va_list arglist;
    va_start(arglist, message);
    vfprintf(stderr, message, arglist);
    va_end(arglist);
    fprintf(stderr, "\n");
}

const char *gnu_basename(const char *path) {
    char *base = strrchr(path, '/');
    return base ? base+1 : path;
}

const char *filename_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) return "";
    return dot+1;
}

void print_help(poptContext popt, int ret) {
    poptPrintHelp(popt, ret == 0 ? stdout : stderr, 0);
    printf("\n\
Examples:\n\
  nordlicht video.mp4                                   generate video.mp4.png of 1000 x 100 pixels size\n\
  nordlicht video.mp4 --style=vertical                  compress individual frames to columns\n\
  nordlicht video.mp4 -w 1920 -h 200 -o barcode.png     override size and name of the output file\n\
");
    exit(ret);
}

void interesting_stuff(char *filename, char *output_file, int width, int height, nordlicht_style style, nordlicht_strategy strategy, int quiet) {
    nordlicht *n = nordlicht_init(filename, width, height);
    unsigned char *data = NULL;

    if (n == NULL) {
        print_error(nordlicht_error());
        exit(1);
    }

    nordlicht_set_style(n, style);
    nordlicht_set_strategy(n, strategy);

    if (strategy == NORDLICHT_STRATEGY_LIVE) {
        int fd = open(output_file, O_CREAT | O_TRUNC | O_RDWR, 0666);
        if (fd == -1) {
            print_error("Could not open '%s'.", output_file);
            exit(1);
        }
        ftruncate(fd, nordlicht_buffer_size(n));
        data = mmap(NULL, nordlicht_buffer_size(n), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        nordlicht_set_buffer(n, data);
    } else {
        // Try to write the empty buffer to fail early if this does not work
        if (nordlicht_write(n, output_file) != 0) {
            print_error(nordlicht_error());
            exit(1);
        }
    }

    pthread_t thread;
    pthread_create(&thread, NULL, (void*(*)(void*))nordlicht_generate, n);

    float progress = 0;

    if (! quiet) {
        printf("nordlicht: Building keyframe index... ");
        fflush(stdout);
        while (progress == 0) {
            progress = nordlicht_progress(n);
            usleep(100000);
        }
        printf("done.\n");

        while (progress < 1) {
            progress = nordlicht_progress(n);
            printf("\rnordlicht: %02.0f%%", progress*100);
            fflush(stdout);
            usleep(100000);
        }
    }

    pthread_join(thread, NULL);

    if (strategy != NORDLICHT_STRATEGY_LIVE) {
        if (nordlicht_write(n, output_file) != 0) {
            print_error(nordlicht_error());
            exit(1);
        }
    }

    nordlicht_free(n);
    munmap(data, nordlicht_buffer_size(n));
    // close

    if (! quiet) {
        printf(" -> '%s'\n", output_file);
    }
}

int main(int argc, const char **argv) {
    int width = -1;
    int height = -1;
    char *output_file = NULL;
    char *style_string = NULL;
    nordlicht_style style;
    nordlicht_strategy strategy;
    int free_output_file = 0;

    int quiet = 0;
    int help = 0;
    int version = 0;

    struct poptOption optionsTable[] = {
        {"width", 'w', POPT_ARG_INT, &width, 0, "set the barcode's width; by default it's \"height*10\", or 1000 pixels, if both are undefined", NULL},
        {"height", 'h', POPT_ARG_INT, &height, 0, "set the barcode's height; by default it's \"width/10\"", NULL},
        {"output", 'o', POPT_ARG_STRING, &output_file, 0, "set output filename, the default is $(basename VIDEOFILE).png; when you specify an *.bgra file, you'll get a raw 32-bit BGRA file that is updated as the barcode is generated", "FILENAME"},
        {"style", 's', POPT_ARG_STRING, &style_string, 0, "default is 'horizontal'; can also be 'vertical', which compresses the frames \"down\" to rows, rotates them counterclockwise by 90 degrees and then appends them", "STYLE"},
        {"quiet", 'q', 0, &quiet, 0, "don't show progress indicator", NULL},
        {"help", '\0', 0, &help, 0, "display this help and exit", NULL},
        {"version", '\0', 0, &version, 0, "output version information and exit", NULL},
        POPT_TABLEEND
    };

    poptContext popt = poptGetContext(NULL, argc, argv, optionsTable, 0);
    poptSetOtherOptionHelp(popt, "[OPTION]... VIDEOFILE\n\nOptions:");

    char c;

    // The next line leaks 3 bytes, blame popt!
    while ((c = poptGetNextOpt(popt)) >= 0) { }

    if (c < -1) {
        fprintf(stderr, "nordlicht: %s: %s\n", poptBadOption(popt, POPT_BADOPTION_NOALIAS), poptStrerror(c));
        return 1;
    }

    if (version) {
      printf("nordlicht %s\n\nWritten by Sebastian Morr and contributors.\n", NORDLICHT_VERSION);
      return 0;
    }

    if (help) {
        print_help(popt, 0);
    }

    char *filename = (char*)poptGetArg(popt);

    if (filename == NULL) {
        print_error("Please specify an input file.\n");
        print_help(popt, 1);
    }

    if (poptGetArg(popt) != NULL) {
        print_error("Please specify only one input file.\n");
        print_help(popt, 1);
    }

    if (output_file == NULL) {
        output_file = malloc(snprintf(NULL, 0, "%s.png", gnu_basename(filename)) + 1);
        sprintf(output_file, "%s.png", gnu_basename(filename));
        free_output_file = 1;
    }

    if (width == -1 && height != -1) {
        width = height*10;
    }
    if (height == -1 && width != -1) {
        height = width/10;
        if (height < 1) {
            height = 1;
        }
    }
    if (height == -1 && width == -1) {
        width = 1000;
        height = 100;
    }

    if (style_string == NULL) {
        style = NORDLICHT_STYLE_HORIZONTAL;
    } else {
        if (strcmp(style_string, "horizontal") == 0) {
            style = NORDLICHT_STYLE_HORIZONTAL;
        } else if (strcmp(style_string, "vertical") == 0) {
            style = NORDLICHT_STYLE_VERTICAL;
        } else {
            print_error("Unknown style '%s'.\n", style_string);
            print_help(popt, 1);
        }
    }

    const char *ext = filename_ext(output_file);
    if (strcmp(ext, "png") == 0) {
        strategy = NORDLICHT_STRATEGY_FAST;
    } else if (strcmp(ext, "bgra") == 0) {
        strategy = NORDLICHT_STRATEGY_LIVE;
    } else {
        print_error("Unsupported file extension '%s'\n", ext);
        print_help(popt, 1);
    }

    interesting_stuff(filename, output_file, width, height, style, strategy, quiet);

    if (free_output_file) {
        free(output_file);
    }

    poptFreeContext(popt);
    return 0;
}
