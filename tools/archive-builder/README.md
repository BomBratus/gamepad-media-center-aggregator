# Local Archive builder

This development tool builds the same schema-v2 SQLite index as PS4, using
`buildImdbIndex`. It does not replace the console builder or change app behavior.
Supply your own locally downloaded IMDb datasets in a private directory. This
tool has no upload or distribution mechanism; do not publish datasets or indexes.

```sh
cmake -S tests/imdb -B archive-tools -DCMAKE_BUILD_TYPE=Release \
  -DGMCA_ARCHIVE_BUILDER=ON \
  -DGMCA_JSON_INCLUDE="$PWD/library/borealis/library/include/borealis/extern"
cmake --build archive-tools --target gmca-archive-builder -j2
archive-tools/gmca-archive-builder /absolute/private/index.sqlite /private/imdb-datasets
archive-tools/gmca-archive-builder --validate /absolute/private/index.sqlite
```

Inputs are `title.ratings.tsv.gz`, `title.basics.tsv.gz`, and `title.akas.tsv.gz`.
The source files stay intact. Temporary copies and `.building` staging share the
output directory; imports resume after interruption. SIGINT/SIGTERM cancel at
cooperative checkpoints. Previous published output remains intact on failure.

Phase timings go to stderr. The final stdout JSON reports schema, indexed and
browsable counts, refresh timestamp, exact bytes and SHA-256 after quick_check.
Move a privately built index to your own console only while GMCA is stopped.
No transfer, installation or public dataset hosting is performed by the tool.
