# apex.jobs/1: background work

`submit(host, user, work, done, &job_id)` runs `work(user, job_context)` on a worker thread and then calls
`done(user, status)` **on the editor thread**, never on the worker. `work` returns an `apex_status`, and must not
call host interfaces other than checking `job_context->cancelled(job_context->job)` (non-zero once the job was
cancelled). Do not touch the document or any shared state from `work` without your own locking: read what you need
before submitting.

- `submit` and `cancel` are thread-safe. `submit` is `APEX_ERR_STATE` once the host is shutting down.
- `cancel(host, job_id)` returns `APEX_OK` if the completion had not been delivered; `done` is then never called
  for that job. It returns `APEX_ERR_NOT_FOUND` for a finished (delivered) or unknown job.
- If `work` throws, `done` receives `APEX_ERR_INTERNAL`. An exception in `done` is caught and logged.
- At shutdown the host cancels, joins and discards pending jobs before the plugin is unloaded, so `done` is not
  called then. The `user` pointer must stay valid until `done` runs or the job is cancelled.

The host interface is Qt-free: the application calls `Host::drainJobs()` regularly (the Qt shell does so from a
50 ms timer, `PluginPump`). A host embedded elsewhere must call it from its own main thread.

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_job_context {
    uint32_t struct_size;
    uint32_t reserved;
    void* job;
    int (*cancelled)(void* job);
} apex_job_context;
typedef apex_status (*apex_job_work)(void* user, const apex_job_context* context);
typedef void (*apex_job_done)(void* user, apex_status status);
typedef struct apex_jobs_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*submit)(void* host, void* user, apex_job_work work, apex_job_done done, uint64_t* job_id);
    apex_status (*cancel)(void* host, uint64_t job_id); /* APEX_ERR_NOT_FOUND once done() has been delivered */
} apex_jobs_v1;
```

No example plugin submits a job; the C++ `Job` base class wraps the table.
