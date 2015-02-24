/******************************************************************************
** Copyright (c) 2014-2015, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Hans Pabst (Intel Corp.)
******************************************************************************/
#include "libxstream_capture.hpp"
#include <algorithm>
#include <cstdio>

#if defined(LIBXSTREAM_STDFEATURES)
# include <thread>
# include <atomic>
#else
# if defined(__GNUC__)
#   include <pthread.h>
# else
#   include <Windows.h>
# endif
#endif

//#define LIBXSTREAM_CAPTURE_DEBUG
//#define LIBXSTREAM_CAPTURE_UNLOCK_EARLY


namespace libxstream_capture_internal {

class queue_type {
public:
  queue_type()
    : m_lock(libxstream_lock_create())
    , m_terminated(false)
    , m_index(0)
#if defined(LIBXSTREAM_STDFEATURES)
    , m_thread() // do not start here
#else
    , m_thread(0)
#endif
    , m_size(0)
  {
    std::fill_n(m_buffer, LIBXSTREAM_MAX_QSIZE, static_cast<libxstream_capture_base*>(0));
  }

  ~queue_type() {
    terminate();
    libxstream_lock_destroy(m_lock);
  }

public:
  bool start() {
    if (!m_terminated) {
#if defined(LIBXSTREAM_STDFEATURES)
      if (!m_thread.joinable()) {
        libxstream_lock_acquire(m_lock);
        if (!m_thread.joinable()) {
          std::thread(run, this).swap(m_thread);
        }
        libxstream_lock_release(m_lock);
      }
#else
      if (0 == m_thread) {
        libxstream_lock_acquire(m_lock);
        if (0 == m_thread) {
# if defined(__GNUC__)
          pthread_create(&m_thread, 0, run, this);
# else
          m_thread = CreateThread(0, 0, run, this, 0, 0);
# endif
        }
        libxstream_lock_release(m_lock);
      }
#endif
    }

    return !m_terminated;
  }

  void terminate() {
    push(terminator, false); // terminates the background thread

#if defined(LIBXSTREAM_DEBUG)
    size_t dangling = 0;
    for (size_t i = 0; i < LIBXSTREAM_MAX_QSIZE; ++i) {
      const libxstream_capture_base* item = m_buffer[i];
      if (0 != item && terminator != item) {
        m_buffer[i] = 0;
        ++dangling;
        delete item;
      }
    }
    if (0 < dangling) {
      LIBXSTREAM_PRINT_WARN("%lu work item%s dangling!", static_cast<unsigned long>(dangling), 1 < dangling ? "s are" : " is");
    }
#endif

#if defined(LIBXSTREAM_STDFEATURES)
    if (m_thread.joinable()) {
      m_thread.join();
    }
#else
    if (0 != m_thread) {
# if defined(__GNUC__)
      pthread_join(m_thread, 0);
# else
      WaitForSingleObject(m_thread, INFINITE);
# endif
      m_thread = 0;
    }
#endif

    m_terminated = true;
  }

  bool empty() const {
    return 0 == get();
  }

  size_t size() const {
    const size_t offset = m_size, index = m_index;
    const libxstream_capture_base *const entry = m_buffer[offset%LIBXSTREAM_MAX_QSIZE];
    return 0 != entry ? (offset - index) : (std::max<size_t>(offset - index, 1) - 1);
  }

  void push(const libxstream_capture_base& capture_region, bool wait) {
    push(&capture_region, wait);
  }

  const libxstream_capture_base* get() const { // not thread-safe!
    return m_buffer[m_index%LIBXSTREAM_MAX_QSIZE];
  }

  void pop() { // not thread-safe!
    LIBXSTREAM_ASSERT(!empty());
    m_buffer[m_index%LIBXSTREAM_MAX_QSIZE] = 0;
    ++m_index;
  }

private:
  void push(const libxstream_capture_base* capture_region, bool wait) {
    LIBXSTREAM_ASSERT(0 != capture_region);
    const libxstream_capture_base** entry = 0;
#if defined(LIBXSTREAM_STDFEATURES)
    entry = m_buffer + (m_size++ % LIBXSTREAM_MAX_QSIZE);
#elif defined(_OPENMP)
    size_t size1 = 0;
# if (201107 <= _OPENMP)
#   pragma omp atomic capture
# else
#   pragma omp critical
# endif
    size1 = ++m_size;
    entry = m_buffer + ((size1 - 1) % LIBXSTREAM_MAX_QSIZE);
#else // generic
    libxstream_lock_acquire(m_lock);
    entry = m_buffer + (m_size++ % LIBXSTREAM_MAX_QSIZE);
    libxstream_lock_release(m_lock);
#endif
    LIBXSTREAM_ASSERT(0 != entry);

#if defined(LIBXSTREAM_DEBUG)
    if (0 != *entry) {
      LIBXSTREAM_PRINT_WARN0("queuing work is stalled!");
    }
#endif
    // stall the push if LIBXSTREAM_MAX_QSIZE is exceeded
    while (0 != *entry) {
      this_thread_yield();
    }

    LIBXSTREAM_ASSERT(0 == *entry);
    const libxstream_capture_base* new_entry = terminator != capture_region ? capture_region->clone() : terminator;
    *entry = new_entry;

    if (wait) {
      while (new_entry == *entry) {
        this_thread_yield();
      }
    }
  }

#if defined(LIBXSTREAM_STDFEATURES) || defined(__GNUC__)
  static void* run(void* queue)
#else
  static DWORD WINAPI run(_In_ LPVOID queue)
#endif
  {
    queue_type& q = *static_cast<queue_type*>(queue);
    const libxstream_capture_base* capture_region = 0;

#if defined(LIBXSTREAM_ASYNCHOST) && defined(_OPENMP) && !defined(LIBXSTREAM_OFFLOAD)
#   pragma omp parallel
#   pragma omp master
#endif
    for (;;) {
      while (0 == (capture_region = q.get())) {
        this_thread_yield();
      }

      if (terminator != capture_region) {
        (*capture_region)();
        delete capture_region;
        q.pop();
      }
      else {
        q.pop();
        break;
      }
    }

#if defined(LIBXSTREAM_STDFEATURES) || defined(__GNUC__)
    return queue;
#else
    return EXIT_SUCCESS;
#endif
  }

private:
  static const libxstream_capture_base* terminator;
  const libxstream_capture_base* m_buffer[LIBXSTREAM_MAX_QSIZE];
  libxstream_lock* m_lock;
  bool m_terminated;
  size_t m_index;
#if defined(LIBXSTREAM_STDFEATURES)
  std::thread m_thread;
  std::atomic<size_t> m_size;
#elif defined(__GNUC__)
  pthread_t m_thread;
  size_t m_size;
#else
  HANDLE m_thread;
  size_t m_size;
#endif
#if defined(LIBXSTREAM_CAPTURE_DEBUG)
};
#else
} queue;
#endif
/*static*/ const libxstream_capture_base* queue_type::terminator = reinterpret_cast<libxstream_capture_base*>(-1);

} // namespace libxstream_capture_internal


