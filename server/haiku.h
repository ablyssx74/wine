/*
 * Server-side access to another team's memory on Haiku (see haiku.c)
 */

#ifndef __WINE_SERVER_HAIKU_H
#define __WINE_SERVER_HAIKU_H

#include <sys/types.h>

extern ssize_t haiku_team_memory( int team, int write, unsigned long addr, size_t size, char *buffer );

#endif /* __WINE_SERVER_HAIKU_H */
