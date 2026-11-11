Virtual hosts with many routes now build a config-time route index (an exact-path map and a
prefix trie, honoring case sensitivity for exact, prefix, path-separated-prefix and google_re2
regex routes) and evaluate only candidate routes per request, in configured order; when no
candidate exists, no route is evaluated. Regex, URI-template and connect routes without an
indexable shape keep their ordered scan position. Route selection is unchanged, verified by
differential and randomized index-vs-linear-scan tests covering alternation, quantifier, group,
escape, fold-case, Latin-1 byte and case-insensitive-exact patterns.
