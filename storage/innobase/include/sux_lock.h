/*****************************************************************************

Copyright (c) 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

#pragma once
#include "srw_lock.h"
#include "my_atomic_wrapper.h"
#include "os0thread.h"
#ifdef UNIV_DEBUG
# include <set>
#endif

/** A "fat" rw-lock that supports
S (shared), U (update, or shared-exclusive), and X (exclusive) modes
as well as recursive U and X latch acquisition */
template<typename srw>
class sux_lock final
{
  /** The underlying non-recursive lock */
  srw lock;
  /** The owner of the U or X lock (0 if none); protected by lock */
  std::atomic<os_thread_id_t> writer;
  /** Special writer!=0 value to indicate that the lock is non-recursive
  and will be released by an I/O thread */
#if defined __linux__ || defined _WIN32
  static constexpr os_thread_id_t FOR_IO= os_thread_id_t(~0UL);
#else
# define FOR_IO ((os_thread_id_t) ~0UL) /* it could be a pointer */
#endif
  /** Numbers of U and X locks. Protected by lock. */
  uint32_t recursive;
  /** Number of blocking waits */
  std::atomic<uint32_t> waits;
#ifdef UNIV_DEBUG
  /** Protects readers */
  mutable srw_mutex readers_lock;
  /** Threads that hold the lock in shared mode */
  std::atomic<std::set<os_thread_id_t>*> readers;
#endif

  /** The multiplier in recursive for X locks */
  static constexpr uint32_t RECURSIVE_X= 1U;
  /** The multiplier in recursive for U locks */
  static constexpr uint32_t RECURSIVE_U= 1U << 16;
  /** The maximum allowed level of recursion */
  static constexpr uint32_t RECURSIVE_MAX= RECURSIVE_U - 1;

public:
#ifdef UNIV_PFS_RWLOCK
  inline void init();
#endif

  void SRW_LOCK_INIT(mysql_pfs_key_t key)
  {
    lock.SRW_LOCK_INIT(key);
    ut_ad(!writer.load(std::memory_order_relaxed));
    ut_ad(!recursive);
    ut_ad(!waits.load(std::memory_order_relaxed));
    ut_d(readers_lock.init());
    ut_ad(!readers.load(std::memory_order_relaxed));
  }

  /** Free the rw-lock after create() */
  void free()
  {
    ut_ad(!writer.load(std::memory_order_relaxed));
    ut_ad(!recursive);
#ifdef UNIV_DEBUG
    readers_lock.destroy();
    if (auto r= readers.load(std::memory_order_relaxed))
    {
      ut_ad(r->empty());
      delete r;
      readers.store(nullptr, std::memory_order_relaxed);
    }
#endif
    lock.destroy();
  }

  /** @return number of blocking waits */
  uint32_t waited() const { return waits.load(std::memory_order_relaxed); }
  /** Reset the number of blocking waits */
  void reset_waited() { waits.store(0, std::memory_order_relaxed); }

  /** needed for dict_index_t::clone() */
  inline void operator=(const sux_lock&);

#ifdef UNIV_DEBUG
  /** @return whether no recursive locks are being held */
  bool not_recursive() const
  {
    ut_ad(recursive);
    return recursive == RECURSIVE_X || recursive == RECURSIVE_U;
  }
#endif

  /** Acquire a recursive lock */
  template<bool allow_readers> void writer_recurse()
  {
    ut_ad(writer == os_thread_get_curr_id());
    ut_d(auto rec= (recursive / (allow_readers ? RECURSIVE_U : RECURSIVE_X)) &
         RECURSIVE_MAX);
    ut_ad(allow_readers ? recursive : rec);
    ut_ad(rec < RECURSIVE_MAX);
    recursive+= allow_readers ? RECURSIVE_U : RECURSIVE_X;
  }

private:
  /** Transfer the ownership of a write lock to another thread
  @param id the new owner of the U or X lock */
  void set_new_owner(os_thread_id_t id)
  {
    IF_DBUG(DBUG_ASSERT(writer.exchange(id, std::memory_order_relaxed)),
            writer.store(id, std::memory_order_relaxed));
  }
  /** Assign the ownership of a write lock to a thread
  @param id the owner of the U or X lock */
  void set_first_owner(os_thread_id_t id)
  {
    IF_DBUG(DBUG_ASSERT(!writer.exchange(id, std::memory_order_relaxed)),
            writer.store(id, std::memory_order_relaxed));
  }
#ifdef UNIV_DEBUG
  /** Register the current thread as a holder of a shared lock */
  void s_lock_register()
  {
    readers_lock.wr_lock();
    auto r= readers.load(std::memory_order_relaxed);
    if (!r)
    {
      r= new std::set<os_thread_id_t>();
      readers.store(r, std::memory_order_relaxed);
    }
    ut_ad(r->emplace(os_thread_get_curr_id()).second);
    readers_lock.wr_unlock();
  }
#endif

public:
  /** In crash recovery or the change buffer, claim the ownership
  of the exclusive block lock to the current thread */
  void claim_ownership() { set_new_owner(os_thread_get_curr_id()); }

