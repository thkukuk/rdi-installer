// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/utsname.h>
#include <curl/curl.h>
#include <ncurses.h>

#include "basics.h"
#include "logger.h"
#include "nc-dialogs.h"
#include "rdii-menu.h"
#include "rdii-select-online-image.h"
#include "download.h"

typedef struct {
  const char *token; // Pattern bounded by '.' or '-' (or start/end of string)
  const char *arch;  // Standardized architecture name
} ArchMap;

// Data structure for the architecture-specific items
typedef struct {
  char *name;
  const char *arch; // always one of arch_map[].arch or the "unknown" literal, never heap-owned
} Image;

typedef struct {
  Image *data;
  size_t size;
  size_t capacity;
} ImageList;

static const ArchMap arch_map[] = {
  {"x86-64", "x86-64"},
  {"x86_64", "x86-64"},
  {"amd64",  "x86-64"},
  {"x64",    "x86-64"},
  {"x86",    "x86"},
  {"i386",   "x86"},
  {"i486",   "x86"},
  {"i586",   "x86"},
  {"i686",   "x86"},
  {"arm64",  "arm64"},
  {"aarch64","arm64"},
  {"s390x",  "s390x"},
  {"s390",   "s390"}
};

// Dynamic list of options for the filter box
static char **arch_options = NULL;
static int arch_options_count = 0;
static int current_arch_idx = 0;

// Initialize the list
static int
init_list(ImageList *list, size_t initial_capacity)
{
  list->size = 0;
  list->capacity = initial_capacity;
  list->data = malloc(list->capacity * sizeof(Image));
  if (!list->data)
    {
      perror("Failed to allocate image list");
      return -ENOMEM;
    }
  return 0;
}

// Add an element to the list (allocating memory for strings)
static int
add_image(ImageList *list, const char *name, const char *arch)
{
  // Resize the array if it reaches capacity
  if (list->size >= list->capacity)
    {
      list->capacity *= 2;
      Image *new_data = realloc(list->data, list->capacity * sizeof(Image));
      if (!new_data)
        {
          perror("Failed to reallocate image list");
          return -ENOMEM;
        }
      list->data = new_data;
    }

  // name needs its own copy (it points into the caller's line buffer); arch
  // is always one of arch_map[].arch or the "unknown" literal (see
  // extract_arch_bounded()), so it's safe - and avoids a pointless
  // allocation - to store that constant pointer directly.
  char *name_dup = strdup(name);
  if (!name_dup)
    {
      perror("Failed to duplicate image entry");
      return -ENOMEM;
    }

  list->data[list->size].name = name_dup;
  list->data[list->size].arch = arch;

  list->size++;
  return 0;
}

// Free all allocated memory within the list
static void
free_list(ImageList *list)
{
  for (size_t i = 0; i < list->size; i++)
    free(list->data[i].name);
  free(list->data);
  list->data = NULL;
  list->size = 0;
  list->capacity = 0;
}

// Clean up dynamically allocated memory
static void
free_arch_options(void)
{
  if (!arch_options)
    return;
  for (int i = 0; i < arch_options_count; i++)
    free(arch_options[i]);
  free(arch_options);
  arch_options = NULL;
  arch_options_count = 0;
}

// Adapts free_arch_options() to the _cleanup_() attribute, which always
// passes the address of the annotated variable regardless of its type.
static void
free_arch_options_cleanup(void *unused)
{
  (void)unused;
  free_arch_options();
}

bool
is_supported_image(const char *name)
{
  const char *exts[] = {
    ".img",     ".raw",
    ".img.gz",  ".raw.gz",
    ".img.bz2", ".raw.bz2",
    ".img.xz",  ".raw.xz",
    ".img.zst", ".raw.zst"
  };
  const size_t num_exts = sizeof(exts) / sizeof(exts[0]);

  for (size_t i = 0; i < num_exts; i++)
    if (endswith(name, exts[i]))
      return true;

  return false;
}

