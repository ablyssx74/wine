/*
 * Emulator initialisation code
 *
 * Copyright 2000 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <limits.h>
#ifdef __HAIKU__
#include <errno.h>
#include <image.h>
#include <signal.h>
#endif
#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif
#ifdef __APPLE__
# include <mach-o/dyld.h>
#endif

#include "main.h"

#if defined(__APPLE__) && defined(__x86_64__) && !defined(HAVE_WINE_PRELOADER)

/* Not using the preloader on x86_64:
 * Reserve the same areas as the preloader does, but using zero-fill sections
 * (the only way to prevent system frameworks from using them, including allocations
 * before main() runs).
 */
__asm__(".zerofill WINE_RESERVE,WINE_RESERVE");
static char __wine_reserve[0x1fffff000] __attribute__((section("WINE_RESERVE, WINE_RESERVE")));

static const struct wine_preload_info preload_info[] =
{
    { __wine_reserve,  sizeof(__wine_reserve)  }, /*         0x1000 -    0x200000000: low 8GB */
    { 0, 0 }                                      /* end of list */
};

const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = preload_info;

static void init_reserved_areas(void)
{
    int i;

    for (i = 0; wine_main_preload_info[i].size != 0; i++)
    {
        /* Match how the preloader maps reserved areas: */
        mmap(wine_main_preload_info[i].addr, wine_main_preload_info[i].size, PROT_NONE,
             MAP_FIXED | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1, 0);
    }
}

#else

/* the preloader will set this variable */
const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = NULL;

static void init_reserved_areas(void)
{
}

#endif

/* canonicalize path and return its directory name */
static char *realpath_dirname( const char *name )
{
    char *p, *fullpath = realpath( name, NULL );

    if (fullpath)
    {
        p = strrchr( fullpath, '/' );
        if (p == fullpath) p++;
        if (p) *p = 0;
    }
    return fullpath;
}

/* if string ends with tail, remove it */
static char *remove_tail( const char *str, const char *tail )
{
    size_t len = strlen( str );
    size_t tail_len = strlen( tail );
    char *ret;

    if (len < tail_len) return NULL;
    if (strcmp( str + len - tail_len, tail )) return NULL;
    ret = malloc( len - tail_len + 1 );
    memcpy( ret, str, len - tail_len );
    ret[len - tail_len] = 0;
    return ret;
}

/* build a path from the specified dir and name */
static char *build_path( const char *dir, const char *name )
{
    size_t len = strlen( dir );
    char *ret = malloc( len + strlen( name ) + 2 );

    memcpy( ret, dir, len );
    if (len && ret[len - 1] != '/') ret[len++] = '/';
    strcpy( ret + len, name );
    return ret;
}

