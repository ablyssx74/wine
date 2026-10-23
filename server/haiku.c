/*
 * Server-side access to another team's memory on Haiku
 *
 * Haiku has neither ptrace() nor process_vm_readv(). The only supported way into another team is its debug nub: install a
 * debugger for the team, ask the nub for the bytes (1 KB per message), and remove the debugger again. The debugger is only
 * installed for the duration of one transfer, because a team that has one stops at every exception and waits for it, where it
 * would otherwise reach debug_server.
 *
 * This file takes <OS.h>, which clashes with the server's own names, so it must not include anything of the server's.
 */

#include "config.h"

#ifdef __HAIKU__

#include <string.h>
#include <OS.h>
#include <debugger.h>

#include "haiku.h"

#define NUB_TIMEOUT 5000000    /* microseconds */

static int nub_request( port_id nub, port_id reply_port, int32 code, void *msg, size_t msg_size, void *reply, size_t reply_size )
{
    int32 reply_code;

    if (write_port_etc( nub, code, msg, msg_size, B_RELATIVE_TIMEOUT, NUB_TIMEOUT ) != B_OK) return 0;
    return read_port_etc( reply_port, &reply_code, reply, reply_size, B_RELATIVE_TIMEOUT, NUB_TIMEOUT ) >= 0;
}

/* read from the clone of the area that holds addr; returns the number of bytes copied (at most those left in the area) */
static ssize_t clone_read( port_id nub, port_id reply_port, unsigned long addr, size_t len, char *buffer )
{
    debug_nub_clone_area msg;
    debug_nub_clone_area_reply reply;
    area_info info;
    size_t avail;

    msg.reply_port = reply_port;
    msg.address = (const void *)addr;
    reply.area = B_ERROR;
    if (!nub_request( nub, reply_port, B_DEBUG_MESSAGE_CLONE_AREA, &msg, sizeof(msg), &reply, sizeof(reply) )) return -1;
    if (reply.area < 0) return -1;
    if (get_area_info( reply.area, &info ) != B_OK)
    {
        delete_area( reply.area );
        return -1;
    }
    avail = (char *)info.address + info.size - (const char *)reply.address;
    if (len > avail) len = avail;
    memcpy( buffer, reply.address, len );
    delete_area( reply.area );
    return len;
}

/* read or write a team's memory; returns the number of bytes transferred (less than size if part of it is not accessible),
 * or -1 if the team cannot be reached */
ssize_t haiku_team_memory( int team, int write, unsigned long addr, size_t size, char *buffer )
{
    port_id debugger_port, reply_port, nub;
    size_t done = 0;

    if ((debugger_port = create_port( 1, "wineserver debugger" )) < 0) return -1;
    if ((reply_port = create_port( 1, "wineserver nub reply" )) < 0)
    {
        delete_port( debugger_port );
        return -1;
    }
    if ((nub = install_team_debugger( team, debugger_port )) < 0)
    {
        delete_port( reply_port );
        delete_port( debugger_port );
        return -1;
    }

    while (done < size)
    {
        size_t len = size - done;
        if (len > B_MAX_READ_WRITE_MEMORY_SIZE) len = B_MAX_READ_WRITE_MEMORY_SIZE;

        if (write)
        {
            debug_nub_write_memory msg;
            debug_nub_write_memory_reply reply;

            msg.reply_port = reply_port;
            msg.address = (void *)(addr + done);
            msg.size = len;
            memcpy( msg.data, buffer + done, len );
            if (!nub_request( nub, reply_port, B_DEBUG_MESSAGE_WRITE_MEMORY, &msg, sizeof(msg), &reply, sizeof(reply) )) break;
            if (reply.error != B_OK || reply.size <= 0) break;
            done += reply.size;
        }
        else
        {
            debug_nub_read_memory msg;
            debug_nub_read_memory_reply reply;

            reply.error = B_ERROR;
            reply.size = 0;
            msg.reply_port = reply_port;
            msg.address = (void *)(addr + done);
            msg.size = len;
            nub_request( nub, reply_port, B_DEBUG_MESSAGE_READ_MEMORY, &msg, sizeof(msg), &reply, sizeof(reply) );
            if (reply.error != B_OK || reply.size <= 0)
            {
                /* the page may be one that Haiku keeps at no access although the program has it readable (the pages of a
                 * mapping that was reserved and then opened up piece by piece): the nub cannot read those, a clone of the area can */
                ssize_t got = clone_read( nub, reply_port, addr + done, len, buffer + done );
                if (got <= 0) break;
                done += got;
                continue;
            }
            memcpy( buffer + done, reply.data, reply.size );
            done += reply.size;
        }
    }

    remove_team_debugger( team );
    delete_port( reply_port );
    delete_port( debugger_port );
    return done;
}

#endif /* __HAIKU__ */
