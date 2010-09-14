#include "alloc.h"
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>
#include "ns3/assert.h"
#include "ns3/log.h"

NS_LOG_COMPONENT_DEFINE ("Alloc");

#ifdef HAVE_VALGRIND_H
# include "valgrind/valgrind.h"
# include "valgrind/memcheck.h"
# define REPORT_MALLOC(buffer, size) \
  VALGRIND_MALLOCLIKE_BLOCK (buffer,size, 0, 0)
# define REPORT_FREE(buffer) \
  VALGRIND_FREELIKE_BLOCK (buffer, 0)
# define MARK_DEFINED(buffer, size)				\
  VALGRIND_MAKE_MEM_DEFINED(buffer, size)
# define MARK_UNDEFINED(buffer, size)			\
  VALGRIND_MAKE_MEM_UNDEFINED(buffer, size)
#else
# define REPORT_MALLOC(buffer, size)
# define REPORT_FREE(buffer)
# define MARK_DEFINED(buffer, size)
# define MARK_UNDEFINED(buffer, size)
#endif

Alloc::~Alloc ()
{}

StupidAlloc::StupidAlloc ()
{
  NS_LOG_FUNCTION (this);
}
StupidAlloc::~StupidAlloc ()
{
  NS_LOG_FUNCTION (this);
  for (std::list<uint8_t *>::iterator i = m_alloced.begin (); i != m_alloced.end (); ++i)
    {
      ::free (*i);
    }
}
uint8_t *
StupidAlloc::Malloc (uint32_t size)
{
  NS_LOG_FUNCTION (this << size);
  uint8_t *buffer = (uint8_t*)::malloc (size);
  m_alloced.push_back (buffer);
  return buffer;
}
void 
StupidAlloc::Free (uint8_t *buffer, uint32_t size)
{
  NS_LOG_FUNCTION (this << (void*)buffer << size);
  ::free ((uint8_t*)buffer);
  m_alloced.remove (buffer);
}
uint8_t *
StupidAlloc::Realloc(uint8_t *oldBuffer, uint32_t oldSize, uint32_t newSize)
{
  NS_LOG_FUNCTION (this << (void*)oldBuffer << oldSize << newSize);
  uint8_t *newBuffer = (uint8_t*)::realloc ((void*)oldBuffer, newSize);
  if (newBuffer != oldBuffer)
    {
      m_alloced.remove (oldBuffer);
      m_alloced.push_back (newBuffer);
    }
  return newBuffer;
}

KingsleyAlloc::KingsleyAlloc ()
  : m_defaultMmapSize (1<<15)
{
  NS_LOG_FUNCTION (this);
  memset(m_buckets, 0, sizeof(m_buckets));
}
KingsleyAlloc::~KingsleyAlloc ()
{
  NS_LOG_FUNCTION (this);
  for (std::list<struct KingsleyAlloc::MmapChunk>::iterator i = m_chunks.begin ();
       i != m_chunks.end (); ++i)
    {
      MmapFree (i->buffer, i->size);
    }
  m_chunks.clear ();
}

