/*
 * Wayland driver support for windows that belong to another process
 *
 * A Wayland surface can only be made part of the surfaces of its own client, so
 * a process that renders into a window of another one, as the GPU process of a
 * Chromium based application does, cannot show anything by itself. Its frames
 * are read back and sent to the process that owns the window, which shows them
 * in a subsurface of the window.
 *
 * Copyright 2026 Michael Lothian
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "waylanddrv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

#define REMOTE_MAGIC 0x57525346
#define REMOTE_MAX_SIZE (256 * 1024 * 1024)

enum remote_message_type
{
    REMOTE_FRAME = 1,
    REMOTE_DESTROY,
};

struct remote_header
{
    UINT magic;
    UINT type;
    ULONGLONG hwnd;
    UINT width;
    UINT height;
    UINT bgra;
    UINT size;
};

/* The sockets of the processes of a prefix are told apart by the prefix and the process id. */
static socklen_t remote_address(struct sockaddr_un *addr, DWORD pid)
{
    const char *prefix = getenv("WINEPREFIX"), *home = getenv("HOME");
    char default_prefix[512];
    ULONGLONG hash = 0xcbf29ce484222325ull;
    int len;

    if (!prefix)
    {
        snprintf(default_prefix, sizeof(default_prefix), "%s/.wine", home ? home : "");
        prefix = default_prefix;
    }
    for (; *prefix; prefix++) hash = (hash ^ (unsigned char)*prefix) * 0x100000001b3ull;

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    len = snprintf(addr->sun_path + 1, sizeof(addr->sun_path) - 1, "wine-wayland-remote-%016llx-%x",
                   (unsigned long long)hash, (unsigned int)pid);
    return offsetof(struct sockaddr_un, sun_path) + 1 + len;
}

