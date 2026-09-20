# What the codestyle checks look at: everything git tracks, minus the trees that
# are not ours to reformat.
#
#   dep/             vendored libraries
#   src/modules/SD3  the script library, kept in its upstream shape
#   extra/doc        prose, where two trailing spaces are a Markdown line break
#
# codestyle_minimum_files is a floor, not decoration: grep reports "nothing found"
# and "nothing to look at" with the same exit code, so a wrong root or a broken
# checkout would otherwise pass this job for ever.

codestyle_exclude_re='^(dep/|src/modules/SD3/|extra/doc/)'
codestyle_minimum_files=800
