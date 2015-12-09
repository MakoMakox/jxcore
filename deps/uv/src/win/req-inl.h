/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef UV_WIN_REQ_INL_H_
#define UV_WIN_REQ_INL_H_

#include <assert.h>

#include "uv.h"
#include "internal.h"

#define SET_REQ_STATUS(req, status) \
  (req)->overlapped.Internal = (ULONG_PTR)(status)

#define SET_REQ_ERROR(req, error) \
  SET_REQ_STATUS((req), NTSTATUS_FROM_WIN32((error)))

#define SET_REQ_SUCCESS(req) SET_REQ_STATUS((req), STATUS_SUCCESS)

#define GET_REQ_STATUS(req) ((NTSTATUS)(req)->overlapped.Internal)

#define REQ_SUCCESS(req) (NT_SUCCESS(GET_REQ_STATUS((req))))

#define GET_REQ_ERROR(req) (pRtlNtStatusToDosError(GET_REQ_STATUS((req))))

#define GET_REQ_SOCK_ERROR(req) \
  (uv_ntstatus_to_winsock_error(GET_REQ_STATUS((req))))

#define REGISTER_HANDLE_REQ(loop, handle, req) \
  do {                                         \
    INCREASE_ACTIVE_COUNT((loop), (handle));   \
    uv__req_register((loop), (req));           \
  } while (0)

#define UNREGISTER_HANDLE_REQ(loop, handle, req) \
  do {                                           \
    DECREASE_ACTIVE_COUNT((loop), (handle));     \
    uv__req_unregister((loop), (req));           \
  } while (0)

#define UV_SUCCEEDED_WITHOUT_IOCP(result) \
  ((result) && (handle->flags& UV_HANDLE_SYNC_BYPASS_IOCP))

#define UV_SUCCEEDED_WITH_IOCP(result) \
  ((result) || (GetLastError() == ERROR_IO_PENDING))

#define POST_COMPLETION_FOR_REQ(loop, req)                                     \
  if (!PostQueuedCompletionStatus((loop)->iocp, 0, 0, &((req)->overlapped))) { \
    uv_fatal_error(GetLastError(), "PostQueuedCompletionStatus");              \
  }

INLINE static void uv_req_init(uv_loop_t* loop, uv_req_t* req) {
  req->type = UV_UNKNOWN_REQ;
  SET_REQ_SUCCESS(req);
}

INLINE static uv_req_t* uv_overlapped_to_req(OVERLAPPED* overlapped) {
  return CONTAINING_RECORD(overlapped, uv_req_t, overlapped);
}

uv_mutex_t loopex[65];
INLINE static void uv_insert_pending_req(uv_loop_t* loop, uv_req_t* req) {
  uv_mutex_lock(&loopex[loop->loopId]);
  req->next_req = NULL;
  if (loop->pending_reqs_tail) {
    req->next_req = loop->pending_reqs_tail->next_req;
    loop->pending_reqs_tail->next_req = req;
    loop->pending_reqs_tail = req;
  } else {
    req->next_req = req;
    loop->pending_reqs_tail = req;
  }
  uv_mutex_unlock(&loopex[loop->loopId]);
}

#define DELEGATE_STREAM_REQ(loop, req, method, handle_at)                    \
  do {                                                                       \
    switch (((uv_handle_t*)(req)->handle_at)->type) {                        \
      case UV_TCP:                                                           \
        uv_process_tcp_##method##_req(loop, (uv_tcp_t*)((req)->handle_at),   \
                                      req);                                  \
        break;                                                               \
                                                                             \
      case UV_NAMED_PIPE:                                                    \
        uv_process_pipe_##method##_req(loop, (uv_pipe_t*)((req)->handle_at), \
                                       req);                                 \
        break;                                                               \
                                                                             \
      case UV_TTY:                                                           \
        uv_process_tty_##method##_req(loop, (uv_tty_t*)((req)->handle_at),   \
                                      req);                                  \
        break;                                                               \
                                                                             \
      default:                                                               \
        assert(0);                                                           \
    }                                                                        \
  } while (0)

