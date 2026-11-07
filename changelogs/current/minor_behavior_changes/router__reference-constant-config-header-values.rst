Header values added or replaced by static route configuration (``request_headers_to_add`` and
``response_headers_to_add`` entries whose values contain no substitution commands) are now applied
by reference to configuration-owned storage instead of being copied per request, removing a
per-request allocation for values above the small-string limit. Formatted values are unchanged.
