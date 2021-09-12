/* HTTP File Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include <driver/sdmmc_defs.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_types.h>
#include <driver/sdspi_host.h>
#include <esp_spiffs.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "diskio.h"
#include <fcntl.h>
#include "ff.h"

#include "esp_err.h"
#include "esp_log.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "esp_wifi_types.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "wifi.h"
#include <math.h>

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");
extern const char app_css_start[] asm("_binary_app_css_start");
extern const char app_css_end[] asm("_binary_app_css_end");

/* Max length a file path can have on storage */
#define FILE_PATH_MAX 255
#define VFS_MOUNT "/files"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE 20 * 1024

/**
 * Helper function to encode URL with %20 (space) in folder/file name
 * 
 */
unsigned char h2int(char c)
{
    if (c >= '0' && c <= '9')
    {
        return ((unsigned char)c - '0');
    }
    if (c >= 'a' && c <= 'f')
    {
        return ((unsigned char)c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F')
    {
        return ((unsigned char)c - 'A' + 10);
    }
    return (0);
}

void urldecode(char *file)
{
    char encodedString[256] = {};
    char c;
    char code0;
    char code1;
    int n = 0;
    for (int i = 0; i < strlen(file); i++)
    {
        c = file[i];
        if (c == '+')
        {
            encodedString[n++] = ' ';
        }
        else if (c == '%')
        {
            i++;
            code0 = file[i];
            i++;
            code1 = file[i];
            c = (h2int(code0) << 4) | h2int(code1);
            encodedString[n++] = c;
        }
        else
        {

            encodedString[n++] = c;
        }
        // yield();
    }
    encodedString[n++] = 0x0;

    memset(file, 0, strlen(file));
    strlcpy(file, encodedString, strlen(encodedString) + 1);
}

struct web_server_data
{
    /* Base path of file storage */
    char base_path[ESP_VFS_PATH_MAX + 1];

    /* Scratch buffer for temporary storage during file transfer */
    char *scratch;
};

static const char *TAG = "web_server";
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename);

/**
 * Make files tree to JSON string
 * TODO add indexing or send in chunks
 */
static esp_err_t http_resp_dir_js(httpd_req_t *req, const char *dirpath)
{
    char entrypath[FILE_PATH_MAX];
    char entrysize[32];
    const char *entrytype;

    struct dirent *entry;
    struct stat entry_stat;
    char _dirpath[255] = {0};
    const size_t dirpath_len = strlen(dirpath);
    strncpy(_dirpath, dirpath, 255);
    _dirpath[dirpath_len - 1] = 0x0;
    int len = httpd_req_get_url_query_len(req);
    int queryindex = 0;
    char buf[20] = {0};
    if (len)
    {
        httpd_req_get_url_query_str(req, buf, 20);
        char *index = strstr(buf, "index=");
        queryindex = atoi(index + 6);
        ESP_LOGI(TAG, "query string len: %d => %s", queryindex, buf);
    }

    DIR *dir = opendir(_dirpath);

    /* Retrieve the base path of file storage to construct the full path */
    strlcpy(entrypath, dirpath, sizeof(entrypath));

    if (!dir)
    {
        ESP_LOGE(TAG, "Failed to stat dir : %s", _dirpath);
        /* Respond with 404 Not Found */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory does not exist");
        return ESP_FAIL;
    }

    uint8_t n = 0;
    // /* Iterate over all files / folders and fetch their names and sizes */
    char _script[500] = {0};
    strcpy(_script, "[");
    if (strlen(dirpath) > 7)
    {
        sprintf(_script + strlen(_script), "{\"path\":\"%s/..\", \"dir\":1,\"name\":\"Up\", \"size\":\"0\"}", _dirpath);
    }
    else
    {
        sprintf(_script + strlen(_script), "{\"path\":\"%s\", \"dir\":1,\"name\":\"Refresh\", \"size\":\"0\"}", _dirpath);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr_chunk(req, _script);
    memset(_script, 0, 500);

    entry = readdir(dir);

    while (entry != NULL)
    {
        entrytype = (entry->d_type == DT_DIR ? "directory" : "file");

        strlcpy(entrypath + dirpath_len, entry->d_name, sizeof(entrypath) - dirpath_len);

        int st = 0;
        st = stat(entrypath, &entry_stat);

        if (st == -1)
        {
            ESP_LOGE(TAG, "Failed to stat %s : %s", entrytype, entry->d_name);
            entry = readdir(dir);
            continue;
        }
        sprintf(entrysize, "%ld", entry_stat.st_size);
        // ESP_LOGI(TAG, "Found %s : %s (%s bytes)", entrytype, entry->d_name, entrysize);

        strlcpy(_script + strlen(_script), ", ", 3);
        /* Send chunk of HTML file containing table entries with file name and size */
        strlcpy(_script + strlen(_script), "{\"path\":\"", 10);
        strlcpy(_script + strlen(_script), dirpath, strlen(dirpath) + 1);
        strlcpy(_script + strlen(_script), entry->d_name, strlen(entry->d_name) + 1);
        strlcpy(_script + strlen(_script), "\", ", 4);

        if (entry->d_type == DT_DIR)
        {
            strlcpy(_script + strlen(_script), "\"dir\":1,", 9);
        }
        else
        {
            strlcpy(_script + strlen(_script), "\"dir\":0,", 9);
        }

        strlcpy(_script + strlen(_script), "\"name\":\"", 9);
        strlcpy(_script + strlen(_script), entry->d_name, strlen(entry->d_name) + 1);
        strlcpy(_script + strlen(_script), "\", \"size\":\"", 12);
        strlcpy(_script + strlen(_script), entrysize, strlen(entrysize) + 1);
        strlcpy(_script + strlen(_script), "\"", 2);
        strlcpy(_script + strlen(_script), "}", 4);

        n++;
        httpd_resp_sendstr_chunk(req, _script);
        memset(_script, 0, 500);

        entry = readdir(dir);
    }
    closedir(dir);
    strlcpy(_script + strlen(_script), "]", 2);
    ESP_LOGD(TAG, "[%d] => %s", strlen(_script), _script);

    httpd_resp_sendstr_chunk(req, _script);

    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".html"))
    {
        return httpd_resp_set_type(req, "text/html");
    }
    else if (IS_FILE_EXT(filename, ".htm"))
    {
        return httpd_resp_set_type(req, "text/html");
    }
    else if (IS_FILE_EXT(filename, ".jpeg"))
    {
        return httpd_resp_set_type(req, "image/jpeg");
    }
    else if (IS_FILE_EXT(filename, ".png"))
    {
        return httpd_resp_set_type(req, "image/png");
    }
    else if (IS_FILE_EXT(filename, ".ico"))
    {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    else if (IS_FILE_EXT(filename, ".mpg"))
    {
        return httpd_resp_set_type(req, "audio/mpeg");
    }
    else if (IS_FILE_EXT(filename, ".mp3"))
    {
        return httpd_resp_set_type(req, "audio/mpeg");
    }
    else if (IS_FILE_EXT(filename, ".css"))
    {
        return httpd_resp_set_type(req, "text/css");
    }
    else if (IS_FILE_EXT(filename, ".js"))
    {
        return httpd_resp_set_type(req, "application/javascript");
    }
    else if (IS_FILE_EXT(filename, ".pdf"))
    {
        return httpd_resp_set_type(req, "application/pdf");
    }

    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char *get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest)
    {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash)
    {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize)
    {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + strlen(base_path), &uri[6], pathlen + 1);
    urldecode(dest);
    /* Return pointer to path, skipping the base */
    return dest;
}

