// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <limits.h>
#include <termios.h>

#include <getopt.h>

/*
   Waits for a key press or a specific timeout period.
   Return values:
     > 0: key pressed
     = 0: timeout
     < 0: error
*/
static int
keypress_timeout(int timeout_sec)
{
  struct termios oldt, newt;
  struct pollfd fds[1];
  int timeout_msecs = timeout_sec == 0 ? -1 : timeout_sec * 1000;
  int r;

  fds[0].fd = STDIN_FILENO; /* Monitor Standard Input (keyboard) */
  fds[0].events = POLLIN;   /* Wait for data to be available to read */

  /* Disable line buffering (ICANON) and echo (ECHO) so we can detect
     a key immediately without waiting for Enter. */
  tcgetattr(STDIN_FILENO, &oldt); // Get current settings
  newt = oldt;

  newt.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);

  /* wait until input arrives or time runs out */
  r = poll(fds, 1, timeout_msecs);
  if (r == -1)
    r = -errno;

  /* flush stdin to remove pressed key from input */
  tcflush(STDIN_FILENO, TCIFLUSH);

  /* restore terminal settings immediately */
  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

  return r;
}

static void
print_usage(FILE *stream)
{
  fprintf(stream, "Usage: keywait [--text <msg>] [--timeout <seconds>] [--help] [--version]\n");
}

static void
print_help(void)
{
  fprintf(stdout, "keywait - wait for key pressed or timeout\n\n");
  print_usage(stdout);

  fputs("  -t, --text <msg>         Text to display\n", stdout);
  fputs("  -s, --timeout <seconds>  Set timeout to number of seconds\n", stdout);
  fputs("  -h, --help               Give this help list\n", stdout);
  fputs("  -v, --version            Print program version\n", stdout);
}

static void
print_error(void)
{
  fputs("Try `keywait --help' for more information.\n", stderr);
}

int
main(int argc, char **argv)
{
  const char *text = "Please press any key...";
  int seconds = 5;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
          {"text",    required_argument, NULL, 't' },
          {"timeout", required_argument, NULL, 's' },
          {"help",    no_argument,       NULL, 'h' },
          {"version", no_argument,       NULL, 'v' },
          {NULL,      0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "t:s:hv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
        case 't':
	  text = optarg;
          break;
        case 's':
	  {
	    char *ep;
	    long l;

	    errno = 0;
	    l = strtol(optarg, &ep, 10);
	    if (errno == ERANGE || l < -1 || optarg == ep ||
		*ep != '\0' || l > INT_MAX)
	      {
		fprintf(stderr, "Cannot parse 'optarg=%s'\n", optarg);
		return EINVAL;
	      }
	    seconds = l;
	  }
          break;
        case 'h':
          print_help();
          return 0;
        case 'v':
          printf("keywait (%s) %s\n", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return 1;
        }
    }

  argc -= optind;
  argv += optind;

  if (argc > 0)
    {
      fprintf(stderr, "keywait: Too many arguments.\n");
      print_error();
      return EINVAL;
    }

  if (strlen(text) > 0)
    printf("%s\n", text);

  r = keypress_timeout(seconds);
  if (r < 0)
    {
      fprintf(stderr, "Error: %s\n", strerror(-r));
      return -r;
    }

  return 0;
}