// sysext images are only meant to be picked explicitly (via the "all"
// filter), not offered under "matching" or a specific architecture, since
// they are overlay images rather than bootable system images.
static bool
is_sysext_image(const char *name)
{
  const char *exts[] = {
    ".sysext.raw",
    ".sysext.raw.gz",
    ".sysext.raw.bz2",
    ".sysext.raw.xz",
    ".sysext.raw.zst"
  };
  const size_t num_exts = sizeof(exts) / sizeof(exts[0]);

  for (size_t i = 0; i < num_exts; i++)
    if (endswith(name, exts[i]))
      return true;

  return false;
}

// A token match is only accepted if it isn't glued to surrounding
// alphanumeric characters, e.g. "x86" must not match inside "x8600". '_' is
// treated as a boundary (not alphanumeric-glue) since it's a common
// filename separator and several arch_map tokens (e.g. "x86_64") contain it
// themselves.
static bool
is_arch_boundary(char c)
{
  return c == '\0' || !isalnum((unsigned char)c);
}

// Finds the architecture token that appears earliest in `filename`, rather
// than the first one in arch_map's declaration order, so classification
// depends on filename content rather than table ordering (relevant if a
// filename were ever to contain more than one recognized token). Ties at
// the same position (e.g. "x86" and "x86_64" both start at the same offset
// in "...-x86_64...") are broken by preferring the longer, more specific
// token, rather than depending on which one arch_map happens to list first.
static const char *
extract_arch_bounded(const char *filename)
{
  size_t count = sizeof(arch_map) / sizeof(arch_map[0]);
  const char *best_arch = "unknown";
  size_t best_pos = SIZE_MAX;
  size_t best_len = 0;

  for (size_t i = 0; i < count; i++)
    {
      const char *token = arch_map[i].token;
      size_t token_len = strlen(token);
      const char *p = filename;

      while ((p = strstr(p, token)) != NULL)
        {
          char before = (p == filename) ? '\0' : p[-1];
          char after = p[token_len];

          if (is_arch_boundary(before) && is_arch_boundary(after))
            {
              size_t pos = (size_t)(p - filename);
              if (pos < best_pos || (pos == best_pos && token_len > best_len))
                {
                  best_pos = pos;
                  best_len = token_len;
                  best_arch = arch_map[i].arch;
                }
              break;
            }

          p++;
        }
    }

  return best_arch;
}

// Normalize a `uname -m` style machine name to the same canonical
// architecture names used as arch_map values, reusing arch_map itself so
// the filename-token and uname-value alias lists can't drift apart.
static const char *
normalize_machine_arch(const char *machine)
{
  if (!machine)
    return "unknown-host";

  size_t count = sizeof(arch_map) / sizeof(arch_map[0]);
  for (size_t i = 0; i < count; i++)
    if (streq(machine, arch_map[i].token))
      return arch_map[i].arch;

  // Fall back to the raw uname() value rather than the literal "unknown":
  // that string is also what extract_arch_bounded() reports for images
  // whose filename doesn't match any known token, and having both sides
  // collapse to the same literal would make an unrelated image on an
  // unrecognized host architecture look like a "matching" filter hit.
  return machine;
}

// Returns the canonical architecture name of the running system.
static const char *
get_system_arch(void)
{
  static char system_arch[32] = "";

  if (system_arch[0] == '\0')
    {
      struct utsname uts;
      const char *arch = (uname(&uts) == 0) ? normalize_machine_arch(uts.machine) : "unknown-host";
      strncpy(system_arch, arch, sizeof(system_arch) - 1);
      system_arch[sizeof(system_arch) - 1] = '\0';
    }

  return system_arch;
}

// Helper to check if a string already exists in an array of strings
static bool
string_exists(char **arr, int count, const char *str)
{
  for (int i = 0; i < count; i++)
    if (streq(arr[i], str))
      return true;
  return false;
}

/*
 *  select an image from a remote SHA256SUMS listing
 */

