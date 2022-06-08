#include "types.h"

#define NVMA 16		/* how many vma area support */

struct vma_area {
	char *addr;		/* starting address of mapping */
	uint64 len;
	int prot;
	int flags;	
	struct file *file;	
	uint64 off;
};