/*
 * Structure holding server handle
 * and internal socket fd in order
 * to use out of request send
 */
struct async_resp_arg
{
    httpd_handle_t hd;
    int fd;
};

/**
 * Handles to send static files
 */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_send_chunk(req, index_html_start, index_html_end - index_html_start);

    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t appcss_handler(httpd_req_t *req)
{
    set_content_type_from_file(req, "app.css");
    httpd_resp_send_chunk(req, app_css_start, app_css_end - app_css_start);

    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/**
 * Set STA or AP wifi credentials and init wifi accordingly
 */
static esp_err_t wifi_credentials_handler(httpd_req_t *req)
{
    char content[200] = {0};

    /* Truncate if content length larger than the buffer */
    size_t len = MIN(req->content_len, sizeof(content));

    int ret = httpd_req_recv(req, content, len);
    if (ret <= 0)
    { /* 0 return value indicates connection closed */
        /* Check if timeout occurred */
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    char *ssid = strstr(content, "ssid=");
    char *pass = strstr(content, "pass=");
    char *ap = strstr(content, "ap=");
    char *auth = strstr(content, "auth=");
    char _ssid[32] = {0}, _pass[32] = {0};
    int ssid_len = pass - ssid - 6;
    int pass_len = ap - pass - 6;
    int type = atoi(ap + 3);
    int _auth = atoi(auth + 5);
    strncpy(_ssid, ssid + 5, ssid_len);
    strncpy(_pass, pass + 5, pass_len);
    ESP_LOGI(TAG, "query string len: %d => %s", len, content);
    ESP_LOGI(TAG, "ssid: %s, pass: %s, type: %s, auth: %d", _ssid, _pass, type ? "AP" : "STA", _auth);

    if (ssid_len && (!pass_len || (8 <= pass_len && pass_len <= 32)))
    {
        if (type)
        {
            wifi_init_softap(_ssid, _pass, _auth);
        }
        else
        {
            wifi_init_sta(_ssid, _pass);
        }
    }

    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Handler to download a file kept on the server */
static esp_err_t files_tree_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    const char *filename = get_path_from_uri(filepath, ((struct web_server_data *)req->user_ctx)->base_path,
                                             req->uri, sizeof(filepath));
    if (!filename)
    {
        ESP_LOGE(TAG, "Filename is too long");
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* If name has trailing '/', respond with directory contents */
    if (filename[strlen(filename) - 1] == '/')
    {
        return http_resp_dir_js(req, filename);
    }

    if (stat(filepath, &file_stat) == -1)
    {
        /* If file not present on SPIFFS check if URI
         * corresponds to one of the hardcoded paths */
        if (strcmp(filename, "/index.html") == 0)
        {
            return index_handler(req);
            // } else if (strcmp(filename, "/favicon.ico") == 0) {
            //     return favicon_get_handler(req);
        }
        ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
        /* Respond with 404 Not Found */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "r");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending file : %s (%ld bytes)...", filename, file_stat.st_size);
    set_content_type_from_file(req, filename);

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *chunk = ((struct web_server_data *)req->user_ctx)->scratch;
    size_t chunksize;
    do
    {
        /* Read file in chunks into the scratch buffer */
        chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);

        if (chunksize > 0)
        {
            esp_err_t err;
            /* Send the buffer contents as HTTP response chunk */
            if ((err = httpd_resp_send_chunk(req, chunk, chunksize)) != ESP_OK)
            {
                fclose(fd);
                ESP_LOGE(TAG, "File sending failed! => %d", err);
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
        /* Keep looping till the whole file is sent */
    } while (chunksize > 0);

    /* Close file after sending complete */
    fclose(fd);
    ESP_LOGI(TAG, "File sending complete");

    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t appjs_handler(httpd_req_t *req)
{
    set_content_type_from_file(req, "app.js");
    httpd_resp_send_chunk(req, app_js_start, app_js_end - app_js_start);

    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t formatSD()
{
    FRESULT res = FR_OK;
    esp_err_t err = ESP_OK;
    const size_t workbuf_size = 4096;
    void *workbuf = NULL;
    ESP_LOGW(TAG, "partitioning card");

    workbuf = ff_memalloc(workbuf_size);
    if (workbuf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    DWORD plist[] = {20, 0};
    res = f_fdisk(0, plist, workbuf);
    if (res != FR_OK)
    {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "f_fdisk failed (%d)", res);
    }
    else
    {
        size_t alloc_unit_size = 8 * 512;
        ESP_LOGW(TAG, "formatting card, allocation unit size=%d", alloc_unit_size);
        res = f_mkfs("", FM_FAT32, alloc_unit_size, workbuf, workbuf_size);
        if (res != FR_OK)
        {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "f_mkfs failed (%d)", res);
        }
    }
    mkdir("/files/tmp", 0755); // add tmp folder

    ESP_LOGW(TAG, "partitioning card finished");
    free(workbuf);
    return err;
}

bool isFormatting;
static esp_err_t format_card_handler(httpd_req_t *req)
{
    if (isFormatting)
        return ESP_OK;
    isFormatting = true;
    formatSD();
    isFormatting = false;
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t mkdir_handler(httpd_req_t *req)
{
    char content[200] = {0};

    /* Truncate if content length larger than the buffer */
    size_t len = MIN(req->content_len, sizeof(content));

    int ret = httpd_req_recv(req, content, len);
    if (ret <= 0)
    { /* 0 return value indicates connection closed */
        /* Check if timeout occurred */
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    char *name = strstr(content, "dirname=");
    char path[256] = {};
    sprintf(path, "%s", name + 8);
    if (mkdir(path, 0755))
    {
        ESP_LOGE("", "failed to mkdir: %s", path);
        httpd_resp_set_status(req, "409");
        httpd_resp_send(req, NULL, 0);
    }
    else
    {
        httpd_resp_set_status(req, "201");
        httpd_resp_send(req, NULL, 0);
    }
    /* Respond with an empty chunk to signal HTTP response completion */
    // httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char content[200] = {0};

    /* Truncate if content length larger than the buffer */
    size_t len = MIN(req->content_len, sizeof(content));

    int ret = httpd_req_recv(req, content, len);
    if (ret <= 0)
    { /* 0 return value indicates connection closed */
        /* Check if timeout occurred */
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    char *name = strstr(content, "file=");
    char path[256] = {};
    sprintf(path, "%s", name + 5);
    if (unlink(path))
    {
        ESP_LOGE("", "failed to unlink: %s", path);
        httpd_resp_set_status(req, "409");
        httpd_resp_send(req, NULL, 0);
    }
    else
    {
        httpd_resp_set_status(req, "201");
        httpd_resp_send(req, NULL, 0);
    }
    /* Respond with an empty chunk to signal HTTP response completion */
    // httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/**
 * Function to start the file server
 */
void web_server(char *path)
{
    static struct web_server_data *server_data = NULL;

    /* Allocate memory for server data */
    server_data = calloc(1, sizeof(struct web_server_data));
    server_data->scratch = heap_caps_calloc(1, SCRATCH_BUFSIZE, MALLOC_CAP_INTERNAL);

    if (!server_data)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        vTaskDelete(NULL);
    }
    strlcpy(server_data->base_path, path,
            sizeof(server_data->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 7;
    config.stack_size = 10 * 1024;
    config.max_uri_handlers = 10;

    ESP_LOGI(TAG, "Starting HTTP Server");
    if (httpd_start(&server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start file server!");
        free(server_data->scratch);
        free(server_data);
    }

    httpd_uri_t index = {
        .uri = "/", // Match all URIs of type /path/to/file
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &index);

    httpd_uri_t app_css = {
        .uri = "/app.css", // Match all URIs of type /path/to/file
        .method = HTTP_GET,
        .handler = appcss_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &app_css);

    /* URI handler for getting files tree and display files */
    httpd_uri_t files_tree = {
        .uri = "/files/*", // Match all URIs of type /path/to/file
        .method = HTTP_GET,
        .handler = files_tree_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &files_tree);

    httpd_uri_t wifi = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = wifi_credentials_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &wifi);

    httpd_uri_t app_js = {
        .uri = "/app.js", // Match all URIs of type /path/to/file
        .method = HTTP_GET,
        .handler = appjs_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &app_js);

    httpd_uri_t format = {
        .uri = "/format",
        .method = HTTP_POST,
        .handler = format_card_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &format);

    httpd_uri_t _mkdir = {
        .uri = "/mkdir",
        .method = HTTP_POST,
        .handler = mkdir_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &_mkdir);

    httpd_uri_t _delete = {
        .uri = "/delete",
        .method = HTTP_POST,
        .handler = delete_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &_delete);
}
