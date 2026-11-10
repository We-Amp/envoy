Route matching now evaluates each route's path condition before the remaining match criteria
(headers, query parameters, runtime fraction, cookies, and so on), so configurations with many
routes spend noticeably less time per request on routes that cannot match. Matching semantics are
unchanged.
