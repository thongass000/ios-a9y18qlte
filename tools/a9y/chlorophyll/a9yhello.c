// a9y18qlte: a dylib whose constructor only says which process loaded it, to stderr. Run a platform binary with
// DYLD_INSERT_LIBRARIES=/usr/local/chl/a9yhello.dylib: the line appears when dyld on this kernel lets an inserted
// library into it (the route to Chlorophyll in backboardd), and is missing when dyld drops the variable.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

__attribute__((constructor)) static void
a9yhello(void)
{
	fprintf(stderr, "a9yhello: loaded into %s (pid %d, uid %d)\n", getprogname(), getpid(), getuid());
}