libxstream_capture_base::libxstream_capture_base(size_t argc, const arg_type argv[], libxstream_stream* stream, int flags)
  : m_function(0)
  , m_stream(stream)
  , m_flags(flags)
#if defined(LIBXSTREAM_THREADLOCAL_SIGNALS)
  , m_thread(this_thread_id())
#endif
#if defined(LIBXSTREAM_CAPTURE_UNLOCK_EARLY)
  , m_unlock(true)
#else
  , m_unlock(false)
#endif
{
  if (2 == argc && (argv[0].signature() || argv[1].signature())) {
    const libxstream_argument* signature = 0;
    if (argv[1].signature()) {
      m_function = *reinterpret_cast<const libxstream_function*>(libxstream_address(argv[0]));
      signature = reinterpret_cast<const libxstream_argument*>(libxstream_get_data(argv[1]));
    }
    else {
      LIBXSTREAM_ASSERT(argv[0].signature());
      m_function = *reinterpret_cast<const libxstream_function*>(libxstream_address(argv[1]));
      signature = reinterpret_cast<const libxstream_argument*>(libxstream_get_data(argv[0]));
    }

    size_t arity = 0;
    libxstream_fn_arity(signature, &arity);
#if defined(__INTEL_COMPILER)
#   pragma loop_count min(0), max(LIBXSTREAM_MAX_NARGS), avg(LIBXSTREAM_MAX_NARGS/2)
#endif
    for (size_t i = 0; i <= arity; ++i) m_signature[i] = signature[i];
  }
  else {
    for (size_t i = 0; i < argc; ++i) m_signature[i] = argv[i];
    libxstream_construct(m_signature, argc, libxstream_argument::kind_invalid, 0, LIBXSTREAM_TYPE_INVALID, 0, 0);
#if defined(LIBXSTREAM_DEBUG)
    size_t arity = 0;
    LIBXSTREAM_ASSERT(LIBXSTREAM_ERROR_NONE == libxstream_fn_arity(m_signature, &arity) && arity == argc);
#endif
  }

  if (stream) {
    if (0 == (flags & LIBXSTREAM_CALL_WAIT) && 0 != stream->demux()) {
      stream->lock(0 > stream->demux());
    }
    stream->begin();
  }
}


libxstream_capture_base::~libxstream_capture_base()
{
  if (m_unlock && m_stream) {
    m_stream->end();
    if (0 != (m_flags & LIBXSTREAM_CALL_UNLOCK) && 0 != m_stream->demux()) {
      m_stream->unlock();
    }
  }
}


int libxstream_capture_base::thread() const
{
#if defined(LIBXSTREAM_THREADLOCAL_SIGNALS)
  return m_thread;
#else
  return 0;
#endif
}


libxstream_capture_base* libxstream_capture_base::clone() const
{
  libxstream_capture_base *const instance = virtual_clone();
#if defined(LIBXSTREAM_CAPTURE_UNLOCK_EARLY)
  instance->m_unlock = false;
#else
  instance->m_unlock = true;
#endif
  return instance;
}


void libxstream_capture_base::operator()() const
{
  virtual_run();
}


LIBXSTREAM_EXPORT_INTERNAL void libxstream_enqueue(const libxstream_capture_base& capture_region, bool wait)
{
#if !defined(LIBXSTREAM_CAPTURE_DEBUG)
  if (libxstream_capture_internal::queue.start()) {
# if defined(LIBXSTREAM_SYNCHRONOUS)
    libxstream_use_sink(&wait);
    libxstream_capture_internal::queue.push(capture_region, true);
# else
    libxstream_capture_internal::queue.push(capture_region, wait);
# endif
  }
#else
  capture_region();
#endif
}


void libxstream_capture_shutdown()
{
#if !defined(LIBXSTREAM_CAPTURE_DEBUG)
  libxstream_capture_internal::queue.terminate();
#endif
}