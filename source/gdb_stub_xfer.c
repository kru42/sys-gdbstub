
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "gdb_stub_priv.h"


typedef struct
{
    const char* object;
    const char* op;
    const char* annex;
    size_t offset;
    size_t length;
} gdb_xfer_req_t;

typedef struct
{
    const char* object;
    bool (*func)(gdb_stub_t* stub, gdb_xfer_req_t* req);
} gdb_xfer_handler_t;

static bool gdb_stub_xfer_osdata(gdb_stub_t* stub, gdb_xfer_req_t* req);

static const gdb_xfer_handler_t xfer_handler[] =
{
    { "osdata", gdb_stub_xfer_osdata },
};

static const char* proc_list_header =
        "<osdata type=\"processes\">\n";

static const char* proc_list_fmt =
        "<item>\n"
        "<column name=\"pid\">%u</column>\n"
        "<column name=\"command\">%s</column>\n"
        "</item>\n";

static const char* proc_list_footer = "</osdata>";

static const char* thread_list_header =
        "<?xml version=\"1.0\"?>\n"
        "<threads>\n";

static const char* thread_list_footer = "</threads>";
static const char* thread_list_fmt = "<thread id=\"%s\" name=\"%s\" />";

bool gdb_stub_query_xfer(gdb_stub_t* stub, char* packet, size_t length)
{
    logf("%s\n", __FUNCTION__);
    size_t index = 0;
    gdb_xfer_req_t req;
    req.object = "";
    req.op = "";
    req.annex = "";
    req.offset = 0u;
    req.length = 0u;

    char* token = packet;
    char* pos = packet;

    while (token != NULL)
    {
        while (*pos != '\0' && *pos != ':')
        {
            pos++;
        }

        bool last = *pos == '\0';
        if (!last)
        {
            *pos = '\0';
            pos++;
        }

        switch(index)
        {
        case 1:
            req.object = token;
            break;
        case 2:
            req.op = token;

            if (strcmp(req.op, "read") != 0)
            {
                logf("qXfer: unsupported op: %s\n", req.op);
                return false;
            }
            break;
        case 3:
            req.annex = token;
            break;
        case 4:
        {
            char* end = token;
            // should be offset,length
            req.offset = strtoul(end, &end, 16);
            if (*end == ',')
            {
                end++;
                req.length = strtoul(end, &end, 16);
            }
            break;
        }
        }

        index++;
        token = last ? NULL : pos;
    }

    logf("xfer (object=%s, op=%s, annex=%s, offset=%lu, length=%lu)\n", req.object, req.op, req.annex, req.offset, req.length);

    for(size_t i = 0u; i < sizeof(xfer_handler) / sizeof(xfer_handler[0]); ++i)
    {
        if (strncmp(req.object, xfer_handler[i].object, strlen(xfer_handler[i].object)) == 0)
        {
            return xfer_handler[i].func(stub, &req);
        }
    }

    return false;
}

static bool xfer_printf(gdb_stub_t* stub, const char* fmt, ...)
{
    int remaining = sizeof(stub->xfer) - stub->xfer_pos;

    if (remaining > 0)
    {
        va_list arglist;
        va_start(arglist, fmt);
        stub->xfer_pos += vsnprintf(&stub->xfer[stub->xfer_pos], remaining, fmt, arglist);
        va_end(arglist);

        if (stub->xfer_pos >= sizeof(stub->xfer))
        {
            stub->xfer_pos = sizeof(stub->xfer) - 1u;
            logf("xfer truncated (len=%lu)\n", stub->xfer_pos);
            return false;
        }

        return true;
    }
    
    logf("xfer truncated (len=%lu)\n", stub->xfer_pos);
    return false;
}

static bool xfer_snap_processes(gdb_stub_t* stub)
{
    Result res;
    u64 our_pid;
    u64 pids[100];
    s32 num_pids = 0;

    stub->xfer[0] = '\0';
    stub->xfer_pos = 0u;

    if (!xfer_printf(stub, "%s", proc_list_header))
    {
        goto err;
    }

    // get our pid
    res = svcGetProcessId(&our_pid, CUR_PROCESS_HANDLE);
    if (R_FAILED(res))
    {
        goto err;
    }

    // get the process list
    res = svcGetProcessList(&num_pids, pids, sizeof(pids) / sizeof(pids[0]));
    if (R_FAILED(res))
    {
        goto err;
    }

    logf("printing %d processes\n", num_pids);
    for(s32 i = 0; i < num_pids; ++i)
    {
        Handle proc;
        debug_event_t event;

        // don't try to debug our process
        if (pids[i] == our_pid)
        {
            continue;
        }

        logf("\t\tdebugging pid %lu\n", pids[i]);
        res = svcDebugActiveProcess(&proc, pids[i]);
        if (R_FAILED(res))
        {
            logf("failed\n");
            continue;
        }

        bool ok = true;

        while(R_SUCCEEDED(svcGetDebugEvent((u8*)&event, proc)))
        {
            if (event.type == DEBUG_EVENT_ATTACH_PROCESS)
            {
                if (!xfer_printf(stub, proc_list_fmt,
                    event.attach_process.process_id, event.attach_process.process_name))
                {
                    ok = false;
                }
                break;
            }
        }

        svcCloseHandle(proc);
        if (!ok)
        {
            logf("failed to print process (pid=%lu)\n", pids[i]);
            goto err;
        }
    }
    
    if (!xfer_printf(stub, "%s", proc_list_footer))
    {
        goto err;
    }

    return true;

err:
    stub->xfer[0] = '\0';
    stub->xfer_pos = 0u;
    return false;
}

static bool gdb_stub_xfer_osdata(gdb_stub_t* stub, gdb_xfer_req_t* req)
{
    logf("%s\n", __FUNCTION__);

    // TODO: check if the annex is different than the snapshot
    if (req->offset == 0u)
    {
        if (*req->annex == '\0' ||
            strcmp(req->annex, "processes") == 0)
        {
            if (!xfer_snap_processes(stub))
            {
                gdb_stub_send_error(stub, 0u);
                return true;
            }
        }
        else
        {
            return false;
        }
    }

    if (stub->xfer_pos != 0u)
    {
        logf("xfer_pos=%lu\n", stub->xfer_pos);
        bool more = true;
        size_t start = req->offset;
        size_t end = req->offset + req->length;

        if (end >= stub->xfer_pos)
        {
            end = stub->xfer_pos;
            more = false;
        }

        logf("start=%lu, end=%lu, more=%d\n", start, end, more ? 1u : 0u);
        gdb_stub_packet_begin(stub);
        gdb_stub_packet_write(stub, more ? "m" : "l", 1u);
        if (start < end)
        {
            gdb_stub_packet_write(stub, &stub->xfer[start], end - start);
        }
        gdb_stub_packet_end(stub);
        return true;
    }

    return false;
}