/*
  Parse SHA256SUMS file and return the names of all supported images.
  Returns the number of found entries (>= 0), or a negative errno code.
*/
static int
parse_sha256sums(const char *path, ImageList *ret_images)
{
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  int count = 0;

  MSG_FUNC("path='%s'", path);

  current_arch_idx = 0;

  arch_options = malloc(2 * sizeof(char *));
  if (!arch_options)
    {
      perror("Failed to allocate architecture filter list");
      return -ENOMEM;
    }
  arch_options[0] = strdup("matching");
  arch_options[1] = strdup("all");
  if (!arch_options[0] || !arch_options[1])
    {
      free(arch_options[0]);
      free(arch_options[1]);
      free(arch_options);
      arch_options = NULL;
      perror("Failed to allocate architecture filter list");
      return -ENOMEM;
    }
  arch_options_count = 2;

  fp = fopen(path, "r");
  if (!fp)
    return -errno;

  while ((linelen = getline(&line, &linecap, fp)) > 0)
    {
      char *nl = strchr(line, '\n');
      if (nl)
        *nl = '\0';

      // A valid line is "<64 hex char hash><separator><filename>"
      if (linelen < 66)
        continue;

      char *name = strchr(line, ' ');
      if (!name)
        continue;
      while (*name == ' ')
        ++name;

      if (isempty(name) || !is_supported_image(name))
        continue;

      const char *arch = extract_arch_bounded(name);
      int ret = add_image(ret_images, name, arch);
      if (ret == 0)
        {
          count++;
          // If it's a new unique architecture, add it
          if (!string_exists(arch_options, arch_options_count, arch))
            {
              char **new_options = realloc(arch_options, (arch_options_count + 1) * sizeof(char *));
              if (!new_options)
                {
                  perror("Failed to reallocate architecture filter list");
                  return -ENOMEM;
                }
              arch_options = new_options;
              arch_options[arch_options_count] = strdup(arch);
              if (!arch_options[arch_options_count])
                {
                  perror("Failed to duplicate architecture name");
                  return -ENOMEM;
                }
              arch_options_count++;
            }
        }
      else
        return ret;
    }

  MSG_INFO("Found %i supported image(s) in '%s'", count, path);

  return count;
}

// Helper to determine if an item matches the active filter
static bool
item_matches_filter(const char *image_name, const char *image_arch, int arch_idx)
{
  const char *target_arch = arch_options[arch_idx];

  if (streq(target_arch, "all"))
    // "all architectures" selected: show complete array of ALL_ITEMS
    return true;

  if (is_sysext_image(image_name))
    // sysext images are only shown with "all", never under "matching" or a
    // specific architecture filter
    return false;

  if (streq(target_arch, "matching"))
    // "matching" selected: show only images compatible with this system's architecture
    return streq(image_arch, get_system_arch());

  // Specific filter selected: show matching items only
  return streq(image_arch, target_arch);
}

// Row at which the scrollable image list starts; shared with choose_image()
// so it can compute the same viewport height for scroll-offset tracking.
static const int list_y_start = 6;

// Draws the parts of the screen that only change when the architecture
// filter changes: header/footer/title (which clear() the whole screen) and
// the combobox. Kept separate from draw_list() so plain navigation keys
// don't force a full-screen clear-and-redraw on every keystroke.
static void
draw_chrome(int current_arch, const char *title)
{
  print_global_header_footer("F1: Help | Left/Right: Change Filter", SELECTION);
  print_title((title ? title : ""));

  // Draw Combobox / Filter Selection
  mvprintw(4, 2, "Architecture: ");
  attron(A_REVERSE | A_BOLD);
  mvprintw(4, 23, " [ %-17s v] ", arch_options[current_arch]);
  attroff(A_REVERSE | A_BOLD);

  mvprintw(4, 48, "(LEFT/RIGHT: change filter)");

  mvprintw(5, 2, "Available Images:");
}

static void
draw_list(size_t current_item, size_t scroll_offset, int max_visible,
         const char **names, size_t total_matching)
{
  if (total_matching == 0)
    mvprintw(list_y_start + 1, 6, "< No image found >");
  else
    render_scrollable_list(list_y_start, max_visible, names, (int)total_matching,
                           (int)current_item, (int)scroll_offset);

  refresh();
}