static uv_loop_t* getCorrectLoop(uv_loop_t* loop, uv_req_t* req) {
  uv_tcp_t* handle = NULL;
  uv_loop_t* sloop = NULL;
  uv_stream_t* stream = NULL;

  switch (req->type) {
    case UV_READ: {
      if (((uv_handle_t*)(req)->data)->type == UV_TCP) {
        handle = (uv_tcp_t*)req->data;
        if (handle != NULL) {
          sloop = handle->loop;
        }
      }
    } break;

    case UV_ACCEPT: {
      if (((uv_handle_t*)(req)->data)->type == UV_TCP) {
        handle = (uv_tcp_t*)req->data;
        if (handle != NULL) {
          sloop = handle->loop;
        }
      }
    } break;

    case UV_FS: {
      sloop = ((uv_fs_t*)req)->loop;
    } break;
    case UV_WRITE: {
      stream = ((uv_write_t*)req)->handle;
      if (stream != NULL) {
        sloop = stream->loop;
      }
    } break;
    case UV_GETADDRINFO: {
      sloop = ((uv_getaddrinfo_t*)req)->loop;
    } break;

    default:
      // assert(0);
      {}
  }

  if (sloop != NULL) {
    return sloop;
  } else {
    return loop;
  }
}

static uv_loop_t* getCorrectLoop2(uv_loop_t* loop, uv_req_t* req) {
  int filled = 0;
  return getCorrectLoop(loop, req, &filled);
}

INLINE static void uv_process_reqs(uv_loop_t* loop) {
  uv_req_t* req;
  uv_req_t* first;
  uv_req_t* next;

  uv_mutex_lock(&loopex[loop->loopId]);
  if (loop->pending_reqs_tail == NULL) {
    uv_mutex_unlock(&loopex[loop->loopId]);
    return;
  }

  first = loop->pending_reqs_tail->next_req;
  next = first;
  loop->pending_reqs_tail = NULL;
  uv_mutex_unlock(&loopex[loop->loopId]);

  while (next != NULL) {
    req = next;
    next = req->next_req != first ? req->next_req : NULL;

    switch (req->type) {
      case UV_READ:
        DELEGATE_STREAM_REQ(loop, req, read, data);
        break;

      case UV_WRITE:
        DELEGATE_STREAM_REQ(loop, (uv_write_t*)req, write, handle);
        break;

      case UV_ACCEPT:
        DELEGATE_STREAM_REQ(loop, req, accept, data);
        break;

      case UV_CONNECT:
        DELEGATE_STREAM_REQ(loop, (uv_connect_t*)req, connect, handle);
        break;

      case UV_SHUTDOWN:
        /* Tcp shutdown requests don't come here. */
        assert(((uv_shutdown_t*)req)->handle->type == UV_NAMED_PIPE);
        uv_process_pipe_shutdown_req(loop,
                                     (uv_pipe_t*)((uv_shutdown_t*)req)->handle,
                                     (uv_shutdown_t*)req);
        break;

      case UV_UDP_RECV:
        uv_process_udp_recv_req(loop, (uv_udp_t*)req->data, req);
        break;

      case UV_UDP_SEND:
        uv_process_udp_send_req(loop, ((uv_udp_send_t*)req)->handle,
                                (uv_udp_send_t*)req);
        break;

      case UV_WAKEUP:
        uv_process_async_wakeup_req(loop, (uv_async_t*)req->data, req);
        break;

      case UV_SIGNAL_REQ:
        uv_process_signal_req(loop, (uv_signal_t*)req->data, req);
        break;

      case UV_POLL_REQ:
        uv_process_poll_req(loop, (uv_poll_t*)req->data, req);
        break;

      case UV_GETADDRINFO:
        uv_process_getaddrinfo_req(loop, (uv_getaddrinfo_t*)req);
        break;

      case UV_PROCESS_EXIT:
        uv_process_proc_exit(loop, (uv_process_t*)req->data);
        break;

      case UV_FS:
        uv_process_fs_req(loop, (uv_fs_t*)req);
        break;

      case UV_WORK:
        uv_process_work_req(loop, (uv_work_t*)req);
        break;

      case UV_FS_EVENT_REQ:
        uv_process_fs_event_req(loop, req, (uv_fs_event_t*)req->data);
        break;

      default:
        assert(0);
    }
  }
}

#endif /* UV_WIN_REQ_INL_H_ */
