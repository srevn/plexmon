/**
 * plexapi.c - Plex API client
 */

#include "plexmon.h"
#include <curl/curl.h>
#include <json-c/json.h>

/* CURL handle */
static CURL *curl_handle = NULL;

/* Structure for curl response data */
typedef struct {
    char *data;
    size_t size;
} curl_response_t;

/* Callback for writing curl response data */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    curl_response_t *mem = (curl_response_t *)userp;
    
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
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    
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

/* Get libraries from Plex server */
bool plexapi_get_libraries(void) {
    curl_response_t response;
    char url[1024];
    struct curl_slist *headers = NULL;
    json_object *root, *sections, *section, *section_obj;
    int num_sections, section_id;
    json_object *location_array, *location, *path_obj;
    const char *section_path;
    
    log_message(LOG_INFO, "Retrieving library sections from Plex");
    
    if (!curl_handle) {
        log_message(LOG_ERR, "CURL not initialized");
        return false;
    }
    
    /* Check if kqueue is valid */
    if (fsmonitor_get_kqueue_fd() == -1) {
        log_message(LOG_ERR, "Invalid kqueue descriptor");
        return false;
    }
    
    /* Initialize response struct */
    response.data = malloc(1);
    response.size = 0;
    
    /* Construct request URL */
    snprintf(url, sizeof(url), "%s/library/sections", g_config.plex_url);
    
    /* Set up headers */
    headers = curl_slist_append(headers, "Accept: application/json");
    
    char auth_header[TOKEN_MAX_LEN + 20];
    snprintf(auth_header, sizeof(auth_header), "X-Plex-Token: %s", g_config.plex_token);
    headers = curl_slist_append(headers, auth_header);
    
    /* Set curl options */
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&response);
    
    /* Perform the request */
    CURLcode res = curl_easy_perform(curl_handle);
    
    /* Clean up headers */
    curl_slist_free_all(headers);
    
    if (res != CURLE_OK) {
        log_message(LOG_ERR, "Failed to get library sections: %s", curl_easy_strerror(res));
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
    if (!json_object_object_get_ex(root, "MediaContainer", &root) ||
        !json_object_object_get_ex(root, "Directory", &sections)) {
        log_message(LOG_ERR, "Invalid JSON response structure");
        json_object_put(root);
        free(response.data);
        return false;
    }
    
    /* Process each library section */
    num_sections = json_object_array_length(sections);
    log_message(LOG_INFO, "Found %d library sections", num_sections);
    
    for (int i = 0; i < num_sections; i++) {
        section = json_object_array_get_idx(sections, i);
        json_object_object_get_ex(section, "key", &section_obj);
        section_id = json_object_get_int(section_obj);
        
        /* Get locations for this section */
        if (json_object_object_get_ex(section, "Location", &location_array)) {
            int num_locations = json_object_array_length(location_array);
            
            for (int j = 0; j < num_locations; j++) {
                location = json_object_array_get_idx(location_array, j);
                
                if (json_object_object_get_ex(location, "path", &path_obj)) {
                    section_path = json_object_get_string(path_obj);
                    
                    /* Add this directory to the watch list */
                    log_message(LOG_INFO, "Monitoring library: %s (section %d)", section_path, section_id);
                    
                    if (!watch_directory_tree(section_path, section_id)) {
                        log_message(LOG_WARNING, "Failed to add directory %s to watch list", section_path);
                    }
                }
            }
        }
    }
    
    /* Clean up */
    json_object_put(root);
    free(response.data);
    
    return true;
}

/* Trigger a partial scan for a specific path */
bool plexapi_trigger_scan(const char *path, int section_id) {
    curl_response_t response;
    char url[1024];
    struct curl_slist *headers = NULL;
    CURLcode res;
    
    log_message(LOG_DEBUG, "Triggering Plex scan for path: %s (section %d)", path, section_id);
    
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
    headers = curl_slist_append(headers, "Accept: application/json");
    
    char auth_header[TOKEN_MAX_LEN + 20];
    snprintf(auth_header, sizeof(auth_header), "X-Plex-Token: %s", g_config.plex_token);
    headers = curl_slist_append(headers, auth_header);
    
    /* Set curl options */
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&response);
    
    /* Perform the request */
    res = curl_easy_perform(curl_handle);
    
    /* Clean up headers */
    curl_slist_free_all(headers);
    
    if (res != CURLE_OK) {
        log_message(LOG_ERR, "Failed to trigger Plex scan: %s", curl_easy_strerror(res));
        free(response.data);
        return false;
    }
    
    /* Clean up */
    free(response.data);
    
    log_message(LOG_DEBUG, "Successfully triggered scan for %s", path);
    return true;
}