/*************************************************************************
 *
 * REALM CONFIDENTIAL
 * __________________
 *
 *  [2011] - [2012] Realm Inc
 *  All Rights Reserved.
 *
 * NOTICE:  All information contained herein is, and remains
 * the property of Realm Incorporated and its suppliers,
 * if any.  The intellectual and technical concepts contained
 * herein are proprietary to Realm Incorporated
 * and its suppliers and may be covered by U.S. and Foreign Patents,
 * patents in process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material
 * is strictly forbidden unless prior written permission is obtained
 * from Realm Incorporated.
 *
 **************************************************************************/
#ifndef REALM_ALLOC_HPP
#define REALM_ALLOC_HPP

#include <stdint.h>
#include <cstddef>

#include <realm/util/features.h>
#include <realm/util/terminate.hpp>
#include <realm/util/assert.hpp>
#include <realm/util/safe_int_ops.hpp>

namespace realm {

class Allocator;

#ifdef REALM_ENABLE_REPLICATION
class Replication;
#endif

typedef std::size_t ref_type;

ref_type to_ref(int64_t) REALM_NOEXCEPT;

class MemRef {
public:
    char* m_addr;
    ref_type m_ref;

    MemRef() REALM_NOEXCEPT;
    ~MemRef() REALM_NOEXCEPT;
    MemRef(char* addr, ref_type ref) REALM_NOEXCEPT;
    MemRef(ref_type ref, Allocator& alloc) REALM_NOEXCEPT;
};


/// The common interface for Realm allocators.
///
/// A Realm allocator must associate a 'ref' to each allocated
/// object and be able to efficiently map any 'ref' to the
/// corresponding memory address. The 'ref' is an integer and it must
/// always be divisible by 8. Also, a value of zero is used to
/// indicate a null-reference, and must therefore never be returned by
/// Allocator::alloc().
///
/// The purpose of the 'refs' is to decouple the memory reference from
/// the actual address and thereby allowing objects to be relocated in
/// memory without having to modify stored references.
///
/// \sa SlabAlloc
class Allocator {
public:
    /// The specified size must be divisible by 8, and must not be
    /// zero.
    ///
    /// \throw std::bad_alloc If insufficient memory was available.
    MemRef alloc(std::size_t size);

    /// Calls do_realloc().
    ///
    /// Note: The underscore has been added because the name `realloc`
    /// would conflict with a macro on the Windows platform.
    MemRef realloc_(ref_type, const char* addr, std::size_t old_size,
                    std::size_t new_size);

    /// Calls do_free().
    ///
    /// Note: The underscore has been added because the name `free
    /// would conflict with a macro on the Windows platform.
    void free_(ref_type, const char* addr) REALM_NOEXCEPT;

    /// Shorthand for free_(mem.m_ref, mem.m_addr).
    void free_(MemRef mem) REALM_NOEXCEPT;

    /// Calls do_translate().
    char* translate(ref_type ref) const REALM_NOEXCEPT;

    /// Returns true if, and only if the object at the specified 'ref'
    /// is in the immutable part of the memory managed by this
    /// allocator. The method by which some objects become part of the
    /// immuatble part is entirely up to the class that implements
    /// this interface.
    bool is_read_only(ref_type) const REALM_NOEXCEPT;

    /// Returns a simple allocator that can be used with free-standing
    /// Realm objects (such as a free-standing table). A
    /// free-standing object is one that is not part of a Group, and
    /// therefore, is not part of an actual database.
    static Allocator& get_default() REALM_NOEXCEPT;

    virtual ~Allocator() REALM_NOEXCEPT;

#ifdef REALM_DEBUG
    virtual void Verify() const = 0;

    /// Terminate the program precisely when the specified 'ref' is
    /// freed (or reallocated). You can use this to detect whether the
    /// ref is freed (or reallocated), and even to get a stacktrace at
    /// the point where it happens. Call watch(0) to stop watching
    /// that ref.
    void watch(ref_type);
#endif

#ifdef REALM_ENABLE_REPLICATION
    Replication* get_replication() REALM_NOEXCEPT;
#endif

protected:
    std::size_t m_baseline; // Separation line between immutable and mutable refs.

#ifdef REALM_ENABLE_REPLICATION
    Replication* m_replication;
#endif

#ifdef REALM_DEBUG
    ref_type m_watch;
#endif

    /// The specified size must be divisible by 8, and must not be
    /// zero.
    ///
    /// \throw std::bad_alloc If insufficient memory was available.
    virtual MemRef do_alloc(std::size_t size) = 0;

    /// The specified size must be divisible by 8, and must not be
    /// zero.
    ///
    /// The default version of this function simply allocates a new
    /// chunk of memory, copies over the old contents, and then frees
    /// the old chunk.
    ///
    /// \throw std::bad_alloc If insufficient memory was available.
    virtual MemRef do_realloc(ref_type, const char* addr, std::size_t old_size,
                              std::size_t new_size);