void
KingsleyAlloc::MmapFree (uint8_t *buffer, uint32_t size)
{
  NS_LOG_FUNCTION (this << (void*)buffer << size);
  int status;
  status = ::munmap (buffer, size);
  NS_ASSERT_MSG (status == 0, "Unable to release mmaped buffer");
}
void
KingsleyAlloc::MmapAlloc (uint32_t size)
{
  NS_LOG_FUNCTION (this << size);
  struct MmapChunk chunk;
  chunk.size = size;
  chunk.brk = 0;
  chunk.buffer = (uint8_t*)::mmap (0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  NS_ASSERT_MSG (chunk.buffer != MAP_FAILED, "Unable to mmap memory buffer");
  m_chunks.push_front (chunk);
  NS_LOG_DEBUG ("mmap alloced=" << size << " at=" << (void*)chunk.buffer);
  MARK_UNDEFINED (chunk.buffer, size);
}

uint8_t *
KingsleyAlloc::Brk (uint32_t needed)
{
  NS_LOG_FUNCTION (this << needed);
  for (std::list<struct KingsleyAlloc::MmapChunk>::iterator i = m_chunks.begin ();
       i != m_chunks.end (); ++i)
    {
      NS_ASSERT (i->size >= i->brk);
      if (i->size - i->brk >= needed)
	{
	  uint8_t *buffer = i->buffer + i->brk;
	  i->brk += needed;
	  NS_LOG_DEBUG ("brk: needed=" << needed << ", left=" << i->size - i->brk);
	  return buffer;
	}
    }
  NS_ASSERT_MSG (needed <= m_defaultMmapSize, needed << " " << m_defaultMmapSize);
  MmapAlloc (m_defaultMmapSize);
  return Brk (needed);
}
uint8_t 
KingsleyAlloc::SizeToBucket (uint32_t sz)
{
  NS_LOG_FUNCTION (this << sz);
  uint8_t bucket = 0;
  uint32_t size = sz;
  size--;
  while (size > 7)
    {
      size >>= 1;
      bucket++;
    }
  NS_ASSERT (bucket < 32);
  NS_LOG_DEBUG ("size=" << sz << ", bucket=" << (uint32_t)bucket << ", size=" << BucketToSize (bucket));
  return bucket;
}
uint32_t
KingsleyAlloc::BucketToSize (uint8_t bucket)
{
  uint32_t size = (1<<(bucket+3));
  return size;
}

uint8_t * 
KingsleyAlloc::Malloc (uint32_t size)
{
  NS_LOG_FUNCTION (this << size);
  if (size < m_defaultMmapSize)
    {
      uint8_t bucket = SizeToBucket (size);
      if (m_buckets[bucket] == 0)
	{
	  struct Available *avail = (struct Available *)Brk (BucketToSize (bucket));
	  MARK_DEFINED(avail, sizeof(void*));
	  avail->next = 0;
	  MARK_UNDEFINED(avail, sizeof(void*));
	  m_buckets[bucket] = avail;
	}
      // fast path.
      struct Available *avail = m_buckets[bucket];
      MARK_DEFINED(avail, sizeof(void*));
      m_buckets[bucket] = avail->next;
      MARK_UNDEFINED(avail, sizeof(void*));
      REPORT_MALLOC(avail, size);
      return (uint8_t*)avail;
    }
  else
    {
      MmapAlloc (size);
      uint8_t *buffer = Brk (size);
      REPORT_MALLOC(buffer, size);
      return buffer;
    }
}
void 
KingsleyAlloc::Free (uint8_t *buffer, uint32_t size)
{
  NS_LOG_FUNCTION (this << (void*)buffer << size);
  if (size < m_defaultMmapSize)
    {
      // return to bucket list.
      uint8_t bucket = SizeToBucket (size);
      struct Available *avail = (struct Available *)buffer;
      avail->next = m_buckets[bucket];
      m_buckets[bucket] = avail;
      REPORT_FREE(buffer);
    }
  else
    {
      for (std::list<struct KingsleyAlloc::MmapChunk>::iterator i = m_chunks.begin ();
	   i != m_chunks.end (); ++i)
	{
	  if (i->buffer == buffer && i->size == size)
	    {
	      REPORT_FREE(buffer);
	      MmapFree (buffer, size);
	      return;
	    }
	}
      // this should never happen but it happens in case of a double-free
      REPORT_FREE(buffer);
    }
}
uint8_t *
KingsleyAlloc::Realloc(uint8_t *oldBuffer, uint32_t oldSize, uint32_t newSize)
{
  NS_LOG_FUNCTION (this << (void*)oldBuffer << oldSize << newSize);
  if (newSize < oldSize)
    {
      return oldBuffer;
    }
  uint8_t *newBuffer = Malloc (newSize);
  memcpy (newBuffer, oldBuffer, oldSize);
  Free (oldBuffer, oldSize);
  return newBuffer;
}
