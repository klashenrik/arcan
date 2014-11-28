#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>
#include <math.h>

#include <stdbool.h>
#include <sys/types.h>
#include <arcan_math.h>
#include <arcan_general.h>

unsigned arcan_glob(char* basename, enum arcan_namespaces space,
	void (*cb)(char*, void*), void* tag)
{
	unsigned count = 0;

	if (!basename || verify_traverse(basename) == NULL)
		return 0;

/* need to track so that we don't glob namespaces that
 * happen to collide */
	size_t nspaces = (size_t) log2(RESOURCE_SYS_ENDM);

	char* globslots[ nspaces ];
	memset(globslots, '\0', sizeof(globslots));
	size_t ofs = 0;

	for (size_t i = 1; i <= RESOURCE_SYS_ENDM; i <<= 1){
		if ( (space & i) == 0 )
			continue;

		glob_t res = {0};
		char* path = arcan_expand_resource(basename, i);
		bool match = false;

		for (size_t i = 0; i < ofs; i++){
			if (globslots[ i ] == NULL)
				break;

			if (strcmp(path, globslots[i]) == 0){
				arcan_mem_free(path);
				match = true;
			}
		}

		if (match)
			continue;

		globslots[ofs++] = path;

		if ( glob(path, 0, NULL, &res) == 0 ){
			char** beg = res.gl_pathv;

			while(*beg){
				cb(strrchr(*beg, '/') ? strrchr(*beg, '/')+1 : *beg, tag);
				beg++;
				count++;
			}

			globfree(&res);
		}
	}

	for (size_t i = 0; i < nspaces && globslots[i] != NULL; i++)
		arcan_mem_free(globslots[i]);

	return count;
}

