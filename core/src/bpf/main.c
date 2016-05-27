#ifdef TEST_BPF

#include "sfbpf.h"
#include "sfbpf_dlt.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int
main()
{
	struct sfbpf_program fcode;
	const char *filter = "tcp";
	char data[1514];
	int len = 1514;

	memset(data, 0, sizeof(data));

	if (sfbpf_compile(1514, DLT_EN10MB, &fcode, filter, 1, 0) < 0) {
		fprintf(stderr, "%s: BPF state machine compilation failed!", __FUNCTION__);
		return EXIT_FAILURE;
	}
	
	if (fcode.bf_insns && sfbpf_filter(fcode.bf_insns, data, len, len) == 0) {
		fprintf(stderr, "Packet ignored!\n");
	}
	
	sfbpf_freecode(&fcode);

	return EXIT_SUCCESS;
}

#endif /* !TEST_BPF */