// (Re)builds `filtered` with the images matching the current architecture
// filter, along with `names`, a `filtered`-sized array of pointers into
// `filtered`'s Image.name fields used for rendering. Only needs to run when
// the filter changes or on first entry, since the underlying image_list is
// immutable while choose_image() is running.
static int
rebuild_filtered_list(const ImageList *image_list, ImageList *filtered,
                      const char ***names)
{
  free(filtered->data);
  free(*names);
  *filtered = (ImageList){ .data = NULL, .size = 0 };
  *names = NULL;

  if (image_list->size == 0)
    return 0;

  filtered->data = malloc(image_list->size * sizeof(*image_list->data));
  if (!filtered->data)
    {
      MSG_ERROR("Failed to allocate filtered image list");
      return -ENOMEM;
    }
  filtered->capacity = image_list->size;

  for (size_t i = 0; i < image_list->size; i++)
    if (item_matches_filter(image_list->data[i].name, image_list->data[i].arch, current_arch_idx))
      filtered->data[filtered->size++] = image_list->data[i];

  if (filtered->size == 0)
    return 0;

  *names = malloc(filtered->size * sizeof(**names));
  if (!*names)
    {
      MSG_ERROR("Failed to allocate name list for rendering");
      free(filtered->data);
      *filtered = (ImageList){ .data = NULL, .size = 0 };
      return -ENOMEM;
    }

  for (size_t i = 0; i < filtered->size; i++)
    (*names)[i] = filtered->data[i].name;

  return 0;
}

// Selects an image interactively. On success returns 0 and sets *ret_name to
// the chosen entry (owned by image_list, not to be freed by the caller); on
// ESC returns -ECANCELED; on allocation failure returns -ENOMEM, distinct
// from a user-initiated cancel.
static int
choose_image(ImageList *image_list, const char *title, char **ret_name)
{
  size_t selected = 0;
  size_t scroll_offset = 0;
  int last_arch_idx = -1;
  ImageList filtered = { .data = NULL, .size = 0 };
  const char **names = NULL;
  const char *help_text = "Select raw disk image which has to be installed on the target system.\n"
    "The name of the default download server can be changed by the value of rdii.download_server, set "
    "by the kernel cmdline during boot or by a configuration file.\n";

  MSG_FUNC("number of images=%zu", image_list->size);

  *ret_name = NULL;

  while (1)
    {
      // Only rebuild the filtered list (and its name projection), and
      // redraw the chrome (which does a full-screen clear), when the
      // architecture filter changed - not on every navigation keystroke.
      if (current_arch_idx != last_arch_idx)
        {
          int rc = rebuild_filtered_list(image_list, &filtered, &names);
          if (rc != 0)
            return rc;
          last_arch_idx = current_arch_idx;
          selected = 0;
          scroll_offset = 0;
          draw_chrome(current_arch_idx, title);
        }

      // Keep 'selected' visible in the viewport, sharing choose_entry()'s
      // sticky (hysteresis-based) scrolling instead of recentering every frame
      int max_visible = list_viewport_height(list_y_start);
      scroll_offset = (size_t)scroll_offset_for_selection((int)selected, (int)scroll_offset, max_visible);

      draw_list(selected, scroll_offset, max_visible, names, filtered.size);

      int ch = getch();

      if (ch == 27) // ESC
        {
          MSG_INFO("Canceled with ESC");
          free(filtered.data);
          free(names);
          return -ECANCELED;
        }
      else if (ch == KEY_UP)
        {
          if (filtered.size > 0)
            selected = (selected - 1 + filtered.size) % filtered.size;
        }
      else if (ch == KEY_DOWN)
        {
          if (filtered.size > 0)
            selected = (selected + 1) % filtered.size;
        }
      else if (ch == KEY_PPAGE) // Page Up
        {
          size_t step = (size_t)max_visible;
          selected = (selected < step) ? 0 : selected - step;
        }
      else if (ch == KEY_NPAGE) // Page Down
        {
          size_t step = (size_t)max_visible;
          selected = (filtered.size == 0) ? 0 :
            (selected + step >= filtered.size) ? filtered.size - 1 : selected + step;
        }
      else if (ch == KEY_LEFT)
        {
          current_arch_idx = (current_arch_idx > 0)
            ? current_arch_idx - 1
            : arch_options_count - 1;
        }
      else if (ch == KEY_RIGHT)
        {
          current_arch_idx = (current_arch_idx < arch_options_count - 1)
            ? current_arch_idx + 1
            : 0;
        }
      else if (ch == KEY_F1)
        show_help_dialog(title, help_text);
      else if ((ch == '\n' || ch == KEY_ENTER) && filtered.size > 0)
        {
          *ret_name = filtered.data[selected].name;
          MSG_INFO("Selected entry %zu (Name: %s)", selected, *ret_name);
          free(filtered.data);
          free(names);
          return 0;
        }
    }
}

