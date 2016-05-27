#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include "http_parsing.h"
#include "tdate_parse.h"

#define SPACE_OR_TAB(x)  ((x) == ' '  || (x) == '\t')
#define CR_OR_NEWLINE(x) ((x) == '\r' || (x) == '\n')

/*---------------------------------------------------------------------------*/
int 
find_http_header(char *data, int len)
{
	char *temp = data;
	int hdr_len = 0;
	char ch = data[len]; /* remember it */

	/* null terminate the string first */
	data[len] = 0;
	while (!hdr_len && (temp = strchr(temp, '\n')) != NULL) {
		temp++;
		if (*temp == '\n') 
			hdr_len = temp - data + 1;
		else if (len > 0 && *temp == '\r' && *(temp + 1) == '\n') 
			hdr_len = temp - data + 2;
	}
	data[len] = ch; /* put it back */

	/* terminate the header if found */
	if (hdr_len) 
		data[hdr_len-1] = 0;

	return hdr_len;
}
/*--------------------------------------------------------------------------*/
char *
http_header_str_val(const char* buf, const char *key, const int keylen, 
		    char* value, int value_len)
{
	char *temp = (char *) buf;  /* temporarily de-const buf */
	int i = 0;
	
	/* find the header field */
	while (*temp && (temp = strchr(temp, '\n'))) {
		temp++; /* advance to the next line */
		if (!strncasecmp(temp, key, keylen))
			break;
	}
	if (temp == NULL || *temp == '\0') {
		*value = 0;
		return NULL;
	}
	
	/* skip whitespace or tab */
	temp += keylen;
	while (*temp && SPACE_OR_TAB(*temp))
		temp++;
	
	/* if we reached the end of the line, forget it */
	if (*temp == '\0' || CR_OR_NEWLINE(*temp)) {
		*value = 0;
		return NULL;
	}
	
	/* copy value data */
	while (*temp && !CR_OR_NEWLINE(*temp) && i < value_len-1)
		value[i++] = *temp++;
	value[i] = 0;
	return (i == 0) ? NULL : value; 
}
/*---------------------------------------------------------------------------*/
long int 
http_header_long_val(const char * response, const char* key, int key_len)
{
#define C_TYPE_LEN 50
	long int len;
	char value[C_TYPE_LEN];
	char *temp = http_header_str_val(response, key, key_len, value, C_TYPE_LEN);

	if (temp == NULL)
		return -1;

	len = strtol(temp, NULL, 10);
	if (errno == EINVAL || errno == ERANGE)
		return -1;

	return len;
}
/*--------------------------------------------------------------------------*/
char*
http_get_url(char * data, int data_len, char* value, int value_len)
{
	char *ret = data;
	char *temp;
	int i = 0;

	if (strncmp(data, HTTP_GET, sizeof(HTTP_GET)-1)) {
		*value = 0;
		return NULL;
	}
	
	ret += sizeof(HTTP_GET);
	while (*ret && SPACE_OR_TAB(*ret)) 
		ret++;

	temp = ret;
	while (*temp && *temp != ' ' && i < value_len - 1) {
		value[i++] = *temp++;
	}
	value[i] = 0;
	
	return ret;
}
/*---------------------------------------------------------------------------*/

