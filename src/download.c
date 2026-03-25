// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "basics.h"
#include "download.h"

/* Return value are:
   < 0: negative errno codes
   > 0: CURLcode values
   0: success
*/
int
curl_download_file(const char *url, const char *output)
{
  _cleanup_fclose_ FILE *fp = NULL;
  CURL *curl;
  int r;

  if (isempty(url) || isempty(output))
    return CURLE_URL_MALFORMAT;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl = curl_easy_init();
  if (curl == NULL)
    {
      curl_global_cleanup();
      return CURLE_FAILED_INIT;
    }

  fp = fopen(output, "wb");
  if (!fp)
    {
      r = -errno;
      curl_easy_cleanup(curl);
      curl_global_cleanup();
      return r;
    }

  // set URL and write callback function
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // -L
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); // --fail
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK)
    {
      //cleanup
      fclose(fp);
      fp = NULL;
      remove(output);
    }

  // Cleanup
  curl_easy_cleanup(curl);
  curl_global_cleanup();

  return res;
}