static BOOL write_all(int fd, const void *data, size_t size)
{
    const char *ptr = data;

    while (size)
    {
        ssize_t ret = send(fd, ptr, size, MSG_NOSIGNAL);
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

static BOOL read_all(int fd, void *data, size_t size)
{
    char *ptr = data;

    while (size)
    {
        ssize_t ret = recv(fd, ptr, size, 0);
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

/***********************************************************************
 *           Process rendering into the window of another one
 */

static int remote_connect(HWND hwnd)
{
    struct sockaddr_un addr;
    socklen_t len;
    DWORD pid = 0;
    int fd;

    NtUserGetWindowThread(hwnd, &pid);
    if (!pid) return -1;
    len = remote_address(&addr, pid);
    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, len))
    {
        WARN("could not connect to the owner of %p: %s\n", hwnd, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/***********************************************************************
 *           wayland_remote_present_pixels
 *
 * Sends a frame rendered for a window of another process to its owner.
 */
void wayland_remote_present_pixels(struct client_surface *client, UINT width, UINT height, UINT stride,
                                   BOOL bgra, const void *pixels)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct remote_header header = {.magic = REMOTE_MAGIC, .type = REMOTE_FRAME, .hwnd = (ULONG_PTR)client->hwnd,
                                   .width = width, .height = height, .bgra = bgra, .size = width * height * 4};
    UINT y;

    TRACE("%s %ux%u stride %u %s\n", debugstr_client_surface(client), width, height, stride, bgra ? "BGRA" : "RGBA");

    if (surface->remote_fd < 0)
    {
        /* do not try to reach an owner that is not listening on every frame */
        if (surface->remote_retry && surface->remote_retry--) return;
        if ((surface->remote_fd = remote_connect(client->hwnd)) < 0)
        {
            surface->remote_retry = 120;
            return;
        }
    }

    if (!write_all(surface->remote_fd, &header, sizeof(header))) goto failed;
    if (stride == width * 4)
    {
        if (!write_all(surface->remote_fd, pixels, header.size)) goto failed;
    }
    else for (y = 0; y < height; y++)
    {
        if (!write_all(surface->remote_fd, (const char *)pixels + y * stride, width * 4)) goto failed;
    }
    return;

failed:
    close(surface->remote_fd);
    surface->remote_fd = -1;
}

/***********************************************************************
 *           wayland_remote_destroy_surface
 */
void wayland_remote_destroy_surface(struct wayland_client_surface *surface)
{
    struct remote_header header = {.magic = REMOTE_MAGIC, .type = REMOTE_DESTROY,
                                   .hwnd = (ULONG_PTR)surface->client.hwnd};

    if (surface->remote_fd < 0) return;
    write_all(surface->remote_fd, &header, sizeof(header));
    close(surface->remote_fd);
    surface->remote_fd = -1;
}

/***********************************************************************
 *           Owner of the window
 */

struct remote_surface
{
    struct list entry;
    HWND hwnd;
    struct client_surface *client;
    struct wayland_buffer_queue *queue;
    UINT width, height;
};

static pthread_mutex_t remote_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct list remote_surfaces = LIST_INIT(remote_surfaces);

static void free_remote_surface(struct remote_surface *remote)
{
    list_remove(&remote->entry);
    if (remote->client) client_surface_release(remote->client);
    if (remote->queue) wayland_buffer_queue_destroy(remote->queue);
    free(remote);
}

static struct remote_surface *find_remote_surface(HWND hwnd)
{
    struct remote_surface *remote;

    LIST_FOR_EACH_ENTRY(remote, &remote_surfaces, struct remote_surface, entry)
        if (remote->hwnd == hwnd) return remote;
    return NULL;
}

/***********************************************************************
 *           wayland_remote_destroy_window
 *
 * Called when a window of this process goes away, not with the window data locked.
 */
void wayland_remote_destroy_window(HWND hwnd)
{
    struct remote_surface *remote;

    pthread_mutex_lock(&remote_mutex);
    if ((remote = find_remote_surface(hwnd))) free_remote_surface(remote);
    pthread_mutex_unlock(&remote_mutex);
}

static BOOL window_is_ours(HWND hwnd)
{
    DWORD pid = 0;

    if (!NtUserIsWindow(hwnd)) return FALSE;
    NtUserGetWindowThread(hwnd, &pid);
    return pid == HandleToULong(NtCurrentTeb()->ClientId.UniqueProcess);
}

static void show_frame(const struct remote_header *header, const unsigned char *pixels)
{
    struct wayland_shm_buffer *buffer;
    struct wayland_client_surface *client;
    struct remote_surface *remote;
    HWND hwnd = (HWND)(ULONG_PTR)header->hwnd;
    UINT32 *dst;
    UINT i, count = header->width * header->height;

    if (!window_is_ours(hwnd)) return;

    pthread_mutex_lock(&remote_mutex);

    if (!(remote = find_remote_surface(hwnd)))
    {
        if (!(remote = calloc(1, sizeof(*remote)))) goto done;
        remote->hwnd = hwnd;
        if (!(remote->client = WAYLAND_CreateClientSurface(hwnd, 0, FALSE)))
        {
            free(remote);
            goto done;
        }
        list_add_tail(&remote_surfaces, &remote->entry);
    }
    client = impl_from_client_surface(remote->client);

    if (!remote->queue || remote->width != header->width || remote->height != header->height)
    {
        if (remote->queue) wayland_buffer_queue_destroy(remote->queue);
        remote->width = header->width;
        remote->height = header->height;
        if (!(remote->queue = wayland_buffer_queue_create(header->width, header->height, WL_SHM_FORMAT_ARGB8888))) goto done;
    }

    if (!(buffer = wayland_buffer_queue_get_free_buffer(remote->queue))) goto done;

    /* The swapchain alpha is not meant to be shown, so it is not used. */
    dst = buffer->map_data;
    if (header->bgra)
        for (i = 0; i < count; i++) dst[i] = ((const UINT32 *)pixels)[i] | 0xff000000;
    else
        for (i = 0; i < count; i++)
        {
            UINT32 p = ((const UINT32 *)pixels)[i];
            dst[i] = 0xff000000 | (p & 0xff00ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff);
        }

    /* Let the window place the surface in its client area, then give it the frame. */
    client_surface_present(remote->client);

    buffer->busy = TRUE;
    wayland_shm_buffer_ref(buffer);
    wl_surface_attach(client->wl_surface, buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(client->wl_surface, 0, 0, header->width, header->height);
    wl_surface_commit(client->wl_surface);
    wl_display_flush(process_wayland.wl_display);

done:
    pthread_mutex_unlock(&remote_mutex);
}

static void hide_surface(HWND hwnd)
{
    wayland_remote_destroy_window(hwnd);
}

static BOOL handle_message(int fd)
{
    struct remote_header header;
    unsigned char *pixels;

    if (!read_all(fd, &header, sizeof(header)) || header.magic != REMOTE_MAGIC) return FALSE;
    if (header.type == REMOTE_DESTROY)
    {
        hide_surface((HWND)(ULONG_PTR)header.hwnd);
        return TRUE;
    }
    if (header.type != REMOTE_FRAME || header.size != header.width * header.height * 4 ||
        header.size > REMOTE_MAX_SIZE || !header.size)
        return FALSE;

    if (!(pixels = malloc(header.size))) return FALSE;
    if (!read_all(fd, pixels, header.size))
    {
        free(pixels);
        return FALSE;
    }
    show_frame(&header, pixels);
    free(pixels);
    return TRUE;
}

static BOOL peer_is_ours(int fd)
{
    struct ucred cred;
    socklen_t len = sizeof(cred);

    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) && cred.uid == getuid();
}

/***********************************************************************
 *           waylanddrv_unix_remote_run
 *
 * Runs on a thread of every process, taking the frames sent for its windows.
 */
NTSTATUS waylanddrv_unix_remote_run(void *arg)
{
    struct pollfd *fds;
    struct sockaddr_un addr;
    socklen_t len;
    int listener, count = 1, capacity = 8, i;

    if ((listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return STATUS_UNSUCCESSFUL;
    len = remote_address(&addr, HandleToULong(NtCurrentTeb()->ClientId.UniqueProcess));
    if (bind(listener, (struct sockaddr *)&addr, len) || listen(listener, 4))
    {
        WARN("could not listen for frames: %s\n", strerror(errno));
        close(listener);
        return STATUS_UNSUCCESSFUL;
    }

    if (!(fds = calloc(capacity, sizeof(*fds)))) return STATUS_NO_MEMORY;
    fds[0].fd = listener;
    fds[0].events = POLLIN;

    for (;;)
    {
        if (poll(fds, count, -1) < 0)
        {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & POLLIN)
        {
            int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
            if (fd >= 0)
            {
                if (!peer_is_ours(fd)) close(fd);
                else
                {
                    if (count == capacity)
                    {
                        struct pollfd *new = realloc(fds, capacity * 2 * sizeof(*fds));
                        if (!new) { close(fd); continue; }
                        fds = new;
                        capacity *= 2;
                    }
                    fds[count].fd = fd;
                    fds[count].events = POLLIN;
                    fds[count].revents = 0;
                    count++;
                }
            }
        }

        for (i = 1; i < count; i++)
        {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (!handle_message(fds[i].fd))
            {
                close(fds[i].fd);
                fds[i] = fds[--count];
                i--;
            }
        }
    }

    free(fds);
    close(listener);
    return STATUS_SUCCESS;
}