// Joins `base` and `path` with exactly one '/' between them, regardless of
// whether `base` already ends with a slash.
static int
build_url(char **ret, const char *base, const char *path)
{
  size_t len = strlen(base);
  bool ends_with_slash = len > 0 && base[len - 1] == '/';

  return (asprintf(ret, "%s%s%s", base, ends_with_slash ? "" : "/", path) < 0)
    ? -ENOMEM : 0;
}

int
get_url_from_list(char **ret)
{
  _cleanup_free_ char *sha256sums_url = NULL;
  _cleanup_free_ char *sha256sums_asc_url = NULL;
  _cleanup_free_ char *sha256sums_fn = NULL;
  _cleanup_free_ char *sha256sums_asc_fn = NULL;
  _cleanup_(free_list) ImageList image_list = { .data = NULL, .size = 0, .capacity = 0 };
  _cleanup_(free_arch_options_cleanup) int arch_options_guard = 0;
  int num_images;
  char *selected_image = NULL;
  int r = 0;

  MSG_FUNC();

  r = init_list(&image_list, 2);
  if (r != 0)
    return r;

  if (build_url(&sha256sums_url, rdii_download_server, "SHA256SUMS") != 0)
    return -ENOMEM;
  if (build_url(&sha256sums_asc_url, rdii_download_server, "SHA256SUMS.asc") != 0)
    return -ENOMEM;
  if (build_url(&sha256sums_fn, rdii_tmp_dir, "SHA256SUMS") != 0)
    return -ENOMEM;
  if (build_url(&sha256sums_asc_fn, rdii_tmp_dir, "SHA256SUMS.asc") != 0)
    return -ENOMEM;

  r = curl_download_file(sha256sums_url, sha256sums_fn);
  if (r != 0)
    {
      MSG_ERROR("Error downloading SHA256SUMS file: %s",
                r < 0 ? strerror(-r) : curl_easy_strerror(r));
      show_error_popup("Error downloading SHA256SUMS file:",
                       r < 0 ? strerror(-r) : curl_easy_strerror(r), NULL);
      return -EIO;
    }

  r = curl_download_file(sha256sums_asc_url, sha256sums_asc_fn);
  if (r != 0)
    {
      MSG_ERROR("Error downloading SHA256SUMS.asc file: %s",
                r < 0 ? strerror(-r) : curl_easy_strerror(r));
      if (!show_warning_popup("Error downloading SHA256SUMS.asc file:",
                              r < 0 ? strerror(-r) : curl_easy_strerror(r),
                              "Continue without signature verification?"))
        return -ECANCELED;
    }
  else
    {
      char *error_msg = NULL;
      if (!verify_signature(sha256sums_fn, sha256sums_asc_fn, &error_msg))
        {
          MSG_WARN("Cannot verify SHA256SUMS signature: %s", error_msg);
          if (!show_warning_popup("Cannot verify signature.", error_msg,
                                  "Continue without signature verification?"))
            {
              MSG_ERROR("Canceld");
              return -ECANCELED;
            }
        }
    }

  num_images = parse_sha256sums(sha256sums_fn, &image_list);
  if (num_images < 0)
    {
      MSG_ERROR("Error parsing SHA256SUMS file: %s",
                strerror(-num_images));
      show_error_popup("Error parsing SHA256SUMS file:",
                       strerror(-num_images), NULL);
      return num_images;
    }
  if (num_images == 0)
    {
      MSG_INFO("No supported images found in %s", rdii_download_server);
      show_error_popup("No supported images found in SHA256SUMS file.",
                        NULL, NULL);
      return -ENOENT;
    }

  _cleanup_free_ char *header = NULL;

  if (asprintf(&header, "Select image from download server %s", rdii_download_server) < 0)
    return -ENOMEM;

  r = choose_image(&image_list, header, &selected_image);
  if (r == 0)
    r = build_url(ret, rdii_download_server, selected_image);

  return r;
}
