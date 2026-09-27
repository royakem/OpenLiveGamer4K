# Repository boundaries

The public repository contains reviewed source and user documentation only.
Never add internal plans, agent handovers, lab access details, recordings,
credentials, personal paths or unreviewed images. Preserve author and upstream
attribution. Public contact: royakemartinsson@gmail.com.

If `private/PUBLISHING.md` exists in this checkout, this is the private development
repository: read that protocol before preparing any export. Never push its Git
history to the public remote or merge a development branch into publication.

In a publication checkout, `.publication-files` is the explicit file allowlist.
Run `python3 tools/check-publication.py --history` before pushing. New files and
image hashes require review; do not weaken the check just to make a push pass.
A local hook and CI checks complement review; neither proves that content is
free of every possible secret. Commit only under an approved project identity.