static const char *get_self_exe(void)
{
#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__NetBSD__)
    return "/proc/self/exe";
#elif defined (__FreeBSD__) || defined(__DragonFly__)
    static int pathname[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t path_size = PATH_MAX;
    char *path = malloc( path_size );
    if (path && !sysctl( pathname, sizeof(pathname)/sizeof(pathname[0]), path, &path_size, NULL, 0 ))
        return path;
    free( path );
#elif defined(__APPLE__)
    uint32_t path_size = PATH_MAX;
    char *path = malloc( path_size );
    if (path && !_NSGetExecutablePath( path, &path_size ))
        return path;
    free( path );
#elif defined(__HAIKU__)
    image_info info;
    int32 cookie = 0;
    while (get_next_image_info( B_CURRENT_TEAM, &cookie, &info ) == B_OK)
        if (info.type == B_APP_IMAGE) return strdup( info.name );
#endif
    return NULL;
}

static void *try_dlopen( const char *argv0 )
{
    char *dir, *path, *p;
    void *handle;

    if (!argv0) return NULL;
    if (!(dir = realpath_dirname( argv0 ))) return NULL;

    if ((p = remove_tail( dir, "/loader" )))
        path = build_path( p, "dlls/ntdll/ntdll.so" );
    else
        path = build_path( dir, "ntdll.so" );

    handle = dlopen( path, RTLD_NOW );
    free( p );
    free( dir );
    free( path );
    return handle;
}


#ifdef __HAIKU__
/* Haiku's file system (BFS) cannot preallocate, posix_fallocate() fails with EOPNOTSUPP. Mesa's shader disk cache, which the Vulkan
 * driver loads into every Wine process that uses DXVK, sizes its index file with posix_fallocate() and gives up on the whole cache
 * when that fails: nothing is ever stored and every shader is compiled again at every start (pauses of seconds in games).
 * The symbol of the program is found before the one of libroot by every library of the process, so defining it here lets the
 * driver use it: the system's function first, and when it answers EOPNOTSUPP the file is extended with ftruncate() instead (which
 * is what Mesa itself does without posix_fallocate, and what recent Mesa does on EOPNOTSUPP). */
__attribute__((visibility("default"))) int posix_fallocate( int fd, off_t offset, off_t len )
{
    static int (*real)( int, off_t, off_t );
    struct stat st;
    int ret;

    if (!real) real = dlsym( RTLD_NEXT, "posix_fallocate" );
    if (real && (ret = real( fd, offset, len )) != EOPNOTSUPP) return ret;
    if (fstat( fd, &st )) return errno;
    if (st.st_size >= offset + len) return 0;
    return ftruncate( fd, offset + len ) ? errno : 0;
}

/* Wine suspends a thread (SuspendThread, which Chromium's stack sampling profiler calls all the time) with a signal that parks the
 * thread wherever it is. In dlopen() Haiku's runtime loader holds its global lock, and a thread parked there with the lock keeps
 * every thread that needs the loader (pthread_create(), dlopen(), dladdr()) waiting for ever: Steam's browser hung in its first
 * seconds, when it loads many libraries (and the three audio drivers that are not there, which make the loader write a kernel debug
 * line each time), and its helper processes never got started. The suspend signal (SIGUSR1) is blocked while dlopen() runs, so it is
 * delivered when the loader lock has been released. Same way as posix_fallocate() above: the program's own symbol is found first. */
__attribute__((visibility("default"))) void *dlopen( const char *name, int flags )
{
    static void *(*real)( const char *, int );
    sigset_t set, old;
    void *ret;

    if (!real) real = dlsym( RTLD_NEXT, "dlopen" );
    if (!real) return NULL;
    sigemptyset( &set );
    sigaddset( &set, SIGUSR1 );
    pthread_sigmask( SIG_BLOCK, &set, &old );
    ret = real( name, flags );
    if (!sigismember( &old, SIGUSR1 )) pthread_sigmask( SIG_UNBLOCK, &set, NULL );
    return ret;
}

/* The parent starts this loader with load_image() instead of posix_spawn() with file actions (which forks, and the child can wait for
 * ever for a malloc lock held by another thread of the parent: see haiku_spawn_process in dlls/ntdll/unix/process.c) and tells what the
 * actions and attributes would have done in WINE_HAIKU_SPAWN="in,out,stdin_fd,stdout_fd,unixdir,new_session". Done first thing. */
static void haiku_spawn_prologue(void)
{
    int in, out, stdin_fd, stdout_fd, unixdir, new_session, fd;
    const char *desc = getenv( "WINE_HAIKU_SPAWN" );

    if (!desc) return;
    if (sscanf( desc, "%d,%d,%d,%d,%d,%d", &in, &out, &stdin_fd, &stdout_fd, &unixdir, &new_session ) != 6) return;
    unsetenv( "WINE_HAIKU_SPAWN" );
    signal( SIGPIPE, SIG_DFL );
    if (new_session) setsid();
    if (in == -1)
    {
        if ((fd = open( "/dev/null", O_RDWR )) != -1 && fd != 0) { dup2( fd, 0 ); close( fd ); }
    }
    else if (in != 0) dup2( in, 0 );
    if (out == -1)
    {
        if ((fd = open( "/dev/null", O_RDWR )) != -1 && fd != 1) { dup2( fd, 1 ); close( fd ); }
    }
    else if (out != 1) dup2( out, 1 );
    if (stdin_fd > 1) close( stdin_fd );
    if (stdout_fd > 1 && stdout_fd != stdin_fd) close( stdout_fd );
    if (unixdir != -1)
    {
        if (fchdir( unixdir )) {}
        close( unixdir );
    }
}
#endif

/**********************************************************************
 *           main
 */
int main( int argc, char *argv[] )
{
    void *handle;

#ifdef __HAIKU__
    haiku_spawn_prologue();
    /* Mesa's default shader cache writes one file per shader, and on BFS that made UT4 lag all the time (thousands of small
     * file creations while playing). Its single-file backend (one append-only file) does not: measured with UT4, per-file was
     * the worst, the database backend better, the single file the best. A choice made by the user in the environment wins. */
    if (!getenv( "MESA_DISK_CACHE_SINGLE_FILE" ) && !getenv( "MESA_DISK_CACHE_DATABASE" ) &&
        !getenv( "MESA_DISK_CACHE_MULTI_FILE" ))
        setenv( "MESA_DISK_CACHE_SINGLE_FILE", "true", 0 );
#endif

    init_reserved_areas();

    if ((handle = try_dlopen( get_self_exe() )) ||
        (handle = try_dlopen( argv[0] )))
    {
        void (*init_func)(int, char **) = dlsym( handle, "__wine_main" );
        if (init_func) init_func( argc, argv );
        fprintf( stderr, "wine: __wine_main function not found in ntdll.so\n" );
        exit(1);
    }

    fprintf( stderr, "wine: could not load ntdll.so: %s\n", dlerror() );
    pthread_detach( pthread_self() );  /* force importing libpthread for OpenGL */
    exit(1);
}
