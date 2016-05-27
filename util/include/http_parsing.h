
#ifndef __NRE_HTTP_PARSING
#define __NRE_HTTP_PARSING


#define HTTP_GET           "GET"
#define CONTENT_LENGTH_HDR "Content-Length:"
#define CONN_HDR_FLD 		"Connection:"
#define KEEP_ALIVE_STR 		"Keep-Alive"

enum {
	GET = 1,
	POST
};

int find_http_header(char *data, int len);
int is_http_response(char *data, int len);
int is_http_request(char *data, int len);

char* http_header_str_val(const char* buf, const char *key, const int key_len, char* value, int value_len);
long int http_header_long_val(const char* buf, const char *key, int key_len);

char* http_get_http_version_resp(char* data, int len, char* value, int value_len);
char* http_get_url(char * data, int data_len, char* value, int value_len);
int http_get_status_code(void *response);
int http_get_maxage(char *cache_ctl, int len);

time_t http_header_date(const char* data, const char* field, int len);

enum {HTTP_09, HTTP_10, HTTP_11}; /* http version */
int http_parse_first_resp_line(const char* data, int len, int* scode, int* ver);
int http_check_header_field(const char* data, const char* field);

#endif