  /** @return whether the current thread is holding X or U latch */
  bool have_u_or_x() const
  {
    if (os_thread_get_curr_id() != writer.load(std::memory_order_relaxed))
      return false;
    ut_ad(recursive);
    return true;
  }
  /** @return whether the current thread is holding U but not X latch */
  bool have_u_not_x() const
  { return have_u_or_x() && !((recursive / RECURSIVE_X) & RECURSIVE_MAX); }
  /** @return whether the current thread is holding X latch */
  bool have_x() const
  { return have_u_or_x() && ((recursive / RECURSIVE_X) & RECURSIVE_MAX); }
#ifdef UNIV_DEBUG
  /** @return whether the current thread is holding S latch */
  bool have_s() const
  {
    if (auto r= readers.load(std::memory_order_relaxed))
    {
      readers_lock.wr_lock();
      bool found= r->find(os_thread_get_curr_id()) != r->end();
      readers_lock.wr_unlock();
      return found;
    }
    return false;
  }
  /** @return whether the current thread is holding the latch */
  bool have_any() const { return have_u_or_x() || have_s(); }
#endif

  /** Acquire a shared lock */
  inline void s_lock();
  inline void s_lock(const char *file, unsigned line);
  /** Acquire an update lock */
  inline void u_lock();
  inline void u_lock(const char *file, unsigned line);
  /** Acquire an exclusive lock */
  inline void x_lock(bool for_io= false);
  inline void x_lock(const char *file, unsigned line);
  /** Acquire a recursive exclusive lock */
  void x_lock_recursive() { writer_recurse<false>(); }
  /** Upgrade an update lock */
  inline void u_x_upgrade();
  inline void u_x_upgrade(const char *file, unsigned line);

  /** Acquire an exclusive lock or upgrade an update lock
  @return whether U locks were upgraded to X */
  bool x_lock_upgraded()
  {
    os_thread_id_t id= os_thread_get_curr_id();
    if (writer.load(std::memory_order_relaxed) == id)
    {
      ut_ad(recursive);
      static_assert(RECURSIVE_X == 1, "compatibility");
      if (recursive & RECURSIVE_MAX)
      {
        writer_recurse<false>();
        return false;
      }
      /* Upgrade the lock. */
      lock.u_wr_upgrade();
      recursive/= RECURSIVE_U;
      return true;
    }
    else
    {
      lock.template wr_lock<true>();
      ut_ad(!recursive);
      recursive= RECURSIVE_X;
      set_first_owner(id);
      return false;
    }
  }

  /** @return whether a shared lock was acquired */
  bool s_lock_try()
  {
    bool acquired= lock.rd_lock_try();
    ut_d(if (acquired) s_lock_register());
    return acquired;
  }

  /** Try to acquire an update lock
  @param for_io  whether the lock will be released by another thread
  @return whether the update lock was acquired */
  inline bool u_lock_try(bool for_io);

  /** Try to acquire an exclusive lock
  @return whether an exclusive lock was acquired */
  inline bool x_lock_try();

  /** Release a shared lock */
  void s_unlock()
  {
#ifdef UNIV_DEBUG
    auto r= readers.load(std::memory_order_relaxed);
    ut_ad(r);
    readers_lock.wr_lock();
    ut_ad(r->erase(os_thread_get_curr_id()) == 1);
    readers_lock.wr_unlock();
#endif
    lock.rd_unlock();
  }
  /** Release an update or exclusive lock
  @param allow_readers    whether we are releasing a U lock
  @param claim_ownership  whether the lock was acquired by another thread */
  void u_or_x_unlock(bool allow_readers, bool claim_ownership= false)
  {
    ut_d(auto owner= writer.load(std::memory_order_relaxed));
    ut_ad(owner == os_thread_get_curr_id() ||
          (owner == FOR_IO && claim_ownership &&
           recursive == (allow_readers ? RECURSIVE_U : RECURSIVE_X)));
    ut_d(auto rec= (recursive / (allow_readers ? RECURSIVE_U : RECURSIVE_X)) &
         RECURSIVE_MAX);
    ut_ad(rec);
    if (!(recursive-= allow_readers ? RECURSIVE_U : RECURSIVE_X))
    {
      set_new_owner(0);
      if (allow_readers)
        lock.u_unlock();
      else
        lock.wr_unlock();
    }
  }
  /** Release an update lock */
  void u_unlock(bool claim_ownership= false)
  { u_or_x_unlock(true, claim_ownership); }
  /** Release an exclusive lock */
  void x_unlock(bool claim_ownership= false)
  { u_or_x_unlock(false, claim_ownership); }

  /** @return whether any writer is waiting */
  bool is_waiting() const { return lock.is_waiting(); }
};

