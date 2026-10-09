/* xmlc -- a non-validating XML 1.0 (Fifth Edition) well-formedness checker.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * STUB.  This commit exists to stand the oracle up before the parser, because
 * the oracle is the deliverable and an oracle nobody has watched fail is
 * decoration.  It rejects every input.  Against the W3C conformance suite that
 * scores a perfect 1498/1498 on `not-wf` and 0/812 on `valid`, which is the
 * whole argument for publishing both numbers: a `not-wf` score on its own
 * measures nothing at all.
 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	const char *path = NULL;
	int i;

	for (i = 1; i < argc; i++)
		if (argv[i][0] != '-')
			path = argv[i];
	if (path) {
		FILE *f = fopen(path, "rb");
		if (!f) {
			fprintf(stderr, "xmlc: %s: cannot read\n", path);
			return 2;
		}
		fclose(f);
	}
	fprintf(stderr, "xmlc: not implemented\n");
	return 1;
}
