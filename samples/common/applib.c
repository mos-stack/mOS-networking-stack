
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <mos_api.h>
#include <mtcp_api.h>
#include <mtcp_epoll.h>
#include <assert.h>
#include <ctype.h>

#include "applib.h"
/*----------------------------------------------------------------------------*/
char *
strevent(uint64_t ev)
{
	switch (ev) {
#define EV_CASE(e) case e: return #e
		/* mtcp-defined epoll events */
		EV_CASE(MOS_EPOLLNONE);
		EV_CASE(MOS_EPOLLIN);
		EV_CASE(MOS_EPOLLPRI);
		EV_CASE(MOS_EPOLLOUT);
		EV_CASE(MOS_EPOLLRDNORM);
		EV_CASE(MOS_EPOLLRDBAND);
		EV_CASE(MOS_EPOLLWRNORM);
		EV_CASE(MOS_EPOLLWRBAND);
		EV_CASE(MOS_EPOLLMSG);
		EV_CASE(MOS_EPOLLERR);
		EV_CASE(MOS_EPOLLHUP);
		EV_CASE(MOS_EPOLLRDHUP);

		/* mtcp-defined epoll events */
		EV_CASE(MOS_EPOLLONESHOT);
		EV_CASE(MOS_EPOLLET);
		
		default:
			return "Unknown";

#undef EV_CASE
	}
}
/*----------------------------------------------------------------------------*/
char *
strcbevent(uint64_t ev)
{
	switch (ev) {
#define EV_CASE(e) case e: return #e
		
		/* mos-defined tcp build-in events */
		EV_CASE(MOS_ON_PKT_IN);
		EV_CASE(MOS_ON_PKT_OUT);
		EV_CASE(MOS_ON_CONN_SETUP);
		EV_CASE(MOS_ON_CONN_NEW_DATA);
		EV_CASE(MOS_ON_TCP_STATE_CHANGE);
		EV_CASE(MOS_ON_CONN_END);
		EV_CASE(MOS_ON_TIMEOUT);
		EV_CASE(MOS_ON_ERROR);
		EV_CASE(MOS_ON_ORPHAN);
		
		default:
			return "Unknown";

#undef EV_CASE
	}
}

/*----------------------------------------------------------------------------*/
#define SKIP_SPACES(x) while (*x && isspace((int)*x)) x++;
/*----------------------------------------------------------------------------*/
int
LoadConfig(char *file_name, struct conf_var *vlist, int size)
{
	assert(file_name != NULL);

	FILE *fp = fopen(file_name, "r");
	char line_buf[MAX_LINE_LEN] = {0};
	int i;
	char *p;

	if (fp == NULL) {
		fprintf(stderr, "%s not found\n", file_name);
		return -1;
	}

	while ((p = fgets(line_buf, MAX_LINE_LEN, fp)) != NULL) {
		char *temp;

		if (p[MAX_LINE_LEN - 1]) {
			fprintf(stderr, "%s has a line longer than %d\n",
					file_name, MAX_LINE_LEN);
			return -1;
		}

		SKIP_SPACES(p);
		if (*p == '\0' || *p == '#')
			continue;

		/* comments */
		if ((temp = strchr(p, '#')))
			*temp = '\0';

		/* remove spaces at the end of the line */
		while (isspace(p[strlen(p) - 1]))
			p[strlen(p) - 1] = '\0';

		for (i = 0; i < size; i++) {
			int len = strlen(vlist[i].name);
			if (strncmp(vlist[i].name, p, len))
				continue;

			if (!isspace(p[len]))
				continue;

			if (!(p = strchr(p, '=')))
				break;

			p++;
			SKIP_SPACES(p);

			if ((len = strlen(p)) > CONF_VALUE_LEN)
				break;

			strncpy(vlist[i].value, p, len);
			vlist[i].value[len] = '\0';
		}
	}
	
	fclose(fp);
	return 0;
}
/*----------------------------------------------------------------------------*/
