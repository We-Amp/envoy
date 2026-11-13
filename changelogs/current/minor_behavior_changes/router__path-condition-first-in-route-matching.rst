Route matching now evaluates the path condition of prefix, exact path and path-separated-prefix
routes before the remaining match criteria (headers, query parameters, runtime fraction, cookies,
and so on), so configurations with many routes spend less time per request on routes that cannot
match. Regex and URI-template routes keep evaluating the other criteria first. Matching semantics
are unchanged.