typedef sux_lock<srw_lock_low> block_lock;
/** needed for dict_index_t::clone() */
template<> inline void sux_lock<srw_lock>::operator=(const sux_lock&)
{
  memset((void*) this, 0, sizeof *this);
}

#ifndef UNIV_PFS_RWLOCK
typedef block_lock index_lock;
#else
typedef sux_lock<srw_lock> index_lock;

template<> inline void sux_lock<srw_lock_low>::init()
{
  lock.init();
  ut_ad(!writer.load(std::memory_order_relaxed));
  ut_ad(!recursive);
  ut_ad(!waits.load(std::memory_order_relaxed));
  ut_d(readers_lock.init());
  ut_ad(!readers.load(std::memory_order_relaxed));
}

template<>
inline void sux_lock<srw_lock>::s_lock(const char *file, unsigned line)
{
  ut_ad(!have_x());
  ut_ad(!have_s());
  if (!lock.template rd_lock<true>(file, line))
    waits.fetch_add(1, std::memory_order_relaxed);
  ut_d(s_lock_register());
}

template<>
inline void sux_lock<srw_lock>::u_lock(const char *file, unsigned line)
{
  os_thread_id_t id= os_thread_get_curr_id();
  if (writer.load(std::memory_order_relaxed) == id)
    writer_recurse<true>();
  else
  {
    if (!lock.u_lock(file, line))
      waits.fetch_add(1, std::memory_order_relaxed);
    ut_ad(!recursive);
    recursive= RECURSIVE_U;
    set_first_owner(id);
  }
}

template<>
inline void sux_lock<srw_lock>::x_lock(const char *file, unsigned line)
{
  os_thread_id_t id= os_thread_get_curr_id();
  if (writer.load(std::memory_order_relaxed) == id)
    writer_recurse<false>();
  else
  {
    if (!lock.template wr_lock<true>(file, line))
      waits.fetch_add(1, std::memory_order_relaxed);
    ut_ad(!recursive);
    recursive= RECURSIVE_X;
    set_first_owner(id);
  }
}

template<>
inline void sux_lock<srw_lock>::u_x_upgrade(const char *file, unsigned line)
{
  ut_ad(have_u_not_x());
  if (!lock.u_wr_upgrade(file, line))
    waits.fetch_add(1, std::memory_order_relaxed);
  recursive/= RECURSIVE_U;
}
#endif

template<>
inline void sux_lock<srw_lock_low>::s_lock()
{
  ut_ad(!have_x());
  ut_ad(!have_s());
  if (!lock.template rd_lock<true>())
    waits.fetch_add(1, std::memory_order_relaxed);
  ut_d(s_lock_register());
}

template<>
inline void sux_lock<srw_lock_low>::u_lock()
{
  os_thread_id_t id= os_thread_get_curr_id();
  if (writer.load(std::memory_order_relaxed) == id)
    writer_recurse<true>();
  else
  {
    if (!lock.u_lock())
      waits.fetch_add(1, std::memory_order_relaxed);
    ut_ad(!recursive);
    recursive= RECURSIVE_U;
    set_first_owner(id);
  }
}

template<>
inline void sux_lock<srw_lock_low>::x_lock(bool for_io)
{
  os_thread_id_t id= os_thread_get_curr_id();
  if (writer.load(std::memory_order_relaxed) == id)
  {
    ut_ad(!for_io);
    writer_recurse<false>();
  }
  else
  {
    if (!lock.template wr_lock<true>())
      waits.fetch_add(1, std::memory_order_relaxed);
    ut_ad(!recursive);
    recursive= RECURSIVE_X;
    set_first_owner(for_io ? FOR_IO : id);
  }
}

template<>
inline void sux_lock<srw_lock_low>::u_x_upgrade()
{
  ut_ad(have_u_not_x());
  if (!lock.u_wr_upgrade())
    waits.fetch_add(1, std::memory_order_relaxed);
  recursive/= RECURSIVE_U;
}

template<>
inline bool sux_lock<srw_lock_low>::u_lock_try(bool for_io)
{
  os_thread_id_t id= os_thread_get_curr_id();
  if (writer.load(std::memory_order_relaxed) == id)
  {
    if (for_io)
      return false;
    writer_recurse<true>();
    return true;
  }
  if (lock.u_lock_try())
  {
    ut_ad(!recursive);
    recursive= RECURSIVE_U;
    set_first_owner(for_io ? FOR_IO : id);
    return true;
  }
  return false;
}

template<>
inline bool sux_lock<srw_lock_low>::x_lock_try()
{
  os_thread_id_t id= os_thread_get_curr_id();
  if (writer.load(std::memory_order_relaxed) == id)
  {
    writer_recurse<false>();
    return true;
  }
  if (lock.wr_lock_try())
  {
    ut_ad(!recursive);
    recursive= RECURSIVE_X;
    set_first_owner(id);
    return true;
  }
  return false;
}