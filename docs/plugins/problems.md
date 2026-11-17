# apex.problems/1

Fetch `"apex.problems/1"`. In `activate`, `register_check(host, id, name, user, check)` adds a check
(`id` is `plugin.<yourid>.<name>`). Whenever Problems refreshes, `check(user, sink)` runs on the editor thread and
reports `{severity (`APEX_PROBLEM_ERROR` 0, `APEX_PROBLEM_WARNING` 1, `APEX_PROBLEM_NOTE` 2), text, objects}` through the sink; a bad severity, an object
that is not in the document or a bad `struct_size` rejects that one report. The Problems list groups plugin
reports under a header per plugin (Apex's own come first), and clicking one selects its `objects`. A check that
throws or fails reports one warning ("failed") and none of its partial results. Registration closes once the
first Problems run starts, so register in `activate`.

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_problem {
    uint32_t struct_size;
    uint32_t severity; /* 0 error, 1 warning, 2 note */
    apex_str text;
    const apex_node_id* objects; size_t object_count;
} apex_problem;
typedef struct apex_problem_sink {
    uint32_t struct_size;
    uint32_t reserved;
    void* sink;
    apex_status (*report)(void* sink, const apex_problem* problem);
} apex_problem_sink;
typedef apex_status (*apex_problem_check)(void* user, const apex_problem_sink* sink);
typedef struct apex_problems_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_check)(void* host, apex_str id, apex_str name, void* user, apex_problem_check check);
} apex_problems_v1;
```

<!-- snippet: include/apex/apex_plugin.h -->
```c
enum apex_problem_severity { APEX_PROBLEM_ERROR = 0, APEX_PROBLEM_WARNING = 1, APEX_PROBLEM_NOTE = 2 };
```

No example plugin reports a problem; the C++ `ProblemCheck` base class wraps the table.
