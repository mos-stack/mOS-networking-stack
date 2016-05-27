#ifndef _APP_LIB_H_
#define _APP_LIB_H_

#define MAX_LINE_LEN  (1000)
#define MAX_FLOW_NUM  (1000000)
#define MAX_EVENTS    (MAX_FLOW_NUM * 3)
#define MAX_CPUS      (16)                      // max number of CPU cores

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define TIMEVAL_TO_MSEC(t)		((t.tv_sec * 1000) + (t.tv_usec / 1000))
#define TIMEVAL_TO_USEC(t)		((t.tv_sec * 1000000) + (t.tv_usec))
#define TS_GT(a,b)				((int64_t)((a)-(b)) > 0)

/*----------------------------------------------------------------------------*/
#define CONF_VALUE_LEN 100
struct conf_var {
	char *name;
	char value[CONF_VALUE_LEN + 1];
};
/*----------------------------------------------------------------------------*/

char *
strevent(uint64_t ev);

char *
strcbevent(uint64_t ev);

int
LoadConfig(char *file_name, struct conf_var *vlist, int size);

#endif
