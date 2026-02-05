// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "basics.h"
#include "download.h"

// Struct to handle the file writing
struct FileStruct {
    const char *filename;
    FILE *stream;
};

// Callback function for libcurl to write data to the file
static size_t
write_data(void *buffer, size_t size, size_t nmemb, void *stream)
{
  struct FileStruct *out = (struct FileStruct *)stream;

  if (!out->stream)
    {
      out->stream = fopen(out->filename, "wb");
      if (!out->stream)
	{
	  fprintf(stderr, "Cannot open '%s': %s\n",
		  out->filename, strerror(errno));
	  return -1; /* failure, can't open file to write */
	}
    }
  return fwrite(buffer, size, nmemb, out->stream);
}

int
curl_download_config(const char *url, const char *cfg)
{
  struct FileStruct file_info = {cfg, NULL};
  CURL *curl;
  CURLcode res;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl = curl_easy_init();

  if (curl == NULL)
    {
      fprintf(stderr, "curl_easy_init() failed\n");
      return -CURLE_FAILED_INIT;
    }

  if (isempty(url) || isempty(cfg))
    return -CURLE_URL_MALFORMAT;

  // set URL and write callback function
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file_info);
  // Fail on HTTP errors (like 404) so we don't save an error page
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK)
    {
      if (file_info.stream)
	fclose(file_info.stream);
      remove(file_info.filename);
    }
  else
    {
      printf("Download successful! Saved to '%s'\n", file_info.filename);
      if (file_info.stream) fclose(file_info.stream);
    }

  // Cleanup
  curl_easy_cleanup(curl);
  curl_global_cleanup();

  return -res;
}
