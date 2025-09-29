#include "plexapi.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "logger.h"
#include "monitor.h"

static CURL *curl_handle = NULL;        /* CURL handle */

/* Callback for writing curl response data */
static size_t curl_write(void *contents, size_t size, size_t nmemb, void *userp) {
	size_t realsize = size * nmemb;
	curl_response_t *mem = (curl_response_t *) userp;

	char *ptr = realloc(mem->data, mem->size + realsize + 1);
	if (!ptr) {
		log_message(LOG_ERR, "Not enough memory for CURL response");
		return 0;
	}

	mem->data = ptr;
	memcpy(&(mem->data[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->data[mem->size] = 0;

	return realsize;
}

/* Create curl headers for Plex API requests */
static struct curl_slist *curl_headers(void) {
	struct curl_slist *headers = NULL;

	/* Set common headers */
	headers = curl_slist_append(headers, "Accept: application/json");

	/* Add auth token if available */
	if (strlen(g_config.plex_token) > 0) {
		char auth_header[TOKEN_MAX_LEN + 20];
		snprintf(auth_header, sizeof(auth_header), "X-Plex-Token: %s",
				 g_config.plex_token);
		headers = curl_slist_append(headers, auth_header);
	}

	return headers;
}

/* Initialize Plex API client */
bool plexapi_init(void) {
	log_message(LOG_INFO, "Initializing Plex API client");

	/* Initialize curl */
	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl_handle = curl_easy_init();

	if (!curl_handle) {
		log_message(LOG_ERR, "Failed to initialize CURL");
		return false;
	}

	/* Set common curl options */
	curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_write);

	return true;
}

/* Clean up Plex API client */
void plexapi_cleanup(void) {
	log_message(LOG_INFO, "Cleaning up Plex API client");

	if (curl_handle) {
		curl_easy_cleanup(curl_handle);
		curl_handle = NULL;
	}

	curl_global_cleanup();
}

/* Check connectivity to the Plex Media Server */
bool plexapi_check(void) {
	curl_response_t response;
	char url[1024];
	struct curl_slist *headers = NULL;
	CURLcode res;
	long http_code = 0;
	time_t start_time, current_time;

	log_message(LOG_INFO, "Attempting to connect to %s", g_config.plex_url);

	if (!curl_handle) {
		log_message(LOG_ERR, "CURL not initialized");
		return false;
	}

	/* Construct request URL for server identity endpoint */
	snprintf(url, sizeof(url), "%s/identity", g_config.plex_url);

	/* Set up headers */
	headers = curl_headers();

	/* Set curl options */
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 5L);

	start_time = time(NULL);

	do {
		/* Initialize response struct */
		response.data = malloc(1);
		if (!response.data) {
			curl_slist_free_all(headers);
			log_message(LOG_ERR, "Memory allocation failed");
			return false;
		}
		response.size = 0;

		curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) &response);

		/* Perform the request */
		res = curl_easy_perform(curl_handle);

		if (res == CURLE_OK) {
			/* Check HTTP status code */
			curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

			if (http_code >= 200 && http_code < 300) {
				log_message(LOG_INFO, "Successfully connected to Plex Media Server");
				free(response.data);
				curl_slist_free_all(headers);
				return true;
			} else {
				log_message(LOG_DEBUG, "Plex server responded with HTTP %ld",
							http_code);
			}
		} else {
			log_message(LOG_DEBUG, "Failed to connect to Plex: %s",
						curl_easy_strerror(res));
		}

		/* Clean up response */
		free(response.data);

		/* Check timeout */
		current_time = time(NULL);
		if (current_time - start_time >= g_config.startup_timeout) {
			curl_slist_free_all(headers);
			log_message(LOG_ERR, "Connection timeout reached after %d seconds",
						g_config.startup_timeout);
			return false;
		}

		/* Wait before retrying */
		log_message(LOG_DEBUG, "Retrying connection in 5 seconds...");
		sleep(5);

	} while (1);

	/* This point should never be reached */
	curl_slist_free_all(headers);
	return false;
}