    /// Release the specified chunk of memory.
    virtual void do_free(ref_type, const char* addr) REALM_NOEXCEPT = 0;

    /// Map the specified \a ref to the corresponding memory
    /// address. Note that if is_read_only(ref) returns true, then the
    /// referenced object is to be considered immutable, and it is
    /// then entirely the responsibility of the caller that the memory
    /// is not modified by way of the returned memory pointer.
    virtual char* do_translate(ref_type ref) const REALM_NOEXCEPT = 0;

    Allocator() REALM_NOEXCEPT;

    // FIXME: This really doesn't belong in an allocator, but it is the best
    // place for now, because every table has a pointer leading here. It would
    // be more obvious to place it in Group, but that would add a runtime overhead,
    // and access is time critical.
    uint_fast64_t m_table_versioning_counter;

    /// Bump the global version counter. This method should be called when
    /// version bumping is initiated. Then following calls to should_propagate_version()
    /// can be used to prune the version bumping.
    uint_fast64_t bump_global_version() REALM_NOEXCEPT;

    /// Determine if the "local_version" is out of sync, so that it should
    /// be updated. In that case: also update it. Called from Table::bump_version
    /// to control propagation of version updates on tables within the group.
    bool should_propagate_version(uint_fast64_t& local_version) REALM_NOEXCEPT;

    friend class Table;
    friend class Group;
};

inline uint_fast64_t Allocator::bump_global_version() REALM_NOEXCEPT
{
    ++m_table_versioning_counter;
    return m_table_versioning_counter;
}


inline bool Allocator::should_propagate_version(uint_fast64_t& local_version) REALM_NOEXCEPT
{
    if (local_version != m_table_versioning_counter) {
        local_version = m_table_versioning_counter;
        return true;
    }
    else {
        return false;
    }
}



// Implementation:

inline int_fast64_t from_ref(ref_type v) REALM_NOEXCEPT
{
    // Check that v is divisible by 8 (64-bit aligned).
    REALM_ASSERT_DEBUG(v % 8 == 0);
    return util::from_twos_compl<int_fast64_t>(v);
}

inline ref_type to_ref(int_fast64_t v) REALM_NOEXCEPT
{
    REALM_ASSERT_DEBUG(!util::int_cast_has_overflow<ref_type>(v));
    // Check that v is divisible by 8 (64-bit aligned).
    REALM_ASSERT_DEBUG(v % 8 == 0);
    return ref_type(v);
}

inline MemRef::MemRef() REALM_NOEXCEPT:
    m_addr(nullptr),
    m_ref(0)
{
}

inline MemRef::~MemRef() REALM_NOEXCEPT
{
}

inline MemRef::MemRef(char* addr, ref_type ref) REALM_NOEXCEPT:
    m_addr(addr),
    m_ref(ref)
{
}

inline MemRef::MemRef(ref_type ref, Allocator& alloc) REALM_NOEXCEPT:
    m_addr(alloc.translate(ref)),
    m_ref(ref)
{
}


inline MemRef Allocator::alloc(std::size_t size)
{
    return do_alloc(size);
}

inline MemRef Allocator::realloc_(ref_type ref, const char* addr, std::size_t old_size,
                                  std::size_t new_size)
{
#ifdef REALM_DEBUG
    if (ref == m_watch)
        REALM_TERMINATE("Allocator watch: Ref was reallocated");
#endif
    return do_realloc(ref, addr, old_size, new_size);
}

inline void Allocator::free_(ref_type ref, const char* addr) REALM_NOEXCEPT
{
#ifdef REALM_DEBUG
    if (ref == m_watch)
        REALM_TERMINATE("Allocator watch: Ref was freed");
#endif
    return do_free(ref, addr);
}

inline void Allocator::free_(MemRef mem) REALM_NOEXCEPT
{
    free_(mem.m_ref, mem.m_addr);
}

inline char* Allocator::translate(ref_type ref) const REALM_NOEXCEPT
{
    return do_translate(ref);
}

inline bool Allocator::is_read_only(ref_type ref) const REALM_NOEXCEPT
{
    REALM_ASSERT_DEBUG(ref != 0);
    REALM_ASSERT_DEBUG(m_baseline != 0); // Attached SlabAlloc
    return ref < m_baseline;
}

inline Allocator::Allocator() REALM_NOEXCEPT
{
#ifdef REALM_ENABLE_REPLICATION
    m_replication = nullptr;
#endif
#ifdef REALM_DEBUG
    m_watch = 0;
#endif
    m_table_versioning_counter = 0;
}

inline Allocator::~Allocator() REALM_NOEXCEPT
{
}

#ifdef REALM_ENABLE_REPLICATION
inline Replication* Allocator::get_replication() REALM_NOEXCEPT
{
    return m_replication;
}
#endif

#ifdef REALM_DEBUG
inline void Allocator::watch(ref_type ref)
{
    m_watch = ref;
}
#endif


} // namespace realm

#endif // REALM_ALLOC_HPP