/* Process library section */
static bool plexapi_process(json_object *section) {
	json_object *section_obj, *location_array, *location, *path_obj;
	int section_id;
	const char *section_path;

	/* Get section ID */
	if (!json_object_object_get_ex(section, "key", &section_obj)) {
		log_message(LOG_WARNING, "Library section missing 'key' field");
		return false;
	}

	section_id = json_object_get_int(section_obj);

	/* Get locations for this section */
	if (!json_object_object_get_ex(section, "Location", &location_array)) {
		log_message(LOG_WARNING, "Library section %d has no locations",
					section_id);
		return false;
	}

	int num_locations = json_object_array_length(location_array);
	bool success = false;

	for (int j = 0; j < num_locations; j++) {
		location = json_object_array_get_idx(location_array, j);

		if (!json_object_object_get_ex(location, "path", &path_obj)) {
			log_message(LOG_WARNING, "Location %d in section %d missing path",
						j, section_id);
			continue;
		}

		section_path = json_object_get_string(path_obj);

		/* Add this directory to the watch list */
		log_message(LOG_INFO, "Monitoring library: %s (section %d)",
					section_path, section_id);

		if (monitor_tree(section_path, section_id)) {
			success = true;
		} else {
			log_message(LOG_WARNING, "Failed to add directory %s to watch list",
						section_path);
		}
	}

	return success;
}

/* Get libraries from Plex server */
bool plexapi_libraries(void) {
	curl_response_t response;
	char url[1024];
	struct curl_slist *headers = NULL;
	json_object *root, *container, *sections, *section;
	CURLcode res;
	bool success = true;

	log_message(LOG_INFO, "Retrieving library sections from Plex");

	if (!curl_handle) {
		log_message(LOG_ERR, "CURL not initialized");
		return false;
	}

	/* Check if kqueue is valid */
	if (monitor_kqueue() == -1) {
		log_message(LOG_ERR, "Invalid kqueue descriptor");
		return false;
	}

	/* Initialize response struct */
	response.data = malloc(1);
	response.size = 0;

	/* Construct request URL */
	snprintf(url, sizeof(url), "%s/library/sections", g_config.plex_url);

	/* Set up headers */
	headers = curl_headers();

	/* Set curl options */
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) &response);

	/* Perform the request */
	res = curl_easy_perform(curl_handle);

	/* Clean up headers */
	curl_slist_free_all(headers);

	if (res != CURLE_OK) {
		log_message(LOG_ERR, "Failed to get library sections: %s",
					curl_easy_strerror(res));
		free(response.data);
		return false;
	}

	/* Parse JSON response */
	root = json_tokener_parse(response.data);
	if (!root) {
		log_message(LOG_ERR, "Failed to parse JSON response");
		free(response.data);
		return false;
	}

	/* Get sections array */
	if (!json_object_object_get_ex(root, "MediaContainer", &container) ||
		!json_object_object_get_ex(container, "Directory", &sections)) {
		log_message(LOG_ERR, "Invalid JSON response structure");
		json_object_put(root);
		free(response.data);
		return false;
	}

	/* Process each library section */
	int num_sections = json_object_array_length(sections);
	log_message(LOG_INFO, "Found %d library sections", num_sections);

	for (int i = 0; i < num_sections; i++) {
		section = json_object_array_get_idx(sections, i);
		if (!plexapi_process(section)) {
			success = false;
		}
	}

	/* Clean up */
	json_object_put(root);
	free(response.data);

	return success;
}

/* Trigger a partial scan for a specific path */
bool plexapi_scan(const char *path, int section_id) {
	curl_response_t response;
	char url[1024];
	struct curl_slist *headers = NULL;
	CURLcode res;

	log_message(LOG_DEBUG, "Triggering Plex scan for path: %s (section %d)",
				path, section_id);

	if (!curl_handle) {
		log_message(LOG_ERR, "CURL not initialized");
		return false;
	}

	/* Initialize response struct */
	response.data = malloc(1);
	response.size = 0;

	/* Construct request URL with path encoded */
	char *escaped_path = curl_easy_escape(curl_handle, path, 0);
	if (escaped_path) {
		snprintf(url, sizeof(url), "%s/library/sections/%d/refresh?path=%s",
				 g_config.plex_url, section_id, escaped_path);
		curl_free(escaped_path);
	} else {
		log_message(LOG_ERR, "Failed to URL encode path");
		free(response.data);
		return false;
	}

	/* Set up headers */
	headers = curl_headers();

	/* Set curl options */
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) &response);

	/* Perform the request */
	res = curl_easy_perform(curl_handle);

	/* Clean up headers */
	curl_slist_free_all(headers);

	if (res != CURLE_OK) {
		log_message(LOG_ERR, "Failed to trigger Plex scan: %s",
					curl_easy_strerror(res));
		free(response.data);
		return false;
	}

	/* Clean up */
	free(response.data);

	log_message(LOG_DEBUG, "Successfully triggered scan for %s", path);
	return true;
}
