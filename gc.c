/**********************************************************************

  gc.c -

  $Author$
  created at: Tue Oct  5 09:44:46 JST 1993

  Copyright (C) 1993-2007 Yukihiro Matsumoto
  Copyright (C) 2000  Network Applied Communication Laboratory, Inc.
  Copyright (C) 2000  Information-technology Promotion Agency, Japan

**********************************************************************/

#define rb_data_object_alloc rb_data_object_alloc
#define rb_data_typed_object_alloc rb_data_typed_object_alloc

#include "internal.h"
#include "ruby/st.h"
#include "ruby/re.h"
#include "ruby/io.h"
#include "ruby/thread.h"
#include "ruby/util.h"
#include "ruby/debug.h"
#include "eval_intern.h"
#include "vm_core.h"
#include "gc.h"
#include "constant.h"
#include "ruby_atomic.h"
#include "probes.h"
#include "id_table.h"
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>
#include <sys/types.h>
#include <assert.h>

#undef rb_data_object_wrap

#ifndef HAVE_MALLOC_USABLE_SIZE
# ifdef _WIN32
#   define HAVE_MALLOC_USABLE_SIZE
#   define malloc_usable_size(a) _msize(a)
# elif defined HAVE_MALLOC_SIZE
#   define HAVE_MALLOC_USABLE_SIZE
#   define malloc_usable_size(a) malloc_size(a)
# endif
#endif
#ifdef HAVE_MALLOC_USABLE_SIZE
# ifdef HAVE_MALLOC_H
#  include <malloc.h>
# elif defined(HAVE_MALLOC_NP_H)
#  include <malloc_np.h>
# elif defined(HAVE_MALLOC_MALLOC_H)
#  include <malloc/malloc.h>
# endif
#endif

#if /* is ASAN enabled? */ \
    __has_feature(address_sanitizer) /* Clang */ || \
    defined(__SANITIZE_ADDRESS__)  /* GCC 4.8.x */
  #define ATTRIBUTE_NO_ADDRESS_SAFETY_ANALYSIS \
        __attribute__((no_address_safety_analysis)) \
        __attribute__((noinline))
#else
  #define ATTRIBUTE_NO_ADDRESS_SAFETY_ANALYSIS
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#if defined(__native_client__) && defined(NACL_NEWLIB)
# include "nacl/resource.h"
# undef HAVE_POSIX_MEMALIGN
# undef HAVE_MEMALIGN

#endif

#if defined _WIN32 || defined __CYGWIN__
#include <windows.h>
#elif defined(HAVE_POSIX_MEMALIGN)
#elif defined(HAVE_MEMALIGN)
#include <malloc.h>
#endif

#define rb_setjmp(env) RUBY_SETJMP(env)
#define rb_jmp_buf rb_jmpbuf_t

#if defined(HAVE_RB_GC_GUARDED_PTR_VAL) && HAVE_RB_GC_GUARDED_PTR_VAL
/* trick the compiler into thinking a external signal handler uses this */
volatile VALUE rb_gc_guarded_val;
volatile VALUE *
rb_gc_guarded_ptr_val(volatile VALUE *ptr, VALUE val)
{
    rb_gc_guarded_val = val;

    return ptr;
}
#endif

#ifndef GC_HEAP_INIT_SLOTS
#define GC_HEAP_INIT_SLOTS 10000
#endif
#ifndef GC_HEAP_FREE_SLOTS
#define GC_HEAP_FREE_SLOTS  4096
#endif
#ifndef GC_HEAP_GROWTH_FACTOR
#define GC_HEAP_GROWTH_FACTOR 1.8
#endif
#ifndef GC_HEAP_GROWTH_MAX_SLOTS
#define GC_HEAP_GROWTH_MAX_SLOTS 0 /* 0 is disable */
#endif
#ifndef GC_HEAP_OLDOBJECT_LIMIT_FACTOR
#define GC_HEAP_OLDOBJECT_LIMIT_FACTOR 2.0
#endif

#ifndef GC_HEAP_FREE_SLOTS_MIN_RATIO
#define GC_HEAP_FREE_SLOTS_MIN_RATIO 0.3
#endif
#ifndef GC_HEAP_FREE_SLOTS_MAX_RATIO
#define GC_HEAP_FREE_SLOTS_MAX_RATIO 0.8
#endif

#ifndef GC_MALLOC_LIMIT_MIN
#define GC_MALLOC_LIMIT_MIN (16 * 1024 * 1024 /* 16MB */)
#endif
#ifndef GC_MALLOC_LIMIT_MAX
#define GC_MALLOC_LIMIT_MAX (32 * 1024 * 1024 /* 32MB */)
#endif
#ifndef GC_MALLOC_LIMIT_GROWTH_FACTOR
#define GC_MALLOC_LIMIT_GROWTH_FACTOR 1.4
#endif

#ifndef GC_OLDMALLOC_LIMIT_MIN
#define GC_OLDMALLOC_LIMIT_MIN (16 * 1024 * 1024 /* 16MB */)
#endif
#ifndef GC_OLDMALLOC_LIMIT_GROWTH_FACTOR
#define GC_OLDMALLOC_LIMIT_GROWTH_FACTOR 1.2
#endif
#ifndef GC_OLDMALLOC_LIMIT_MAX
#define GC_OLDMALLOC_LIMIT_MAX (128 * 1024 * 1024 /* 128MB */)
#endif

#ifndef PRINT_MEASURE_LINE
#define PRINT_MEASURE_LINE 0
#endif
#ifndef PRINT_ENTER_EXIT_TICK
#define PRINT_ENTER_EXIT_TICK 0
#endif
#ifndef PRINT_ROOT_TICKS
#define PRINT_ROOT_TICKS 0
#endif

#define USE_TICK_T                 (PRINT_ENTER_EXIT_TICK || PRINT_MEASURE_LINE || PRINT_ROOT_TICKS)
#define TICK_TYPE 1

typedef struct {
    size_t heap_init_slots;
    size_t heap_free_slots;
    double growth_factor;
    size_t growth_max_slots;
    double oldobject_limit_factor;
    size_t malloc_limit_min;
    size_t malloc_limit_max;
    double malloc_limit_growth_factor;
    size_t oldmalloc_limit_min;
    size_t oldmalloc_limit_max;
    double oldmalloc_limit_growth_factor;
    VALUE gc_stress;
} ruby_gc_params_t;

static ruby_gc_params_t gc_params = {
    GC_HEAP_INIT_SLOTS,
    GC_HEAP_FREE_SLOTS,
    GC_HEAP_GROWTH_FACTOR,
    GC_HEAP_GROWTH_MAX_SLOTS,
    GC_HEAP_OLDOBJECT_LIMIT_FACTOR,
    GC_MALLOC_LIMIT_MIN,
    GC_MALLOC_LIMIT_MAX,
    GC_MALLOC_LIMIT_GROWTH_FACTOR,
    GC_OLDMALLOC_LIMIT_MIN,
    GC_OLDMALLOC_LIMIT_MAX,
    GC_OLDMALLOC_LIMIT_GROWTH_FACTOR,
    FALSE,
};

/* GC_DEBUG:
 *  enable to embed GC debugging information.
 */
#ifndef GC_DEBUG
#define GC_DEBUG 0
#endif

#if USE_RGENGC
/* RGENGC_DEBUG:
 * 1: basic information
 * 2: remember set operation
 * 3: mark
 * 4:
 * 5: sweep
 */
#ifndef RGENGC_DEBUG
#define RGENGC_DEBUG       0
#endif

/* RGENGC_CHECK_MODE
 * 0: disable all assertions
 * 1: enable assertions (to debug RGenGC)
 * 2: enable internal consistency check at each GC (for debugging)
 * 3: enable internal consistency check at each GC steps (for debugging)
 * 4: enable livness check
 * 5: show all references
 */
#ifndef RGENGC_CHECK_MODE
#define RGENGC_CHECK_MODE  0
#endif

/* RGENGC_OLD_NEWOBJ_CHECK
 * 0:  disable all assertions
 * >0: make a OLD object when new object creation.
 *
 * Make one OLD object per RGENGC_OLD_NEWOBJ_CHECK WB protected objects creation.
 */
#ifndef RGENGC_OLD_NEWOBJ_CHECK
#define RGENGC_OLD_NEWOBJ_CHECK 0
#endif

/* RGENGC_PROFILE
 * 0: disable RGenGC profiling
 * 1: enable profiling for basic information
 * 2: enable profiling for each types
 */
#ifndef RGENGC_PROFILE
#define RGENGC_PROFILE     0
#endif

/* RGENGC_ESTIMATE_OLDMALLOC
 * Enable/disable to estimate increase size of malloc'ed size by old objects.
 * If estimation exceeds threshold, then will invoke full GC.
 * 0: disable estimation.
 * 1: enable estimation.
 */
#ifndef RGENGC_ESTIMATE_OLDMALLOC
#define RGENGC_ESTIMATE_OLDMALLOC 1
#endif

/* RGENGC_FORCE_MAJOR_GC
 * Force major/full GC if this macro is not 0.
 */
#ifndef RGENGC_FORCE_MAJOR_GC
#define RGENGC_FORCE_MAJOR_GC 0
#endif

#else /* USE_RGENGC */

#ifdef RGENGC_DEBUG
#undef RGENGC_DEBUG
#endif
#define RGENGC_DEBUG       0
#ifdef RGENGC_CHECK_MODE
#undef RGENGC_CHECK_MODE
#endif
#define RGENGC_CHECK_MODE  0
#define RGENGC_PROFILE     0
#define RGENGC_ESTIMATE_OLDMALLOC 0
#define RGENGC_FORCE_MAJOR_GC 0

#endif /* USE_RGENGC */

#ifndef GC_PROFILE_MORE_DETAIL
#define GC_PROFILE_MORE_DETAIL 0
#endif
#ifndef GC_PROFILE_DETAIL_MEMORY
#define GC_PROFILE_DETAIL_MEMORY 0
#endif
#ifndef GC_ENABLE_INCREMENTAL_MARK
#define GC_ENABLE_INCREMENTAL_MARK USE_RINCGC
#endif
#ifndef GC_ENABLE_LAZY_SWEEP
#define GC_ENABLE_LAZY_SWEEP   1
#endif
#ifndef CALC_EXACT_MALLOC_SIZE
#define CALC_EXACT_MALLOC_SIZE 0
#endif
#if defined(HAVE_MALLOC_USABLE_SIZE) || CALC_EXACT_MALLOC_SIZE > 0
#ifndef MALLOC_ALLOCATED_SIZE
#define MALLOC_ALLOCATED_SIZE 0
#endif
#else
#define MALLOC_ALLOCATED_SIZE 0
#endif
#ifndef MALLOC_ALLOCATED_SIZE_CHECK
#define MALLOC_ALLOCATED_SIZE_CHECK 0
#endif

#ifndef GC_DEBUG_STRESS_TO_CLASS
#define GC_DEBUG_STRESS_TO_CLASS 0
#endif

#ifndef RGENGC_OBJ_INFO
#define RGENGC_OBJ_INFO (RGENGC_DEBUG | RGENGC_CHECK_MODE)
#endif

typedef enum {
    GPR_FLAG_NONE               = 0x000,
    /* major reason */
    GPR_FLAG_MAJOR_BY_NOFREE    = 0x001,
    GPR_FLAG_MAJOR_BY_OLDGEN    = 0x002,
    GPR_FLAG_MAJOR_BY_SHADY     = 0x004,
    GPR_FLAG_MAJOR_BY_FORCE     = 0x008,
#if RGENGC_ESTIMATE_OLDMALLOC
    GPR_FLAG_MAJOR_BY_OLDMALLOC = 0x020,
#endif
    GPR_FLAG_MAJOR_MASK         = 0x0ff,

    /* gc reason */
    GPR_FLAG_NEWOBJ             = 0x100,
    GPR_FLAG_MALLOC             = 0x200,
    GPR_FLAG_METHOD             = 0x400,
    GPR_FLAG_CAPI               = 0x800,
    GPR_FLAG_STRESS            = 0x1000,

    /* others */
    GPR_FLAG_IMMEDIATE_SWEEP   = 0x2000,
    GPR_FLAG_HAVE_FINALIZE     = 0x4000
} gc_profile_record_flag;

typedef struct gc_profile_record {
    int flags;

    double gc_time;
    double gc_invoke_time;

    size_t heap_total_objects;
    size_t heap_use_size;
    size_t heap_total_size;

#if GC_PROFILE_MORE_DETAIL
    double gc_mark_time;
    double gc_sweep_time;

    size_t heap_use_pages;
    size_t heap_live_objects;
    size_t heap_free_objects;

    size_t allocate_increase;
    size_t allocate_limit;

    double prepare_time;
    size_t removing_objects;
    size_t empty_objects;
#if GC_PROFILE_DETAIL_MEMORY
    long maxrss;
    long minflt;
    long majflt;
#endif
#endif
#if MALLOC_ALLOCATED_SIZE
    size_t allocated_size;
#endif

#if RGENGC_PROFILE > 0
    size_t old_objects;
    size_t remembered_normal_objects;
    size_t remembered_shady_objects;
#endif
} gc_profile_record;

#if defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CYGWIN__)
#pragma pack(push, 1) /* magic for reducing sizeof(RVALUE): 24 -> 20 */
#endif

typedef struct RVALUE {
    union {
	struct {
	    VALUE flags;		/* always 0 for freed obj */
	    struct RVALUE *next;
	} free;
	struct RBasic  basic;
	struct RObject object;
	struct RClass  klass;
	struct RFloat  flonum;
	struct RString string;
	struct RArray  array;
	struct RRegexp regexp;
	struct RHash   hash;
	struct RData   data;
	struct RTypedData   typeddata;
	struct RStruct rstruct;
	struct RBignum bignum;
	struct RFile   file;
	struct RNode   node;
	struct RMatch  match;
	struct RRational rational;
	struct RComplex complex;
	union {
	    rb_cref_t cref;
	    struct vm_svar svar;
	    struct vm_throw_data throw_data;
	    struct vm_ifunc ifunc;
	    struct MEMO memo;
	    struct rb_method_entry_struct ment;
	    const rb_iseq_t iseq;
	} imemo;
	struct {
	    struct RBasic basic;
	    VALUE v1;
	    VALUE v2;
	    VALUE v3;
	} values;
    } as;
#if GC_DEBUG
    const char *file;
    int line;
#endif
} RVALUE;

#if defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CYGWIN__)
#pragma pack(pop)
#endif

typedef uintptr_t bits_t;
enum {
    BITS_SIZE = sizeof(bits_t),
    BITS_BITLENGTH = ( BITS_SIZE * CHAR_BIT )
};

struct heap_page_header {
    struct heap_page *page;
};

struct heap_page_body {
    struct heap_page_header header;
    /* char gap[];      */
    /* RVALUE values[]; */
};

struct gc_list {
    VALUE *varptr;
    struct gc_list *next;
};

#define STACK_CHUNK_SIZE 500

typedef struct stack_chunk {
    VALUE data[STACK_CHUNK_SIZE];
    struct stack_chunk *next;
} stack_chunk_t;

typedef struct mark_stack {
    stack_chunk_t *chunk;
    stack_chunk_t *cache;
    int index;
    int limit;
    size_t cache_size;
    size_t unused_cache_size;
} mark_stack_t;

typedef struct rb_heap_struct {
    RVALUE *freelist;

    struct heap_page *free_pages;
    struct heap_page *using_page;
    struct heap_page *pages;
    struct heap_page *sweep_pages;
#if GC_ENABLE_INCREMENTAL_MARK
    struct heap_page *pooled_pages;
#endif
    size_t page_length;      /* total page count in a heap */
    size_t total_slots;      /* total slot count (page_length * HEAP_OBJ_LIMIT) */
} rb_heap_t;

enum gc_stat {
    gc_stat_none,
    gc_stat_marking,
    gc_stat_sweeping
};

typedef struct rb_objspace {
    struct {
	size_t limit;
	size_t increase;
#if MALLOC_ALLOCATED_SIZE
	size_t allocated_size;
	size_t allocations;
#endif
    } malloc_params;

    struct {
	enum gc_stat stat : 2;
	unsigned int immediate_sweep : 1;
	unsigned int dont_gc : 1;
	unsigned int dont_incremental : 1;
	unsigned int during_gc : 1;
	unsigned int gc_stressful: 1;
#if USE_RGENGC
	unsigned int during_minor_gc : 1;
#endif
#if GC_ENABLE_INCREMENTAL_MARK
	unsigned int during_incremental_marking : 1;
#endif
    } flags;

    rb_event_flag_t hook_events;
    size_t total_allocated_objects;

    rb_heap_t eden_heap;
    rb_heap_t tomb_heap; /* heap for zombies and ghosts */

    struct {
	rb_atomic_t finalizing;
    } atomic_flags;

    struct mark_func_data_struct {
	void *data;
	void (*mark_func)(VALUE v, void *data);
    } *mark_func_data;

    mark_stack_t mark_stack;
    size_t marked_slots;

    struct {
	struct heap_page **sorted;
	size_t allocated_pages;
	size_t allocatable_pages;
	size_t sorted_length;
	RVALUE *range[2];

	size_t swept_slots;
	size_t min_free_slots;
	size_t max_free_slots;

	/* final */
	size_t final_slots;
	VALUE deferred_final;
    } heap_pages;

    st_table *finalizer_table;

    struct {
	int run;
	int latest_gc_info;
	gc_profile_record *records;
	gc_profile_record *current_record;
	size_t next_index;
	size_t size;

#if GC_PROFILE_MORE_DETAIL
	double prepare_time;
#endif
	double invoke_time;

#if USE_RGENGC
	size_t minor_gc_count;
	size_t major_gc_count;
#if RGENGC_PROFILE > 0
	size_t total_generated_normal_object_count;
	size_t total_generated_shady_object_count;
	size_t total_shade_operation_count;
	size_t total_promoted_count;
	size_t total_remembered_normal_object_count;
	size_t total_remembered_shady_object_count;

#if RGENGC_PROFILE >= 2
	size_t generated_normal_object_count_types[RUBY_T_MASK];
	size_t generated_shady_object_count_types[RUBY_T_MASK];
	size_t shade_operation_count_types[RUBY_T_MASK];
	size_t promoted_types[RUBY_T_MASK];
	size_t remembered_normal_object_count_types[RUBY_T_MASK];
	size_t remembered_shady_object_count_types[RUBY_T_MASK];
#endif
#endif /* RGENGC_PROFILE */
#endif /* USE_RGENGC */

	/* temporary profiling space */
	double gc_sweep_start_time;
	size_t total_allocated_objects_at_gc_start;
	size_t heap_used_at_gc_start;

	/* basic statistics */
	size_t count;
	size_t total_freed_objects;
	size_t total_allocated_pages;
	size_t total_freed_pages;
    } profile;
    struct gc_list *global_list;

    VALUE gc_stress_mode;

#if USE_RGENGC
    struct {
	VALUE parent_object;
	int need_major_gc;
	size_t last_major_gc;
	size_t uncollectible_wb_unprotected_objects;
	size_t uncollectible_wb_unprotected_objects_limit;
	size_t old_objects;
	size_t old_objects_limit;

#if RGENGC_ESTIMATE_OLDMALLOC
	size_t oldmalloc_increase;
	size_t oldmalloc_increase_limit;
#endif

#if RGENGC_CHECK_MODE >= 2
	struct st_table *allrefs_table;
	size_t error_count;
#endif
    } rgengc;
#if GC_ENABLE_INCREMENTAL_MARK
    struct {
	size_t pooled_slots;
	size_t step_slots;
    } rincgc;
#endif
#endif /* USE_RGENGC */

#if GC_DEBUG_STRESS_TO_CLASS
    VALUE stress_to_class;
#endif
} rb_objspace_t;


#ifndef HEAP_ALIGN_LOG
/* default tiny heap size: 16KB */
#define HEAP_ALIGN_LOG 14
#endif
#define CEILDIV(i, mod) (((i) + (mod) - 1)/(mod))
enum {
    HEAP_ALIGN = (1UL << HEAP_ALIGN_LOG),
    HEAP_ALIGN_MASK = (~(~0UL << HEAP_ALIGN_LOG)),
    REQUIRED_SIZE_BY_MALLOC = (sizeof(size_t) * 5),
    HEAP_SIZE = (HEAP_ALIGN - REQUIRED_SIZE_BY_MALLOC),
    HEAP_OBJ_LIMIT = (unsigned int)((HEAP_SIZE - sizeof(struct heap_page_header))/sizeof(struct RVALUE)),
    HEAP_BITMAP_LIMIT = CEILDIV(CEILDIV(HEAP_SIZE, sizeof(struct RVALUE)), BITS_BITLENGTH),
    HEAP_BITMAP_SIZE = ( BITS_SIZE * HEAP_BITMAP_LIMIT),
    HEAP_BITMAP_PLANES = USE_RGENGC ? 3 : 1 /* RGENGC: mark bits, rememberset bits and oldgen bits */
};

struct heap_page {
    struct heap_page_body *body;
    struct heap_page *prev;
    rb_heap_t *heap;
    int total_slots;
    int free_slots;
    int final_slots;
    struct {
	unsigned int before_sweep : 1;
	unsigned int has_remembered_objects : 1;
	unsigned int has_uncollectible_shady_objects : 1;
    } flags;

    struct heap_page *free_next;
    RVALUE *start;
    RVALUE *freelist;
    struct heap_page *next;

#if USE_RGENGC
    bits_t wb_unprotected_bits[HEAP_BITMAP_LIMIT];
#endif
    /* the following three bitmaps are cleared at the beginning of full GC */
    bits_t mark_bits[HEAP_BITMAP_LIMIT];
#if USE_RGENGC
    bits_t uncollectible_bits[HEAP_BITMAP_LIMIT];
    bits_t marking_bits[HEAP_BITMAP_LIMIT];
#endif
};

#define GET_PAGE_BODY(x)   ((struct heap_page_body *)((bits_t)(x) & ~(HEAP_ALIGN_MASK)))
#define GET_PAGE_HEADER(x) (&GET_PAGE_BODY(x)->header)
#define GET_HEAP_PAGE(x)   (GET_PAGE_HEADER(x)->page)

#define NUM_IN_PAGE(p)   (((bits_t)(p) & HEAP_ALIGN_MASK)/sizeof(RVALUE))
#define BITMAP_INDEX(p)  (NUM_IN_PAGE(p) / BITS_BITLENGTH )
#define BITMAP_OFFSET(p) (NUM_IN_PAGE(p) & (BITS_BITLENGTH-1))
#define BITMAP_BIT(p)    ((bits_t)1 << BITMAP_OFFSET(p))

/* Bitmap Operations */
#define MARKED_IN_BITMAP(bits, p)    ((bits)[BITMAP_INDEX(p)] & BITMAP_BIT(p))
#define MARK_IN_BITMAP(bits, p)      ((bits)[BITMAP_INDEX(p)] = (bits)[BITMAP_INDEX(p)] | BITMAP_BIT(p))
#define CLEAR_IN_BITMAP(bits, p)     ((bits)[BITMAP_INDEX(p)] = (bits)[BITMAP_INDEX(p)] & ~BITMAP_BIT(p))

/* getting bitmap */
#define GET_HEAP_MARK_BITS(x)           (&GET_HEAP_PAGE(x)->mark_bits[0])
#if USE_RGENGC
#define GET_HEAP_UNCOLLECTIBLE_BITS(x)  (&GET_HEAP_PAGE(x)->uncollectible_bits[0])
#define GET_HEAP_WB_UNPROTECTED_BITS(x) (&GET_HEAP_PAGE(x)->wb_unprotected_bits[0])
#define GET_HEAP_MARKING_BITS(x)        (&GET_HEAP_PAGE(x)->marking_bits[0])
#endif

/* Aliases */
#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
#define rb_objspace (*GET_VM()->objspace)
#else
static rb_objspace_t rb_objspace = {{GC_MALLOC_LIMIT_MIN}};
#endif

#define ruby_initial_gc_stress	gc_params.gc_stress

VALUE *ruby_initial_gc_stress_ptr = &ruby_initial_gc_stress;

#define malloc_limit		objspace->malloc_params.limit
#define malloc_increase 	objspace->malloc_params.increase
#define malloc_allocated_size 	objspace->malloc_params.allocated_size
#define heap_pages_sorted       objspace->heap_pages.sorted
#define heap_allocated_pages    objspace->heap_pages.allocated_pages
#define heap_pages_sorted_length objspace->heap_pages.sorted_length
#define heap_pages_lomem	objspace->heap_pages.range[0]
#define heap_pages_himem	objspace->heap_pages.range[1]
#define heap_pages_swept_slots	objspace->heap_pages.swept_slots
#define heap_allocatable_pages	objspace->heap_pages.allocatable_pages
#define heap_pages_min_free_slots	objspace->heap_pages.min_free_slots
#define heap_pages_max_free_slots	objspace->heap_pages.max_free_slots
#define heap_pages_final_slots		objspace->heap_pages.final_slots
#define heap_pages_deferred_final	objspace->heap_pages.deferred_final
#define heap_eden               (&objspace->eden_heap)
#define heap_tomb               (&objspace->tomb_heap)
#define dont_gc 		objspace->flags.dont_gc
#define during_gc		objspace->flags.during_gc
#define finalizing		objspace->atomic_flags.finalizing
#define finalizer_table 	objspace->finalizer_table
#define global_list		objspace->global_list
#define ruby_gc_stressful	objspace->flags.gc_stressful
#define ruby_gc_stress_mode     objspace->gc_stress_mode
#if GC_DEBUG_STRESS_TO_CLASS
#define stress_to_class         objspace->stress_to_class
#else
#define stress_to_class         0
#endif

#define is_marking(objspace)             ((objspace)->flags.stat == gc_stat_marking)
#define is_sweeping(objspace)            ((objspace)->flags.stat == gc_stat_sweeping)
#if USE_RGENGC
#define is_full_marking(objspace)        ((objspace)->flags.during_minor_gc == FALSE)
#else
#define is_full_marking(objspace)        TRUE
#endif
#if GC_ENABLE_INCREMENTAL_MARK
#define is_incremental_marking(objspace) ((objspace)->flags.during_incremental_marking != FALSE)
#else
#define is_incremental_marking(objspace) FALSE
#endif
#if GC_ENABLE_INCREMENTAL_MARK
#define will_be_incremental_marking(objspace) ((objspace)->rgengc.need_major_gc != GPR_FLAG_NONE)
#else
#define will_be_incremental_marking(objspace) FALSE
#endif
#define has_sweeping_pages(heap)         ((heap)->sweep_pages != 0)
#define is_lazy_sweeping(heap)           (GC_ENABLE_LAZY_SWEEP && has_sweeping_pages(heap))

#if SIZEOF_LONG == SIZEOF_VOIDP
# define nonspecial_obj_id(obj) (VALUE)((SIGNED_VALUE)(obj)|FIXNUM_FLAG)
# define obj_id_to_ref(objid) ((objid) ^ FIXNUM_FLAG) /* unset FIXNUM_FLAG */
#elif SIZEOF_LONG_LONG == SIZEOF_VOIDP
# define nonspecial_obj_id(obj) LL2NUM((SIGNED_VALUE)(obj) / 2)
# define obj_id_to_ref(objid) (FIXNUM_P(objid) ? \
   ((objid) ^ FIXNUM_FLAG) : (NUM2PTR(objid) << 1))
#else
# error not supported
#endif

#define RANY(o) ((RVALUE*)(o))

struct RZombie {
    struct RBasic basic;
    VALUE next;
    void (*dfree)(void *);
    void *data;
};

#define RZOMBIE(o) ((struct RZombie *)(o))

#define nomem_error GET_VM()->special_exceptions[ruby_error_nomemory]

int ruby_gc_debug_indent = 0;
VALUE rb_mGC;
int ruby_disable_gc = 0;

void rb_iseq_mark(const rb_iseq_t *iseq);
void rb_iseq_free(const rb_iseq_t *iseq);

void rb_gcdebug_print_obj_condition(VALUE obj);

static void rb_objspace_call_finalizer(rb_objspace_t *objspace);
static VALUE define_final0(VALUE obj, VALUE block);

static void negative_size_allocation_error(const char *);
static void *aligned_malloc(size_t, size_t);
static void aligned_free(void *);

static void init_mark_stack(mark_stack_t *stack);

static int ready_to_gc(rb_objspace_t *objspace);

static int garbage_collect(rb_objspace_t *, int full_mark, int immediate_mark, int immediate_sweep, int reason);

static int  gc_start(rb_objspace_t *objspace, const int full_mark, const int immediate_mark, const unsigned int immediate_sweep, int reason);
static void gc_rest(rb_objspace_t *objspace);
static inline void gc_enter(rb_objspace_t *objspace, const char *event);
static inline void gc_exit(rb_objspace_t *objspace, const char *event);

static void gc_marks(rb_objspace_t *objspace, int full_mark);
static void gc_marks_start(rb_objspace_t *objspace, int full);
static int  gc_marks_finish(rb_objspace_t *objspace);
static void gc_marks_rest(rb_objspace_t *objspace);
#if GC_ENABLE_INCREMENTAL_MARK
static void gc_marks_step(rb_objspace_t *objspace, int slots);
static void gc_marks_continue(rb_objspace_t *objspace, rb_heap_t *heap);
#endif

static void gc_sweep(rb_objspace_t *objspace);
static void gc_sweep_start(rb_objspace_t *objspace);
static void gc_sweep_finish(rb_objspace_t *objspace);
static int  gc_sweep_step(rb_objspace_t *objspace, rb_heap_t *heap);
static void gc_sweep_rest(rb_objspace_t *objspace);
#if GC_ENABLE_LAZY_SWEEP
static void gc_sweep_continue(rb_objspace_t *objspace, rb_heap_t *heap);
#endif

static void gc_mark(rb_objspace_t *objspace, VALUE ptr);
static void gc_mark_ptr(rb_objspace_t *objspace, VALUE ptr);
static void gc_mark_maybe(rb_objspace_t *objspace, VALUE ptr);
static void gc_mark_children(rb_objspace_t *objspace, VALUE ptr);

static int gc_mark_stacked_objects_incremental(rb_objspace_t *, size_t count);
static int gc_mark_stacked_objects_all(rb_objspace_t *);
static void gc_grey(rb_objspace_t *objspace, VALUE ptr);

static inline int gc_mark_set(rb_objspace_t *objspace, VALUE obj);
static inline int is_pointer_to_heap(rb_objspace_t *objspace, void *ptr);

static void   push_mark_stack(mark_stack_t *, VALUE);
static int    pop_mark_stack(mark_stack_t *, VALUE *);
static size_t mark_stack_size(mark_stack_t *stack);
static void   shrink_stack_chunk_cache(mark_stack_t *stack);

static size_t obj_memsize_of(VALUE obj, int use_all_types);
static VALUE gc_verify_internal_consistency(VALUE self);
static int gc_verify_heap_page(rb_objspace_t *objspace, struct heap_page *page, VALUE obj);
static int gc_verify_heap_pages(rb_objspace_t *objspace);

static void gc_stress_set(rb_objspace_t *objspace, VALUE flag);

static double getrusage_time(void);
static inline void gc_prof_setup_new_record(rb_objspace_t *objspace, int reason);
static inline void gc_prof_timer_start(rb_objspace_t *);
static inline void gc_prof_timer_stop(rb_objspace_t *);
static inline void gc_prof_mark_timer_start(rb_objspace_t *);
static inline void gc_prof_mark_timer_stop(rb_objspace_t *);
static inline void gc_prof_sweep_timer_start(rb_objspace_t *);
static inline void gc_prof_sweep_timer_stop(rb_objspace_t *);
static inline void gc_prof_set_malloc_info(rb_objspace_t *);
static inline void gc_prof_set_heap_info(rb_objspace_t *);

#define gc_prof_record(objspace) (objspace)->profile.current_record
#define gc_prof_enabled(objspace) ((objspace)->profile.run && (objspace)->profile.current_record)

#ifdef HAVE_VA_ARGS_MACRO
# define gc_report(level, objspace, fmt, ...) \
    if ((level) > RGENGC_DEBUG) {} else gc_report_body(level, objspace, fmt, ##__VA_ARGS__)
#else
# define gc_report if (!(RGENGC_DEBUG)) {} else gc_report_body
#endif
PRINTF_ARGS(static void gc_report_body(int level, rb_objspace_t *objspace, const char *fmt, ...), 3, 4);
static const char *obj_info(VALUE obj);

#define PUSH_MARK_FUNC_DATA(v) do { \
    struct mark_func_data_struct *prev_mark_func_data = objspace->mark_func_data; \
    objspace->mark_func_data = (v);

#define POP_MARK_FUNC_DATA() objspace->mark_func_data = prev_mark_func_data;} while (0)

/*
 * 1 - TSC (H/W Time Stamp Counter)
 * 2 - getrusage
 */
#ifndef TICK_TYPE
#define TICK_TYPE 1
#endif

#if USE_TICK_T

#if TICK_TYPE == 1
/* the following code is only for internal tuning. */

/* Source code to use RDTSC is quoted and modified from
 * http://www.mcs.anl.gov/~kazutomo/rdtsc.html
 * written by Kazutomo Yoshii <kazutomo@mcs.anl.gov>
 */

#if defined(__GNUC__) && defined(__i386__)
typedef unsigned long long tick_t;
#define PRItick "llu"
static inline tick_t
tick(void)
{
    unsigned long long int x;
    __asm__ __volatile__ ("rdtsc" : "=A" (x));
    return x;
}

#elif defined(__GNUC__) && defined(__x86_64__)
typedef unsigned long long tick_t;
#define PRItick "llu"

static __inline__ tick_t
tick(void)
{
    unsigned long hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)lo)|( ((unsigned long long)hi)<<32);
}

#elif defined(_WIN32) && defined(_MSC_VER)
#include <intrin.h>
typedef unsigned __int64 tick_t;
#define PRItick "llu"

static inline tick_t
tick(void)
{
    return __rdtsc();
}

#else /* use clock */
typedef clock_t tick_t;
#define PRItick "llu"

static inline tick_t
tick(void)
{
    return clock();
}
#endif /* TSC */

#elif TICK_TYPE == 2
typedef double tick_t;
#define PRItick "4.9f"

static inline tick_t
tick(void)
{
    return getrusage_time();
}
#else /* TICK_TYPE */
#error "choose tick type"
#endif /* TICK_TYPE */

#define MEASURE_LINE(expr) do { \
    volatile tick_t start_time = tick(); \
    volatile tick_t end_time; \
    expr; \
    end_time = tick(); \
    fprintf(stderr, "0\t%"PRItick"\t%s\n", end_time - start_time, #expr); \
} while (0)

#else /* USE_TICK_T */
#define MEASURE_LINE(expr) expr
#endif /* USE_TICK_T */

#define FL_TEST2(x,f)         ((RGENGC_CHECK_MODE && SPECIAL_CONST_P(x)) ? (rb_bug("FL_TEST2: SPECIAL_CONST (%p)", (void *)(x)), 0) : FL_TEST_RAW((x),(f)) != 0)
#define FL_SET2(x,f)          do {if (RGENGC_CHECK_MODE && SPECIAL_CONST_P(x)) rb_bug("FL_SET2: SPECIAL_CONST");   RBASIC(x)->flags |= (f);} while (0)
#define FL_UNSET2(x,f)        do {if (RGENGC_CHECK_MODE && SPECIAL_CONST_P(x)) rb_bug("FL_UNSET2: SPECIAL_CONST"); RBASIC(x)->flags &= ~(f);} while (0)

#define RVALUE_MARK_BITMAP(obj)           MARKED_IN_BITMAP(GET_HEAP_MARK_BITS(obj), (obj))
#define RVALUE_PAGE_MARKED(page, obj)     MARKED_IN_BITMAP((page)->mark_bits, (obj))

#if USE_RGENGC
#define RVALUE_WB_UNPROTECTED_BITMAP(obj) MARKED_IN_BITMAP(GET_HEAP_WB_UNPROTECTED_BITS(obj), (obj))
#define RVALUE_UNCOLLECTIBLE_BITMAP(obj)  MARKED_IN_BITMAP(GET_HEAP_UNCOLLECTIBLE_BITS(obj), (obj))
#define RVALUE_MARKING_BITMAP(obj)        MARKED_IN_BITMAP(GET_HEAP_MARKING_BITS(obj), (obj))

#define RVALUE_PAGE_WB_UNPROTECTED(apge, obj) MARKED_IN_BITMAP((page)->wb_unprotected_bits, (obj))
#define RVALUE_PAGE_UNCOLLECTIBLE(page, obj)  MARKED_IN_BITMAP((page)->uncollectible_bits, (obj))
#define RVALUE_PAGE_MARKING(page, obj)        MARKED_IN_BITMAP((page)->marking_bits, (obj))

#define RVALUE_OLD_AGE   3
#define RVALUE_AGE_SHIFT 5 /* FL_PROMOTED0 bit */

static int rgengc_remembered(rb_objspace_t *objspace, VALUE obj);
static int rgengc_remember(rb_objspace_t *objspace, VALUE obj);
static void rgengc_mark_and_rememberset_clear(rb_objspace_t *objspace, rb_heap_t *heap);
static void rgengc_rememberset_mark(rb_objspace_t *objspace, rb_heap_t *heap);

static inline int
RVALUE_FLAGS_AGE(VALUE flags)
{
    return (int)((flags & (FL_PROMOTED0 | FL_PROMOTED1)) >> RVALUE_AGE_SHIFT);
}

#endif /* USE_RGENGC */


#if RGENGC_CHECK_MODE == 0
static inline VALUE
check_rvalue_consistency(const VALUE obj)
{
    return obj;
}
#else
static VALUE
check_rvalue_consistency(const VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;

    if (SPECIAL_CONST_P(obj)) {
	rb_bug("check_rvalue_consistency: %p is a special const.", (void *)obj);
    }
    else if (!is_pointer_to_heap(objspace, (void *)obj)) {
	rb_bug("check_rvalue_consistency: %p is not a Ruby object.", (void *)obj);
    }
    else {
	const int wb_unprotected_bit = RVALUE_WB_UNPROTECTED_BITMAP(obj) != 0;
	const int uncollectible_bit = RVALUE_UNCOLLECTIBLE_BITMAP(obj) != 0;
	const int mark_bit = RVALUE_MARK_BITMAP(obj) != 0;
	const int marking_bit = RVALUE_MARKING_BITMAP(obj) != 0, remembered_bit = marking_bit;
	const int age = RVALUE_FLAGS_AGE(RBASIC(obj)->flags);

	if (BUILTIN_TYPE(obj) == T_NONE)   rb_bug("check_rvalue_consistency: %s is T_NONE", obj_info(obj));
	if (BUILTIN_TYPE(obj) == T_ZOMBIE) rb_bug("check_rvalue_consistency: %s is T_ZOMBIE", obj_info(obj));
	obj_memsize_of((VALUE)obj, FALSE);

	/* check generation
	 *
	 * OLD == age == 3 && old-bitmap && mark-bit (except incremental marking)
	 */
	if (age > 0 && wb_unprotected_bit) {
	    rb_bug("check_rvalue_consistency: %s is not WB protected, but age is %d > 0.", obj_info(obj), age);
	}

	if (!is_marking(objspace) && uncollectible_bit && !mark_bit) {
	    rb_bug("check_rvalue_consistency: %s is uncollectible, but is not marked while !gc.", obj_info(obj));
	}

	if (!is_full_marking(objspace)) {
	    if (uncollectible_bit && age != RVALUE_OLD_AGE && !wb_unprotected_bit) {
		rb_bug("check_rvalue_consistency: %s is uncollectible, but not old (age: %d) and not WB unprotected.", obj_info(obj), age);
	    }
	    if (remembered_bit && age != RVALUE_OLD_AGE) {
		rb_bug("check_rvalue_consistency: %s is rememberd, but not old (age: %d).", obj_info(obj), age);
	    }
	}

	/*
	 * check coloring
	 *
	 *               marking:false marking:true
	 * marked:false  white         *invalid*
	 * marked:true   black         grey
	 */
	if (is_incremental_marking(objspace) && marking_bit) {
	    if (!is_marking(objspace) && !mark_bit) rb_bug("check_rvalue_consistency: %s is marking, but not marked.", obj_info(obj));
	}
    }
    return obj;
}
#endif

static inline int
RVALUE_MARKED(VALUE obj)
{
    check_rvalue_consistency(obj);
    return RVALUE_MARK_BITMAP(obj) != 0;
}

#if USE_RGENGC
static inline int
RVALUE_WB_UNPROTECTED(VALUE obj)
{
    check_rvalue_consistency(obj);
    return RVALUE_WB_UNPROTECTED_BITMAP(obj) != 0;
}

static inline int
RVALUE_MARKING(VALUE obj)
{
    check_rvalue_consistency(obj);
    return RVALUE_MARKING_BITMAP(obj) != 0;
}

static inline int
RVALUE_REMEMBERED(VALUE obj)
{
    check_rvalue_consistency(obj);
    return RVALUE_MARKING_BITMAP(obj) != 0;
}

static inline int
RVALUE_UNCOLLECTIBLE(VALUE obj)
{
    check_rvalue_consistency(obj);
    return RVALUE_UNCOLLECTIBLE_BITMAP(obj) != 0;
}

static inline int
RVALUE_OLD_P_RAW(VALUE obj)
{
    const VALUE promoted = FL_PROMOTED0 | FL_PROMOTED1;
    return (RBASIC(obj)->flags & promoted) == promoted;
}

static inline int
RVALUE_OLD_P(VALUE obj)
{
    check_rvalue_consistency(obj);
    return RVALUE_OLD_P_RAW(obj);
}

#if RGENGC_CHECK_MODE || GC_DEBUG
static inline int
RVALUE_AGE(VALUE obj)
{
    check_rvalue_consistency(obj);
    return RVALUE_FLAGS_AGE(RBASIC(obj)->flags);
}
#endif

static inline void
RVALUE_PAGE_OLD_UNCOLLECTIBLE_SET(rb_objspace_t *objspace, struct heap_page *page, VALUE obj)
{
    MARK_IN_BITMAP(&page->uncollectible_bits[0], obj);
    objspace->rgengc.old_objects++;

#if RGENGC_PROFILE >= 2
    objspace->profile.total_promoted_count++;
    objspace->profile.promoted_types[BUILTIN_TYPE(obj)]++;
#endif
}

static inline void
RVALUE_OLD_UNCOLLECTIBLE_SET(rb_objspace_t *objspace, VALUE obj)
{
    RVALUE_PAGE_OLD_UNCOLLECTIBLE_SET(objspace, GET_HEAP_PAGE(obj), obj);
}

static inline VALUE
RVALUE_FLAGS_AGE_SET(VALUE flags, int age)
{
    flags &= ~(FL_PROMOTED0 | FL_PROMOTED1);
    flags |= (age << RVALUE_AGE_SHIFT);
    return flags;
}

/* set age to age+1 */
static inline void
RVALUE_AGE_INC(rb_objspace_t *objspace, VALUE obj)
{
    VALUE flags = RBASIC(obj)->flags;
    int age = RVALUE_FLAGS_AGE(flags);

    if (RGENGC_CHECK_MODE && age == RVALUE_OLD_AGE) {
	rb_bug("RVALUE_AGE_INC: can not increment age of OLD object %s.", obj_info(obj));
    }

    age++;
    RBASIC(obj)->flags = RVALUE_FLAGS_AGE_SET(flags, age);

    if (age == RVALUE_OLD_AGE) {
	RVALUE_OLD_UNCOLLECTIBLE_SET(objspace, obj);
    }
    check_rvalue_consistency(obj);
}

/* set age to RVALUE_OLD_AGE */
static inline void
RVALUE_AGE_SET_OLD(rb_objspace_t *objspace, VALUE obj)
{
    check_rvalue_consistency(obj);
    if (RGENGC_CHECK_MODE) assert(!RVALUE_OLD_P(obj));

    RBASIC(obj)->flags = RVALUE_FLAGS_AGE_SET(RBASIC(obj)->flags, RVALUE_OLD_AGE);
    RVALUE_OLD_UNCOLLECTIBLE_SET(objspace, obj);

    check_rvalue_consistency(obj);
}

/* set age to RVALUE_OLD_AGE - 1 */
static inline void
RVALUE_AGE_SET_CANDIDATE(rb_objspace_t *objspace, VALUE obj)
{
    check_rvalue_consistency(obj);
    if (RGENGC_CHECK_MODE) assert(!RVALUE_OLD_P(obj));

    RBASIC(obj)->flags = RVALUE_FLAGS_AGE_SET(RBASIC(obj)->flags, RVALUE_OLD_AGE - 1);

    check_rvalue_consistency(obj);
}

static inline void
RVALUE_DEMOTE_RAW(rb_objspace_t *objspace, VALUE obj)
{
    RBASIC(obj)->flags = RVALUE_FLAGS_AGE_SET(RBASIC(obj)->flags, 0);
    CLEAR_IN_BITMAP(GET_HEAP_UNCOLLECTIBLE_BITS(obj), obj);
}

static inline void
RVALUE_DEMOTE(rb_objspace_t *objspace, VALUE obj)
{
    check_rvalue_consistency(obj);
    if (RGENGC_CHECK_MODE) assert(RVALUE_OLD_P(obj));

    if (!is_incremental_marking(objspace) && RVALUE_REMEMBERED(obj)) {
	CLEAR_IN_BITMAP(GET_HEAP_MARKING_BITS(obj), obj);
    }

    RVALUE_DEMOTE_RAW(objspace, obj);

    if (RVALUE_MARKED(obj)) {
	objspace->rgengc.old_objects--;
    }

    check_rvalue_consistency(obj);
}

static inline void
RVALUE_AGE_RESET_RAW(VALUE obj)
{
    RBASIC(obj)->flags = RVALUE_FLAGS_AGE_SET(RBASIC(obj)->flags, 0);
}

static inline void
RVALUE_AGE_RESET(VALUE obj)
{
    check_rvalue_consistency(obj);
    if (RGENGC_CHECK_MODE) assert(!RVALUE_OLD_P(obj));
    RVALUE_AGE_RESET_RAW(obj);
    check_rvalue_consistency(obj);
}

static inline int
RVALUE_BLACK_P(VALUE obj)
{
    return RVALUE_MARKED(obj) && !RVALUE_MARKING(obj);
}

#if 0
static inline int
RVALUE_GREY_P(VALUE obj)
{
    return RVALUE_MARKED(obj) && RVALUE_MARKING(obj);
}
#endif

static inline int
RVALUE_WHITE_P(VALUE obj)
{
    return RVALUE_MARKED(obj) == FALSE;
}

#endif /* USE_RGENGC */

/*
  --------------------------- ObjectSpace -----------------------------
*/

rb_objspace_t *
rb_objspace_alloc(void)
{
#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
    rb_objspace_t *objspace = calloc(1, sizeof(rb_objspace_t));
#else
    rb_objspace_t *objspace = &rb_objspace;
#endif
    malloc_limit = gc_params.malloc_limit_min;

    return objspace;
}

static void free_stack_chunks(mark_stack_t *);
static void heap_page_free(rb_objspace_t *objspace, struct heap_page *page);

void
rb_objspace_free(rb_objspace_t *objspace)
{
    if (is_lazy_sweeping(heap_eden))
	rb_bug("lazy sweeping underway when freeing object space");

    if (objspace->profile.records) {
	free(objspace->profile.records);
	objspace->profile.records = 0;
    }

    if (global_list) {
	struct gc_list *list, *next;
	for (list = global_list; list; list = next) {
	    next = list->next;
	    xfree(list);
	}
    }
    if (heap_pages_sorted) {
	size_t i;
	for (i = 0; i < heap_allocated_pages; ++i) {
	    heap_page_free(objspace, heap_pages_sorted[i]);
	}
	free(heap_pages_sorted);
	heap_allocated_pages = 0;
	heap_pages_sorted_length = 0;
	heap_pages_lomem = 0;
	heap_pages_himem = 0;

	objspace->eden_heap.page_length = 0;
	objspace->eden_heap.total_slots = 0;
	objspace->eden_heap.pages = NULL;
    }
    free_stack_chunks(&objspace->mark_stack);
#if !(defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE)
    if (objspace == &rb_objspace) return;
#endif
    free(objspace);
}

static void
heap_pages_expand_sorted(rb_objspace_t *objspace)
{
    size_t next_length = heap_allocatable_pages;
    next_length += heap_eden->page_length;
    next_length += heap_tomb->page_length;

    if (next_length > heap_pages_sorted_length) {
	struct heap_page **sorted;
	size_t size = next_length * sizeof(struct heap_page *);

	gc_report(3, objspace, "heap_pages_expand_sorted: next_length: %d, size: %d\n", (int)next_length, (int)size);

	if (heap_pages_sorted_length > 0) {
	    sorted = (struct heap_page **)realloc(heap_pages_sorted, size);
	    if (sorted) heap_pages_sorted = sorted;
	}
	else {
	    sorted = heap_pages_sorted = (struct heap_page **)malloc(size);
	}

	if (sorted == 0) {
	    rb_memerror();
	}

	heap_pages_sorted_length = next_length;
    }
}

static inline void
heap_page_add_freeobj(rb_objspace_t *objspace, struct heap_page *page, VALUE obj)
{
    RVALUE *p = (RVALUE *)obj;
    p->as.free.flags = 0;
    p->as.free.next = page->freelist;
    page->freelist = p;

    if (RGENGC_CHECK_MODE && !is_pointer_to_heap(objspace, p)) {
	rb_bug("heap_page_add_freeobj: %p is not rvalue.", p);
    }

    gc_report(3, objspace, "heap_page_add_freeobj: add %p to freelist\n", (void *)obj);
}

static inline void
heap_add_freepage(rb_objspace_t *objspace, rb_heap_t *heap, struct heap_page *page)
{
    if (page->freelist) {
	page->free_next = heap->free_pages;
	heap->free_pages = page;
    }
}

#if GC_ENABLE_INCREMENTAL_MARK
static inline int
heap_add_poolpage(rb_objspace_t *objspace, rb_heap_t *heap, struct heap_page *page)
{
    if (page->freelist) {
	page->free_next = heap->pooled_pages;
	heap->pooled_pages = page;
	objspace->rincgc.pooled_slots += page->free_slots;
	return TRUE;
    }
    else {
	return FALSE;
    }
}
#endif

static void
heap_unlink_page(rb_objspace_t *objspace, rb_heap_t *heap, struct heap_page *page)
{
    if (page->prev) page->prev->next = page->next;
    if (page->next) page->next->prev = page->prev;
    if (heap->pages == page) heap->pages = page->next;
    page->prev = NULL;
    page->next = NULL;
    page->heap = NULL;
    heap->page_length--;
    heap->total_slots -= page->total_slots;
}

static void
heap_page_free(rb_objspace_t *objspace, struct heap_page *page)
{
    heap_allocated_pages--;
    objspace->profile.total_freed_pages++;
    aligned_free(page->body);
    free(page);
}

static void
heap_pages_free_unused_pages(rb_objspace_t *objspace)
{
    size_t i, j;

    if (heap_tomb->pages && heap_pages_swept_slots > heap_pages_max_free_slots) {
	for (i = j = 1; j < heap_allocated_pages; i++) {
	    struct heap_page *page = heap_pages_sorted[i];

	    if (page->heap == heap_tomb && page->free_slots == page->total_slots) {
		if (heap_pages_swept_slots - page->total_slots > heap_pages_max_free_slots) {
		    if (0) fprintf(stderr, "heap_pages_free_unused_pages: %d free page %p, heap_pages_swept_slots: %d, heap_pages_max_free_slots: %d\n",
				   (int)i, page, (int)heap_pages_swept_slots, (int)heap_pages_max_free_slots);
		    heap_pages_swept_slots -= page->total_slots;
		    heap_unlink_page(objspace, heap_tomb, page);
		    heap_page_free(objspace, page);
		    continue;
		}
		else if (i == j) {
		    return; /* no need to check rest pages */
		}
	    }
	    if (i != j) {
		heap_pages_sorted[j] = page;
	    }
	    j++;
	}
	if (RGENGC_CHECK_MODE) assert(j == heap_allocated_pages);
    }
}

static struct heap_page *
heap_page_allocate(rb_objspace_t *objspace)
{
    RVALUE *start, *end, *p;
    struct heap_page *page;
    struct heap_page_body *page_body = 0;
    size_t hi, lo, mid;
    int limit = HEAP_OBJ_LIMIT;

    /* assign heap_page body (contains heap_page_header and RVALUEs) */
    page_body = (struct heap_page_body *)aligned_malloc(HEAP_ALIGN, HEAP_SIZE);
    if (page_body == 0) {
	rb_memerror();
    }

    /* assign heap_page entry */
    page = (struct heap_page *)calloc(1, sizeof(struct heap_page));
    if (page == 0) {
	aligned_free(page_body);
	rb_memerror();
    }

    page->body = page_body;

    /* setup heap_pages_sorted */
    lo = 0;
    hi = heap_allocated_pages;
    while (lo < hi) {
	struct heap_page *mid_page;

	mid = (lo + hi) / 2;
	mid_page = heap_pages_sorted[mid];
	if (mid_page->body < page_body) {
	    lo = mid + 1;
	}
	else if (mid_page->body > page_body) {
	    hi = mid;
	}
	else {
	    rb_bug("same heap page is allocated: %p at %"PRIuVALUE, (void *)page_body, (VALUE)mid);
	}
    }
    if (hi < heap_allocated_pages) {
	MEMMOVE(&heap_pages_sorted[hi+1], &heap_pages_sorted[hi], struct heap_page_header*, heap_allocated_pages - hi);
    }

    heap_pages_sorted[hi] = page;

    heap_allocated_pages++;
    objspace->profile.total_allocated_pages++;

    if (RGENGC_CHECK_MODE) assert(heap_allocated_pages <= heap_pages_sorted_length);

    /* adjust obj_limit (object number available in this page) */
    start = (RVALUE*)((VALUE)page_body + sizeof(struct heap_page_header));
    if ((VALUE)start % sizeof(RVALUE) != 0) {
	int delta = (int)(sizeof(RVALUE) - ((VALUE)start % sizeof(RVALUE)));
	start = (RVALUE*)((VALUE)start + delta);
	limit = (HEAP_SIZE - (int)((VALUE)start - (VALUE)page_body))/(int)sizeof(RVALUE);
    }
    end = start + limit;

    if (heap_pages_lomem == 0 || heap_pages_lomem > start) heap_pages_lomem = start;
    if (heap_pages_himem < end) heap_pages_himem = end;

    page->start = start;
    page->total_slots = limit;
    page_body->header.page = page;

    for (p = start; p != end; p++) {
	gc_report(3, objspace, "assign_heap_page: %p is added to freelist\n", p);
	heap_page_add_freeobj(objspace, page, (VALUE)p);
    }
    page->free_slots = limit;

    return page;
}

static struct heap_page *
heap_page_resurrect(rb_objspace_t *objspace)
{
    struct heap_page *page;

    if ((page = heap_tomb->pages) != NULL) {
	heap_unlink_page(objspace, heap_tomb, page);
	return page;
    }
    return NULL;
}

static struct heap_page *
heap_page_create(rb_objspace_t *objspace)
{
    struct heap_page *page = heap_page_resurrect(objspace);
    const char *method = "recycle";
    if (page == NULL) {
	page = heap_page_allocate(objspace);
	method = "allocate";
    }
    if (0) fprintf(stderr, "heap_page_create: %s - %p, heap_allocated_pages: %d, heap_allocated_pages: %d, tomb->page_length: %d\n",
		   method, page, (int)heap_pages_sorted_length, (int)heap_allocated_pages, (int)heap_tomb->page_length);
    return page;
}

static void
heap_add_page(rb_objspace_t *objspace, rb_heap_t *heap, struct heap_page *page)
{
    page->heap = heap;
    page->next = heap->pages;
    if (heap->pages) heap->pages->prev = page;
    heap->pages = page;
    heap->page_length++;
    heap->total_slots += page->total_slots;
}

static void
heap_assign_page(rb_objspace_t *objspace, rb_heap_t *heap)
{
    struct heap_page *page = heap_page_create(objspace);
    heap_add_page(objspace, heap, page);
    heap_add_freepage(objspace, heap, page);
}

static void
heap_add_pages(rb_objspace_t *objspace, rb_heap_t *heap, size_t add)
{
    size_t i;

    heap_allocatable_pages = add;
    heap_pages_expand_sorted(objspace);
    for (i = 0; i < add; i++) {
	heap_assign_page(objspace, heap);
    }
    heap_allocatable_pages = 0;
}

static size_t
heap_extend_pages(rb_objspace_t *objspace)
{
    size_t used = heap_allocated_pages - heap_tomb->page_length;
    size_t next_used_limit = (size_t)(used * gc_params.growth_factor);

    if (gc_params.growth_max_slots > 0) {
	size_t max_used_limit = (size_t)(used + gc_params.growth_max_slots/HEAP_OBJ_LIMIT);
	if (next_used_limit > max_used_limit) next_used_limit = max_used_limit;
    }

    return next_used_limit - used;
}

static void
heap_set_increment(rb_objspace_t *objspace, size_t additional_pages)
{
    size_t used = heap_eden->page_length;
    size_t next_used_limit = used + additional_pages;

    if (next_used_limit == heap_allocated_pages) next_used_limit++;

    heap_allocatable_pages = next_used_limit - used;
    heap_pages_expand_sorted(objspace);

    gc_report(1, objspace, "heap_set_increment: heap_allocatable_pages is %d\n", (int)heap_allocatable_pages);
}

static int
heap_increment(rb_objspace_t *objspace, rb_heap_t *heap)
{
    if (heap_allocatable_pages > 0) {
	gc_report(1, objspace, "heap_increment: heap_pages_sorted_length: %d, heap_pages_inc: %d, heap->page_length: %d\n",
		  (int)heap_pages_sorted_length, (int)heap_allocatable_pages, (int)heap->page_length);
	heap_allocatable_pages--;
	heap_assign_page(objspace, heap);
	return TRUE;
    }
    return FALSE;
}

static void
heap_prepare(rb_objspace_t *objspace, rb_heap_t *heap)
{
    if (RGENGC_CHECK_MODE) assert(heap->free_pages == NULL);

#if GC_ENABLE_LAZY_SWEEP
    if (is_lazy_sweeping(heap)) {
	gc_sweep_continue(objspace, heap);
    }
#endif
#if GC_ENABLE_INCREMENTAL_MARK
    else if (is_incremental_marking(objspace)) {
	gc_marks_continue(objspace, heap);
    }
#endif

    if (heap->free_pages == NULL &&
	(will_be_incremental_marking(objspace) || heap_increment(objspace, heap) == FALSE) &&
	gc_start(objspace, FALSE, FALSE, FALSE, GPR_FLAG_NEWOBJ) == FALSE) {
	rb_memerror();
    }
}

static RVALUE *
heap_get_freeobj_from_next_freepage(rb_objspace_t *objspace, rb_heap_t *heap)
{
    struct heap_page *page;
    RVALUE *p;

    while (UNLIKELY(heap->free_pages == NULL)) {
	heap_prepare(objspace, heap);
    }
    page = heap->free_pages;
    heap->free_pages = page->free_next;
    heap->using_page = page;

    if (RGENGC_CHECK_MODE) assert(page->free_slots != 0);
    p = page->freelist;
    page->freelist = NULL;
    page->free_slots = 0;
    return p;
}

static inline VALUE
heap_get_freeobj(rb_objspace_t *objspace, rb_heap_t *heap)
{
    RVALUE *p = heap->freelist;

    while (1) {
	if (LIKELY(p != NULL)) {
	    heap->freelist = p->as.free.next;
	    return (VALUE)p;
	}
	else {
	    p = heap_get_freeobj_from_next_freepage(objspace, heap);
	}
    }
}

void
rb_objspace_set_event_hook(const rb_event_flag_t event)
{
    rb_objspace_t *objspace = &rb_objspace;
    objspace->hook_events = event & RUBY_INTERNAL_EVENT_OBJSPACE_MASK;
}

static void
gc_event_hook_body(rb_thread_t *th, rb_objspace_t *objspace, const rb_event_flag_t event, VALUE data)
{
    EXEC_EVENT_HOOK(th, event, th->cfp->self, 0, 0, data);
}

#define gc_event_hook(objspace, event, data) do { \
    if (UNLIKELY((objspace)->hook_events & (event))) { \
	gc_event_hook_body(GET_THREAD(), (objspace), (event), (data)); \
    } \
} while (0)

static inline VALUE
newobj_of(VALUE klass, VALUE flags, VALUE v1, VALUE v2, VALUE v3)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE obj;

#if GC_DEBUG_STRESS_TO_CLASS
    if (UNLIKELY(stress_to_class)) {
	long i, cnt = RARRAY_LEN(stress_to_class);
	const VALUE *ptr = RARRAY_CONST_PTR(stress_to_class);
	for (i = 0; i < cnt; ++i) {
	    if (klass == ptr[i]) rb_memerror();
	}
    }
#endif

    if (UNLIKELY(during_gc || ruby_gc_stressful)) {
	if (during_gc) {
	    dont_gc = 1;
	    during_gc = 0;
	    rb_bug("object allocation during garbage collection phase");
	}

	if (ruby_gc_stressful) {
	    if (!garbage_collect(objspace, FALSE, FALSE, FALSE, GPR_FLAG_NEWOBJ)) {
		rb_memerror();
	    }
	}
    }

    obj = heap_get_freeobj(objspace, heap_eden);

    if (RGENGC_CHECK_MODE > 0) assert(BUILTIN_TYPE(obj) == T_NONE);

    /* OBJSETUP */
    RBASIC(obj)->flags = flags & ~FL_WB_PROTECTED;
    RBASIC_SET_CLASS_RAW(obj, klass);
    RANY(obj)->as.values.v1 = v1;
    RANY(obj)->as.values.v2 = v2;
    RANY(obj)->as.values.v3 = v3;

#if RGENGC_CHECK_MODE
    assert(RVALUE_MARKED(obj) == FALSE);
    assert(RVALUE_MARKING(obj) == FALSE);
    assert(RVALUE_OLD_P(obj) == FALSE);
    assert(RVALUE_WB_UNPROTECTED(obj) == FALSE);

    if (flags & FL_PROMOTED1) {
	if (RVALUE_AGE(obj) != 2) rb_bug("newobj: %s of age (%d) != 2.", obj_info(obj), RVALUE_AGE(obj));
    }
    else {
	if (RVALUE_AGE(obj) > 0) rb_bug("newobj: %s of age (%d) > 0.", obj_info(obj), RVALUE_AGE(obj));
    }
    if (rgengc_remembered(objspace, (VALUE)obj)) rb_bug("newobj: %s is remembered.", obj_info(obj));
#endif

#if USE_RGENGC
    if ((flags & FL_WB_PROTECTED) == 0) {
	MARK_IN_BITMAP(GET_HEAP_WB_UNPROTECTED_BITS(obj), obj);
    }
#endif

#if RGENGC_PROFILE
    if (flags & FL_WB_PROTECTED) {
	objspace->profile.total_generated_normal_object_count++;
#if RGENGC_PROFILE >= 2
	objspace->profile.generated_normal_object_count_types[BUILTIN_TYPE(obj)]++;
#endif
    }
    else {
	objspace->profile.total_generated_shady_object_count++;
#if RGENGC_PROFILE >= 2
	objspace->profile.generated_shady_object_count_types[BUILTIN_TYPE(obj)]++;
#endif
    }
#endif

#if GC_DEBUG
    RANY(obj)->file = rb_sourcefile();
    RANY(obj)->line = rb_sourceline();
    assert(!SPECIAL_CONST_P(obj)); /* check alignment */
#endif

    objspace->total_allocated_objects++;
    gc_event_hook(objspace, RUBY_INTERNAL_EVENT_NEWOBJ, obj);
    gc_report(5, objspace, "newobj: %s\n", obj_info(obj));

#if RGENGC_OLD_NEWOBJ_CHECK > 0
    {
	static int newobj_cnt = RGENGC_OLD_NEWOBJ_CHECK;

	if (!is_incremental_marking(objspace) &&
	    flags & FL_WB_PROTECTED &&   /* do not promote WB unprotected objects */
	    ! RB_TYPE_P(obj, T_ARRAY)) { /* array.c assumes that allocated objects are new */
	    if (--newobj_cnt == 0) {
		newobj_cnt = RGENGC_OLD_NEWOBJ_CHECK;

		gc_mark_set(objspace, obj);
		RVALUE_AGE_SET_OLD(objspace, obj);

		rb_gc_writebarrier_remember(obj);
	    }
	}
    }
#endif
    check_rvalue_consistency(obj);
    return obj;
}

VALUE
rb_newobj(void)
{
    return newobj_of(0, T_NONE, 0, 0, 0);
}

VALUE
rb_newobj_of(VALUE klass, VALUE flags)
{
    return newobj_of(klass, flags, 0, 0, 0);
}

NODE*
rb_node_newnode(enum node_type type, VALUE a0, VALUE a1, VALUE a2)
{
    VALUE flags = 0;
    NODE *n = (NODE *)newobj_of(0, T_NODE | flags, a0, a1, a2);
    nd_set_type(n, type);
    return n;
}

#undef rb_imemo_new

VALUE
rb_imemo_new(enum imemo_type type, VALUE v1, VALUE v2, VALUE v3, VALUE v0)
{
    VALUE flags = T_IMEMO | (type << FL_USHIFT) | FL_WB_PROTECTED;
    return newobj_of(v0, flags, v1, v2, v3);
}

#if IMEMO_DEBUG
VALUE
rb_imemo_new_debug(enum imemo_type type, VALUE v1, VALUE v2, VALUE v3, VALUE v0, const char *file, int line)
{
    VALUE memo = rb_imemo_new(type, v1, v2, v3, v0);
    fprintf(stderr, "memo %p (type: %d) @ %s:%d\n", memo, imemo_type(memo), file, line);
    return memo;
}
#endif

VALUE
rb_data_object_wrap(VALUE klass, void *datap, RUBY_DATA_FUNC dmark, RUBY_DATA_FUNC dfree)
{
    if (klass) Check_Type(klass, T_CLASS);
    return newobj_of(klass, T_DATA, (VALUE)dmark, (VALUE)dfree, (VALUE)datap);
}

#undef rb_data_object_alloc
RUBY_ALIAS_FUNCTION(rb_data_object_alloc(VALUE klass, void *datap,
					 RUBY_DATA_FUNC dmark, RUBY_DATA_FUNC dfree),
		    rb_data_object_wrap, (klass, datap, dmark, dfree));


VALUE
rb_data_object_zalloc(VALUE klass, size_t size, RUBY_DATA_FUNC dmark, RUBY_DATA_FUNC dfree)
{
    VALUE obj = rb_data_object_wrap(klass, 0, dmark, dfree);
    DATA_PTR(obj) = xcalloc(1, size);
    return obj;
}

VALUE
rb_data_typed_object_wrap(VALUE klass, void *datap, const rb_data_type_t *type)
{
    if (klass) Check_Type(klass, T_CLASS);
    return newobj_of(klass, T_DATA | (type->flags & ~T_MASK), (VALUE)type, (VALUE)1, (VALUE)datap);
}

#undef rb_data_typed_object_alloc
RUBY_ALIAS_FUNCTION(rb_data_typed_object_alloc(VALUE klass, void *datap,
					       const rb_data_type_t *type),
		    rb_data_typed_object_wrap, (klass, datap, type));

VALUE
rb_data_typed_object_zalloc(VALUE klass, size_t size, const rb_data_type_t *type)
{
    VALUE obj = rb_data_typed_object_wrap(klass, 0, type);
    DATA_PTR(obj) = xcalloc(1, size);
    return obj;
}

size_t
rb_objspace_data_type_memsize(VALUE obj)
{
    if (RTYPEDDATA_P(obj) && RTYPEDDATA_TYPE(obj)->function.dsize) {
	return RTYPEDDATA_TYPE(obj)->function.dsize(RTYPEDDATA_DATA(obj));
    }
    else {
	return 0;
    }
}

const char *
rb_objspace_data_type_name(VALUE obj)
{
    if (RTYPEDDATA_P(obj)) {
	return RTYPEDDATA_TYPE(obj)->wrap_struct_name;
    }
    else {
	return 0;
    }
}

static inline int
is_pointer_to_heap(rb_objspace_t *objspace, void *ptr)
{
    register RVALUE *p = RANY(ptr);
    register struct heap_page *page;
    register size_t hi, lo, mid;

    if (p < heap_pages_lomem || p > heap_pages_himem) return FALSE;
    if ((VALUE)p % sizeof(RVALUE) != 0) return FALSE;

    /* check if p looks like a pointer using bsearch*/
    lo = 0;
    hi = heap_allocated_pages;
    while (lo < hi) {
	mid = (lo + hi) / 2;
	page = heap_pages_sorted[mid];
	if (page->start <= p) {
	    if (p < page->start + page->total_slots) {
		return TRUE;
	    }
	    lo = mid + 1;
	}
	else {
	    hi = mid;
	}
    }
    return FALSE;
}

static int
free_const_entry_i(st_data_t key, st_data_t value, st_data_t data)
{
    rb_const_entry_t *ce = (rb_const_entry_t *)value;
    xfree(ce);
    return ST_CONTINUE;
}

void
rb_free_const_table(st_table *tbl)
{
    st_foreach(tbl, free_const_entry_i, 0);
    st_free_table(tbl);
}

static inline void
make_zombie(rb_objspace_t *objspace, VALUE obj, void (*dfree)(void *), void *data)
{
    struct RZombie *zombie = RZOMBIE(obj);
    zombie->basic.flags = T_ZOMBIE;
    zombie->dfree = dfree;
    zombie->data = data;
    zombie->next = heap_pages_deferred_final;
    heap_pages_deferred_final = (VALUE)zombie;
}

static inline void
make_io_zombie(rb_objspace_t *objspace, VALUE obj)
{
    rb_io_t *fptr = RANY(obj)->as.file.fptr;
    make_zombie(objspace, obj, (void (*)(void*))rb_io_fptr_finalize, fptr);
}

static int
obj_free(rb_objspace_t *objspace, VALUE obj)
{
    gc_event_hook(objspace, RUBY_INTERNAL_EVENT_FREEOBJ, obj);

    switch (BUILTIN_TYPE(obj)) {
      case T_NIL:
      case T_FIXNUM:
      case T_TRUE:
      case T_FALSE:
	rb_bug("obj_free() called for broken object");
	break;
    }

    if (FL_TEST(obj, FL_EXIVAR)) {
	rb_free_generic_ivar((VALUE)obj);
	FL_UNSET(obj, FL_EXIVAR);
    }

#if USE_RGENGC
    if (RVALUE_WB_UNPROTECTED(obj)) CLEAR_IN_BITMAP(GET_HEAP_WB_UNPROTECTED_BITS(obj), obj);

#if RGENGC_CHECK_MODE
#define CHECK(x) if (x(obj) != FALSE) rb_bug("obj_free: " #x "(%s) != FALSE", obj_info(obj))
	CHECK(RVALUE_WB_UNPROTECTED);
	CHECK(RVALUE_MARKED);
	CHECK(RVALUE_MARKING);
	CHECK(RVALUE_UNCOLLECTIBLE);
#undef CHECK
#endif
#endif

    switch (BUILTIN_TYPE(obj)) {
      case T_OBJECT:
	if (!(RANY(obj)->as.basic.flags & ROBJECT_EMBED) &&
            RANY(obj)->as.object.as.heap.ivptr) {
	    xfree(RANY(obj)->as.object.as.heap.ivptr);
	}
	break;
      case T_MODULE:
      case T_CLASS:
	rb_id_table_free(RCLASS_M_TBL(obj));
	if (RCLASS_IV_TBL(obj)) {
	    st_free_table(RCLASS_IV_TBL(obj));
	}
	if (RCLASS_CONST_TBL(obj)) {
	    rb_free_const_table(RCLASS_CONST_TBL(obj));
	}
	if (RCLASS_IV_INDEX_TBL(obj)) {
	    st_free_table(RCLASS_IV_INDEX_TBL(obj));
	}
	if (RCLASS_EXT(obj)->subclasses) {
	    if (BUILTIN_TYPE(obj) == T_MODULE) {
		rb_class_detach_module_subclasses(obj);
	    }
	    else {
		rb_class_detach_subclasses(obj);
	    }
	    RCLASS_EXT(obj)->subclasses = NULL;
	}
	rb_class_remove_from_module_subclasses(obj);
	rb_class_remove_from_super_subclasses(obj);
	if (RANY(obj)->as.klass.ptr)
	    xfree(RANY(obj)->as.klass.ptr);
	RANY(obj)->as.klass.ptr = NULL;
	break;
      case T_STRING:
	rb_str_free(obj);
	break;
      case T_ARRAY:
	rb_ary_free(obj);
	break;
      case T_HASH:
	if (RANY(obj)->as.hash.ntbl) {
	    st_free_table(RANY(obj)->as.hash.ntbl);
	}
	break;
      case T_REGEXP:
	if (RANY(obj)->as.regexp.ptr) {
	    onig_free(RANY(obj)->as.regexp.ptr);
	}
	break;
      case T_DATA:
	if (DATA_PTR(obj)) {
	    int free_immediately = FALSE;
	    void (*dfree)(void *);
	    void *data = DATA_PTR(obj);

	    if (RTYPEDDATA_P(obj)) {
		free_immediately = (RANY(obj)->as.typeddata.type->flags & RUBY_TYPED_FREE_IMMEDIATELY) != 0;
		dfree = RANY(obj)->as.typeddata.type->function.dfree;
		if (0 && free_immediately == 0) {
		    /* to expose non-free-immediate T_DATA */
		    fprintf(stderr, "not immediate -> %s\n", RANY(obj)->as.typeddata.type->wrap_struct_name);
		}
	    }
	    else {
		dfree = RANY(obj)->as.data.dfree;
	    }

	    if (dfree) {
		if (dfree == RUBY_DEFAULT_FREE) {
		    xfree(data);
		}
		else if (free_immediately) {
		    (*dfree)(data);
		}
		else {
		    make_zombie(objspace, obj, dfree, data);
		    return 1;
		}
	    }
	}
	break;
      case T_MATCH:
	if (RANY(obj)->as.match.rmatch) {
            struct rmatch *rm = RANY(obj)->as.match.rmatch;
	    onig_region_free(&rm->regs, 0);
            if (rm->char_offset)
		xfree(rm->char_offset);
	    xfree(rm);
	}
	break;
      case T_FILE:
	if (RANY(obj)->as.file.fptr) {
	    make_io_zombie(objspace, obj);
	    return 1;
	}
	break;
      case T_RATIONAL:
      case T_COMPLEX:
	break;
      case T_ICLASS:
	/* Basically , T_ICLASS shares table with the module */
	if (FL_TEST(obj, RICLASS_IS_ORIGIN)) {
	    rb_id_table_free(RCLASS_M_TBL(obj));
	}
	if (RCLASS_CALLABLE_M_TBL(obj) != NULL) {
	    rb_id_table_free(RCLASS_CALLABLE_M_TBL(obj));
	}
	if (RCLASS_EXT(obj)->subclasses) {
	    rb_class_detach_subclasses(obj);
	    RCLASS_EXT(obj)->subclasses = NULL;
	}
	rb_class_remove_from_module_subclasses(obj);
	rb_class_remove_from_super_subclasses(obj);
	xfree(RANY(obj)->as.klass.ptr);
	RANY(obj)->as.klass.ptr = NULL;
	break;

      case T_FLOAT:
	break;

      case T_BIGNUM:
	if (!(RBASIC(obj)->flags & BIGNUM_EMBED_FLAG) && BIGNUM_DIGITS(obj)) {
	    xfree(BIGNUM_DIGITS(obj));
	}
	break;

      case T_NODE:
	rb_gc_free_node(obj);
	break;			/* no need to free iv_tbl */

      case T_STRUCT:
	if ((RBASIC(obj)->flags & RSTRUCT_EMBED_LEN_MASK) == 0 &&
	    RANY(obj)->as.rstruct.as.heap.ptr) {
	    xfree((void *)RANY(obj)->as.rstruct.as.heap.ptr);
	}
	break;

      case T_SYMBOL:
	{
            rb_gc_free_dsymbol(obj);
	}
	break;

      case T_IMEMO:
	{
	    switch (imemo_type(obj)) {
	      case imemo_ment:
		rb_free_method_entry(&RANY(obj)->as.imemo.ment);
		break;
	      case imemo_iseq:
		rb_iseq_free(&RANY(obj)->as.imemo.iseq);
		break;
	      default:
		break;
	    }
	}
	return 0;

      default:
	rb_bug("gc_sweep(): unknown data type 0x%x(%p) 0x%"PRIxVALUE,
	       BUILTIN_TYPE(obj), (void*)obj, RBASIC(obj)->flags);
    }

    if (FL_TEST(obj, FL_FINALIZE)) {
	make_zombie(objspace, obj, 0, 0);
	return 1;
    }
    else {
	return 0;
    }
}

void
Init_heap(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    gc_stress_set(objspace, ruby_initial_gc_stress);

#if RGENGC_ESTIMATE_OLDMALLOC
    objspace->rgengc.oldmalloc_increase_limit = gc_params.oldmalloc_limit_min;
#endif

    heap_add_pages(objspace, heap_eden, gc_params.heap_init_slots / HEAP_OBJ_LIMIT);
    init_mark_stack(&objspace->mark_stack);

#ifdef USE_SIGALTSTACK
    {
	/* altstack of another threads are allocated in another place */
	rb_thread_t *th = GET_THREAD();
	void *tmp = th->altstack;
	th->altstack = malloc(rb_sigaltstack_size());
	free(tmp); /* free previously allocated area */
    }
#endif

    objspace->profile.invoke_time = getrusage_time();
    finalizer_table = st_init_numtable();
}

typedef int each_obj_callback(void *, void *, size_t, void *);

struct each_obj_args {
    each_obj_callback *callback;
    void *data;
};

static VALUE
objspace_each_objects(VALUE arg)
{
    size_t i;
    struct heap_page_body *last_body = 0;
    struct heap_page *page;
    RVALUE *pstart, *pend;
    rb_objspace_t *objspace = &rb_objspace;
    struct each_obj_args *args = (struct each_obj_args *)arg;

    i = 0;
    while (i < heap_allocated_pages) {
	while (0 < i && last_body < heap_pages_sorted[i-1]->body)              i--;
	while (i < heap_allocated_pages && heap_pages_sorted[i]->body <= last_body) i++;
	if (heap_allocated_pages <= i) break;

	page = heap_pages_sorted[i];
	last_body = page->body;

	pstart = page->start;
	pend = pstart + page->total_slots;

	if ((*args->callback)(pstart, pend, sizeof(RVALUE), args->data)) {
	    break;
	}
    }

    return Qnil;
}

static VALUE
incremental_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    objspace->flags.dont_incremental = FALSE;
    return Qnil;
}

/*
 * rb_objspace_each_objects() is special C API to walk through
 * Ruby object space.  This C API is too difficult to use it.
 * To be frank, you should not use it. Or you need to read the
 * source code of this function and understand what this function does.
 *
 * 'callback' will be called several times (the number of heap page,
 * at current implementation) with:
 *   vstart: a pointer to the first living object of the heap_page.
 *   vend: a pointer to next to the valid heap_page area.
 *   stride: a distance to next VALUE.
 *
 * If callback() returns non-zero, the iteration will be stopped.
 *
 * This is a sample callback code to iterate liveness objects:
 *
 *   int
 *   sample_callback(void *vstart, void *vend, int stride, void *data) {
 *     VALUE v = (VALUE)vstart;
 *     for (; v != (VALUE)vend; v += stride) {
 *       if (RBASIC(v)->flags) { // liveness check
 *       // do something with live object 'v'
 *     }
 *     return 0; // continue to iteration
 *   }
 *
 * Note: 'vstart' is not a top of heap_page.  This point the first
 *       living object to grasp at least one object to avoid GC issue.
 *       This means that you can not walk through all Ruby object page
 *       including freed object page.
 *
 * Note: On this implementation, 'stride' is same as sizeof(RVALUE).
 *       However, there are possibilities to pass variable values with
 *       'stride' with some reasons.  You must use stride instead of
 *       use some constant value in the iteration.
 */
void
rb_objspace_each_objects(each_obj_callback *callback, void *data)
{
    struct each_obj_args args;
    rb_objspace_t *objspace = &rb_objspace;
    int prev_dont_incremental = objspace->flags.dont_incremental;

    gc_rest(objspace);
    objspace->flags.dont_incremental = TRUE;

    args.callback = callback;
    args.data = data;

    if (prev_dont_incremental) {
	objspace_each_objects((VALUE)&args);
    }
    else {
	rb_ensure(objspace_each_objects, (VALUE)&args, incremental_enable, Qnil);
    }
}

void
rb_objspace_each_objects_without_setup(each_obj_callback *callback, void *data)
{
    struct each_obj_args args;
    args.callback = callback;
    args.data = data;

    objspace_each_objects((VALUE)&args);
}

struct os_each_struct {
    size_t num;
    VALUE of;
};

static int
internal_object_p(VALUE obj)
{
    RVALUE *p = (RVALUE *)obj;

    if (p->as.basic.flags) {
	switch (BUILTIN_TYPE(p)) {
	  case T_NONE:
	  case T_IMEMO:
	  case T_ICLASS:
	  case T_NODE:
	  case T_ZOMBIE:
	    break;
	  default:
	    if (!p->as.basic.klass) break;
	    return 0;
	}
    }
    return 1;
}

int
rb_objspace_internal_object_p(VALUE obj)
{
    return internal_object_p(obj);
}

static int
os_obj_of_i(void *vstart, void *vend, size_t stride, void *data)
{
    struct os_each_struct *oes = (struct os_each_struct *)data;
    RVALUE *p = (RVALUE *)vstart, *pend = (RVALUE *)vend;

    for (; p != pend; p++) {
	volatile VALUE v = (VALUE)p;
	if (!internal_object_p(v)) {
	    if (!oes->of || rb_obj_is_kind_of(v, oes->of)) {
		rb_yield(v);
		oes->num++;
	    }
	}
    }

    return 0;
}

static VALUE
os_obj_of(VALUE of)
{
    struct os_each_struct oes;

    oes.num = 0;
    oes.of = of;
    rb_objspace_each_objects(os_obj_of_i, &oes);
    return SIZET2NUM(oes.num);
}

/*
 *  call-seq:
 *     ObjectSpace.each_object([module]) {|obj| ... } -> fixnum
 *     ObjectSpace.each_object([module])              -> an_enumerator
 *
 *  Calls the block once for each living, nonimmediate object in this
 *  Ruby process. If <i>module</i> is specified, calls the block
 *  for only those classes or modules that match (or are a subclass of)
 *  <i>module</i>. Returns the number of objects found. Immediate
 *  objects (<code>Fixnum</code>s, <code>Symbol</code>s
 *  <code>true</code>, <code>false</code>, and <code>nil</code>) are
 *  never returned. In the example below, <code>each_object</code>
 *  returns both the numbers we defined and several constants defined in
 *  the <code>Math</code> module.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     a = 102.7
 *     b = 95       # Won't be returned
 *     c = 12345678987654321
 *     count = ObjectSpace.each_object(Numeric) {|x| p x }
 *     puts "Total count: #{count}"
 *
 *  <em>produces:</em>
 *
 *     12345678987654321
 *     102.7
 *     2.71828182845905
 *     3.14159265358979
 *     2.22044604925031e-16
 *     1.7976931348623157e+308
 *     2.2250738585072e-308
 *     Total count: 7
 *
 */

static VALUE
os_each_obj(int argc, VALUE *argv, VALUE os)
{
    VALUE of;

    if (argc == 0) {
	of = 0;
    }
    else {
	rb_scan_args(argc, argv, "01", &of);
    }
    RETURN_ENUMERATOR(os, 1, &of);
    return os_obj_of(of);
}

/*
 *  call-seq:
 *     ObjectSpace.undefine_finalizer(obj)
 *
 *  Removes all finalizers for <i>obj</i>.
 *
 */

static VALUE
undefine_final(VALUE os, VALUE obj)
{
    return rb_undefine_finalizer(obj);
}

VALUE
rb_undefine_finalizer(VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;
    st_data_t data = obj;
    rb_check_frozen(obj);
    st_delete(finalizer_table, &data, 0);
    FL_UNSET(obj, FL_FINALIZE);
    return obj;
}

static void
should_be_callable(VALUE block)
{
    if (!rb_obj_respond_to(block, rb_intern("call"), TRUE)) {
	rb_raise(rb_eArgError, "wrong type argument %"PRIsVALUE" (should be callable)",
		 rb_obj_class(block));
    }
}
static void
should_be_finalizable(VALUE obj)
{
    if (!FL_ABLE(obj)) {
	rb_raise(rb_eArgError, "cannot define finalizer for %s",
		 rb_obj_classname(obj));
    }
    rb_check_frozen(obj);
}

/*
 *  call-seq:
 *     ObjectSpace.define_finalizer(obj, aProc=proc())
 *
 *  Adds <i>aProc</i> as a finalizer, to be called after <i>obj</i>
 *  was destroyed. The object ID of the <i>obj</i> will be passed
 *  as an argument to <i>aProc</i>. If <i>aProc</i> is a lambda or
 *  method, make sure it can be called with a single argument.
 *
 */

static VALUE
define_final(int argc, VALUE *argv, VALUE os)
{
    VALUE obj, block;

    rb_scan_args(argc, argv, "11", &obj, &block);
    should_be_finalizable(obj);
    if (argc == 1) {
	block = rb_block_proc();
    }
    else {
	should_be_callable(block);
    }

    return define_final0(obj, block);
}

static VALUE
define_final0(VALUE obj, VALUE block)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE table;
    st_data_t data;

    RBASIC(obj)->flags |= FL_FINALIZE;

    block = rb_ary_new3(2, INT2FIX(rb_safe_level()), block);
    OBJ_FREEZE(block);

    if (st_lookup(finalizer_table, obj, &data)) {
	table = (VALUE)data;

	/* avoid duplicate block, table is usually small */
	{
	    const VALUE *ptr = RARRAY_CONST_PTR(table);
	    long len = RARRAY_LEN(table);
	    long i;

	    for (i = 0; i < len; i++, ptr++) {
		if (rb_funcall(*ptr, idEq, 1, block)) {
		    return *ptr;
		}
	    }
	}

	rb_ary_push(table, block);
    }
    else {
	table = rb_ary_new3(1, block);
	RBASIC_CLEAR_CLASS(table);
	st_add_direct(finalizer_table, obj, table);
    }
    return block;
}

VALUE
rb_define_finalizer(VALUE obj, VALUE block)
{
    should_be_finalizable(obj);
    should_be_callable(block);
    return define_final0(obj, block);
}

void
rb_gc_copy_finalizer(VALUE dest, VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE table;
    st_data_t data;

    if (!FL_TEST(obj, FL_FINALIZE)) return;
    if (st_lookup(finalizer_table, obj, &data)) {
	table = (VALUE)data;
	st_insert(finalizer_table, dest, table);
    }
    FL_SET(dest, FL_FINALIZE);
}

static VALUE
run_single_final(VALUE arg)
{
    VALUE *args = (VALUE *)arg;

    return rb_check_funcall(args[0], idCall, 1, args+1);
}

static void
run_finalizer(rb_objspace_t *objspace, VALUE obj, VALUE table)
{
    long i;
    VALUE args[2];
    const int safe = rb_safe_level();
    const VALUE errinfo = rb_errinfo();

    args[1] = nonspecial_obj_id(obj);

    for (i=0; i<RARRAY_LEN(table); i++) {
	const VALUE final = RARRAY_AREF(table, i);
	const VALUE cmd = RARRAY_AREF(final, 1);
	const int level = OBJ_TAINTED(cmd) ?
	    RUBY_SAFE_LEVEL_MAX : FIX2INT(RARRAY_AREF(final, 0));
	int status = 0;

	args[0] = cmd;
	rb_set_safe_level_force(level);
	rb_protect(run_single_final, (VALUE)args, &status);
	rb_set_safe_level_force(safe);
	rb_set_errinfo(errinfo);
    }
}

static void
run_final(rb_objspace_t *objspace, VALUE zombie)
{
    st_data_t key, table;

    if (RZOMBIE(zombie)->dfree) {
	RZOMBIE(zombie)->dfree(RZOMBIE(zombie)->data);
    }

    key = (st_data_t)zombie;
    if (st_delete(finalizer_table, &key, &table)) {
	run_finalizer(objspace, zombie, (VALUE)table);
    }
}

static void
finalize_list(rb_objspace_t *objspace, VALUE zombie)
{
    while (zombie) {
	VALUE next_zombie = RZOMBIE(zombie)->next;
	struct heap_page *page = GET_HEAP_PAGE(zombie);

	run_final(objspace, zombie);

	RZOMBIE(zombie)->basic.flags = 0;
	heap_pages_final_slots--;
	page->final_slots--;
	page->free_slots++;
	heap_page_add_freeobj(objspace, GET_HEAP_PAGE(zombie), zombie);

	heap_pages_swept_slots++;
	objspace->profile.total_freed_objects++;

	zombie = next_zombie;
    }
}

static void
finalize_deferred(rb_objspace_t *objspace)
{
    VALUE zombie;

    while ((zombie = ATOMIC_VALUE_EXCHANGE(heap_pages_deferred_final, 0)) != 0) {
	finalize_list(objspace, zombie);
    }
}

static void
gc_finalize_deferred(void *dmy)
{
    rb_objspace_t *objspace = &rb_objspace;
    if (ATOMIC_EXCHANGE(finalizing, 1)) return;
    finalize_deferred(objspace);
    ATOMIC_SET(finalizing, 0);
}

/* TODO: to keep compatibility, maybe unused. */
void
rb_gc_finalize_deferred(void)
{
    gc_finalize_deferred(0);
}

static void
gc_finalize_deferred_register(void)
{
    if (rb_postponed_job_register_one(0, gc_finalize_deferred, 0) == 0) {
	rb_bug("gc_finalize_deferred_register: can't register finalizer.");
    }
}

struct force_finalize_list {
    VALUE obj;
    VALUE table;
    struct force_finalize_list *next;
};

static int
force_chain_object(st_data_t key, st_data_t val, st_data_t arg)
{
    struct force_finalize_list **prev = (struct force_finalize_list **)arg;
    struct force_finalize_list *curr = ALLOC(struct force_finalize_list);
    curr->obj = key;
    curr->table = val;
    curr->next = *prev;
    *prev = curr;
    return ST_CONTINUE;
}

void
rb_gc_call_finalizer_at_exit(void)
{
#if RGENGC_CHECK_MODE >= 2
    gc_verify_internal_consistency(Qnil);
#endif
    rb_objspace_call_finalizer(&rb_objspace);
}

static void
rb_objspace_call_finalizer(rb_objspace_t *objspace)
{
    RVALUE *p, *pend;
    size_t i;

    gc_rest(objspace);

    if (ATOMIC_EXCHANGE(finalizing, 1)) return;

    /* run finalizers */
    finalize_deferred(objspace);
    assert(heap_pages_deferred_final == 0);

    gc_rest(objspace);
    /* prohibit incremental GC */
    objspace->flags.dont_incremental = 1;

    /* force to run finalizer */
    while (finalizer_table->num_entries) {
	struct force_finalize_list *list = 0;
	st_foreach(finalizer_table, force_chain_object, (st_data_t)&list);
	while (list) {
	    struct force_finalize_list *curr = list;
	    st_data_t obj = (st_data_t)curr->obj;
	    run_finalizer(objspace, curr->obj, curr->table);
	    st_delete(finalizer_table, &obj, 0);
	    list = curr->next;
	    xfree(curr);
	}
    }

    /* prohibit GC because force T_DATA finalizers can break an object graph consistency */
    dont_gc = 1;

    /* running data/file finalizers are part of garbage collection */
    gc_enter(objspace, "rb_objspace_call_finalizer");

    /* run data/file object's finalizers */
    for (i = 0; i < heap_allocated_pages; i++) {
	p = heap_pages_sorted[i]->start; pend = p + heap_pages_sorted[i]->total_slots;
	while (p < pend) {
	    switch (BUILTIN_TYPE(p)) {
	      case T_DATA:
		if (!DATA_PTR(p) || !RANY(p)->as.data.dfree) break;
		if (rb_obj_is_thread((VALUE)p)) break;
		if (rb_obj_is_mutex((VALUE)p)) break;
		if (rb_obj_is_fiber((VALUE)p)) break;
		p->as.free.flags = 0;
		if (RTYPEDDATA_P(p)) {
		    RDATA(p)->dfree = RANY(p)->as.typeddata.type->function.dfree;
		}
		if (RANY(p)->as.data.dfree == (RUBY_DATA_FUNC)-1) {
		    xfree(DATA_PTR(p));
		}
		else if (RANY(p)->as.data.dfree) {
		    make_zombie(objspace, (VALUE)p, RANY(p)->as.data.dfree, RANY(p)->as.data.data);
		}
		break;
	      case T_FILE:
		if (RANY(p)->as.file.fptr) {
		    make_io_zombie(objspace, (VALUE)p);
		}
		break;
	    }
	    p++;
	}
    }

    gc_exit(objspace, "rb_objspace_call_finalizer");

    if (heap_pages_deferred_final) {
	finalize_list(objspace, heap_pages_deferred_final);
    }

    st_free_table(finalizer_table);
    finalizer_table = 0;
    ATOMIC_SET(finalizing, 0);
}

static inline int
is_id_value(rb_objspace_t *objspace, VALUE ptr)
{
    if (!is_pointer_to_heap(objspace, (void *)ptr)) return FALSE;
    if (BUILTIN_TYPE(ptr) > T_FIXNUM) return FALSE;
    if (BUILTIN_TYPE(ptr) == T_ICLASS) return FALSE;
    return TRUE;
}

static inline int
heap_is_swept_object(rb_objspace_t *objspace, rb_heap_t *heap, VALUE ptr)
{
    struct heap_page *page = GET_HEAP_PAGE(ptr);
    return page->flags.before_sweep ? FALSE : TRUE;
}

static inline int
is_swept_object(rb_objspace_t *objspace, VALUE ptr)
{
    if (heap_is_swept_object(objspace, heap_eden, ptr)) {
	return TRUE;
    }
    else {
	return FALSE;
    }
}

/* garbage objects will be collected soon. */
static inline int
is_garbage_object(rb_objspace_t *objspace, VALUE ptr)
{
    if (!is_lazy_sweeping(heap_eden) ||
	is_swept_object(objspace, ptr) ||
	MARKED_IN_BITMAP(GET_HEAP_MARK_BITS(ptr), ptr)) {

	return FALSE;
    }
    else {
	return TRUE;
    }
}

static inline int
is_live_object(rb_objspace_t *objspace, VALUE ptr)
{
    switch (BUILTIN_TYPE(ptr)) {
      case T_NONE:
      case T_ZOMBIE:
	return FALSE;
    }

    if (!is_garbage_object(objspace, ptr)) {
	return TRUE;
    }
    else {
	return FALSE;
    }
}

static inline int
is_markable_object(rb_objspace_t *objspace, VALUE obj)
{
    if (rb_special_const_p(obj)) return FALSE; /* special const is not markable */
    check_rvalue_consistency(obj);
    return TRUE;
}

int
rb_objspace_markable_object_p(VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;
    return is_markable_object(objspace, obj) && is_live_object(objspace, obj);
}

int
rb_objspace_garbage_object_p(VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;
    return is_garbage_object(objspace, obj);
}

/*
 *  call-seq:
 *     ObjectSpace._id2ref(object_id) -> an_object
 *
 *  Converts an object id to a reference to the object. May not be
 *  called on an object id passed as a parameter to a finalizer.
 *
 *     s = "I am a string"                    #=> "I am a string"
 *     r = ObjectSpace._id2ref(s.object_id)   #=> "I am a string"
 *     r == s                                 #=> true
 *
 */

static VALUE
id2ref(VALUE obj, VALUE objid)
{
#if SIZEOF_LONG == SIZEOF_VOIDP
#define NUM2PTR(x) NUM2ULONG(x)
#elif SIZEOF_LONG_LONG == SIZEOF_VOIDP
#define NUM2PTR(x) NUM2ULL(x)
#endif
    rb_objspace_t *objspace = &rb_objspace;
    VALUE ptr;
    void *p0;

    ptr = NUM2PTR(objid);
    p0 = (void *)ptr;

    if (ptr == Qtrue) return Qtrue;
    if (ptr == Qfalse) return Qfalse;
    if (ptr == Qnil) return Qnil;
    if (FIXNUM_P(ptr)) return (VALUE)ptr;
    if (FLONUM_P(ptr)) return (VALUE)ptr;
    ptr = obj_id_to_ref(objid);

    if ((ptr % sizeof(RVALUE)) == (4 << 2)) {
        ID symid = ptr / sizeof(RVALUE);
        if (rb_id2str(symid) == 0)
	    rb_raise(rb_eRangeError, "%p is not symbol id value", p0);
	return ID2SYM(symid);
    }

    if (!is_id_value(objspace, ptr)) {
	rb_raise(rb_eRangeError, "%p is not id value", p0);
    }
    if (!is_live_object(objspace, ptr)) {
	rb_raise(rb_eRangeError, "%p is recycled object", p0);
    }
    if (RBASIC(ptr)->klass == 0) {
	rb_raise(rb_eRangeError, "%p is internal object", p0);
    }
    return (VALUE)ptr;
}

/*
 *  Document-method: __id__
 *  Document-method: object_id
 *
 *  call-seq:
 *     obj.__id__       -> integer
 *     obj.object_id    -> integer
 *
 *  Returns an integer identifier for +obj+.
 *
 *  The same number will be returned on all calls to +object_id+ for a given
 *  object, and no two active objects will share an id.
 *
 *  Note: that some objects of builtin classes are reused for optimization.
 *  This is the case for immediate values and frozen string literals.
 *
 *  Immediate values are not passed by reference but are passed by value:
 *  +nil+, +true+, +false+, Fixnums, Symbols, and some Floats.
 *
 *      Object.new.object_id  == Object.new.object_id  # => false
 *      (21 * 2).object_id    == (21 * 2).object_id    # => true
 *      "hello".object_id     == "hello".object_id     # => false
 *      "hi".freeze.object_id == "hi".freeze.object_id # => true
 */

VALUE
rb_obj_id(VALUE obj)
{
    /*
     *                32-bit VALUE space
     *          MSB ------------------------ LSB
     *  false   00000000000000000000000000000000
     *  true    00000000000000000000000000000010
     *  nil     00000000000000000000000000000100
     *  undef   00000000000000000000000000000110
     *  symbol  ssssssssssssssssssssssss00001110
     *  object  oooooooooooooooooooooooooooooo00        = 0 (mod sizeof(RVALUE))
     *  fixnum  fffffffffffffffffffffffffffffff1
     *
     *                    object_id space
     *                                       LSB
     *  false   00000000000000000000000000000000
     *  true    00000000000000000000000000000010
     *  nil     00000000000000000000000000000100
     *  undef   00000000000000000000000000000110
     *  symbol   000SSSSSSSSSSSSSSSSSSSSSSSSSSS0        S...S % A = 4 (S...S = s...s * A + 4)
     *  object   oooooooooooooooooooooooooooooo0        o...o % A = 0
     *  fixnum  fffffffffffffffffffffffffffffff1        bignum if required
     *
     *  where A = sizeof(RVALUE)/4
     *
     *  sizeof(RVALUE) is
     *  20 if 32-bit, double is 4-byte aligned
     *  24 if 32-bit, double is 8-byte aligned
     *  40 if 64-bit
     */
    if (STATIC_SYM_P(obj)) {
        return (SYM2ID(obj) * sizeof(RVALUE) + (4 << 2)) | FIXNUM_FLAG;
    }
    else if (FLONUM_P(obj)) {
#if SIZEOF_LONG == SIZEOF_VOIDP
	return LONG2NUM((SIGNED_VALUE)obj);
#else
	return LL2NUM((SIGNED_VALUE)obj);
#endif
    }
    else if (SPECIAL_CONST_P(obj)) {
	return LONG2NUM((SIGNED_VALUE)obj);
    }
    return nonspecial_obj_id(obj);
}

#include "regint.h"

static size_t
obj_memsize_of(VALUE obj, int use_all_types)
{
    size_t size = 0;

    if (SPECIAL_CONST_P(obj)) {
	return 0;
    }

    if (FL_TEST(obj, FL_EXIVAR)) {
	size += rb_generic_ivar_memsize(obj);
    }

    switch (BUILTIN_TYPE(obj)) {
      case T_OBJECT:
	if (!(RBASIC(obj)->flags & ROBJECT_EMBED) &&
	    ROBJECT(obj)->as.heap.ivptr) {
	    size += ROBJECT(obj)->as.heap.numiv * sizeof(VALUE);
	}
	break;
      case T_MODULE:
      case T_CLASS:
	if (RCLASS_M_TBL(obj)) {
	    size += rb_id_table_memsize(RCLASS_M_TBL(obj));
	}
	if (RCLASS_EXT(obj)) {
	    if (RCLASS_IV_TBL(obj)) {
		size += st_memsize(RCLASS_IV_TBL(obj));
	    }
	    if (RCLASS_IV_INDEX_TBL(obj)) {
		size += st_memsize(RCLASS_IV_INDEX_TBL(obj));
	    }
	    if (RCLASS(obj)->ptr->iv_tbl) {
		size += st_memsize(RCLASS(obj)->ptr->iv_tbl);
	    }
	    if (RCLASS(obj)->ptr->const_tbl) {
		size += st_memsize(RCLASS(obj)->ptr->const_tbl);
	    }
	    size += sizeof(rb_classext_t);
	}
	break;
      case T_ICLASS:
	if (FL_TEST(obj, RICLASS_IS_ORIGIN)) {
	    if (RCLASS_M_TBL(obj)) {
		size += rb_id_table_memsize(RCLASS_M_TBL(obj));
	    }
	}
	break;
      case T_STRING:
	size += rb_str_memsize(obj);
	break;
      case T_ARRAY:
	size += rb_ary_memsize(obj);
	break;
      case T_HASH:
	if (RHASH(obj)->ntbl) {
	    size += st_memsize(RHASH(obj)->ntbl);
	}
	break;
      case T_REGEXP:
	if (RREGEXP(obj)->ptr) {
	    size += onig_memsize(RREGEXP(obj)->ptr);
	}
	break;
      case T_DATA:
	if (use_all_types) size += rb_objspace_data_type_memsize(obj);
	break;
      case T_MATCH:
	if (RMATCH(obj)->rmatch) {
            struct rmatch *rm = RMATCH(obj)->rmatch;
	    size += onig_region_memsize(&rm->regs);
	    size += sizeof(struct rmatch_offset) * rm->char_offset_num_allocated;
	    size += sizeof(struct rmatch);
	}
	break;
      case T_FILE:
	if (RFILE(obj)->fptr) {
	    size += rb_io_memsize(RFILE(obj)->fptr);
	}
	break;
      case T_RATIONAL:
      case T_COMPLEX:
      case T_IMEMO:
	break;

      case T_FLOAT:
      case T_SYMBOL:
	break;

      case T_BIGNUM:
	if (!(RBASIC(obj)->flags & BIGNUM_EMBED_FLAG) && BIGNUM_DIGITS(obj)) {
	    size += BIGNUM_LEN(obj) * sizeof(BDIGIT);
	}
	break;

      case T_NODE:
	if (use_all_types) size += rb_node_memsize(obj);
	break;

      case T_STRUCT:
	if ((RBASIC(obj)->flags & RSTRUCT_EMBED_LEN_MASK) == 0 &&
	    RSTRUCT(obj)->as.heap.ptr) {
	    size += sizeof(VALUE) * RSTRUCT_LEN(obj);
	}
	break;

      case T_ZOMBIE:
	break;

      default:
	rb_bug("objspace/memsize_of(): unknown data type 0x%x(%p)",
	       BUILTIN_TYPE(obj), (void*)obj);
    }

    return size + sizeof(RVALUE);
}

size_t
rb_obj_memsize_of(VALUE obj)
{
    return obj_memsize_of(obj, TRUE);
}

static int
set_zero(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE k = (VALUE)key;
    VALUE hash = (VALUE)arg;
    rb_hash_aset(hash, k, INT2FIX(0));
    return ST_CONTINUE;
}

/*
 *  call-seq:
 *     ObjectSpace.count_objects([result_hash]) -> hash
 *
 *  Counts all objects grouped by type.
 *
 *  It returns a hash, such as:
 *	{
 *	  :TOTAL=>10000,
 *	  :FREE=>3011,
 *	  :T_OBJECT=>6,
 *	  :T_CLASS=>404,
 *	  # ...
 *	}
 *
 *  The contents of the returned hash are implementation specific.
 *  It may be changed in future.
 *
 *  The keys starting with +:T_+ means live objects.
 *  For example, +:T_ARRAY+ is the number of arrays.
 *  +:FREE+ means object slots which is not used now.
 *  +:TOTAL+ means sum of above.
 *
 *  If the optional argument +result_hash+ is given,
 *  it is overwritten and returned. This is intended to avoid probe effect.
 *
 *    h = {}
 *    ObjectSpace.count_objects(h)
 *    puts h
 *    # => { :TOTAL=>10000, :T_CLASS=>158280, :T_MODULE=>20672, :T_STRING=>527249 }
 *
 *  This method is only expected to work on C Ruby.
 *
 */

static VALUE
count_objects(int argc, VALUE *argv, VALUE os)
{
    rb_objspace_t *objspace = &rb_objspace;
    size_t counts[T_MASK+1];
    size_t freed = 0;
    size_t total = 0;
    size_t i;
    VALUE hash;

    if (rb_scan_args(argc, argv, "01", &hash) == 1) {
        if (!RB_TYPE_P(hash, T_HASH))
            rb_raise(rb_eTypeError, "non-hash given");
    }

    for (i = 0; i <= T_MASK; i++) {
        counts[i] = 0;
    }

    for (i = 0; i < heap_allocated_pages; i++) {
	struct heap_page *page = heap_pages_sorted[i];
	RVALUE *p, *pend;

	p = page->start; pend = p + page->total_slots;
	for (;p < pend; p++) {
	    if (p->as.basic.flags) {
		counts[BUILTIN_TYPE(p)]++;
	    }
	    else {
		freed++;
	    }
	}
	total += page->total_slots;
    }

    if (hash == Qnil) {
        hash = rb_hash_new();
    }
    else if (!RHASH_EMPTY_P(hash)) {
        st_foreach(RHASH_TBL_RAW(hash), set_zero, hash);
    }
    rb_hash_aset(hash, ID2SYM(rb_intern("TOTAL")), SIZET2NUM(total));
    rb_hash_aset(hash, ID2SYM(rb_intern("FREE")), SIZET2NUM(freed));

    for (i = 0; i <= T_MASK; i++) {
        VALUE type;
        switch (i) {
#define COUNT_TYPE(t) case (t): type = ID2SYM(rb_intern(#t)); break;
	    COUNT_TYPE(T_NONE);
	    COUNT_TYPE(T_OBJECT);
	    COUNT_TYPE(T_CLASS);
	    COUNT_TYPE(T_MODULE);
	    COUNT_TYPE(T_FLOAT);
	    COUNT_TYPE(T_STRING);
	    COUNT_TYPE(T_REGEXP);
	    COUNT_TYPE(T_ARRAY);
	    COUNT_TYPE(T_HASH);
	    COUNT_TYPE(T_STRUCT);
	    COUNT_TYPE(T_BIGNUM);
	    COUNT_TYPE(T_FILE);
	    COUNT_TYPE(T_DATA);
	    COUNT_TYPE(T_MATCH);
	    COUNT_TYPE(T_COMPLEX);
	    COUNT_TYPE(T_RATIONAL);
	    COUNT_TYPE(T_NIL);
	    COUNT_TYPE(T_TRUE);
	    COUNT_TYPE(T_FALSE);
	    COUNT_TYPE(T_SYMBOL);
	    COUNT_TYPE(T_FIXNUM);
	    COUNT_TYPE(T_IMEMO);
	    COUNT_TYPE(T_UNDEF);
	    COUNT_TYPE(T_NODE);
	    COUNT_TYPE(T_ICLASS);
	    COUNT_TYPE(T_ZOMBIE);
#undef COUNT_TYPE
          default:              type = INT2NUM(i); break;
        }
        if (counts[i])
            rb_hash_aset(hash, type, SIZET2NUM(counts[i]));
    }

    return hash;
}

/*
  ------------------------ Garbage Collection ------------------------
*/

/* Sweeping */

static size_t
objspace_available_slots(rb_objspace_t *objspace)
{
    return heap_eden->total_slots + heap_tomb->total_slots;
}

static size_t
objspace_live_slots(rb_objspace_t *objspace)
{
    return (objspace->total_allocated_objects - objspace->profile.total_freed_objects) - heap_pages_final_slots;
}

static size_t
objspace_free_slots(rb_objspace_t *objspace)
{
    return objspace_available_slots(objspace) - objspace_live_slots(objspace) - heap_pages_final_slots;
}

static void
gc_setup_mark_bits(struct heap_page *page)
{
#if USE_RGENGC
    /* copy oldgen bitmap to mark bitmap */
    memcpy(&page->mark_bits[0], &page->uncollectible_bits[0], HEAP_BITMAP_SIZE);
#else
    /* clear mark bitmap */
    memset(&page->mark_bits[0], 0, HEAP_BITMAP_SIZE);
#endif
}

/* TRUE : has empty slots                                             */
/* FALSE: no empty slots (or move to tomb heap because no live slots) */
static inline void
gc_page_sweep(rb_objspace_t *objspace, rb_heap_t *heap, struct heap_page *sweep_page)
{
    int i;
    int empty_slots = 0, freed_slots = 0, final_slots = 0;
    RVALUE *p, *pend,*offset;
    bits_t *bits, bitset;

    gc_report(2, objspace, "page_sweep: start.\n");

    sweep_page->flags.before_sweep = FALSE;

    p = sweep_page->start; pend = p + sweep_page->total_slots;
    offset = p - NUM_IN_PAGE(p);
    bits = sweep_page->mark_bits;

    /* create guard : fill 1 out-of-range */
    bits[BITMAP_INDEX(p)] |= BITMAP_BIT(p)-1;
    bits[BITMAP_INDEX(pend)] |= ~(BITMAP_BIT(pend) - 1);

    for (i=0; i < HEAP_BITMAP_LIMIT; i++) {
	bitset = ~bits[i];
	if (bitset) {
	    p = offset  + i * BITS_BITLENGTH;
	    do {
		if (bitset & 1) {
		    switch (BUILTIN_TYPE(p)) {
		      default: { /* majority case */
			  gc_report(2, objspace, "page_sweep: free %s\n", obj_info((VALUE)p));
#if USE_RGENGC && RGENGC_CHECK_MODE
			  if (!is_full_marking(objspace)) {
			      if (RVALUE_OLD_P((VALUE)p)) rb_bug("page_sweep: %s - old while minor GC.", obj_info((VALUE)p));
			      if (rgengc_remembered(objspace, (VALUE)p)) rb_bug("page_sweep: %s - remembered.", obj_info((VALUE)p));
			  }
#endif
			  if (obj_free(objspace, (VALUE)p)) {
			      final_slots++;
			  }
			  else {
			      (void)VALGRIND_MAKE_MEM_UNDEFINED((void*)p, sizeof(RVALUE));
			      heap_page_add_freeobj(objspace, sweep_page, (VALUE)p);
			      gc_report(3, objspace, "page_sweep: %s is added to freelist\n", obj_info((VALUE)p));
			      freed_slots++;
			  }
			  break;
		      }

			/* minor cases */
		      case T_ZOMBIE:
			/* already counted */
			break;
		      case T_NONE:
			empty_slots++; /* already freed */
			break;
		    }
		}
		p++;
		bitset >>= 1;
	    } while (bitset);
	}
    }

    gc_setup_mark_bits(sweep_page);

#if GC_PROFILE_MORE_DETAIL
    if (gc_prof_enabled(objspace)) {
	gc_profile_record *record = gc_prof_record(objspace);
	record->removing_objects += final_slots + freed_slots;
	record->empty_objects += empty_slots;
    }
#endif
    if (0) fprintf(stderr, "gc_page_sweep(%d): total_slots: %d, freed_slots: %d, empty_slots: %d, final_slots: %d\n",
		   (int)rb_gc_count(),
		   (int)sweep_page->total_slots,
		   freed_slots, empty_slots, final_slots);

    heap_pages_swept_slots += sweep_page->free_slots = freed_slots + empty_slots;
    objspace->profile.total_freed_objects += freed_slots;
    heap_pages_final_slots += final_slots;
    sweep_page->final_slots += final_slots;

    if (heap_pages_deferred_final && !finalizing) {
        rb_thread_t *th = GET_THREAD();
        if (th) {
	    gc_finalize_deferred_register();
        }
    }

    gc_report(2, objspace, "page_sweep: end.\n");
}

/* allocate additional minimum page to work */
static void
gc_heap_prepare_minimum_pages(rb_objspace_t *objspace, rb_heap_t *heap)
{
    if (!heap->free_pages && heap_increment(objspace, heap) == FALSE) {
	/* there is no free after page_sweep() */
	heap_set_increment(objspace, 1);
	if (!heap_increment(objspace, heap)) { /* can't allocate additional free objects */
	    rb_memerror();
	}
    }
}

static void
gc_stat_transition(rb_objspace_t *objspace, enum gc_stat stat)
{
#if RGENGC_CHECK_MODE
    enum gc_stat prev_stat = objspace->flags.stat;
    switch (prev_stat) {
      case gc_stat_none: assert(stat == gc_stat_marking); break;
      case gc_stat_marking: assert(stat == gc_stat_sweeping); break;
      case gc_stat_sweeping: assert(stat == gc_stat_none); break;
    }
#endif
    objspace->flags.stat = stat;
}

static void
gc_sweep_start_heap(rb_objspace_t *objspace, rb_heap_t *heap)
{
    heap->sweep_pages = heap->pages;
    heap->free_pages = NULL;
#if GC_ENABLE_INCREMENTAL_MARK
    heap->pooled_pages = NULL;
    objspace->rincgc.pooled_slots = 0;
#endif
    if (heap->using_page) {
	RVALUE **p = &heap->using_page->freelist;
	while (*p) {
	    p = &(*p)->as.free.next;
	}
	*p = heap->freelist;
	heap->using_page = NULL;
    }
    heap->freelist = NULL;
}

#if defined(__GNUC__) && __GNUC__ == 4 && __GNUC_MINOR__ == 4
__attribute__((noinline))
#endif
static void
gc_sweep_start(rb_objspace_t *objspace)
{
    rb_heap_t *heap;
    size_t total_limit_slot;

    gc_stat_transition(objspace, gc_stat_sweeping);

    /* sometimes heap_allocatable_pages is not 0 */
    heap_pages_swept_slots = heap_allocatable_pages * HEAP_OBJ_LIMIT;
    total_limit_slot = objspace_available_slots(objspace);

    heap_pages_min_free_slots = (size_t)(total_limit_slot * GC_HEAP_FREE_SLOTS_MIN_RATIO);
    if (heap_pages_min_free_slots < gc_params.heap_free_slots) {
	heap_pages_min_free_slots = gc_params.heap_free_slots;
    }
    heap_pages_max_free_slots = (size_t)(total_limit_slot * GC_HEAP_FREE_SLOTS_MAX_RATIO);
    if (heap_pages_max_free_slots < gc_params.heap_init_slots) {
	heap_pages_max_free_slots = gc_params.heap_init_slots;
    }
    if (0) fprintf(stderr, "heap_pages_min_free_slots: %d, heap_pages_max_free_slots: %d\n",
		   (int)heap_pages_min_free_slots, (int)heap_pages_max_free_slots);

    heap = heap_eden;
    gc_sweep_start_heap(objspace, heap);
}

static void
gc_sweep_finish(rb_objspace_t *objspace)
{
    rb_heap_t *heap = heap_eden;

    gc_report(1, objspace, "gc_sweep_finish: heap->total_slots: %d, heap->swept_slots: %d, min_free_slots: %d\n",
		  (int)heap->total_slots, (int)heap_pages_swept_slots, (int)heap_pages_min_free_slots);

    gc_prof_set_heap_info(objspace);

    heap_pages_free_unused_pages(objspace);

    /* if heap_pages has unused pages, then assign them to increment */
    if (heap_allocatable_pages < heap_tomb->page_length) {
	heap_allocatable_pages = heap_tomb->page_length;
    }

    gc_event_hook(objspace, RUBY_INTERNAL_EVENT_GC_END_SWEEP, 0);
    gc_stat_transition(objspace, gc_stat_none);

#if RGENGC_CHECK_MODE >= 2
    gc_verify_internal_consistency(Qnil);
#endif
}

static int
gc_sweep_step(rb_objspace_t *objspace, rb_heap_t *heap)
{
    struct heap_page *sweep_page = heap->sweep_pages, *next;
    int unlink_limit = 3;
#if GC_ENABLE_INCREMENTAL_MARK
    int need_pool = will_be_incremental_marking(objspace) ? TRUE : FALSE;

    gc_report(2, objspace, "gc_sweep_step (need_pool: %d)\n", need_pool);
#else
    gc_report(2, objspace, "gc_sweep_step\n");
#endif

    if (sweep_page == NULL) return FALSE;

#if GC_ENABLE_LAZY_SWEEP
    gc_prof_sweep_timer_start(objspace);
#endif

    while (sweep_page) {
	heap->sweep_pages = next = sweep_page->next;
	gc_page_sweep(objspace, heap, sweep_page);

	if (sweep_page->final_slots + sweep_page->free_slots == sweep_page->total_slots &&
	    unlink_limit > 0) {
	    unlink_limit--;
	    /* there are no living objects -> move this page to tomb heap */
	    heap_unlink_page(objspace, heap, sweep_page);
	    heap_add_page(objspace, heap_tomb, sweep_page);
	}
	else if (sweep_page->free_slots > 0) {
#if GC_ENABLE_INCREMENTAL_MARK
	    if (need_pool) {
		if (heap_add_poolpage(objspace, heap, sweep_page)) {
		    need_pool = FALSE;
		}
	    }
	    else {
		heap_add_freepage(objspace, heap, sweep_page);
		break;
	    }
#else
	    heap_add_freepage(objspace, heap, sweep_page);
	    break;
#endif
	}
	else {
	    sweep_page->free_next = NULL;
	}

	sweep_page = next;
    }

    if (heap->sweep_pages == NULL) {
	gc_sweep_finish(objspace);
    }

#if GC_ENABLE_LAZY_SWEEP
    gc_prof_sweep_timer_stop(objspace);
#endif

    return heap->free_pages != NULL;
}

static void
gc_sweep_rest(rb_objspace_t *objspace)
{
    rb_heap_t *heap = heap_eden; /* lazy sweep only for eden */

    while (has_sweeping_pages(heap)) {
	gc_sweep_step(objspace, heap);
    }
}

#if GC_ENABLE_LAZY_SWEEP
static void
gc_sweep_continue(rb_objspace_t *objspace, rb_heap_t *heap)
{
    if (RGENGC_CHECK_MODE) assert(dont_gc == FALSE);

    gc_enter(objspace, "sweep_continue");
#if USE_RGENGC
    if (objspace->rgengc.need_major_gc == GPR_FLAG_NONE && heap_increment(objspace, heap)) {
	gc_report(3, objspace, "gc_sweep_continue: success heap_increment().\n");
    }
#endif
    gc_sweep_step(objspace, heap);
    gc_exit(objspace, "sweep_continue");
}
#endif

static void
gc_sweep(rb_objspace_t *objspace)
{
    const unsigned int immediate_sweep = objspace->flags.immediate_sweep;

    gc_report(1, objspace, "gc_sweep: immediate: %d\n", immediate_sweep);

    if (immediate_sweep) {
#if !GC_ENABLE_LAZY_SWEEP
	gc_prof_sweep_timer_start(objspace);
#endif
	gc_sweep_start(objspace);
	gc_sweep_rest(objspace);
#if !GC_ENABLE_LAZY_SWEEP
	gc_prof_sweep_timer_stop(objspace);
#endif
    }
    else {
	struct heap_page *page;
	gc_sweep_start(objspace);
	page = heap_eden->sweep_pages;
	while (page) {
	    page->flags.before_sweep = TRUE;
	    page = page->next;
	}
	gc_sweep_step(objspace, heap_eden);
    }

    gc_heap_prepare_minimum_pages(objspace, heap_eden);
}

/* Marking - Marking stack */

static stack_chunk_t *
stack_chunk_alloc(void)
{
    stack_chunk_t *res;

    res = malloc(sizeof(stack_chunk_t));
    if (!res)
        rb_memerror();

    return res;
}

static inline int
is_mark_stack_empty(mark_stack_t *stack)
{
    return stack->chunk == NULL;
}

static size_t
mark_stack_size(mark_stack_t *stack)
{
    size_t size = stack->index;
    stack_chunk_t *chunk = stack->chunk ? stack->chunk->next : NULL;

    while (chunk) {
	size += stack->limit;
	chunk = chunk->next;
    }
    return size;
}

static void
add_stack_chunk_cache(mark_stack_t *stack, stack_chunk_t *chunk)
{
    chunk->next = stack->cache;
    stack->cache = chunk;
    stack->cache_size++;
}

static void
shrink_stack_chunk_cache(mark_stack_t *stack)
{
    stack_chunk_t *chunk;

    if (stack->unused_cache_size > (stack->cache_size/2)) {
        chunk = stack->cache;
        stack->cache = stack->cache->next;
        stack->cache_size--;
        free(chunk);
    }
    stack->unused_cache_size = stack->cache_size;
}

static void
push_mark_stack_chunk(mark_stack_t *stack)
{
    stack_chunk_t *next;

    if (RGENGC_CHECK_MODE) assert(stack->index == stack->limit);

    if (stack->cache_size > 0) {
        next = stack->cache;
        stack->cache = stack->cache->next;
        stack->cache_size--;
        if (stack->unused_cache_size > stack->cache_size)
            stack->unused_cache_size = stack->cache_size;
    }
    else {
        next = stack_chunk_alloc();
    }
    next->next = stack->chunk;
    stack->chunk = next;
    stack->index = 0;
}

static void
pop_mark_stack_chunk(mark_stack_t *stack)
{
    stack_chunk_t *prev;

    prev = stack->chunk->next;
    if (RGENGC_CHECK_MODE) assert(stack->index == 0);
    add_stack_chunk_cache(stack, stack->chunk);
    stack->chunk = prev;
    stack->index = stack->limit;
}

static void
free_stack_chunks(mark_stack_t *stack)
{
    stack_chunk_t *chunk = stack->chunk;
    stack_chunk_t *next = NULL;

    while (chunk != NULL) {
        next = chunk->next;
        free(chunk);
        chunk = next;
    }
}

static void
push_mark_stack(mark_stack_t *stack, VALUE data)
{
    if (stack->index == stack->limit) {
        push_mark_stack_chunk(stack);
    }
    stack->chunk->data[stack->index++] = data;
}

static int
pop_mark_stack(mark_stack_t *stack, VALUE *data)
{
    if (is_mark_stack_empty(stack)) {
        return FALSE;
    }
    if (stack->index == 1) {
        *data = stack->chunk->data[--stack->index];
        pop_mark_stack_chunk(stack);
    }
    else {
	*data = stack->chunk->data[--stack->index];
    }
    return TRUE;
}

#if GC_ENABLE_INCREMENTAL_MARK
static int
invalidate_mark_stack_chunk(stack_chunk_t *chunk, int limit, VALUE obj)
{
    int i;
    for (i=0; i<limit; i++) {
	if (chunk->data[i] == obj) {
	    chunk->data[i] = Qundef;
	    return TRUE;
	}
    }
    return FALSE;
}

static void
invalidate_mark_stack(mark_stack_t *stack, VALUE obj)
{
    stack_chunk_t *chunk = stack->chunk;
    int limit = stack->index;

    while (chunk) {
	if (invalidate_mark_stack_chunk(chunk, limit, obj)) return;
	chunk = chunk->next;
	limit = stack->limit;
    }
    rb_bug("invalid_mark_stack: unreachable");
}
#endif

static void
init_mark_stack(mark_stack_t *stack)
{
    int i;

    MEMZERO(stack, mark_stack_t, 1);
    stack->index = stack->limit = STACK_CHUNK_SIZE;
    stack->cache_size = 0;

    for (i=0; i < 4; i++) {
        add_stack_chunk_cache(stack, stack_chunk_alloc());
    }
    stack->unused_cache_size = stack->cache_size;
}

/* Marking */

#ifdef __ia64
#define SET_STACK_END (SET_MACHINE_STACK_END(&th->machine.stack_end), th->machine.register_stack_end = rb_ia64_bsp())
#else
#define SET_STACK_END SET_MACHINE_STACK_END(&th->machine.stack_end)
#endif

#define STACK_START (th->machine.stack_start)
#define STACK_END (th->machine.stack_end)
#define STACK_LEVEL_MAX (th->machine.stack_maxsize/sizeof(VALUE))

#if STACK_GROW_DIRECTION < 0
# define STACK_LENGTH  (size_t)(STACK_START - STACK_END)
#elif STACK_GROW_DIRECTION > 0
# define STACK_LENGTH  (size_t)(STACK_END - STACK_START + 1)
#else
# define STACK_LENGTH  ((STACK_END < STACK_START) ? (size_t)(STACK_START - STACK_END) \
			: (size_t)(STACK_END - STACK_START + 1))
#endif
#if !STACK_GROW_DIRECTION
int ruby_stack_grow_direction;
int
ruby_get_stack_grow_direction(volatile VALUE *addr)
{
    VALUE *end;
    SET_MACHINE_STACK_END(&end);

    if (end > addr) return ruby_stack_grow_direction = 1;
    return ruby_stack_grow_direction = -1;
}
#endif

size_t
ruby_stack_length(VALUE **p)
{
    rb_thread_t *th = GET_THREAD();
    SET_STACK_END;
    if (p) *p = STACK_UPPER(STACK_END, STACK_START, STACK_END);
    return STACK_LENGTH;
}

#if !(defined(POSIX_SIGNAL) && defined(SIGSEGV) && defined(HAVE_SIGALTSTACK))
static int
stack_check(int water_mark)
{
    int ret;
    rb_thread_t *th = GET_THREAD();
    SET_STACK_END;
    ret = STACK_LENGTH > STACK_LEVEL_MAX - water_mark;
#ifdef __ia64
    if (!ret) {
        ret = (VALUE*)rb_ia64_bsp() - th->machine.register_stack_start >
              th->machine.register_stack_maxsize/sizeof(VALUE) - water_mark;
    }
#endif
    return ret;
}
#endif

#define STACKFRAME_FOR_CALL_CFUNC 512

int
ruby_stack_check(void)
{
#if defined(POSIX_SIGNAL) && defined(SIGSEGV) && defined(HAVE_SIGALTSTACK)
    return 0;
#else
    return stack_check(STACKFRAME_FOR_CALL_CFUNC);
#endif
}

ATTRIBUTE_NO_ADDRESS_SAFETY_ANALYSIS
static void
mark_locations_array(rb_objspace_t *objspace, register const VALUE *x, register long n)
{
    VALUE v;
    while (n--) {
        v = *x;
	gc_mark_maybe(objspace, v);
	x++;
    }
}

static void
gc_mark_locations(rb_objspace_t *objspace, const VALUE *start, const VALUE *end)
{
    long n;

    if (end <= start) return;
    n = end - start;
    mark_locations_array(objspace, start, n);
}

void
rb_gc_mark_locations(const VALUE *start, const VALUE *end)
{
    gc_mark_locations(&rb_objspace, start, end);
}

void
rb_gc_mark_values(long n, const VALUE *values)
{
    rb_objspace_t *objspace = &rb_objspace;
    long i;

    for (i=0; i<n; i++) {
	gc_mark(objspace, values[i]);
    }
}

#define rb_gc_mark_locations(start, end) gc_mark_locations(objspace, (start), (end))

static int
mark_entry(st_data_t key, st_data_t value, st_data_t data)
{
    rb_objspace_t *objspace = (rb_objspace_t *)data;
    gc_mark(objspace, (VALUE)value);
    return ST_CONTINUE;
}

static void
mark_tbl(rb_objspace_t *objspace, st_table *tbl)
{
    if (!tbl || tbl->num_entries == 0) return;
    st_foreach(tbl, mark_entry, (st_data_t)objspace);
}

static int
mark_key(st_data_t key, st_data_t value, st_data_t data)
{
    rb_objspace_t *objspace = (rb_objspace_t *)data;
    gc_mark(objspace, (VALUE)key);
    return ST_CONTINUE;
}

static void
mark_set(rb_objspace_t *objspace, st_table *tbl)
{
    if (!tbl) return;
    st_foreach(tbl, mark_key, (st_data_t)objspace);
}

void
rb_mark_set(st_table *tbl)
{
    mark_set(&rb_objspace, tbl);
}

static int
mark_keyvalue(st_data_t key, st_data_t value, st_data_t data)
{
    rb_objspace_t *objspace = (rb_objspace_t *)data;

    gc_mark(objspace, (VALUE)key);
    gc_mark(objspace, (VALUE)value);
    return ST_CONTINUE;
}

static void
mark_hash(rb_objspace_t *objspace, st_table *tbl)
{
    if (!tbl) return;
    st_foreach(tbl, mark_keyvalue, (st_data_t)objspace);
}

void
rb_mark_hash(st_table *tbl)
{
    mark_hash(&rb_objspace, tbl);
}

static void
mark_method_entry(rb_objspace_t *objspace, const rb_method_entry_t *me)
{
    const rb_method_definition_t *def = me->def;

    gc_mark(objspace, me->owner);
    gc_mark(objspace, me->defined_class);

    if (def) {
	switch (def->type) {
	  case VM_METHOD_TYPE_ISEQ:
	    if (def->body.iseq.iseqptr) gc_mark(objspace, (VALUE)def->body.iseq.iseqptr);
	    gc_mark(objspace, (VALUE)def->body.iseq.cref);
	    break;
	  case VM_METHOD_TYPE_ATTRSET:
	  case VM_METHOD_TYPE_IVAR:
	    gc_mark(objspace, def->body.attr.location);
	    break;
	  case VM_METHOD_TYPE_BMETHOD:
	    gc_mark(objspace, def->body.proc);
	    break;
	  case VM_METHOD_TYPE_ALIAS:
	    gc_mark(objspace, (VALUE)def->body.alias.original_me);
	    return;
	  case VM_METHOD_TYPE_REFINED:
	    gc_mark(objspace, (VALUE)def->body.refined.orig_me);
	    gc_mark(objspace, (VALUE)def->body.refined.owner);
	    break;
	  case VM_METHOD_TYPE_CFUNC:
	  case VM_METHOD_TYPE_ZSUPER:
	  case VM_METHOD_TYPE_MISSING:
	  case VM_METHOD_TYPE_OPTIMIZED:
	  case VM_METHOD_TYPE_UNDEF:
	  case VM_METHOD_TYPE_NOTIMPLEMENTED:
	    break;
	}
    }
}

static enum rb_id_table_iterator_result
mark_method_entry_i(VALUE me, void *data)
{
    rb_objspace_t *objspace = (rb_objspace_t *)data;

    gc_mark(objspace, me);
    return ID_TABLE_CONTINUE;
}

static void
mark_m_tbl(rb_objspace_t *objspace, struct rb_id_table *tbl)
{
    if (tbl) {
	rb_id_table_foreach_values(tbl, mark_method_entry_i, objspace);
    }
}

static int
mark_const_entry_i(st_data_t key, st_data_t value, st_data_t data)
{
    const rb_const_entry_t *ce = (const rb_const_entry_t *)value;
    rb_objspace_t *objspace = (rb_objspace_t *)data;

    gc_mark(objspace, ce->value);
    gc_mark(objspace, ce->file);
    return ST_CONTINUE;
}

static void
mark_const_tbl(rb_objspace_t *objspace, st_table *tbl)
{
    if (!tbl) return;
    st_foreach(tbl, mark_const_entry_i, (st_data_t)objspace);
}

#if STACK_GROW_DIRECTION < 0
#define GET_STACK_BOUNDS(start, end, appendix) ((start) = STACK_END, (end) = STACK_START)
#elif STACK_GROW_DIRECTION > 0
#define GET_STACK_BOUNDS(start, end, appendix) ((start) = STACK_START, (end) = STACK_END+(appendix))
#else
#define GET_STACK_BOUNDS(start, end, appendix) \
    ((STACK_END < STACK_START) ? \
     ((start) = STACK_END, (end) = STACK_START) : ((start) = STACK_START, (end) = STACK_END+(appendix)))
#endif

static void
mark_current_machine_context(rb_objspace_t *objspace, rb_thread_t *th)
{
    union {
	rb_jmp_buf j;
	VALUE v[sizeof(rb_jmp_buf) / sizeof(VALUE)];
    } save_regs_gc_mark;
    VALUE *stack_start, *stack_end;

    FLUSH_REGISTER_WINDOWS;
    /* This assumes that all registers are saved into the jmp_buf (and stack) */
    rb_setjmp(save_regs_gc_mark.j);

    /* SET_STACK_END must be called in this function because
     * the stack frame of this function may contain
     * callee save registers and they should be marked. */
    SET_STACK_END;
    GET_STACK_BOUNDS(stack_start, stack_end, 1);

    mark_locations_array(objspace, save_regs_gc_mark.v, numberof(save_regs_gc_mark.v));

    rb_gc_mark_locations(stack_start, stack_end);
#ifdef __ia64
    rb_gc_mark_locations(th->machine.register_stack_start, th->machine.register_stack_end);
#endif
#if defined(__mc68000__)
    rb_gc_mark_locations((VALUE*)((char*)stack_start + 2),
			 (VALUE*)((char*)stack_end - 2));
#endif
}

void
rb_gc_mark_machine_stack(rb_thread_t *th)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE *stack_start, *stack_end;

    GET_STACK_BOUNDS(stack_start, stack_end, 0);
    rb_gc_mark_locations(stack_start, stack_end);
#ifdef __ia64
    rb_gc_mark_locations(th->machine.register_stack_start, th->machine.register_stack_end);
#endif
#if defined(__mc68000__)
    rb_gc_mark_locations((VALUE*)((char*)stack_start + 2),
			 (VALUE*)((char*)stack_end - 2));
#endif
}

void
rb_mark_tbl(st_table *tbl)
{
    mark_tbl(&rb_objspace, tbl);
}

static void
gc_mark_maybe(rb_objspace_t *objspace, VALUE obj)
{
    (void)VALGRIND_MAKE_MEM_DEFINED(&obj, sizeof(obj));
    if (is_pointer_to_heap(objspace, (void *)obj)) {
	int type = BUILTIN_TYPE(obj);
	if (type != T_ZOMBIE && type != T_NONE) {
	    gc_mark_ptr(objspace, obj);
	}
    }
}

void
rb_gc_mark_maybe(VALUE obj)
{
    gc_mark_maybe(&rb_objspace, obj);
}

static inline int
gc_mark_set(rb_objspace_t *objspace, VALUE obj)
{
    if (RVALUE_MARKED(obj)) return 0;
    MARK_IN_BITMAP(GET_HEAP_MARK_BITS(obj), obj);
    return 1;
}

#if USE_RGENGC
static int
gc_remember_unprotected(rb_objspace_t *objspace, VALUE obj)
{
    struct heap_page *page = GET_HEAP_PAGE(obj);
    bits_t *uncollectible_bits = &page->uncollectible_bits[0];

    if (!MARKED_IN_BITMAP(uncollectible_bits, obj)) {
	page->flags.has_uncollectible_shady_objects = TRUE;
	MARK_IN_BITMAP(uncollectible_bits, obj);
	objspace->rgengc.uncollectible_wb_unprotected_objects++;

#if RGENGC_PROFILE > 0
	objspace->profile.total_remembered_shady_object_count++;
#if RGENGC_PROFILE >= 2
	objspace->profile.remembered_shady_object_count_types[BUILTIN_TYPE(obj)]++;
#endif
#endif
	return TRUE;
    }
    else {
	return FALSE;
    }
}
#endif

static void
rgengc_check_relation(rb_objspace_t *objspace, VALUE obj)
{
#if USE_RGENGC
    const VALUE old_parent = objspace->rgengc.parent_object;

    if (old_parent) { /* parent object is old */
	if (RVALUE_WB_UNPROTECTED(obj)) {
	    if (gc_remember_unprotected(objspace, obj)) {
		gc_report(2, objspace, "relation: (O->S) %s -> %s\n", obj_info(old_parent), obj_info(obj));
	    }
	}
	else {
	    if (!RVALUE_OLD_P(obj)) {
		if (RVALUE_MARKED(obj)) {
		    /* An object pointed from an OLD object should be OLD. */
		    gc_report(2, objspace, "relation: (O->unmarked Y) %s -> %s\n", obj_info(old_parent), obj_info(obj));
		    RVALUE_AGE_SET_OLD(objspace, obj);
		    if (is_incremental_marking(objspace)) {
			if (!RVALUE_MARKING(obj)) {
			    gc_grey(objspace, obj);
			}
		    }
		    else {
			rgengc_remember(objspace, obj);
		    }
		}
		else {
		    gc_report(2, objspace, "relation: (O->Y) %s -> %s\n", obj_info(old_parent), obj_info(obj));
		    RVALUE_AGE_SET_CANDIDATE(objspace, obj);
		}
	    }
	}
    }

    if (RGENGC_CHECK_MODE) assert(old_parent == objspace->rgengc.parent_object);
#endif
}

static void
gc_grey(rb_objspace_t *objspace, VALUE obj)
{
#if RGENGC_CHECK_MODE
    if (RVALUE_MARKED(obj) == FALSE) rb_bug("gc_grey: %s is not marked.", obj_info(obj));
    if (RVALUE_MARKING(obj) == TRUE) rb_bug("gc_grey: %s is marking/remembered.", obj_info(obj));
#endif

#if GC_ENABLE_INCREMENTAL_MARK
    if (is_incremental_marking(objspace)) {
	MARK_IN_BITMAP(GET_HEAP_MARKING_BITS(obj), obj);
    }
#endif

    push_mark_stack(&objspace->mark_stack, obj);
}

static void
gc_aging(rb_objspace_t *objspace, VALUE obj)
{
#if USE_RGENGC
    struct heap_page *page = GET_HEAP_PAGE(obj);

#if RGENGC_CHECK_MODE
    assert(RVALUE_MARKING(obj) == FALSE);
#endif

    check_rvalue_consistency(obj);

    if (!RVALUE_PAGE_WB_UNPROTECTED(page, obj)) {
	if (!RVALUE_OLD_P(obj)) {
	    gc_report(3, objspace, "gc_aging: YOUNG: %s\n", obj_info(obj));
	    RVALUE_AGE_INC(objspace, obj);
	}
	else if (is_full_marking(objspace)) {
	    if (RGENGC_CHECK_MODE) assert(RVALUE_PAGE_UNCOLLECTIBLE(page, obj) == FALSE);
	    RVALUE_PAGE_OLD_UNCOLLECTIBLE_SET(objspace, page, obj);
	}
    }
    check_rvalue_consistency(obj);
#endif /* USE_RGENGC */

    objspace->marked_slots++;
}

static void
gc_mark_ptr(rb_objspace_t *objspace, VALUE obj)
{
    if (LIKELY(objspace->mark_func_data == NULL)) {
	/* check code for Bug #11244 */
	if (BUILTIN_TYPE(obj) == T_NONE) {
	    if (objspace->rgengc.parent_object) {
		rb_bug("gc_mark_ptr: obj is %s (parent: %s)", obj_info(obj),
		       obj_info(objspace->rgengc.parent_object));
	    }
	    else {
		rb_bug("gc_mark_ptr: obj is %s (parent is not old)", obj_info(obj));
	    }
	}

	rgengc_check_relation(objspace, obj);
	if (!gc_mark_set(objspace, obj)) return; /* already marked */
	gc_aging(objspace, obj);
	gc_grey(objspace, obj);
    }
    else {
	objspace->mark_func_data->mark_func(obj, objspace->mark_func_data->data);
    }
}

static void
gc_mark(rb_objspace_t *objspace, VALUE obj)
{
    if (!is_markable_object(objspace, obj)) return;
    gc_mark_ptr(objspace, obj);
}

void
rb_gc_mark(VALUE ptr)
{
    gc_mark(&rb_objspace, ptr);
}

/* CAUTION: THIS FUNCTION ENABLE *ONLY BEFORE* SWEEPING.
 * This function is only for GC_END_MARK timing.
 */

int
rb_objspace_marked_object_p(VALUE obj)
{
    return RVALUE_MARKED(obj) ? TRUE : FALSE;
}

static inline void
gc_mark_set_parent(rb_objspace_t *objspace, VALUE obj)
{
#if USE_RGENGC
    if (RVALUE_OLD_P(obj)) {
	objspace->rgengc.parent_object = obj;
    }
    else {
	objspace->rgengc.parent_object = Qfalse;
    }
#endif
}

static void
gc_mark_children(rb_objspace_t *objspace, VALUE obj)
{
    register RVALUE *any = RANY(obj);
    gc_mark_set_parent(objspace, obj);

    if (FL_TEST(obj, FL_EXIVAR)) {
	rb_mark_generic_ivar(obj);
    }

    switch (BUILTIN_TYPE(obj)) {
      case T_NIL:
      case T_FIXNUM:
	rb_bug("rb_gc_mark() called for broken object");
	break;

      case T_NODE:
	obj = rb_gc_mark_node(&any->as.node);
	if (obj) gc_mark(objspace, obj);
	return;			/* no need to mark class. */

      case T_IMEMO:
	switch (imemo_type(obj)) {
	  case imemo_none:
	    rb_bug("unreachable");
	    return;
	  case imemo_cref:
	    gc_mark(objspace, RANY(obj)->as.imemo.cref.klass);
	    gc_mark(objspace, (VALUE)RANY(obj)->as.imemo.cref.next);
	    gc_mark(objspace, RANY(obj)->as.imemo.cref.refinements);
	    return;
	  case imemo_svar:
	    gc_mark(objspace, RANY(obj)->as.imemo.svar.cref_or_me);
	    gc_mark(objspace, RANY(obj)->as.imemo.svar.lastline);
	    gc_mark(objspace, RANY(obj)->as.imemo.svar.backref);
	    gc_mark(objspace, RANY(obj)->as.imemo.svar.others);
	    return;
	  case imemo_throw_data:
	    gc_mark(objspace, RANY(obj)->as.imemo.throw_data.throw_obj);
	    return;
	  case imemo_ifunc:
	    gc_mark_maybe(objspace, (VALUE)RANY(obj)->as.imemo.ifunc.data);
	    return;
	  case imemo_memo:
	    gc_mark(objspace, RANY(obj)->as.imemo.memo.v1);
	    gc_mark(objspace, RANY(obj)->as.imemo.memo.v2);
	    gc_mark_maybe(objspace, RANY(obj)->as.imemo.memo.u3.value);
	    return;
	  case imemo_ment:
	    mark_method_entry(objspace, &RANY(obj)->as.imemo.ment);
	    return;
	  case imemo_iseq:
	    rb_iseq_mark((rb_iseq_t *)obj);
	    return;
	}
	rb_bug("T_IMEMO: unreachable");
    }

    gc_mark(objspace, any->as.basic.klass);

    switch (BUILTIN_TYPE(obj)) {
      case T_CLASS:
      case T_MODULE:
	mark_m_tbl(objspace, RCLASS_M_TBL(obj));
	if (!RCLASS_EXT(obj)) break;
	mark_tbl(objspace, RCLASS_IV_TBL(obj));
	mark_const_tbl(objspace, RCLASS_CONST_TBL(obj));
	gc_mark(objspace, RCLASS_SUPER((VALUE)obj));
	break;

      case T_ICLASS:
	if (FL_TEST(obj, RICLASS_IS_ORIGIN)) {
	    mark_m_tbl(objspace, RCLASS_M_TBL(obj));
	}
	if (!RCLASS_EXT(obj)) break;
	mark_m_tbl(objspace, RCLASS_CALLABLE_M_TBL(obj));
	gc_mark(objspace, RCLASS_SUPER((VALUE)obj));
	break;

      case T_ARRAY:
	if (FL_TEST(obj, ELTS_SHARED)) {
	    gc_mark(objspace, any->as.array.as.heap.aux.shared);
	}
	else {
	    long i, len = RARRAY_LEN(obj);
	    const VALUE *ptr = RARRAY_CONST_PTR(obj);
	    for (i=0; i < len; i++) {
		gc_mark(objspace, *ptr++);
	    }
	}
	break;

      case T_HASH:
	mark_hash(objspace, any->as.hash.ntbl);
	gc_mark(objspace, any->as.hash.ifnone);
	break;

      case T_STRING:
	if (STR_SHARED_P(obj)) {
	    gc_mark(objspace, any->as.string.as.heap.aux.shared);
	}
	break;

      case T_DATA:
	{
	    void *const ptr = DATA_PTR(obj);
	    if (ptr) {
		RUBY_DATA_FUNC mark_func = RTYPEDDATA_P(obj) ?
		    any->as.typeddata.type->function.dmark :
		    any->as.data.dmark;
		if (mark_func) (*mark_func)(ptr);
	    }
	}
	break;

      case T_OBJECT:
        {
            long i, len = ROBJECT_NUMIV(obj);
	    VALUE *ptr = ROBJECT_IVPTR(obj);
            for (i  = 0; i < len; i++) {
		gc_mark(objspace, *ptr++);
            }
        }
	break;

      case T_FILE:
        if (any->as.file.fptr) {
            gc_mark(objspace, any->as.file.fptr->pathv);
            gc_mark(objspace, any->as.file.fptr->tied_io_for_writing);
            gc_mark(objspace, any->as.file.fptr->writeconv_asciicompat);
            gc_mark(objspace, any->as.file.fptr->writeconv_pre_ecopts);
            gc_mark(objspace, any->as.file.fptr->encs.ecopts);
            gc_mark(objspace, any->as.file.fptr->write_lock);
        }
        break;

      case T_REGEXP:
        gc_mark(objspace, any->as.regexp.src);
	break;

      case T_FLOAT:
      case T_BIGNUM:
      case T_SYMBOL:
	break;

      case T_MATCH:
	gc_mark(objspace, any->as.match.regexp);
	if (any->as.match.str) {
	    gc_mark(objspace, any->as.match.str);
	}
	break;

      case T_RATIONAL:
	gc_mark(objspace, any->as.rational.num);
	gc_mark(objspace, any->as.rational.den);
	break;

      case T_COMPLEX:
	gc_mark(objspace, any->as.complex.real);
	gc_mark(objspace, any->as.complex.imag);
	break;

      case T_STRUCT:
	{
	    long len = RSTRUCT_LEN(obj);
	    const VALUE *ptr = RSTRUCT_CONST_PTR(obj);

	    while (len--) {
		gc_mark(objspace, *ptr++);
	    }
	}
	break;

      default:
#if GC_DEBUG
	rb_gcdebug_print_obj_condition((VALUE)obj);
#endif
	if (BUILTIN_TYPE(obj) == T_NONE)   rb_bug("rb_gc_mark(): %p is T_NONE", (void *)obj);
	if (BUILTIN_TYPE(obj) == T_ZOMBIE) rb_bug("rb_gc_mark(): %p is T_ZOMBIE", (void *)obj);
	rb_bug("rb_gc_mark(): unknown data type 0x%x(%p) %s",
	       BUILTIN_TYPE(obj), any,
	       is_pointer_to_heap(objspace, any) ? "corrupted object" : "non object");
    }
}

/**
 * incremental: 0 -> not incremental (do all)
 * incremental: n -> mark at most `n' objects
 */
static inline int
gc_mark_stacked_objects(rb_objspace_t *objspace, int incremental, size_t count)
{
    mark_stack_t *mstack = &objspace->mark_stack;
    VALUE obj;
#if GC_ENABLE_INCREMENTAL_MARK
    size_t marked_slots_at_the_beggining = objspace->marked_slots;
    size_t popped_count = 0;
#endif

    while (pop_mark_stack(mstack, &obj)) {
	if (obj == Qundef) continue; /* skip */

	if (RGENGC_CHECK_MODE && !RVALUE_MARKED(obj)) {
	    rb_bug("gc_mark_stacked_objects: %s is not marked.", obj_info(obj));
	}
        gc_mark_children(objspace, obj);

#if GC_ENABLE_INCREMENTAL_MARK
	if (incremental) {
	    if (RGENGC_CHECK_MODE && !RVALUE_MARKING(obj)) {
		rb_bug("gc_mark_stacked_objects: incremental, but marking bit is 0");
	    }
	    CLEAR_IN_BITMAP(GET_HEAP_MARKING_BITS(obj), obj);
	    popped_count++;

	    if (popped_count + (objspace->marked_slots - marked_slots_at_the_beggining) > count) {
		break;
	    }
	}
	else {
	    /* just ignore marking bits */
	}
#endif
    }

    if (RGENGC_CHECK_MODE >= 3) gc_verify_internal_consistency(Qnil);

    if (is_mark_stack_empty(mstack)) {
	shrink_stack_chunk_cache(mstack);
	return TRUE;
    }
    else {
	return FALSE;
    }
}

static int
gc_mark_stacked_objects_incremental(rb_objspace_t *objspace, size_t count)
{
    return gc_mark_stacked_objects(objspace, TRUE, count);
}

static int
gc_mark_stacked_objects_all(rb_objspace_t *objspace)
{
    return gc_mark_stacked_objects(objspace, FALSE, 0);
}

#if PRINT_ROOT_TICKS
#define MAX_TICKS 0x100
static tick_t mark_ticks[MAX_TICKS];
static const char *mark_ticks_categories[MAX_TICKS];

static void
show_mark_ticks(void)
{
    int i;
    fprintf(stderr, "mark ticks result:\n");
    for (i=0; i<MAX_TICKS; i++) {
	const char *category = mark_ticks_categories[i];
	if (category) {
	    fprintf(stderr, "%s\t%8lu\n", category, (unsigned long)mark_ticks[i]);
	}
	else {
	    break;
	}
    }
}

#endif /* PRITNT_ROOT_TICKS */

static void
gc_mark_roots(rb_objspace_t *objspace, const char **categoryp)
{
    struct gc_list *list;
    rb_thread_t *th = GET_THREAD();

#if PRINT_ROOT_TICKS
    tick_t start_tick = tick();
    int tick_count = 0;
    const char *prev_category = 0;

    if (mark_ticks_categories[0] == 0) {
	atexit(show_mark_ticks);
    }
#endif

    if (categoryp) *categoryp = "xxx";

#if USE_RGENGC
    objspace->rgengc.parent_object = Qfalse;
#endif

#if PRINT_ROOT_TICKS
#define MARK_CHECKPOINT_PRINT_TICK(category) do { \
    if (prev_category) { \
	tick_t t = tick(); \
	mark_ticks[tick_count] = t - start_tick; \
	mark_ticks_categories[tick_count] = prev_category; \
	tick_count++; \
    } \
    prev_category = category; \
    start_tick = tick(); \
} while (0)
#else /* PRITNT_ROOT_TICKS */
#define MARK_CHECKPOINT_PRINT_TICK(category)
#endif

#define MARK_CHECKPOINT(category) do { \
    if (categoryp) *categoryp = category; \
    MARK_CHECKPOINT_PRINT_TICK(category); \
} while (0)

    MARK_CHECKPOINT("vm");
    SET_STACK_END;
    rb_vm_mark(th->vm);
    if (th->vm->self) gc_mark_set(objspace, th->vm->self);

    MARK_CHECKPOINT("finalizers");
    mark_tbl(objspace, finalizer_table);

    MARK_CHECKPOINT("machine_context");
    mark_current_machine_context(objspace, th);

    MARK_CHECKPOINT("encodings");
    rb_gc_mark_encodings();

    /* mark protected global variables */
    MARK_CHECKPOINT("global_list");
    for (list = global_list; list; list = list->next) {
	rb_gc_mark_maybe(*list->varptr);
    }

    MARK_CHECKPOINT("end_proc");
    rb_mark_end_proc();

    MARK_CHECKPOINT("global_tbl");
    rb_gc_mark_global_tbl();

    if (stress_to_class) rb_gc_mark(stress_to_class);

    MARK_CHECKPOINT("finish");
#undef MARK_CHECKPOINT
}

#if RGENGC_CHECK_MODE >= 4

#define MAKE_ROOTSIG(obj) (((VALUE)(obj) << 1) | 0x01)
#define IS_ROOTSIG(obj)   ((VALUE)(obj) & 0x01)
#define GET_ROOTSIG(obj)  ((const char *)((VALUE)(obj) >> 1))

struct reflist {
    VALUE *list;
    int pos;
    int size;
};

static struct reflist *
reflist_create(VALUE obj)
{
    struct reflist *refs = xmalloc(sizeof(struct reflist));
    refs->size = 1;
    refs->list = ALLOC_N(VALUE, refs->size);
    refs->list[0] = obj;
    refs->pos = 1;
    return refs;
}

static void
reflist_destruct(struct reflist *refs)
{
    xfree(refs->list);
    xfree(refs);
}

static void
reflist_add(struct reflist *refs, VALUE obj)
{
    if (refs->pos == refs->size) {
	refs->size *= 2;
	SIZED_REALLOC_N(refs->list, VALUE, refs->size, refs->size/2);
    }

    refs->list[refs->pos++] = obj;
}

static void
reflist_dump(struct reflist *refs)
{
    int i;
    for (i=0; i<refs->pos; i++) {
	VALUE obj = refs->list[i];
	if (IS_ROOTSIG(obj)) { /* root */
	    fprintf(stderr, "<root@%s>", GET_ROOTSIG(obj));
	}
	else {
	    fprintf(stderr, "<%s>", obj_info(obj));
	}
	if (i+1 < refs->pos) fprintf(stderr, ", ");
    }
}

static int
reflist_refered_from_machine_context(struct reflist *refs)
{
    int i;
    for (i=0; i<refs->pos; i++) {
	VALUE obj = refs->list[i];
	if (IS_ROOTSIG(obj) && strcmp(GET_ROOTSIG(obj), "machine_context") == 0) return 1;
    }
    return 0;
}

struct allrefs {
    rb_objspace_t *objspace;
    /* a -> obj1
     * b -> obj1
     * c -> obj1
     * c -> obj2
     * d -> obj3
     * #=> {obj1 => [a, b, c], obj2 => [c, d]}
     */
    struct st_table *references;
    const char *category;
    VALUE root_obj;
    mark_stack_t mark_stack;
};

static int
allrefs_add(struct allrefs *data, VALUE obj)
{
    struct reflist *refs;

    if (st_lookup(data->references, obj, (st_data_t *)&refs)) {
	reflist_add(refs, data->root_obj);
	return 0;
    }
    else {
	refs = reflist_create(data->root_obj);
	st_insert(data->references, obj, (st_data_t)refs);
	return 1;
    }
}

static void
allrefs_i(VALUE obj, void *ptr)
{
    struct allrefs *data = (struct allrefs *)ptr;

    if (allrefs_add(data, obj)) {
	push_mark_stack(&data->mark_stack, obj);
    }
}

static void
allrefs_roots_i(VALUE obj, void *ptr)
{
    struct allrefs *data = (struct allrefs *)ptr;
    if (strlen(data->category) == 0) rb_bug("!!!");
    data->root_obj = MAKE_ROOTSIG(data->category);

    if (allrefs_add(data, obj)) {
	push_mark_stack(&data->mark_stack, obj);
    }
}

static st_table *
objspace_allrefs(rb_objspace_t *objspace)
{
    struct allrefs data;
    struct mark_func_data_struct mfd;
    VALUE obj;
    int prev_dont_gc = dont_gc;
    dont_gc = TRUE;

    data.objspace = objspace;
    data.references = st_init_numtable();
    init_mark_stack(&data.mark_stack);

    mfd.mark_func = allrefs_roots_i;
    mfd.data = &data;

    /* traverse root objects */
    PUSH_MARK_FUNC_DATA(&mfd);
    objspace->mark_func_data = &mfd;
    gc_mark_roots(objspace, &data.category);
    POP_MARK_FUNC_DATA();

    /* traverse rest objects reachable from root objects */
    while (pop_mark_stack(&data.mark_stack, &obj)) {
	rb_objspace_reachable_objects_from(data.root_obj = obj, allrefs_i, &data);
    }
    free_stack_chunks(&data.mark_stack);

    dont_gc = prev_dont_gc;
    return data.references;
}

static int
objspace_allrefs_destruct_i(st_data_t key, st_data_t value, void *ptr)
{
    struct reflist *refs = (struct reflist *)value;
    reflist_destruct(refs);
    return ST_CONTINUE;
}

static void
objspace_allrefs_destruct(struct st_table *refs)
{
    st_foreach(refs, objspace_allrefs_destruct_i, 0);
    st_free_table(refs);
}

#if RGENGC_CHECK_MODE >= 5
static int
allrefs_dump_i(st_data_t k, st_data_t v, st_data_t ptr)
{
    VALUE obj = (VALUE)k;
    struct reflist *refs = (struct reflist *)v;
    fprintf(stderr, "[allrefs_dump_i] %s <- ", obj_info(obj));
    reflist_dump(refs);
    fprintf(stderr, "\n");
    return ST_CONTINUE;
}

static void
allrefs_dump(rb_objspace_t *objspace)
{
    fprintf(stderr, "[all refs] (size: %d)\n", (int)objspace->rgengc.allrefs_table->num_entries);
    st_foreach(objspace->rgengc.allrefs_table, allrefs_dump_i, 0);
}
#endif

static int
gc_check_after_marks_i(st_data_t k, st_data_t v, void *ptr)
{
    VALUE obj = k;
    struct reflist *refs = (struct reflist *)v;
    rb_objspace_t *objspace = (rb_objspace_t *)ptr;

    /* object should be marked or oldgen */
    if (!MARKED_IN_BITMAP(GET_HEAP_MARK_BITS(obj), obj)) {
	fprintf(stderr, "gc_check_after_marks_i: %s is not marked and not oldgen.\n", obj_info(obj));
	fprintf(stderr, "gc_check_after_marks_i: %p is referred from ", (void *)obj);
	reflist_dump(refs);

	if (reflist_refered_from_machine_context(refs)) {
	    fprintf(stderr, " (marked from machine stack).\n");
	    /* marked from machine context can be false positive */
	}
	else {
	    objspace->rgengc.error_count++;
	    fprintf(stderr, "\n");
	}
    }
    return ST_CONTINUE;
}

static void
gc_marks_check(rb_objspace_t *objspace, int (*checker_func)(ANYARGS), const char *checker_name)
{
    size_t saved_malloc_increase = objspace->malloc_params.increase;
#if RGENGC_ESTIMATE_OLDMALLOC
    size_t saved_oldmalloc_increase = objspace->rgengc.oldmalloc_increase;
#endif
    VALUE already_disabled = rb_gc_disable();

    objspace->rgengc.allrefs_table = objspace_allrefs(objspace);

    if (checker_func) {
	st_foreach(objspace->rgengc.allrefs_table, checker_func, (st_data_t)objspace);
    }

    if (objspace->rgengc.error_count > 0) {
#if RGENGC_CHECK_MODE >= 5
	allrefs_dump(objspace);
#endif
	if (checker_name) rb_bug("%s: GC has problem.", checker_name);
    }

    objspace_allrefs_destruct(objspace->rgengc.allrefs_table);
    objspace->rgengc.allrefs_table = 0;

    if (already_disabled == Qfalse) rb_gc_enable();
    objspace->malloc_params.increase = saved_malloc_increase;
#if RGENGC_ESTIMATE_OLDMALLOC
    objspace->rgengc.oldmalloc_increase = saved_oldmalloc_increase;
#endif
}
#endif /* RGENGC_CHECK_MODE >= 4 */

struct verify_internal_consistency_struct {
    rb_objspace_t *objspace;
    int err_count;
    size_t live_object_count;
    size_t zombie_object_count;

#if USE_RGENGC
    VALUE parent;
    size_t old_object_count;
    size_t remembered_shady_count;
#endif
};

#if USE_RGENGC
static void
check_generation_i(const VALUE child, void *ptr)
{
    struct verify_internal_consistency_struct *data = (struct verify_internal_consistency_struct *)ptr;
    const VALUE parent = data->parent;

    if (RGENGC_CHECK_MODE) assert(RVALUE_OLD_P(parent));

    if (!RVALUE_OLD_P(child)) {
	if (!RVALUE_REMEMBERED(parent) &&
	    !RVALUE_REMEMBERED(child) &&
	    !RVALUE_UNCOLLECTIBLE(child)) {
	    fprintf(stderr, "verify_internal_consistency_reachable_i: WB miss (O->Y) %s -> %s\n", obj_info(parent), obj_info(child));
	    data->err_count++;
	}
    }
}

static void
check_color_i(const VALUE child, void *ptr)
{
    struct verify_internal_consistency_struct *data = (struct verify_internal_consistency_struct *)ptr;
    const VALUE parent = data->parent;

    if (!RVALUE_WB_UNPROTECTED(parent) && RVALUE_WHITE_P(child)) {
	fprintf(stderr, "verify_internal_consistency_reachable_i: WB miss (B->W) - %s -> %s\n",
		obj_info(parent), obj_info(child));
	data->err_count++;
    }
}
#endif

static void
check_children_i(const VALUE child, void *ptr)
{
    check_rvalue_consistency(child);
}

static int
verify_internal_consistency_i(void *page_start, void *page_end, size_t stride, void *ptr)
{
    struct verify_internal_consistency_struct *data = (struct verify_internal_consistency_struct *)ptr;
    VALUE obj;
    rb_objspace_t *objspace = data->objspace;

    for (obj = (VALUE)page_start; obj != (VALUE)page_end; obj += stride) {
	if (is_live_object(objspace, obj)) {
	    /* count objects */
	    data->live_object_count++;

	    rb_objspace_reachable_objects_from(obj, check_children_i, (void *)data);

#if USE_RGENGC
	    /* check health of children */
	    data->parent = obj;

	    if (RVALUE_OLD_P(obj)) data->old_object_count++;
	    if (RVALUE_WB_UNPROTECTED(obj) && RVALUE_UNCOLLECTIBLE(obj)) data->remembered_shady_count++;

	    if (!is_marking(objspace) && RVALUE_OLD_P(obj)) {
		/* reachable objects from an oldgen object should be old or (young with remember) */
		data->parent = obj;
		rb_objspace_reachable_objects_from(obj, check_generation_i, (void *)data);
	    }

	    if (is_incremental_marking(objspace)) {
		if (RVALUE_BLACK_P(obj)) {
		    /* reachable objects from black objects should be black or grey objects */
		    data->parent = obj;
		    rb_objspace_reachable_objects_from(obj, check_color_i, (void *)data);
		}
	    }
#endif
	}
	else {
	    if (BUILTIN_TYPE(obj) == T_ZOMBIE) {
		if (RGENGC_CHECK_MODE) assert(RBASIC(obj)->flags == T_ZOMBIE);
		data->zombie_object_count++;
	    }
	}
    }

    return 0;
}

static int
gc_verify_heap_page(rb_objspace_t *objspace, struct heap_page *page, VALUE obj)
{
#if USE_RGENGC
    int i;
    unsigned int has_remembered_shady = FALSE;
    unsigned int has_remembered_old = FALSE;
    int rememberd_old_objects = 0;

    for (i=0; i<page->total_slots; i++) {
	VALUE obj = (VALUE)&page->start[i];
	if (RVALUE_PAGE_UNCOLLECTIBLE(page, obj) && RVALUE_PAGE_WB_UNPROTECTED(page, obj)) has_remembered_shady = TRUE;
	if (RVALUE_PAGE_MARKING(page, obj)) {
	    has_remembered_old = TRUE;
	    rememberd_old_objects++;
	}
    }

    if (!is_incremental_marking(objspace) &&
	page->flags.has_remembered_objects == FALSE && has_remembered_old == TRUE) {

	for (i=0; i<page->total_slots; i++) {
	    VALUE obj = (VALUE)&page->start[i];
	    if (RVALUE_PAGE_MARKING(page, obj)) {
		fprintf(stderr, "marking -> %s\n", obj_info(obj));
	    }
	}
	rb_bug("page %p's has_remembered_objects should be false, but there are remembered old objects (%d). %s",
	       page, rememberd_old_objects, obj ? obj_info(obj) : "");
    }

    if (page->flags.has_uncollectible_shady_objects == FALSE && has_remembered_shady == TRUE) {
	rb_bug("page %p's has_remembered_shady should be false, but there are remembered shady objects. %s",
	       page, obj ? obj_info(obj) : "");
    }

    return rememberd_old_objects;
#else
    return 0;
#endif
}

static int
gc_verify_heap_pages(rb_objspace_t *objspace)
{
    int rememberd_old_objects = 0;
    struct heap_page *page = heap_eden->pages;

    while (page) {
	if (page->flags.has_remembered_objects == FALSE)
	  rememberd_old_objects += gc_verify_heap_page(objspace, page, Qfalse);
	page = page->next;
    }

    return rememberd_old_objects;
}

/*
 *  call-seq:
 *     GC.verify_internal_consistency                  -> nil
 *
 *  Verify internal consistency.
 *
 *  This method is implementation specific.
 *  Now this method checks generational consistency
 *  if RGenGC is supported.
 */
static VALUE
gc_verify_internal_consistency(VALUE dummy)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct verify_internal_consistency_struct data = {0};
    struct each_obj_args eo_args;

    data.objspace = objspace;
    gc_report(5, objspace, "gc_verify_internal_consistency: start\n");

    /* check relations */

    eo_args.callback = verify_internal_consistency_i;
    eo_args.data = (void *)&data;
    objspace_each_objects((VALUE)&eo_args);

    if (data.err_count != 0) {
#if RGENGC_CHECK_MODE >= 5
	objspace->rgengc.error_count = data.err_count;
	gc_marks_check(objspace, NULL, NULL);
	allrefs_dump(objspace);
#endif
	rb_bug("gc_verify_internal_consistency: found internal inconsistency.");
    }

    /* check heap_page status */
    gc_verify_heap_pages(objspace);

    /* check counters */

    if (!is_lazy_sweeping(heap_eden) && !finalizing) {
	if (objspace_live_slots(objspace) != data.live_object_count) {
	    fprintf(stderr, "heap_pages_final_slots: %d, objspace->profile.total_freed_objects: %d\n",
		    (int)heap_pages_final_slots, (int)objspace->profile.total_freed_objects);
	    rb_bug("inconsistent live slot nubmer: expect %"PRIuSIZE", but %"PRIuSIZE".", objspace_live_slots(objspace), data.live_object_count);
	}
    }

#if USE_RGENGC
    if (!is_marking(objspace)) {
	if (objspace->rgengc.old_objects != data.old_object_count) {
	    rb_bug("inconsistent old slot nubmer: expect %"PRIuSIZE", but %"PRIuSIZE".", objspace->rgengc.old_objects, data.old_object_count);
	}
	if (objspace->rgengc.uncollectible_wb_unprotected_objects != data.remembered_shady_count) {
	    rb_bug("inconsistent old slot nubmer: expect %"PRIuSIZE", but %"PRIuSIZE".", objspace->rgengc.uncollectible_wb_unprotected_objects, data.remembered_shady_count);
	}
    }
#endif

    if (!finalizing) {
	size_t list_count = 0;

	{
	    VALUE z = heap_pages_deferred_final;
	    while (z) {
		list_count++;
		z = RZOMBIE(z)->next;
	    }
	}

	if (heap_pages_final_slots != data.zombie_object_count ||
	    heap_pages_final_slots != list_count) {

	    rb_bug("inconsistent finalizing object count:\n"
		   "  expect %"PRIuSIZE"\n"
		   "  but    %"PRIuSIZE" zombies\n"
		   "  heap_pages_deferred_final list has %"PRIuSIZE" items.",
		   heap_pages_final_slots,
		   data.zombie_object_count,
		   list_count);
	}
    }

    gc_report(5, objspace, "gc_verify_internal_consistency: OK\n");

    return Qnil;
}

void
rb_gc_verify_internal_consistency(void)
{
    gc_verify_internal_consistency(Qnil);
}

/* marks */

static void
gc_marks_start(rb_objspace_t *objspace, int full_mark)
{
    /* start marking */
    gc_report(1, objspace, "gc_marks_start: (%s)\n", full_mark ? "full" : "minor");
    gc_stat_transition(objspace, gc_stat_marking);

#if USE_RGENGC
    if (full_mark) {
#if GC_ENABLE_INCREMENTAL_MARK
	objspace->rincgc.step_slots = (objspace->marked_slots * 2) / ((objspace->rincgc.pooled_slots / HEAP_OBJ_LIMIT) + 1);

	if (0) fprintf(stderr, "objspace->marked_slots: %d, objspace->rincgc.pooled_page_num: %d, objspace->rincgc.step_slots: %d, \n",
		       (int)objspace->marked_slots, (int)objspace->rincgc.pooled_slots, (int)objspace->rincgc.step_slots);
#endif
	objspace->flags.during_minor_gc = FALSE;
	objspace->profile.major_gc_count++;
	objspace->rgengc.uncollectible_wb_unprotected_objects = 0;
	objspace->rgengc.old_objects = 0;
	objspace->rgengc.last_major_gc = objspace->profile.count;
	objspace->marked_slots = 0;
	rgengc_mark_and_rememberset_clear(objspace, heap_eden);
    }
    else {
	objspace->flags.during_minor_gc = TRUE;
	objspace->marked_slots =
	  objspace->rgengc.old_objects + objspace->rgengc.uncollectible_wb_unprotected_objects; /* uncollectible objects are marked already */
	objspace->profile.minor_gc_count++;
	rgengc_rememberset_mark(objspace, heap_eden);
    }
#endif

    gc_mark_roots(objspace, NULL);

    gc_report(1, objspace, "gc_marks_start: (%s) end, stack in %d\n", full_mark ? "full" : "minor", (int)mark_stack_size(&objspace->mark_stack));
}

#if GC_ENABLE_INCREMENTAL_MARK
static void
gc_marks_wb_unprotected_objects(rb_objspace_t *objspace)
{
    struct heap_page *page = heap_eden->pages;

    while (page) {
	bits_t *mark_bits = page->mark_bits;
	bits_t *wbun_bits = page->wb_unprotected_bits;
	RVALUE *p = page->start;
	RVALUE *offset = p - NUM_IN_PAGE(p);
	size_t j;

	for (j=0; j<HEAP_BITMAP_LIMIT; j++) {
	    bits_t bits = mark_bits[j] & wbun_bits[j];

	    if (bits) {
		p = offset  + j * BITS_BITLENGTH;

		do {
		    if (bits & 1) {
			gc_report(2, objspace, "gc_marks_wb_unprotected_objects: marked shady: %s\n", obj_info((VALUE)p));
			if (RGENGC_CHECK_MODE > 0) {
			    assert(RVALUE_WB_UNPROTECTED((VALUE)p));
			    assert(RVALUE_MARKED((VALUE)p));
			}
			gc_mark_children(objspace, (VALUE)p);
		    }
		    p++;
		    bits >>= 1;
		} while (bits);
	    }
	}

	page = page->next;
    }

    gc_mark_stacked_objects_all(objspace);
}

static struct heap_page *
heap_move_pooled_pages_to_free_pages(rb_heap_t *heap)
{
    struct heap_page *page = heap->pooled_pages;

    if (page) {
	heap->pooled_pages = page->free_next;
	page->free_next = heap->free_pages;
	heap->free_pages = page;
    }

    return page;
}
#endif

static int
gc_marks_finish(rb_objspace_t *objspace)
{
#if GC_ENABLE_INCREMENTAL_MARK
    /* finish incremental GC */
    if (is_incremental_marking(objspace)) {
	if (heap_eden->pooled_pages) {
	    heap_move_pooled_pages_to_free_pages(heap_eden);
	    gc_report(1, objspace, "gc_marks_finish: pooled pages are exists. retry.\n");
	    return FALSE; /* continue marking phase */
	}

	if (RGENGC_CHECK_MODE && is_mark_stack_empty(&objspace->mark_stack) == 0) {
	    rb_bug("gc_marks_finish: mark stack is not empty (%d).", (int)mark_stack_size(&objspace->mark_stack));
	}

	gc_mark_roots(objspace, 0);

	if (is_mark_stack_empty(&objspace->mark_stack) == FALSE) {
	    gc_report(1, objspace, "gc_marks_finish: not empty (%d). retry.\n", (int)mark_stack_size(&objspace->mark_stack));
	    return FALSE;
	}

#if RGENGC_CHECK_MODE >= 2
	if (gc_verify_heap_pages(objspace) != 0) {
	    rb_bug("gc_marks_finish (incremental): there are remembered old objects.");
	}
#endif

	objspace->flags.during_incremental_marking = FALSE;
	/* check children of all marked wb-unprotected objects */
	gc_marks_wb_unprotected_objects(objspace);
    }
#endif /* GC_ENABLE_INCREMENTAL_MARK */

#if RGENGC_CHECK_MODE >= 2
    gc_verify_internal_consistency(Qnil);
#endif

#if USE_RGENGC
    if (is_full_marking(objspace)) {
	/* See the comment about RUBY_GC_HEAP_OLDOBJECT_LIMIT_FACTOR */
	const double r = gc_params.oldobject_limit_factor;
	objspace->rgengc.uncollectible_wb_unprotected_objects_limit = (size_t)(objspace->rgengc.uncollectible_wb_unprotected_objects * r);
	objspace->rgengc.old_objects_limit = (size_t)(objspace->rgengc.old_objects * r);
    }
#endif

#if RGENGC_CHECK_MODE >= 4
    gc_marks_check(objspace, gc_check_after_marks_i, "after_marks");
#endif

    {   /* decide full GC is needed or not */
	rb_heap_t *heap = heap_eden;
	size_t sweep_slots =
	  (heap_allocatable_pages * HEAP_OBJ_LIMIT) +   /* allocatable slots in empty pages */
	  (heap->total_slots - objspace->marked_slots); /* will be sweep slots */

#if RGENGC_CHECK_MODE
	assert(heap->total_slots >= objspace->marked_slots);
#endif

	if (sweep_slots < heap_pages_min_free_slots) {
#if USE_RGENGC
	    if (!is_full_marking(objspace) && objspace->profile.count - objspace->rgengc.last_major_gc > 3 /* magic number */) {
		gc_report(1, objspace, "gc_marks_finish: next is full GC!!)\n");
		objspace->rgengc.need_major_gc |= GPR_FLAG_MAJOR_BY_NOFREE;
	    }
	    else {
		gc_report(1, objspace, "gc_marks_finish: heap_set_increment!!\n");
		heap_set_increment(objspace, heap_extend_pages(objspace));
		heap_increment(objspace, heap);
	    }
#else
	    gc_report(1, objspace, "gc_marks_finish: heap_set_increment!!\n");
	    heap_set_increment(objspace, heap_extend_pages(objspace));
	    heap_increment(objspace, heap);
#endif
	}

#if USE_RGENGC
	if (objspace->rgengc.uncollectible_wb_unprotected_objects > objspace->rgengc.uncollectible_wb_unprotected_objects_limit) {
	    objspace->rgengc.need_major_gc |= GPR_FLAG_MAJOR_BY_SHADY;
	}
	if (objspace->rgengc.old_objects > objspace->rgengc.old_objects_limit) {
	    objspace->rgengc.need_major_gc |= GPR_FLAG_MAJOR_BY_OLDGEN;
	}
	if (RGENGC_FORCE_MAJOR_GC) {
	    objspace->rgengc.need_major_gc = GPR_FLAG_MAJOR_BY_FORCE;
	}

	gc_report(1, objspace, "gc_marks_finish (marks %d objects, old %d objects, total %d slots, sweep %d slots, increment: %d, next GC: %s)\n",
		  (int)objspace->marked_slots, (int)objspace->rgengc.old_objects, (int)heap->total_slots, (int)sweep_slots, (int)heap_allocatable_pages,
		  objspace->rgengc.need_major_gc ? "major" : "minor");
#endif
    }

    gc_event_hook(objspace, RUBY_INTERNAL_EVENT_GC_END_MARK, 0);

    return TRUE;
}

#if GC_ENABLE_INCREMENTAL_MARK
static void
gc_marks_step(rb_objspace_t *objspace, int slots)
{
    if (RGENGC_CHECK_MODE) assert(is_marking(objspace));

    if (gc_mark_stacked_objects_incremental(objspace, slots)) {
	if (gc_marks_finish(objspace)) {
	    /* finish */
	    gc_sweep(objspace);
	}
    }
    if (0) fprintf(stderr, "objspace->marked_slots: %d\n", (int)objspace->marked_slots);
}
#endif

static void
gc_marks_rest(rb_objspace_t *objspace)
{
    gc_report(1, objspace, "gc_marks_rest\n");

#if GC_ENABLE_INCREMENTAL_MARK
    heap_eden->pooled_pages = NULL;
#endif

    if (is_incremental_marking(objspace)) {
	do {
	    while (gc_mark_stacked_objects_incremental(objspace, INT_MAX) == FALSE);
	} while (gc_marks_finish(objspace) == FALSE);
    }
    else {
	gc_mark_stacked_objects_all(objspace);
	gc_marks_finish(objspace);
    }

    /* move to sweep */
    gc_sweep(objspace);
}

#if GC_ENABLE_INCREMENTAL_MARK
static void
gc_marks_continue(rb_objspace_t *objspace, rb_heap_t *heap)
{
    int slots = 0;
    const char *from;

    if (RGENGC_CHECK_MODE) assert(dont_gc == FALSE);

    gc_enter(objspace, "marks_continue");

    PUSH_MARK_FUNC_DATA(NULL);
    {
	if (heap->pooled_pages) {
	    while (heap->pooled_pages && slots < HEAP_OBJ_LIMIT) {
		struct heap_page *page = heap_move_pooled_pages_to_free_pages(heap);
		slots += page->free_slots;
	    }
	    from = "pooled-pages";
	}
	else if (heap_increment(objspace, heap)) {
	    slots = heap->free_pages->free_slots;
	    from = "incremented-pages";
	}

	if (slots > 0) {
	    gc_report(2, objspace, "gc_marks_continue: provide %d slots from %s.\n", slots, from);
	    gc_marks_step(objspace, (int)objspace->rincgc.step_slots);
	}
	else {
	    gc_report(2, objspace, "gc_marks_continue: no more pooled pages (stack depth: %d).\n", (int)mark_stack_size(&objspace->mark_stack));
	    gc_marks_rest(objspace);
	}
    }
    POP_MARK_FUNC_DATA();

    gc_exit(objspace, "marks_continue");
}
#endif

static void
gc_marks(rb_objspace_t *objspace, int full_mark)
{
    gc_prof_mark_timer_start(objspace);

    PUSH_MARK_FUNC_DATA(NULL);
    {
	/* setup marking */

#if USE_RGENGC
	gc_marks_start(objspace, full_mark);
	if (!is_incremental_marking(objspace)) {
	    gc_marks_rest(objspace);
	}

#if RGENGC_PROFILE > 0
	if (gc_prof_record(objspace)) {
	    gc_profile_record *record = gc_prof_record(objspace);
	    record->old_objects = objspace->rgengc.old_objects;
	}
#endif

#else /* USE_RGENGC */
	gc_marks_start(objspace, TRUE);
	gc_marks_rest(objspace);
#endif
    }
    POP_MARK_FUNC_DATA();
    gc_prof_mark_timer_stop(objspace);
}

/* RGENGC */

static void
gc_report_body(int level, rb_objspace_t *objspace, const char *fmt, ...)
{
    if (level <= RGENGC_DEBUG) {
	char buf[1024];
	FILE *out = stderr;
	va_list args;
	const char *status = " ";

#if USE_RGENGC
	if (during_gc) {
	    status = is_full_marking(objspace) ? "+" : "-";
	}
	else {
	    if (is_lazy_sweeping(heap_eden)) {
		status = "S";
	    }
	    if (is_incremental_marking(objspace)) {
		status = "M";
	    }
	}
#endif

	va_start(args, fmt);
	vsnprintf(buf, 1024, fmt, args);
	va_end(args);

	fprintf(out, "%s|", status);
	fputs(buf, out);
    }
}

#if USE_RGENGC

/* bit operations */

static int
rgengc_remembersetbits_get(rb_objspace_t *objspace, VALUE obj)
{
    return RVALUE_REMEMBERED(obj);
}

static int
rgengc_remembersetbits_set(rb_objspace_t *objspace, VALUE obj)
{
    struct heap_page *page = GET_HEAP_PAGE(obj);
    bits_t *bits = &page->marking_bits[0];

    if (RGENGC_CHECK_MODE) assert(!is_incremental_marking(objspace));

    if (MARKED_IN_BITMAP(bits, obj)) {
	return FALSE;
    }
    else {
	page->flags.has_remembered_objects = TRUE;
	MARK_IN_BITMAP(bits, obj);
	return TRUE;
    }
}

/* wb, etc */

/* return FALSE if already remembered */
static int
rgengc_remember(rb_objspace_t *objspace, VALUE obj)
{
    gc_report(6, objspace, "rgengc_remember: %s %s\n", obj_info(obj),
	      rgengc_remembersetbits_get(objspace, obj) ? "was already remembered" : "is remembered now");

    check_rvalue_consistency(obj);

    if (RGENGC_CHECK_MODE) {
	if (RVALUE_WB_UNPROTECTED(obj)) rb_bug("rgengc_remember: %s is not wb protected.", obj_info(obj));
    }

#if RGENGC_PROFILE > 0
    if (!rgengc_remembered(objspace, obj)) {
	if (RVALUE_WB_UNPROTECTED(obj) == 0) {
	    objspace->profile.total_remembered_normal_object_count++;
#if RGENGC_PROFILE >= 2
	    objspace->profile.remembered_normal_object_count_types[BUILTIN_TYPE(obj)]++;
#endif
	}
    }
#endif /* RGENGC_PROFILE > 0 */

    return rgengc_remembersetbits_set(objspace, obj);
}

static int
rgengc_remembered(rb_objspace_t *objspace, VALUE obj)
{
    int result = rgengc_remembersetbits_get(objspace, obj);
    check_rvalue_consistency(obj);
    gc_report(6, objspace, "rgengc_remembered: %s\n", obj_info(obj));
    return result;
}

#ifndef PROFILE_REMEMBERSET_MARK
#define PROFILE_REMEMBERSET_MARK 0
#endif

static void
rgengc_rememberset_mark(rb_objspace_t *objspace, rb_heap_t *heap)
{
    size_t j;
    struct heap_page *page = heap->pages;
#if PROFILE_REMEMBERSET_MARK
    int has_old = 0, has_shady = 0, has_both = 0, skip = 0;
#endif
    gc_report(1, objspace, "rgengc_rememberset_mark: start\n");

    while (page) {
	if (page->flags.has_remembered_objects | page->flags.has_uncollectible_shady_objects) {
	    RVALUE *p = page->start;
	    RVALUE *offset = p - NUM_IN_PAGE(p);
	    bits_t bitset, bits[HEAP_BITMAP_LIMIT];
	    bits_t *marking_bits = page->marking_bits;
	    bits_t *uncollectible_bits = page->uncollectible_bits;
	    bits_t *wb_unprotected_bits = page->wb_unprotected_bits;
#if PROFILE_REMEMBERSET_MARK
	    if (page->flags.has_remembered_objects && page->flags.has_uncollectible_shady_objects) has_both++;
	    else if (page->flags.has_remembered_objects) has_old++;
	    else if (page->flags.has_uncollectible_shady_objects) has_shady++;
#endif
	    for (j=0; j<HEAP_BITMAP_LIMIT; j++) {
		bits[j] = marking_bits[j] | (uncollectible_bits[j] & wb_unprotected_bits[j]);
		marking_bits[j] = 0;
	    }
	    page->flags.has_remembered_objects = FALSE;

	    for (j=0; j < HEAP_BITMAP_LIMIT; j++) {
		bitset = bits[j];

		if (bitset) {
		    p = offset  + j * BITS_BITLENGTH;

		    do {
			if (bitset & 1) {
			    VALUE obj = (VALUE)p;
			    gc_report(2, objspace, "rgengc_rememberset_mark: mark %s\n", obj_info(obj));

			    if (RGENGC_CHECK_MODE) {
				assert(RVALUE_UNCOLLECTIBLE(obj));
				assert(RVALUE_OLD_P(obj) || RVALUE_WB_UNPROTECTED(obj));
			    }

			    gc_mark_children(objspace, obj);
			}
			p++;
			bitset >>= 1;
		    } while (bitset);
		}
	    }
	}
#if PROFILE_REMEMBERSET_MARK
	else {
	    skip++;
	}
#endif

	page = page->next;
    }

#if PROFILE_REMEMBERSET_MARK
    fprintf(stderr, "%d\t%d\t%d\t%d\n", has_both, has_old, has_shady, skip);
#endif
    gc_report(1, objspace, "rgengc_rememberset_mark: finished\n");
}

static void
rgengc_mark_and_rememberset_clear(rb_objspace_t *objspace, rb_heap_t *heap)
{
    struct heap_page *page = heap->pages;

    while (page) {
	memset(&page->mark_bits[0],       0, HEAP_BITMAP_SIZE);
	memset(&page->marking_bits[0],    0, HEAP_BITMAP_SIZE);
	memset(&page->uncollectible_bits[0], 0, HEAP_BITMAP_SIZE);
	page->flags.has_uncollectible_shady_objects = FALSE;
	page->flags.has_remembered_objects = FALSE;
	page = page->next;
    }
}

/* RGENGC: APIs */

NOINLINE(static void gc_writebarrier_generational(rb_objspace_t *objspace, VALUE a, VALUE b));

static void
gc_writebarrier_generational(rb_objspace_t *objspace, VALUE a, VALUE b)
{
    if (RGENGC_CHECK_MODE) {
	if (!RVALUE_OLD_P(a)) rb_bug("gc_writebarrier_generational: %s is not an old object.", obj_info(a));
	if ( RVALUE_OLD_P(b)) rb_bug("gc_writebarrier_generational: %s is an old object.", obj_info(b));
	if (is_incremental_marking(objspace)) rb_bug("gc_writebarrier_generational: called while incremental marking: %s -> %s", obj_info(a), obj_info(b));
    }

#if 1
    /* mark `a' and remember (default behaviour) */
    if (!rgengc_remembered(objspace, a)) {
	rgengc_remember(objspace, a);
	gc_report(1, objspace, "gc_writebarrier_generational: %s (remembered) -> %s\n", obj_info(a), obj_info(b));
    }
#else
    /* mark `b' and remember */
    MARK_IN_BITMAP(GET_HEAP_MARK_BITS(b), b);
    if (RVALUE_WB_UNPROTECTED(b)) {
	gc_remember_unprotected(objspace, b);
    }
    else {
	RVALUE_AGE_SET_OLD(objspace, b);
	rgengc_remember(objspace, b);
    }

    gc_report(1, objspace, "gc_writebarrier_generational: %s -> %s (remembered)\n", obj_info(a), obj_info(b));
#endif

    check_rvalue_consistency(a);
    check_rvalue_consistency(b);
}

#if GC_ENABLE_INCREMENTAL_MARK
static void
gc_mark_from(rb_objspace_t *objspace, VALUE obj, VALUE parent)
{
    gc_mark_set_parent(objspace, parent);
    rgengc_check_relation(objspace, obj);
    if (gc_mark_set(objspace, obj) == FALSE) return;
    gc_aging(objspace, obj);
    gc_grey(objspace, obj);
}

NOINLINE(static void gc_writebarrier_incremental(rb_objspace_t *objspace, VALUE a, VALUE b));

static void
gc_writebarrier_incremental(rb_objspace_t *objspace, VALUE a, VALUE b)
{
    gc_report(2, objspace, "gc_writebarrier_incremental: [LG] %s -> %s\n", obj_info(a), obj_info(b));

    if (RVALUE_BLACK_P(a)) {
	if (RVALUE_WHITE_P(b)) {
	    if (!RVALUE_WB_UNPROTECTED(a)) {
		gc_report(2, objspace, "gc_writebarrier_incremental: [IN] %s -> %s\n", obj_info(a), obj_info(b));
		gc_mark_from(objspace, b, a);
	    }
	}
	else if (RVALUE_OLD_P(a) && !RVALUE_OLD_P(b)) {
	    if (!RVALUE_WB_UNPROTECTED(b)) {
		gc_report(1, objspace, "gc_writebarrier_incremental: [GN] %s -> %s\n", obj_info(a), obj_info(b));
		RVALUE_AGE_SET_OLD(objspace, b);

		if (RVALUE_BLACK_P(b)) {
		    gc_grey(objspace, b);
		}
	    }
	    else {
		gc_report(1, objspace, "gc_writebarrier_incremental: [LL] %s -> %s\n", obj_info(a), obj_info(b));
		gc_remember_unprotected(objspace, b);
	    }
	}
    }
}
#else
#define gc_writebarrier_incremental(objspace, a, b)
#endif

void
rb_gc_writebarrier(VALUE a, VALUE b)
{
    rb_objspace_t *objspace = &rb_objspace;

    if (RGENGC_CHECK_MODE && SPECIAL_CONST_P(a)) rb_bug("rb_gc_writebarrier: a is special const");
    if (RGENGC_CHECK_MODE && SPECIAL_CONST_P(b)) rb_bug("rb_gc_writebarrier: b is special const");

    if (LIKELY(!is_incremental_marking(objspace))) {
	if (!RVALUE_OLD_P(a) || RVALUE_OLD_P(b)) {
	    return;
	}
	else {
	    gc_writebarrier_generational(objspace, a, b);
	}
    }
    else { /* slow path */
	gc_writebarrier_incremental(objspace, a, b);
    }
}

void
rb_gc_writebarrier_unprotect(VALUE obj)
{
    if (RVALUE_WB_UNPROTECTED(obj)) {
	return;
    }
    else {
	rb_objspace_t *objspace = &rb_objspace;

	gc_report(2, objspace, "rb_gc_writebarrier_unprotect: %s %s\n", obj_info(obj),
		  rgengc_remembered(objspace, obj) ? " (already remembered)" : "");

	if (RVALUE_OLD_P(obj)) {
	    gc_report(1, objspace, "rb_gc_writebarrier_unprotect: %s\n", obj_info(obj));
	    RVALUE_DEMOTE(objspace, obj);
	    gc_mark_set(objspace, obj);
	    gc_remember_unprotected(objspace, obj);

#if RGENGC_PROFILE
	    objspace->profile.total_shade_operation_count++;
#if RGENGC_PROFILE >= 2
	    objspace->profile.shade_operation_count_types[BUILTIN_TYPE(obj)]++;
#endif /* RGENGC_PROFILE >= 2 */
#endif /* RGENGC_PROFILE */
	}
	else {
	    RVALUE_AGE_RESET(obj);
	}

	MARK_IN_BITMAP(GET_HEAP_WB_UNPROTECTED_BITS(obj), obj);
    }
}

/*
 * remember `obj' if needed.
 */
void
rb_gc_writebarrier_remember(VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;

    gc_report(1, objspace, "rb_gc_writebarrier_remember: %s\n", obj_info(obj));

    if (is_incremental_marking(objspace)) {
	if (RVALUE_BLACK_P(obj)) {
	    gc_grey(objspace, obj);
	}
    }
    else {
	if (RVALUE_OLD_P(obj)) {
	    rgengc_remember(objspace, obj);
	}
    }
}

static st_table *rgengc_unprotect_logging_table;

static int
rgengc_unprotect_logging_exit_func_i(st_data_t key, st_data_t val, st_data_t arg)
{
    fprintf(stderr, "%s\t%d\n", (char *)key, (int)val);
    return ST_CONTINUE;
}

static void
rgengc_unprotect_logging_exit_func(void)
{
    st_foreach(rgengc_unprotect_logging_table, rgengc_unprotect_logging_exit_func_i, 0);
}

void
rb_gc_unprotect_logging(void *objptr, const char *filename, int line)
{
    VALUE obj = (VALUE)objptr;

    if (rgengc_unprotect_logging_table == 0) {
	rgengc_unprotect_logging_table = st_init_strtable();
	atexit(rgengc_unprotect_logging_exit_func);
    }

    if (RVALUE_WB_UNPROTECTED(obj) == 0) {
	char buff[0x100];
	st_data_t cnt = 1;
	char *ptr = buff;

	snprintf(ptr, 0x100 - 1, "%s|%s:%d", obj_info(obj), filename, line);

	if (st_lookup(rgengc_unprotect_logging_table, (st_data_t)ptr, &cnt)) {
	    cnt++;
	}
	else {
	    ptr = (char *)malloc(strlen(buff) + 1);
	    strcpy(ptr, buff);
	}
	st_insert(rgengc_unprotect_logging_table, (st_data_t)ptr, cnt);
    }
}
#endif /* USE_RGENGC */

void
rb_copy_wb_protected_attribute(VALUE dest, VALUE obj)
{
#if USE_RGENGC
    rb_objspace_t *objspace = &rb_objspace;

    if (RVALUE_WB_UNPROTECTED(obj) && !RVALUE_WB_UNPROTECTED(dest)) {
	if (!RVALUE_OLD_P(dest)) {
	    MARK_IN_BITMAP(GET_HEAP_WB_UNPROTECTED_BITS(dest), dest);
	    RVALUE_AGE_RESET_RAW(dest);
	}
	else {
	    RVALUE_DEMOTE(objspace, dest);
	}
    }

    check_rvalue_consistency(dest);
#endif
}

/* RGENGC analysis information */

VALUE
rb_obj_rgengc_writebarrier_protected_p(VALUE obj)
{
#if USE_RGENGC
    return RVALUE_WB_UNPROTECTED(obj) ? Qfalse : Qtrue;
#else
    return Qfalse;
#endif
}

VALUE
rb_obj_rgengc_promoted_p(VALUE obj)
{
    return OBJ_PROMOTED(obj) ? Qtrue : Qfalse;
}

size_t
rb_obj_gc_flags(VALUE obj, ID* flags, size_t max)
{
    size_t n = 0;
    static ID ID_marked;
#if USE_RGENGC
    static ID ID_wb_protected, ID_old, ID_marking, ID_uncollectible;
#endif

    if (!ID_marked) {
#define I(s) ID_##s = rb_intern(#s);
	I(marked);
#if USE_RGENGC
	I(wb_protected);
	I(old);
	I(marking);
	I(uncollectible);
#endif
#undef I
    }

#if USE_RGENGC
    if (RVALUE_WB_UNPROTECTED(obj) == 0 && n<max)                   flags[n++] = ID_wb_protected;
    if (RVALUE_OLD_P(obj) && n<max)                                 flags[n++] = ID_old;
    if (RVALUE_UNCOLLECTIBLE(obj) && n<max)                         flags[n++] = ID_uncollectible;
    if (MARKED_IN_BITMAP(GET_HEAP_MARKING_BITS(obj), obj) && n<max) flags[n++] = ID_marking;
#endif
    if (MARKED_IN_BITMAP(GET_HEAP_MARK_BITS(obj), obj) && n<max)    flags[n++] = ID_marked;
    return n;
}

/* GC */

void
rb_gc_force_recycle(VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;

#if USE_RGENGC
    int is_old = RVALUE_OLD_P(obj);

    gc_report(2, objspace, "rb_gc_force_recycle: %s\n", obj_info(obj));

    if (is_old) {
	if (RVALUE_MARKED(obj)) {
	    objspace->rgengc.old_objects--;
	}
    }
    CLEAR_IN_BITMAP(GET_HEAP_UNCOLLECTIBLE_BITS(obj), obj);
    CLEAR_IN_BITMAP(GET_HEAP_WB_UNPROTECTED_BITS(obj), obj);

#if GC_ENABLE_INCREMENTAL_MARK
    if (is_incremental_marking(objspace)) {
	if (MARKED_IN_BITMAP(GET_HEAP_MARKING_BITS(obj), obj)) {
	    invalidate_mark_stack(&objspace->mark_stack, obj);
	    CLEAR_IN_BITMAP(GET_HEAP_MARKING_BITS(obj), obj);
	}
	CLEAR_IN_BITMAP(GET_HEAP_MARK_BITS(obj), obj);
    }
    else {
#endif
	if (is_old || !GET_HEAP_PAGE(obj)->flags.before_sweep) {
	    CLEAR_IN_BITMAP(GET_HEAP_MARK_BITS(obj), obj);
	}
	CLEAR_IN_BITMAP(GET_HEAP_MARKING_BITS(obj), obj);
#if GC_ENABLE_INCREMENTAL_MARK
    }
#endif
#endif

    objspace->profile.total_freed_objects++;

    heap_page_add_freeobj(objspace, GET_HEAP_PAGE(obj), obj);

    /* Disable counting swept_slots because there are no meaning.
     * if (!MARKED_IN_BITMAP(GET_HEAP_MARK_BITS(p), p)) {
     *   objspace->heap.swept_slots++;
     * }
     */
}

#ifndef MARK_OBJECT_ARY_BUCKET_SIZE
#define MARK_OBJECT_ARY_BUCKET_SIZE 1024
#endif

void
rb_gc_register_mark_object(VALUE obj)
{
    VALUE ary_ary = GET_THREAD()->vm->mark_object_ary;
    VALUE ary = rb_ary_last(0, 0, ary_ary);

    if (ary == Qnil || RARRAY_LEN(ary) >= MARK_OBJECT_ARY_BUCKET_SIZE) {
	ary = rb_ary_tmp_new(MARK_OBJECT_ARY_BUCKET_SIZE);
	rb_ary_push(ary_ary, ary);
    }

    rb_ary_push(ary, obj);
}

void
rb_gc_register_address(VALUE *addr)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct gc_list *tmp;

    tmp = ALLOC(struct gc_list);
    tmp->next = global_list;
    tmp->varptr = addr;
    global_list = tmp;
}

void
rb_gc_unregister_address(VALUE *addr)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct gc_list *tmp = global_list;

    if (tmp->varptr == addr) {
	global_list = tmp->next;
	xfree(tmp);
	return;
    }
    while (tmp->next) {
	if (tmp->next->varptr == addr) {
	    struct gc_list *t = tmp->next;

	    tmp->next = tmp->next->next;
	    xfree(t);
	    break;
	}
	tmp = tmp->next;
    }
}

void
rb_global_variable(VALUE *var)
{
    rb_gc_register_address(var);
}

#define GC_NOTIFY 0

enum {
    gc_stress_no_major,
    gc_stress_no_immediate_sweep,
    gc_stress_full_mark_after_malloc,
    gc_stress_max
};

#define gc_stress_full_mark_after_malloc_p() \
    (FIXNUM_P(ruby_gc_stress_mode) && (FIX2LONG(ruby_gc_stress_mode) & (1<<gc_stress_full_mark_after_malloc)))

static void
heap_ready_to_gc(rb_objspace_t *objspace, rb_heap_t *heap)
{
    if (!heap->freelist && !heap->free_pages) {
	if (!heap_increment(objspace, heap)) {
	    heap_set_increment(objspace, 1);
	    heap_increment(objspace, heap);
	}
    }
}

static int
ready_to_gc(rb_objspace_t *objspace)
{
    if (dont_gc || during_gc || ruby_disable_gc) {
	heap_ready_to_gc(objspace, heap_eden);
	return FALSE;
    }
    else {
	return TRUE;
    }
}

static void
gc_reset_malloc_info(rb_objspace_t *objspace)
{
    gc_prof_set_malloc_info(objspace);
    {
	size_t inc = ATOMIC_SIZE_EXCHANGE(malloc_increase, 0);
	size_t old_limit = malloc_limit;

	if (inc > malloc_limit) {
	    malloc_limit = (size_t)(inc * gc_params.malloc_limit_growth_factor);
	    if (gc_params.malloc_limit_max > 0 && /* ignore max-check if 0 */
		malloc_limit > gc_params.malloc_limit_max) {
		malloc_limit = gc_params.malloc_limit_max;
	    }
	}
	else {
	    malloc_limit = (size_t)(malloc_limit * 0.98); /* magic number */
	    if (malloc_limit < gc_params.malloc_limit_min) {
		malloc_limit = gc_params.malloc_limit_min;
	    }
	}

	if (0) {
	    if (old_limit != malloc_limit) {
		fprintf(stderr, "[%"PRIuSIZE"] malloc_limit: %"PRIuSIZE" -> %"PRIuSIZE"\n",
			rb_gc_count(), old_limit, malloc_limit);
	    }
	    else {
		fprintf(stderr, "[%"PRIuSIZE"] malloc_limit: not changed (%"PRIuSIZE")\n",
			rb_gc_count(), malloc_limit);
	    }
	}
    }

    /* reset oldmalloc info */
#if RGENGC_ESTIMATE_OLDMALLOC
    if (!is_full_marking(objspace)) {
	if (objspace->rgengc.oldmalloc_increase > objspace->rgengc.oldmalloc_increase_limit) {
	    objspace->rgengc.need_major_gc |= GPR_FLAG_MAJOR_BY_OLDMALLOC;;
	    objspace->rgengc.oldmalloc_increase_limit =
	      (size_t)(objspace->rgengc.oldmalloc_increase_limit * gc_params.oldmalloc_limit_growth_factor);

	    if (objspace->rgengc.oldmalloc_increase_limit > gc_params.oldmalloc_limit_max) {
		objspace->rgengc.oldmalloc_increase_limit = gc_params.oldmalloc_limit_max;
	    }
	}

	if (0) fprintf(stderr, "%d\t%d\t%u\t%u\t%d\n",
		       (int)rb_gc_count(),
		       (int)objspace->rgengc.need_major_gc,
		       (unsigned int)objspace->rgengc.oldmalloc_increase,
		       (unsigned int)objspace->rgengc.oldmalloc_increase_limit,
		       (unsigned int)gc_params.oldmalloc_limit_max);
    }
    else {
	/* major GC */
	objspace->rgengc.oldmalloc_increase = 0;

	if ((objspace->profile.latest_gc_info & GPR_FLAG_MAJOR_BY_OLDMALLOC) == 0) {
	    objspace->rgengc.oldmalloc_increase_limit =
	      (size_t)(objspace->rgengc.oldmalloc_increase_limit / ((gc_params.oldmalloc_limit_growth_factor - 1)/10 + 1));
	    if (objspace->rgengc.oldmalloc_increase_limit < gc_params.oldmalloc_limit_min) {
		objspace->rgengc.oldmalloc_increase_limit = gc_params.oldmalloc_limit_min;
	    }
	}
    }
#endif
}

static int
garbage_collect(rb_objspace_t *objspace, int full_mark, int immediate_mark, int immediate_sweep, int reason)
{
#if GC_PROFILE_MORE_DETAIL
    objspace->profile.prepare_time = getrusage_time();
#endif

    gc_rest(objspace);

#if GC_PROFILE_MORE_DETAIL
    objspace->profile.prepare_time = getrusage_time() - objspace->profile.prepare_time;
#endif

    return gc_start(objspace, full_mark, immediate_mark, immediate_sweep, reason);
}

static int
gc_start(rb_objspace_t *objspace, const int full_mark, const int immediate_mark, const unsigned int immediate_sweep, int reason)
{
    int do_full_mark = full_mark;
    objspace->flags.immediate_sweep = immediate_sweep;

    if (!heap_allocated_pages) return FALSE;      /* heap is not ready */
    if (!ready_to_gc(objspace)) return TRUE; /* GC is not allowed */

    if (RGENGC_CHECK_MODE) {
	assert(objspace->flags.stat == gc_stat_none);
	assert(!is_lazy_sweeping(heap_eden));
	assert(!is_incremental_marking(objspace));
#if RGENGC_CHECK_MODE >= 2
	gc_verify_internal_consistency(Qnil);
#endif
    }

    gc_enter(objspace, "gc_start");

    if (ruby_gc_stressful) {
	int flag = FIXNUM_P(ruby_gc_stress_mode) ? FIX2INT(ruby_gc_stress_mode) : 0;

	if ((flag & (1<<gc_stress_no_major)) == 0) {
	    do_full_mark = TRUE;
	}

	objspace->flags.immediate_sweep = !(flag & (1<<gc_stress_no_immediate_sweep));
    }
    else {
#if USE_RGENGC
	if (objspace->rgengc.need_major_gc) {
	    reason |= objspace->rgengc.need_major_gc;
	    do_full_mark = TRUE;
	}
	else if (RGENGC_FORCE_MAJOR_GC) {
	    reason = GPR_FLAG_MAJOR_BY_FORCE;
	    do_full_mark = TRUE;
	}

	objspace->rgengc.need_major_gc = GPR_FLAG_NONE;
#endif
    }

    if (do_full_mark && (reason & GPR_FLAG_MAJOR_MASK) == 0) {
	reason |= GPR_FLAG_MAJOR_BY_FORCE; /* GC by CAPI, METHOD, and so on. */
    }

#if GC_ENABLE_INCREMENTAL_MARK
    if (!GC_ENABLE_INCREMENTAL_MARK || objspace->flags.dont_incremental || immediate_mark) {
	objspace->flags.during_incremental_marking = FALSE;
    }
    else {
	objspace->flags.during_incremental_marking = do_full_mark;
    }
#endif

    if (!GC_ENABLE_LAZY_SWEEP || objspace->flags.dont_incremental) {
	objspace->flags.immediate_sweep = TRUE;
    }

    if (objspace->flags.immediate_sweep) reason |= GPR_FLAG_IMMEDIATE_SWEEP;

    gc_report(1, objspace, "gc_start(%d, %d, %d, reason: %d) => %d, %d, %d\n",
	      full_mark, immediate_mark, immediate_sweep, reason,
	      do_full_mark, !is_incremental_marking(objspace), objspace->flags.immediate_sweep);

    objspace->profile.count++;
    objspace->profile.latest_gc_info = reason;
    objspace->profile.total_allocated_objects_at_gc_start = objspace->total_allocated_objects;
    objspace->profile.heap_used_at_gc_start = heap_allocated_pages;
    gc_prof_setup_new_record(objspace, reason);
    gc_reset_malloc_info(objspace);

    gc_event_hook(objspace, RUBY_INTERNAL_EVENT_GC_START, 0 /* TODO: pass minor/immediate flag? */);
    if (RGENGC_CHECK_MODE) assert(during_gc);

    gc_prof_timer_start(objspace);
    {
	gc_marks(objspace, do_full_mark);
    }
    gc_prof_timer_stop(objspace);

    gc_exit(objspace, "gc_start");
    return TRUE;
}

static void
gc_rest(rb_objspace_t *objspace)
{
    int marking = is_incremental_marking(objspace);
    int sweeping = is_lazy_sweeping(heap_eden);

    if (marking || sweeping) {
	gc_enter(objspace, "gc_rest");

	if (RGENGC_CHECK_MODE >= 2) gc_verify_internal_consistency(Qnil);

	if (is_incremental_marking(objspace)) {
	    PUSH_MARK_FUNC_DATA(NULL);
	    gc_marks_rest(objspace);
	    POP_MARK_FUNC_DATA();
	}
	if (is_lazy_sweeping(heap_eden)) {
	    gc_sweep_rest(objspace);
	}
	gc_exit(objspace, "gc_rest");
    }
}

struct objspace_and_reason {
    rb_objspace_t *objspace;
    int reason;
    int full_mark;
    int immediate_mark;
    int immediate_sweep;
};

static void
gc_current_status_fill(rb_objspace_t *objspace, char *buff)
{
    int i = 0;
    if (is_marking(objspace)) {
	buff[i++] = 'M';
#if USE_RGENGC
	if (is_full_marking(objspace))        buff[i++] = 'F';
#if GC_ENABLE_INCREMENTAL_MARK
	if (is_incremental_marking(objspace)) buff[i++] = 'I';
#endif
#endif
    }
    else if (is_sweeping(objspace)) {
	buff[i++] = 'S';
	if (is_lazy_sweeping(heap_eden))      buff[i++] = 'L';
    }
    else {
	buff[i++] = 'N';
    }
    buff[i] = '\0';
}

static const char *
gc_current_status(rb_objspace_t *objspace)
{
    static char buff[0x10];
    gc_current_status_fill(objspace, buff);
    return buff;
}

#if PRINT_ENTER_EXIT_TICK

static tick_t last_exit_tick;
static tick_t enter_tick;
static int enter_count = 0;
static char last_gc_status[0x10];

static inline void
gc_record(rb_objspace_t *objspace, int direction, const char *event)
{
    if (direction == 0) { /* enter */
	enter_count++;
	enter_tick = tick();
	gc_current_status_fill(objspace, last_gc_status);
    }
    else { /* exit */
	tick_t exit_tick = tick();
	char current_gc_status[0x10];
	gc_current_status_fill(objspace, current_gc_status);
#if 1
	/* [last mutator time] [gc time] [event] */
	fprintf(stderr, "%"PRItick"\t%"PRItick"\t%s\t[%s->%s|%c]\n",
		enter_tick - last_exit_tick,
		exit_tick - enter_tick,
		event,
		last_gc_status, current_gc_status,
		(objspace->profile.latest_gc_info & GPR_FLAG_MAJOR_MASK) ? '+' : '-');
	last_exit_tick = exit_tick;
#else
	/* [enter_tick] [gc time] [event] */
	fprintf(stderr, "%"PRItick"\t%"PRItick"\t%s\t[%s->%s|%c]\n",
		enter_tick,
		exit_tick - enter_tick,
		event,
		last_gc_status, current_gc_status,
		(objspace->profile.latest_gc_info & GPR_FLAG_MAJOR_MASK) ? '+' : '-');
#endif
    }
}
#else /* PRINT_ENTER_EXIT_TICK */
static inline void
gc_record(rb_objspace_t *objspace, int direction, const char *event)
{
    /* null */
}
#endif /* PRINT_ENTER_EXIT_TICK */

static inline void
gc_enter(rb_objspace_t *objspace, const char *event)
{
    if (RGENGC_CHECK_MODE) assert(during_gc == 0);
    if (RGENGC_CHECK_MODE >= 3) gc_verify_internal_consistency(Qnil);

    during_gc = TRUE;
    gc_report(1, objspace, "gc_entr: %s [%s]\n", event, gc_current_status(objspace));
    gc_record(objspace, 0, event);
    gc_event_hook(objspace, RUBY_INTERNAL_EVENT_GC_ENTER, 0); /* TODO: which parameter should be passed? */
}

static inline void
gc_exit(rb_objspace_t *objspace, const char *event)
{
    if (RGENGC_CHECK_MODE) assert(during_gc != 0);

    gc_event_hook(objspace, RUBY_INTERNAL_EVENT_GC_EXIT, 0); /* TODO: which parameter should be passsed? */
    gc_record(objspace, 1, event);
    gc_report(1, objspace, "gc_exit: %s [%s]\n", event, gc_current_status(objspace));
    during_gc = FALSE;
}

static void *
gc_with_gvl(void *ptr)
{
    struct objspace_and_reason *oar = (struct objspace_and_reason *)ptr;
    return (void *)(VALUE)garbage_collect(oar->objspace, oar->full_mark, oar->immediate_mark, oar->immediate_sweep, oar->reason);
}

static int
garbage_collect_with_gvl(rb_objspace_t *objspace, int full_mark, int immediate_mark, int immediate_sweep, int reason)
{
    if (dont_gc) return TRUE;
    if (ruby_thread_has_gvl_p()) {
	return garbage_collect(objspace, full_mark, immediate_mark, immediate_sweep, reason);
    }
    else {
	if (ruby_native_thread_p()) {
	    struct objspace_and_reason oar;
	    oar.objspace = objspace;
	    oar.reason = reason;
	    oar.full_mark = full_mark;
	    oar.immediate_mark = immediate_mark;
	    oar.immediate_sweep = immediate_sweep;
	    return (int)(VALUE)rb_thread_call_with_gvl(gc_with_gvl, (void *)&oar);
	}
	else {
	    /* no ruby thread */
	    fprintf(stderr, "[FATAL] failed to allocate memory\n");
	    exit(EXIT_FAILURE);
	}
    }
}

int
rb_garbage_collect(void)
{
    return garbage_collect(&rb_objspace, TRUE, TRUE, TRUE, GPR_FLAG_CAPI);
}

#undef Init_stack

void
Init_stack(volatile VALUE *addr)
{
    ruby_init_stack(addr);
}

/*
 *  call-seq:
 *     GC.start                     -> nil
 *     GC.garbage_collect           -> nil
 *     GC.start(full_mark: true, immediate_sweep: true)           -> nil
 *     GC.garbage_collect(full_mark: true, immediate_sweep: true) -> nil
 *
 *  Initiates garbage collection, unless manually disabled.
 *
 *  This method is defined with keyword arguments that default to true:
 *
 *     def GC.start(full_mark: true, immediate_sweep: true); end
 *
 *  Use full_mark: false to perform a minor GC.
 *  Use immediate_sweep: false to defer sweeping (use lazy sweep).
 *
 *  Note: These keyword arguments are implementation and version dependent. They
 *  are not guaranteed to be future-compatible, and may be ignored if the
 *  underlying implementation does not support them.
 */

static VALUE
gc_start_internal(int argc, VALUE *argv, VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    int full_mark = TRUE, immediate_mark = TRUE, immediate_sweep = TRUE;
    VALUE opt = Qnil;
    static ID keyword_ids[3];

    rb_scan_args(argc, argv, "0:", &opt);

    if (!NIL_P(opt)) {
	VALUE kwvals[3];

	if (!keyword_ids[0]) {
	    keyword_ids[0] = rb_intern("full_mark");
	    keyword_ids[1] = rb_intern("immediate_mark");
	    keyword_ids[2] = rb_intern("immediate_sweep");
	}

	rb_get_kwargs(opt, keyword_ids, 0, 3, kwvals);

	if (kwvals[0] != Qundef) full_mark = RTEST(kwvals[0]);
	if (kwvals[1] != Qundef) immediate_mark = RTEST(kwvals[1]);
	if (kwvals[2] != Qundef) immediate_sweep = RTEST(kwvals[2]);
    }

    garbage_collect(objspace, full_mark, immediate_mark, immediate_sweep, GPR_FLAG_METHOD);
    if (!finalizing) finalize_deferred(objspace);

    return Qnil;
}

VALUE
rb_gc_start(void)
{
    rb_gc();
    return Qnil;
}

void
rb_gc(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    garbage_collect(objspace, TRUE, TRUE, TRUE, GPR_FLAG_CAPI);
    if (!finalizing) finalize_deferred(objspace);
}

int
rb_during_gc(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    return during_gc;
}

#if RGENGC_PROFILE >= 2

static const char *type_name(int type, VALUE obj);

static void
gc_count_add_each_types(VALUE hash, const char *name, const size_t *types)
{
    VALUE result = rb_hash_new();
    int i;
    for (i=0; i<T_MASK; i++) {
	const char *type = type_name(i, 0);
	rb_hash_aset(result, ID2SYM(rb_intern(type)), SIZET2NUM(types[i]));
    }
    rb_hash_aset(hash, ID2SYM(rb_intern(name)), result);
}
#endif

size_t
rb_gc_count(void)
{
    return rb_objspace.profile.count;
}

/*
 *  call-seq:
 *     GC.count -> Integer
 *
 *  The number of times GC occurred.
 *
 *  It returns the number of times GC occurred since the process started.
 *
 */

static VALUE
gc_count(VALUE self)
{
    return SIZET2NUM(rb_gc_count());
}

static VALUE
gc_info_decode(rb_objspace_t *objspace, const VALUE hash_or_key, const int orig_flags)
{
    static VALUE sym_major_by = Qnil, sym_gc_by, sym_immediate_sweep, sym_have_finalizer, sym_state;
    static VALUE sym_nofree, sym_oldgen, sym_shady, sym_force, sym_stress;
#if RGENGC_ESTIMATE_OLDMALLOC
    static VALUE sym_oldmalloc;
#endif
    static VALUE sym_newobj, sym_malloc, sym_method, sym_capi;
    static VALUE sym_none, sym_marking, sym_sweeping;
    VALUE hash = Qnil, key = Qnil;
    VALUE major_by;
    VALUE flags = orig_flags ? orig_flags : objspace->profile.latest_gc_info;

    if (SYMBOL_P(hash_or_key)) {
	key = hash_or_key;
    }
    else if (RB_TYPE_P(hash_or_key, T_HASH)) {
	hash = hash_or_key;
    }
    else {
	rb_raise(rb_eTypeError, "non-hash or symbol given");
    }

    if (sym_major_by == Qnil) {
#define S(s) sym_##s = ID2SYM(rb_intern_const(#s))
	S(major_by);
	S(gc_by);
	S(immediate_sweep);
	S(have_finalizer);
	S(state);

	S(stress);
	S(nofree);
	S(oldgen);
	S(shady);
	S(force);
#if RGENGC_ESTIMATE_OLDMALLOC
	S(oldmalloc);
#endif
	S(newobj);
	S(malloc);
	S(method);
	S(capi);

	S(none);
	S(marking);
	S(sweeping);
#undef S
    }

#define SET(name, attr) \
    if (key == sym_##name) \
	return (attr); \
    else if (hash != Qnil) \
	rb_hash_aset(hash, sym_##name, (attr));

    major_by =
      (flags & GPR_FLAG_MAJOR_BY_NOFREE) ? sym_nofree :
      (flags & GPR_FLAG_MAJOR_BY_OLDGEN) ? sym_oldgen :
      (flags & GPR_FLAG_MAJOR_BY_SHADY)  ? sym_shady :
      (flags & GPR_FLAG_MAJOR_BY_FORCE)  ? sym_force :
#if RGENGC_ESTIMATE_OLDMALLOC
      (flags & GPR_FLAG_MAJOR_BY_OLDMALLOC) ? sym_oldmalloc :
#endif
      Qnil;
    SET(major_by, major_by);

    SET(gc_by,
	(flags & GPR_FLAG_NEWOBJ) ? sym_newobj :
	(flags & GPR_FLAG_MALLOC) ? sym_malloc :
	(flags & GPR_FLAG_METHOD) ? sym_method :
	(flags & GPR_FLAG_CAPI)   ? sym_capi :
	(flags & GPR_FLAG_STRESS) ? sym_stress :
	Qnil
    );

    SET(have_finalizer, (flags & GPR_FLAG_HAVE_FINALIZE) ? Qtrue : Qfalse);
    SET(immediate_sweep, (flags & GPR_FLAG_IMMEDIATE_SWEEP) ? Qtrue : Qfalse);

    if (orig_flags == 0) {
	SET(state, objspace->flags.stat == gc_stat_none ? sym_none :
	           objspace->flags.stat == gc_stat_marking ? sym_marking : sym_sweeping);
    }
#undef SET

    if (!NIL_P(key)) {/* matched key should return above */
	rb_raise(rb_eArgError, "unknown key: %"PRIsVALUE, rb_sym2str(key));
    }

    return hash;
}

VALUE
rb_gc_latest_gc_info(VALUE key)
{
    rb_objspace_t *objspace = &rb_objspace;
    return gc_info_decode(objspace, key, 0);
}

/*
 *  call-seq:
 *     GC.latest_gc_info -> {:gc_by=>:newobj}
 *     GC.latest_gc_info(hash) -> hash
 *     GC.latest_gc_info(:major_by) -> :malloc
 *
 *  Returns information about the most recent garbage collection.
 */

static VALUE
gc_latest_gc_info(int argc, VALUE *argv, VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE arg = Qnil;

    if (rb_scan_args(argc, argv, "01", &arg) == 1) {
	if (!SYMBOL_P(arg) && !RB_TYPE_P(arg, T_HASH)) {
	    rb_raise(rb_eTypeError, "non-hash or symbol given");
	}
    }

    if (arg == Qnil) {
	arg = rb_hash_new();
    }

    return gc_info_decode(objspace, arg, 0);
}

enum gc_stat_sym {
    gc_stat_sym_count,
    gc_stat_sym_heap_allocated_pages,
    gc_stat_sym_heap_sorted_length,
    gc_stat_sym_heap_allocatable_pages,
    gc_stat_sym_heap_available_slots,
    gc_stat_sym_heap_live_slots,
    gc_stat_sym_heap_free_slots,
    gc_stat_sym_heap_final_slots,
    gc_stat_sym_heap_marked_slots,
    gc_stat_sym_heap_swept_slots,
    gc_stat_sym_heap_eden_pages,
    gc_stat_sym_heap_tomb_pages,
    gc_stat_sym_total_allocated_pages,
    gc_stat_sym_total_freed_pages,
    gc_stat_sym_total_allocated_objects,
    gc_stat_sym_total_freed_objects,
    gc_stat_sym_malloc_increase_bytes,
    gc_stat_sym_malloc_increase_bytes_limit,
#if USE_RGENGC
    gc_stat_sym_minor_gc_count,
    gc_stat_sym_major_gc_count,
    gc_stat_sym_remembered_wb_unprotected_objects,
    gc_stat_sym_remembered_wb_unprotected_objects_limit,
    gc_stat_sym_old_objects,
    gc_stat_sym_old_objects_limit,
#if RGENGC_ESTIMATE_OLDMALLOC
    gc_stat_sym_oldmalloc_increase_bytes,
    gc_stat_sym_oldmalloc_increase_bytes_limit,
#endif
#if RGENGC_PROFILE
    gc_stat_sym_total_generated_normal_object_count,
    gc_stat_sym_total_generated_shady_object_count,
    gc_stat_sym_total_shade_operation_count,
    gc_stat_sym_total_promoted_count,
    gc_stat_sym_total_remembered_normal_object_count,
    gc_stat_sym_total_remembered_shady_object_count,
#endif
#endif
    gc_stat_sym_last
};

enum gc_stat_compat_sym {
    gc_stat_compat_sym_gc_stat_heap_used,
    gc_stat_compat_sym_heap_eden_page_length,
    gc_stat_compat_sym_heap_tomb_page_length,
    gc_stat_compat_sym_heap_increment,
    gc_stat_compat_sym_heap_length,
    gc_stat_compat_sym_heap_live_slot,
    gc_stat_compat_sym_heap_free_slot,
    gc_stat_compat_sym_heap_final_slot,
    gc_stat_compat_sym_heap_swept_slot,
#if USE_RGENGC
    gc_stat_compat_sym_remembered_shady_object,
    gc_stat_compat_sym_remembered_shady_object_limit,
    gc_stat_compat_sym_old_object,
    gc_stat_compat_sym_old_object_limit,
#endif
    gc_stat_compat_sym_total_allocated_object,
    gc_stat_compat_sym_total_freed_object,
    gc_stat_compat_sym_malloc_increase,
    gc_stat_compat_sym_malloc_limit,
#if RGENGC_ESTIMATE_OLDMALLOC
    gc_stat_compat_sym_oldmalloc_increase,
    gc_stat_compat_sym_oldmalloc_limit,
#endif
    gc_stat_compat_sym_last
};

static VALUE gc_stat_symbols[gc_stat_sym_last];
static VALUE gc_stat_compat_symbols[gc_stat_compat_sym_last];
static VALUE gc_stat_compat_table;

static void
setup_gc_stat_symbols(void)
{
    if (gc_stat_symbols[0] == 0) {
#define S(s) gc_stat_symbols[gc_stat_sym_##s] = ID2SYM(rb_intern_const(#s))
	S(count);
	S(heap_allocated_pages);
	S(heap_sorted_length);
	S(heap_allocatable_pages);
	S(heap_available_slots);
	S(heap_live_slots);
	S(heap_free_slots);
	S(heap_final_slots);
	S(heap_marked_slots);
	S(heap_swept_slots);
	S(heap_eden_pages);
	S(heap_tomb_pages);
	S(total_allocated_pages);
	S(total_freed_pages);
	S(total_allocated_objects);
	S(total_freed_objects);
	S(malloc_increase_bytes);
	S(malloc_increase_bytes_limit);
#if USE_RGENGC
	S(minor_gc_count);
	S(major_gc_count);
	S(remembered_wb_unprotected_objects);
	S(remembered_wb_unprotected_objects_limit);
	S(old_objects);
	S(old_objects_limit);
#if RGENGC_ESTIMATE_OLDMALLOC
	S(oldmalloc_increase_bytes);
	S(oldmalloc_increase_bytes_limit);
#endif
#if RGENGC_PROFILE
	S(total_generated_normal_object_count);
	S(total_generated_shady_object_count);
	S(total_shade_operation_count);
	S(total_promoted_count);
	S(total_remembered_normal_object_count);
	S(total_remembered_shady_object_count);
#endif /* RGENGC_PROFILE */
#endif /* USE_RGENGC */
#undef S
#define S(s) gc_stat_compat_symbols[gc_stat_compat_sym_##s] = ID2SYM(rb_intern_const(#s))
	S(gc_stat_heap_used);
	S(heap_eden_page_length);
	S(heap_tomb_page_length);
	S(heap_increment);
	S(heap_length);
	S(heap_live_slot);
	S(heap_free_slot);
	S(heap_final_slot);
	S(heap_swept_slot);
#if USE_RGEGC
	S(remembered_shady_object);
	S(remembered_shady_object_limit);
	S(old_object);
	S(old_object_limit);
#endif
	S(total_allocated_object);
	S(total_freed_object);
	S(malloc_increase);
	S(malloc_limit);
#if RGENGC_ESTIMATE_OLDMALLOC
	S(oldmalloc_increase);
	S(oldmalloc_limit);
#endif
#undef S

	{
	    VALUE table = gc_stat_compat_table = rb_hash_new();
	    rb_obj_hide(table);
	    rb_gc_register_mark_object(table);

	    /* compatibility layer for Ruby 2.1 */
#define OLD_SYM(s) gc_stat_compat_symbols[gc_stat_compat_sym_##s]
#define NEW_SYM(s) gc_stat_symbols[gc_stat_sym_##s]
	    rb_hash_aset(table, OLD_SYM(gc_stat_heap_used), NEW_SYM(heap_allocated_pages));
	    rb_hash_aset(table, OLD_SYM(heap_eden_page_length), NEW_SYM(heap_eden_pages));
	    rb_hash_aset(table, OLD_SYM(heap_tomb_page_length), NEW_SYM(heap_tomb_pages));
	    rb_hash_aset(table, OLD_SYM(heap_increment), NEW_SYM(heap_allocatable_pages));
	    rb_hash_aset(table, OLD_SYM(heap_length), NEW_SYM(heap_sorted_length));
	    rb_hash_aset(table, OLD_SYM(heap_live_slot), NEW_SYM(heap_live_slots));
	    rb_hash_aset(table, OLD_SYM(heap_free_slot), NEW_SYM(heap_free_slots));
	    rb_hash_aset(table, OLD_SYM(heap_final_slot), NEW_SYM(heap_final_slots));
	    rb_hash_aset(table, OLD_SYM(heap_swept_slot), NEW_SYM(heap_swept_slots));
#if USE_RGEGC
	    rb_hash_aset(table, OLD_SYM(remembered_shady_object), NEW_SYM(remembered_wb_unprotected_objects));
	    rb_hash_aset(table, OLD_SYM(remembered_shady_object_limit), NEW_SYM(remembered_wb_unprotected_objects_limit));
	    rb_hash_aset(table, OLD_SYM(old_object), NEW_SYM(old_objects));
	    rb_hash_aset(table, OLD_SYM(old_object_limit), NEW_SYM(old_objects_limit));
#endif
	    rb_hash_aset(table, OLD_SYM(total_allocated_object), NEW_SYM(total_allocated_objects));
	    rb_hash_aset(table, OLD_SYM(total_freed_object), NEW_SYM(total_freed_objects));
	    rb_hash_aset(table, OLD_SYM(malloc_increase), NEW_SYM(malloc_increase_bytes));
	    rb_hash_aset(table, OLD_SYM(malloc_limit), NEW_SYM(malloc_increase_bytes_limit));
#if RGENGC_ESTIMATE_OLDMALLOC
	    rb_hash_aset(table, OLD_SYM(oldmalloc_increase), NEW_SYM(oldmalloc_increase_bytes));
	    rb_hash_aset(table, OLD_SYM(oldmalloc_limit), NEW_SYM(oldmalloc_increase_bytes_limit));
#endif
#undef OLD_SYM
#undef NEW_SYM
	    rb_obj_freeze(table);
	}
    }
}

static VALUE
compat_key(VALUE key)
{
    VALUE new_key = rb_hash_lookup(gc_stat_compat_table, key);

    if (!NIL_P(new_key)) {
	static int warned = 0;
	if (warned == 0) {
	    rb_warn("GC.stat keys were changed from Ruby 2.1. "
		    "In this case, you refer to obsolete `%"PRIsVALUE"' (new key is `%"PRIsVALUE"'). "
		    "Please check <https://bugs.ruby-lang.org/issues/9924> for more information.",
		    key, new_key);
	    warned = 1;
	}
    }

    return new_key;
}

static VALUE
default_proc_for_compat_func(VALUE hash, VALUE dmy, int argc, VALUE *argv)
{
    VALUE key, new_key;

    Check_Type(hash, T_HASH);
    rb_check_arity(argc, 2, 2);
    key = argv[1];

    if ((new_key = compat_key(key)) != Qnil) {
	return rb_hash_lookup(hash, new_key);
    }

    return Qnil;
}

size_t
gc_stat_internal(VALUE hash_or_sym)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE hash = Qnil, key = Qnil;

    setup_gc_stat_symbols();

    if (RB_TYPE_P(hash_or_sym, T_HASH)) {
	hash = hash_or_sym;

	if (NIL_P(RHASH_IFNONE(hash))) {
	    static VALUE default_proc_for_compat = 0;
	    if (default_proc_for_compat == 0) { /* TODO: it should be */
		default_proc_for_compat = rb_proc_new(default_proc_for_compat_func, Qnil);
		rb_gc_register_mark_object(default_proc_for_compat);
	    }
	    rb_hash_set_default_proc(hash, default_proc_for_compat);
	}
    }
    else if (SYMBOL_P(hash_or_sym)) {
	key = hash_or_sym;
    }
    else {
	rb_raise(rb_eTypeError, "non-hash or symbol argument");
    }

#define SET(name, attr) \
    if (key == gc_stat_symbols[gc_stat_sym_##name]) \
	return attr; \
    else if (hash != Qnil) \
	rb_hash_aset(hash, gc_stat_symbols[gc_stat_sym_##name], SIZET2NUM(attr));

  again:
    SET(count, objspace->profile.count);

    /* implementation dependent counters */
    SET(heap_allocated_pages, heap_allocated_pages);
    SET(heap_sorted_length, heap_pages_sorted_length);
    SET(heap_allocatable_pages, heap_allocatable_pages);
    SET(heap_available_slots, objspace_available_slots(objspace));
    SET(heap_live_slots, objspace_live_slots(objspace));
    SET(heap_free_slots, objspace_free_slots(objspace));
    SET(heap_final_slots, heap_pages_final_slots);
    SET(heap_marked_slots, objspace->marked_slots);
    SET(heap_swept_slots, heap_pages_swept_slots);
    SET(heap_eden_pages, heap_eden->page_length);
    SET(heap_tomb_pages, heap_tomb->page_length);
    SET(total_allocated_pages, objspace->profile.total_allocated_pages);
    SET(total_freed_pages, objspace->profile.total_freed_pages);
    SET(total_allocated_objects, objspace->total_allocated_objects);
    SET(total_freed_objects, objspace->profile.total_freed_objects);
    SET(malloc_increase_bytes, malloc_increase);
    SET(malloc_increase_bytes_limit, malloc_limit);
#if USE_RGENGC
    SET(minor_gc_count, objspace->profile.minor_gc_count);
    SET(major_gc_count, objspace->profile.major_gc_count);
    SET(remembered_wb_unprotected_objects, objspace->rgengc.uncollectible_wb_unprotected_objects);
    SET(remembered_wb_unprotected_objects_limit, objspace->rgengc.uncollectible_wb_unprotected_objects_limit);
    SET(old_objects, objspace->rgengc.old_objects);
    SET(old_objects_limit, objspace->rgengc.old_objects_limit);
#if RGENGC_ESTIMATE_OLDMALLOC
    SET(oldmalloc_increase_bytes, objspace->rgengc.oldmalloc_increase);
    SET(oldmalloc_increase_bytes_limit, objspace->rgengc.oldmalloc_increase_limit);
#endif

#if RGENGC_PROFILE
    SET(total_generated_normal_object_count, objspace->profile.total_generated_normal_object_count);
    SET(total_generated_shady_object_count, objspace->profile.total_generated_shady_object_count);
    SET(total_shade_operation_count, objspace->profile.total_shade_operation_count);
    SET(total_promoted_count, objspace->profile.total_promoted_count);
    SET(total_remembered_normal_object_count, objspace->profile.total_remembered_normal_object_count);
    SET(total_remembered_shady_object_count, objspace->profile.total_remembered_shady_object_count);
#endif /* RGENGC_PROFILE */
#endif /* USE_RGENGC */
#undef SET

    if (!NIL_P(key)) { /* matched key should return above */
	VALUE new_key;
	if ((new_key = compat_key(key)) != Qnil) {
	    key = new_key;
	    goto again;
	}
	rb_raise(rb_eArgError, "unknown key: %"PRIsVALUE, rb_sym2str(key));
    }

#if defined(RGENGC_PROFILE) && RGENGC_PROFILE >= 2
    if (hash != Qnil) {
	gc_count_add_each_types(hash, "generated_normal_object_count_types", objspace->profile.generated_normal_object_count_types);
	gc_count_add_each_types(hash, "generated_shady_object_count_types", objspace->profile.generated_shady_object_count_types);
	gc_count_add_each_types(hash, "shade_operation_count_types", objspace->profile.shade_operation_count_types);
	gc_count_add_each_types(hash, "promoted_types", objspace->profile.promoted_types);
	gc_count_add_each_types(hash, "remembered_normal_object_count_types", objspace->profile.remembered_normal_object_count_types);
	gc_count_add_each_types(hash, "remembered_shady_object_count_types", objspace->profile.remembered_shady_object_count_types);
    }
#endif

    return 0;
}

/*
 *  call-seq:
 *     GC.stat -> Hash
 *     GC.stat(hash) -> hash
 *     GC.stat(:key) -> Numeric
 *
 *  Returns a Hash containing information about the GC.
 *
 *  The hash includes information about internal statistics about GC such as:
 *
 *      {
 *          :count=>0,
 *          :heap_allocated_pages=>24,
 *          :heap_sorted_length=>24,
 *          :heap_allocatable_pages=>0,
 *          :heap_available_slots=>9783,
 *          :heap_live_slots=>7713,
 *          :heap_free_slots=>2070,
 *          :heap_final_slots=>0,
 *          :heap_marked_slots=>0,
 *          :heap_swept_slots=>0,
 *          :heap_eden_pages=>24,
 *          :heap_tomb_pages=>0,
 *          :total_allocated_pages=>24,
 *          :total_freed_pages=>0,
 *          :total_allocated_objects=>7796,
 *          :total_freed_objects=>83,
 *          :malloc_increase_bytes=>2389312,
 *          :malloc_increase_bytes_limit=>16777216,
 *          :minor_gc_count=>0,
 *          :major_gc_count=>0,
 *          :remembered_wb_unprotected_objects=>0,
 *          :remembered_wb_unprotected_objects_limit=>0,
 *          :old_objects=>0,
 *          :old_objects_limit=>0,
 *          :oldmalloc_increase_bytes=>2389760,
 *          :oldmalloc_increase_bytes_limit=>16777216
 *      }
 *
 *  The contents of the hash are implementation specific and may be changed in
 *  the future.
 *
 *  This method is only expected to work on C Ruby.
 *
 */

static VALUE
gc_stat(int argc, VALUE *argv, VALUE self)
{
    VALUE arg = Qnil;

    if (rb_scan_args(argc, argv, "01", &arg) == 1) {
	if (SYMBOL_P(arg)) {
	    size_t value = gc_stat_internal(arg);
	    return SIZET2NUM(value);
	}
	else if (!RB_TYPE_P(arg, T_HASH)) {
	    rb_raise(rb_eTypeError, "non-hash or symbol given");
	}
    }

    if (arg == Qnil) {
        arg = rb_hash_new();
    }
    gc_stat_internal(arg);
    return arg;
}

size_t
rb_gc_stat(VALUE key)
{
    if (SYMBOL_P(key)) {
	size_t value = gc_stat_internal(key);
	return value;
    }
    else {
	gc_stat_internal(key);
	return 0;
    }
}

/*
 *  call-seq:
 *    GC.stress	    -> fixnum, true or false
 *
 *  Returns current status of GC stress mode.
 */

static VALUE
gc_stress_get(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return ruby_gc_stress_mode;
}

static void
gc_stress_set(rb_objspace_t *objspace, VALUE flag)
{
    objspace->flags.gc_stressful = RTEST(flag);
    objspace->gc_stress_mode = flag;
}

/*
 *  call-seq:
 *    GC.stress = flag          -> flag
 *
 *  Updates the GC stress mode.
 *
 *  When stress mode is enabled, the GC is invoked at every GC opportunity:
 *  all memory and object allocations.
 *
 *  Enabling stress mode will degrade performance, it is only for debugging.
 *
 *  flag can be true, false, or a fixnum bit-ORed following flags.
 *    0x01:: no major GC
 *    0x02:: no immediate sweep
 *    0x04:: full mark after malloc/calloc/realloc
 */

static VALUE
gc_stress_set_m(VALUE self, VALUE flag)
{
    rb_objspace_t *objspace = &rb_objspace;
    gc_stress_set(objspace, flag);
    return flag;
}

/*
 *  call-seq:
 *     GC.enable    -> true or false
 *
 *  Enables garbage collection, returning +true+ if garbage
 *  collection was previously disabled.
 *
 *     GC.disable   #=> false
 *     GC.enable    #=> true
 *     GC.enable    #=> false
 *
 */

VALUE
rb_gc_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = dont_gc;

    dont_gc = FALSE;
    return old ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     GC.disable    -> true or false
 *
 *  Disables garbage collection, returning +true+ if garbage
 *  collection was already disabled.
 *
 *     GC.disable   #=> false
 *     GC.disable   #=> true
 *
 */

VALUE
rb_gc_disable(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = dont_gc;

    gc_rest(objspace);

    dont_gc = TRUE;
    return old ? Qtrue : Qfalse;
}

static int
get_envparam_size(const char *name, size_t *default_value, size_t lower_bound)
{
    char *ptr = getenv(name);
    ssize_t val;

    if (ptr != NULL && *ptr) {
	size_t unit = 0;
	char *end;
#if SIZEOF_SIZE_T == SIZEOF_LONG_LONG
	val = strtoll(ptr, &end, 0);
#else
	val = strtol(ptr, &end, 0);
#endif
	switch (*end) {
	  case 'k': case 'K':
	    unit = 1024;
	    ++end;
	    break;
	  case 'm': case 'M':
	    unit = 1024*1024;
	    ++end;
	    break;
	  case 'g': case 'G':
	    unit = 1024*1024*1024;
	    ++end;
	    break;
	}
	while (*end && isspace((unsigned char)*end)) end++;
	if (*end) {
	    if (RTEST(ruby_verbose)) fprintf(stderr, "invalid string for %s: %s\n", name, ptr);
	    return 0;
	}
	if (unit > 0) {
	    if (val < -(ssize_t)(SIZE_MAX / 2 / unit) || (ssize_t)(SIZE_MAX / 2 / unit) < val) {
		if (RTEST(ruby_verbose)) fprintf(stderr, "%s=%s is ignored because it overflows\n", name, ptr);
		return 0;
	    }
	    val *= unit;
	}
	if (val > 0 && (size_t)val > lower_bound) {
	    if (RTEST(ruby_verbose)) {
		fprintf(stderr, "%s=%"PRIdSIZE" (default value: %"PRIdSIZE")\n", name, val, *default_value);
	    }
	    *default_value = (size_t)val;
	    return 1;
	}
	else {
	    if (RTEST(ruby_verbose)) {
		fprintf(stderr, "%s=%"PRIdSIZE" (default value: %"PRIdSIZE") is ignored because it must be greater than %"PRIdSIZE".\n",
			name, val, *default_value, lower_bound);
	    }
	    return 0;
	}
    }
    return 0;
}

static int
get_envparam_double(const char *name, double *default_value, double lower_bound)
{
    char *ptr = getenv(name);
    double val;

    if (ptr != NULL && *ptr) {
	char *end;
	val = strtod(ptr, &end);
	if (!*ptr || *end) {
	    if (RTEST(ruby_verbose)) fprintf(stderr, "invalid string for %s: %s\n", name, ptr);
	    return 0;
	}
	if (val > lower_bound) {
	    if (RTEST(ruby_verbose)) fprintf(stderr, "%s=%f (default value: %f)\n", name, val, *default_value);
	    *default_value = val;
	    return 1;
	}
	else {
	    if (RTEST(ruby_verbose)) fprintf(stderr, "%s=%f (default value: %f) is ignored because it must be greater than %f.\n", name, val, *default_value, lower_bound);
	}
    }
    return 0;
}

static void
gc_set_initial_pages(void)
{
    size_t min_pages;
    rb_objspace_t *objspace = &rb_objspace;

    min_pages = gc_params.heap_init_slots / HEAP_OBJ_LIMIT;
    if (min_pages > heap_eden->page_length) {
	heap_add_pages(objspace, heap_eden, min_pages - heap_eden->page_length);
    }
}

/*
 * GC tuning environment variables
 *
 * * RUBY_GC_HEAP_INIT_SLOTS
 *   - Initial allocation slots.
 * * RUBY_GC_HEAP_FREE_SLOTS
 *   - Prepare at least this amount of slots after GC.
 *   - Allocate slots if there are not enough slots.
 * * RUBY_GC_HEAP_GROWTH_FACTOR (new from 2.1)
 *   - Allocate slots by this factor.
 *   - (next slots number) = (current slots number) * (this factor)
 * * RUBY_GC_HEAP_GROWTH_MAX_SLOTS (new from 2.1)
 *   - Allocation rate is limited to this number of slots.
 * * RUBY_GC_HEAP_OLDOBJECT_LIMIT_FACTOR (new from 2.1.1)
 *   - Do full GC when the number of old objects is more than R * N
 *     where R is this factor and
 *           N is the number of old objects just after last full GC.
 *
 *  * obsolete
 *    * RUBY_FREE_MIN       -> RUBY_GC_HEAP_FREE_SLOTS (from 2.1)
 *    * RUBY_HEAP_MIN_SLOTS -> RUBY_GC_HEAP_INIT_SLOTS (from 2.1)
 *
 * * RUBY_GC_MALLOC_LIMIT
 * * RUBY_GC_MALLOC_LIMIT_MAX (new from 2.1)
 * * RUBY_GC_MALLOC_LIMIT_GROWTH_FACTOR (new from 2.1)
 *
 * * RUBY_GC_OLDMALLOC_LIMIT (new from 2.1)
 * * RUBY_GC_OLDMALLOC_LIMIT_MAX (new from 2.1)
 * * RUBY_GC_OLDMALLOC_LIMIT_GROWTH_FACTOR (new from 2.1)
 */

void
ruby_gc_set_params(int safe_level)
{
    if (safe_level > 0) return;

    /* RUBY_GC_HEAP_FREE_SLOTS */
    if (get_envparam_size("RUBY_GC_HEAP_FREE_SLOTS", &gc_params.heap_free_slots, 0)) {
	/* ok */
    }
    else if (get_envparam_size("RUBY_FREE_MIN", &gc_params.heap_free_slots, 0)) {
	rb_warn("RUBY_FREE_MIN is obsolete. Use RUBY_GC_HEAP_FREE_SLOTS instead.");
    }

    /* RUBY_GC_HEAP_INIT_SLOTS */
    if (get_envparam_size("RUBY_GC_HEAP_INIT_SLOTS", &gc_params.heap_init_slots, 0)) {
	gc_set_initial_pages();
    }
    else if (get_envparam_size("RUBY_HEAP_MIN_SLOTS", &gc_params.heap_init_slots, 0)) {
	rb_warn("RUBY_HEAP_MIN_SLOTS is obsolete. Use RUBY_GC_HEAP_INIT_SLOTS instead.");
	gc_set_initial_pages();
    }

    get_envparam_double("RUBY_GC_HEAP_GROWTH_FACTOR", &gc_params.growth_factor, 1.0);
    get_envparam_size  ("RUBY_GC_HEAP_GROWTH_MAX_SLOTS", &gc_params.growth_max_slots, 0);
    get_envparam_double("RUBY_GC_HEAP_OLDOBJECT_LIMIT_FACTOR", &gc_params.oldobject_limit_factor, 0.0);

    get_envparam_size  ("RUBY_GC_MALLOC_LIMIT", &gc_params.malloc_limit_min, 0);
    get_envparam_size  ("RUBY_GC_MALLOC_LIMIT_MAX", &gc_params.malloc_limit_max, 0);
    get_envparam_double("RUBY_GC_MALLOC_LIMIT_GROWTH_FACTOR", &gc_params.malloc_limit_growth_factor, 1.0);

#if RGENGC_ESTIMATE_OLDMALLOC
    if (get_envparam_size("RUBY_GC_OLDMALLOC_LIMIT", &gc_params.oldmalloc_limit_min, 0)) {
	rb_objspace_t *objspace = &rb_objspace;
	objspace->rgengc.oldmalloc_increase_limit = gc_params.oldmalloc_limit_min;
    }
    get_envparam_size  ("RUBY_GC_OLDMALLOC_LIMIT_MAX", &gc_params.oldmalloc_limit_max, 0);
    get_envparam_double("RUBY_GC_OLDMALLOC_LIMIT_GROWTH_FACTOR", &gc_params.oldmalloc_limit_growth_factor, 1.0);
#endif
}

void
rb_objspace_reachable_objects_from(VALUE obj, void (func)(VALUE, void *), void *data)
{
    rb_objspace_t *objspace = &rb_objspace;

    if (is_markable_object(objspace, obj)) {
	struct mark_func_data_struct mfd;
	mfd.mark_func = func;
	mfd.data = data;
	PUSH_MARK_FUNC_DATA(&mfd);
	gc_mark_children(objspace, obj);
	POP_MARK_FUNC_DATA();
    }
}

struct root_objects_data {
    const char *category;
    void (*func)(const char *category, VALUE, void *);
    void *data;
};

static void
root_objects_from(VALUE obj, void *ptr)
{
    const struct root_objects_data *data = (struct root_objects_data *)ptr;
    (*data->func)(data->category, obj, data->data);
}

void
rb_objspace_reachable_objects_from_root(void (func)(const char *category, VALUE, void *), void *passing_data)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct root_objects_data data;
    struct mark_func_data_struct mfd;

    data.func = func;
    data.data = passing_data;

    mfd.mark_func = root_objects_from;
    mfd.data = &data;

    PUSH_MARK_FUNC_DATA(&mfd);
    gc_mark_roots(objspace, &data.category);
    POP_MARK_FUNC_DATA();
}

/*
  ------------------------ Extended allocator ------------------------
*/

static void objspace_xfree(rb_objspace_t *objspace, void *ptr, size_t size);

static void *
negative_size_allocation_error_with_gvl(void *ptr)
{
    rb_raise(rb_eNoMemError, "%s", (const char *)ptr);
    return 0; /* should not be reached */
}

static void
negative_size_allocation_error(const char *msg)
{
    if (ruby_thread_has_gvl_p()) {
	rb_raise(rb_eNoMemError, "%s", msg);
    }
    else {
	if (ruby_native_thread_p()) {
	    rb_thread_call_with_gvl(negative_size_allocation_error_with_gvl, (void *)msg);
	}
	else {
	    fprintf(stderr, "[FATAL] %s\n", msg);
	    exit(EXIT_FAILURE);
	}
    }
}

static void *
ruby_memerror_body(void *dummy)
{
    rb_memerror();
    return 0;
}

static void
ruby_memerror(void)
{
    if (ruby_thread_has_gvl_p()) {
	rb_memerror();
    }
    else {
	if (ruby_native_thread_p()) {
	    rb_thread_call_with_gvl(ruby_memerror_body, 0);
	}
	else {
	    /* no ruby thread */
	    fprintf(stderr, "[FATAL] failed to allocate memory\n");
	    exit(EXIT_FAILURE);
	}
    }
}

void
rb_memerror(void)
{
    rb_thread_t *th = GET_THREAD();
    rb_objspace_t *objspace = &rb_objspace;

    if (during_gc) gc_exit(objspace, "rb_memerror");

    if (!nomem_error ||
	rb_thread_raised_p(th, RAISED_NOMEMORY)) {
	fprintf(stderr, "[FATAL] failed to allocate memory\n");
	exit(EXIT_FAILURE);
    }
    if (rb_thread_raised_p(th, RAISED_NOMEMORY)) {
	rb_thread_raised_clear(th);
	GET_THREAD()->errinfo = nomem_error;
	JUMP_TAG(TAG_RAISE);
    }
    rb_thread_raised_set(th, RAISED_NOMEMORY);
    rb_exc_raise(nomem_error);
}

static void *
aligned_malloc(size_t alignment, size_t size)
{
    void *res;

#if defined __MINGW32__
    res = __mingw_aligned_malloc(size, alignment);
#elif defined _WIN32 && !defined __CYGWIN__
    void *_aligned_malloc(size_t, size_t);
    res = _aligned_malloc(size, alignment);
#elif defined(HAVE_POSIX_MEMALIGN)
    if (posix_memalign(&res, alignment, size) == 0) {
        return res;
    }
    else {
        return NULL;
    }
#elif defined(HAVE_MEMALIGN)
    res = memalign(alignment, size);
#else
    char* aligned;
    res = malloc(alignment + size + sizeof(void*));
    aligned = (char*)res + alignment + sizeof(void*);
    aligned -= ((VALUE)aligned & (alignment - 1));
    ((void**)aligned)[-1] = res;
    res = (void*)aligned;
#endif

#if defined(_DEBUG) || GC_DEBUG
    /* alignment must be a power of 2 */
    assert(((alignment - 1) & alignment) == 0);
    assert(alignment % sizeof(void*) == 0);
#endif
    return res;
}

static void
aligned_free(void *ptr)
{
#if defined __MINGW32__
    __mingw_aligned_free(ptr);
#elif defined _WIN32 && !defined __CYGWIN__
    _aligned_free(ptr);
#elif defined(HAVE_MEMALIGN) || defined(HAVE_POSIX_MEMALIGN)
    free(ptr);
#else
    free(((void**)ptr)[-1]);
#endif
}

static inline size_t
objspace_malloc_size(rb_objspace_t *objspace, void *ptr, size_t hint)
{
#ifdef HAVE_MALLOC_USABLE_SIZE
    return malloc_usable_size(ptr);
#else
    return hint;
#endif
}

enum memop_type {
    MEMOP_TYPE_MALLOC  = 1,
    MEMOP_TYPE_FREE    = 2,
    MEMOP_TYPE_REALLOC = 3
};

static inline void
atomic_sub_nounderflow(size_t *var, size_t sub)
{
    if (sub == 0) return;

    while (1) {
	size_t val = *var;
	if (val < sub) sub = val;
	if (ATOMIC_SIZE_CAS(*var, val, val-sub) == val) break;
    }
}

static void
objspace_malloc_gc_stress(rb_objspace_t *objspace)
{
    if (ruby_gc_stressful && ruby_native_thread_p()) {
	garbage_collect_with_gvl(objspace, gc_stress_full_mark_after_malloc_p(), TRUE, TRUE, GPR_FLAG_STRESS | GPR_FLAG_MALLOC);
    }
}

static void
objspace_malloc_increase(rb_objspace_t *objspace, void *mem, size_t new_size, size_t old_size, enum memop_type type)
{
    if (new_size > old_size) {
	ATOMIC_SIZE_ADD(malloc_increase, new_size - old_size);
#if RGENGC_ESTIMATE_OLDMALLOC
	ATOMIC_SIZE_ADD(objspace->rgengc.oldmalloc_increase, new_size - old_size);
#endif
    }
    else {
	atomic_sub_nounderflow(&malloc_increase, old_size - new_size);
#if RGENGC_ESTIMATE_OLDMALLOC
	atomic_sub_nounderflow(&objspace->rgengc.oldmalloc_increase, old_size - new_size);
#endif
    }

    if (type == MEMOP_TYPE_MALLOC) {
      retry:
	if (malloc_increase > malloc_limit && ruby_native_thread_p() && !dont_gc) {
	    if (ruby_thread_has_gvl_p() && is_lazy_sweeping(heap_eden)) {
		gc_rest(objspace); /* gc_rest can reduce malloc_increase */
		goto retry;
	    }
	    garbage_collect_with_gvl(objspace, FALSE, FALSE, FALSE, GPR_FLAG_MALLOC);
	}
    }

#if MALLOC_ALLOCATED_SIZE
    if (new_size >= old_size) {
	ATOMIC_SIZE_ADD(objspace->malloc_params.allocated_size, new_size - old_size);
    }
    else {
	size_t dec_size = old_size - new_size;
	size_t allocated_size = objspace->malloc_params.allocated_size;

#if MALLOC_ALLOCATED_SIZE_CHECK
	if (allocated_size < dec_size) {
	    rb_bug("objspace_malloc_increase: underflow malloc_params.allocated_size.");
	}
#endif
	atomic_sub_nounderflow(&objspace->malloc_params.allocated_size, dec_size);
    }

    if (0) fprintf(stderr, "increase - ptr: %p, type: %s, new_size: %d, old_size: %d\n",
		   mem,
		   type == MEMOP_TYPE_MALLOC  ? "malloc" :
		   type == MEMOP_TYPE_FREE    ? "free  " :
		   type == MEMOP_TYPE_REALLOC ? "realloc": "error",
		   (int)new_size, (int)old_size);

    switch (type) {
      case MEMOP_TYPE_MALLOC:
	ATOMIC_SIZE_INC(objspace->malloc_params.allocations);
	break;
      case MEMOP_TYPE_FREE:
	{
	    size_t allocations = objspace->malloc_params.allocations;
	    if (allocations > 0) {
		atomic_sub_nounderflow(&objspace->malloc_params.allocations, 1);
	    }
#if MALLOC_ALLOCATED_SIZE_CHECK
	    else {
		if (RGENGC_CHECK_MODE) assert(objspace->malloc_params.allocations > 0);
	    }
#endif
	}
	break;
      case MEMOP_TYPE_REALLOC: /* ignore */ break;
    }
#endif
}

static inline size_t
objspace_malloc_prepare(rb_objspace_t *objspace, size_t size)
{
    if ((ssize_t)size < 0) {
	negative_size_allocation_error("negative allocation size (or too big)");
    }
    if (size == 0) size = 1;

#if CALC_EXACT_MALLOC_SIZE
    size += sizeof(size_t);
#endif

    return size;
}

static inline void *
objspace_malloc_fixup(rb_objspace_t *objspace, void *mem, size_t size)
{
#if CALC_EXACT_MALLOC_SIZE
    ((size_t *)mem)[0] = size;
    mem = (size_t *)mem + 1;
#endif

    return mem;
}

#define TRY_WITH_GC(alloc) do { \
        objspace_malloc_gc_stress(objspace); \
	if (!(alloc) && \
	    (!garbage_collect_with_gvl(objspace, TRUE, TRUE, TRUE, GPR_FLAG_MALLOC) || /* full/immediate mark && immediate sweep */ \
	     !(alloc))) { \
	    ruby_memerror(); \
	} \
    } while (0)

static void *
objspace_xmalloc(rb_objspace_t *objspace, size_t size)
{
    void *mem;

    size = objspace_malloc_prepare(objspace, size);
    TRY_WITH_GC(mem = malloc(size));
    size = objspace_malloc_size(objspace, mem, size);
    objspace_malloc_increase(objspace, mem, size, 0, MEMOP_TYPE_MALLOC);
    return objspace_malloc_fixup(objspace, mem, size);
}

static void *
objspace_xrealloc(rb_objspace_t *objspace, void *ptr, size_t new_size, size_t old_size)
{
    void *mem;

    if ((ssize_t)new_size < 0) {
	negative_size_allocation_error("negative re-allocation size");
    }

    if (!ptr) return objspace_xmalloc(objspace, new_size);

    /*
     * The behavior of realloc(ptr, 0) is implementation defined.
     * Therefore we don't use realloc(ptr, 0) for portability reason.
     * see http://www.open-std.org/jtc1/sc22/wg14/www/docs/dr_400.htm
     */
    if (new_size == 0) {
	objspace_xfree(objspace, ptr, old_size);
	return 0;
    }

#if CALC_EXACT_MALLOC_SIZE
    new_size += sizeof(size_t);
    ptr = (size_t *)ptr - 1;
    old_size = ((size_t *)ptr)[0];
#endif

    old_size = objspace_malloc_size(objspace, ptr, old_size);
    TRY_WITH_GC(mem = realloc(ptr, new_size));
    new_size = objspace_malloc_size(objspace, mem, new_size);

#if CALC_EXACT_MALLOC_SIZE
    ((size_t *)mem)[0] = new_size;
    mem = (size_t *)mem + 1;
#endif

    objspace_malloc_increase(objspace, mem, new_size, old_size, MEMOP_TYPE_REALLOC);

    return mem;
}

static void
objspace_xfree(rb_objspace_t *objspace, void *ptr, size_t old_size)
{
#if CALC_EXACT_MALLOC_SIZE
    ptr = ((size_t *)ptr) - 1;
    old_size = ((size_t*)ptr)[0];
#endif
    old_size = objspace_malloc_size(objspace, ptr, old_size);

    free(ptr);

    objspace_malloc_increase(objspace, ptr, 0, old_size, MEMOP_TYPE_FREE);
}

void *
ruby_xmalloc(size_t size)
{
    return objspace_xmalloc(&rb_objspace, size);
}

void
ruby_malloc_size_overflow(size_t count, size_t elsize)
{
    rb_raise(rb_eArgError,
	     "malloc: possible integer overflow (%"PRIdSIZE"*%"PRIdSIZE")",
	     count, elsize);
}

#define xmalloc2_size ruby_xmalloc2_size

void *
ruby_xmalloc2(size_t n, size_t size)
{
    return objspace_xmalloc(&rb_objspace, xmalloc2_size(n, size));
}

static void *
objspace_xcalloc(rb_objspace_t *objspace, size_t count, size_t elsize)
{
    void *mem;
    size_t size;

    size = xmalloc2_size(count, elsize);
    size = objspace_malloc_prepare(objspace, size);

    TRY_WITH_GC(mem = calloc(1, size));
    size = objspace_malloc_size(objspace, mem, size);
    objspace_malloc_increase(objspace, mem, size, 0, MEMOP_TYPE_MALLOC);
    return objspace_malloc_fixup(objspace, mem, size);
}

void *
ruby_xcalloc(size_t n, size_t size)
{
    return objspace_xcalloc(&rb_objspace, n, size);
}

#ifdef ruby_sized_xrealloc
#undef ruby_sized_xrealloc
#endif
void *
ruby_sized_xrealloc(void *ptr, size_t new_size, size_t old_size)
{
    return objspace_xrealloc(&rb_objspace, ptr, new_size, old_size);
}

void *
ruby_xrealloc(void *ptr, size_t new_size)
{
    return ruby_sized_xrealloc(ptr, new_size, 0);
}

#ifdef ruby_sized_xrealloc2
#undef ruby_sized_xrealloc2
#endif
void *
ruby_sized_xrealloc2(void *ptr, size_t n, size_t size, size_t old_n)
{
    size_t len = size * n;
    if (n != 0 && size != len / n) {
	rb_raise(rb_eArgError, "realloc: possible integer overflow");
    }
    return objspace_xrealloc(&rb_objspace, ptr, len, old_n * size);
}

void *
ruby_xrealloc2(void *ptr, size_t n, size_t size)
{
    return ruby_sized_xrealloc2(ptr, n, size, 0);
}

#ifdef ruby_sized_xfree
#undef ruby_sized_xfree
#endif
void
ruby_sized_xfree(void *x, size_t size)
{
    if (x) {
	objspace_xfree(&rb_objspace, x, size);
    }
}

void
ruby_xfree(void *x)
{
    ruby_sized_xfree(x, 0);
}

/* Mimic ruby_xmalloc, but need not rb_objspace.
 * should return pointer suitable for ruby_xfree
 */
void *
ruby_mimmalloc(size_t size)
{
    void *mem;
#if CALC_EXACT_MALLOC_SIZE
    size += sizeof(size_t);
#endif
    mem = malloc(size);
#if CALC_EXACT_MALLOC_SIZE
    /* set 0 for consistency of allocated_size/allocations */
    ((size_t *)mem)[0] = 0;
    mem = (size_t *)mem + 1;
#endif
    return mem;
}

void
ruby_mimfree(void *ptr)
{
    size_t *mem = (size_t *)ptr;
#if CALC_EXACT_MALLOC_SIZE
    mem = mem - 1;
#endif
    free(mem);
}

void *
rb_alloc_tmp_buffer(volatile VALUE *store, long len)
{
    NODE *s;
    long cnt;
    void *ptr;

    if (len < 0 || (cnt = (long)roomof(len, sizeof(VALUE))) < 0) {
	rb_raise(rb_eArgError, "negative buffer size (or size too big)");
    }

    s = rb_node_newnode(NODE_ALLOCA, 0, 0, 0);
    ptr = ruby_xmalloc(cnt * sizeof(VALUE));
    s->u1.value = (VALUE)ptr;
    s->u3.cnt = cnt;
    *store = (VALUE)s;
    return ptr;
}

void
rb_free_tmp_buffer(volatile VALUE *store)
{
    VALUE s = ATOMIC_VALUE_EXCHANGE(*store, 0);
    if (s) {
	void *ptr = ATOMIC_PTR_EXCHANGE(RNODE(s)->u1.node, 0);
	RNODE(s)->u3.cnt = 0;
	ruby_xfree(ptr);
    }
}

#if MALLOC_ALLOCATED_SIZE
/*
 *  call-seq:
 *     GC.malloc_allocated_size -> Integer
 *
 *  Returns the size of memory allocated by malloc().
 *
 *  Only available if ruby was built with +CALC_EXACT_MALLOC_SIZE+.
 */

static VALUE
gc_malloc_allocated_size(VALUE self)
{
    return UINT2NUM(rb_objspace.malloc_params.allocated_size);
}

/*
 *  call-seq:
 *     GC.malloc_allocations -> Integer
 *
 *  Returns the number of malloc() allocations.
 *
 *  Only available if ruby was built with +CALC_EXACT_MALLOC_SIZE+.
 */

static VALUE
gc_malloc_allocations(VALUE self)
{
    return UINT2NUM(rb_objspace.malloc_params.allocations);
}
#endif

/*
  ------------------------------ WeakMap ------------------------------
*/

struct weakmap {
    st_table *obj2wmap;		/* obj -> [ref,...] */
    st_table *wmap2obj;		/* ref -> obj */
    VALUE final;
};

#define WMAP_DELETE_DEAD_OBJECT_IN_MARK 0

#if WMAP_DELETE_DEAD_OBJECT_IN_MARK
static int
wmap_mark_map(st_data_t key, st_data_t val, st_data_t arg)
{
    rb_objspace_t *objspace = (rb_objspace_t *)arg;
    VALUE obj = (VALUE)val;
    if (!is_live_object(objspace, obj)) return ST_DELETE;
    return ST_CONTINUE;
}
#endif

static void
wmap_mark(void *ptr)
{
    struct weakmap *w = ptr;
#if WMAP_DELETE_DEAD_OBJECT_IN_MARK
    if (w->obj2wmap) st_foreach(w->obj2wmap, wmap_mark_map, (st_data_t)&rb_objspace);
#endif
    rb_gc_mark(w->final);
}

static int
wmap_free_map(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE *ptr = (VALUE *)val;
    ruby_sized_xfree(ptr, (ptr[0] + 1) * sizeof(VALUE));
    return ST_CONTINUE;
}

static void
wmap_free(void *ptr)
{
    struct weakmap *w = ptr;
    st_foreach(w->obj2wmap, wmap_free_map, 0);
    st_free_table(w->obj2wmap);
    st_free_table(w->wmap2obj);
}

static int
wmap_memsize_map(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE *ptr = (VALUE *)val;
    *(size_t *)arg += (ptr[0] + 1) * sizeof(VALUE);
    return ST_CONTINUE;
}

static size_t
wmap_memsize(const void *ptr)
{
    size_t size;
    const struct weakmap *w = ptr;
    if (!w) return 0;
    size = sizeof(*w);
    size += st_memsize(w->obj2wmap);
    size += st_memsize(w->wmap2obj);
    st_foreach(w->obj2wmap, wmap_memsize_map, (st_data_t)&size);
    return size;
}

static const rb_data_type_t weakmap_type = {
    "weakmap",
    {
	wmap_mark,
	wmap_free,
	wmap_memsize,
    },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE
wmap_allocate(VALUE klass)
{
    struct weakmap *w;
    VALUE obj = TypedData_Make_Struct(klass, struct weakmap, &weakmap_type, w);
    w->obj2wmap = st_init_numtable();
    w->wmap2obj = st_init_numtable();
    w->final = rb_obj_method(obj, ID2SYM(rb_intern("finalize")));
    return obj;
}

static int
wmap_final_func(st_data_t *key, st_data_t *value, st_data_t arg, int existing)
{
    VALUE wmap, *ptr, size, i, j;
    if (!existing) return ST_STOP;
    wmap = (VALUE)arg, ptr = (VALUE *)*value;
    for (i = j = 1, size = ptr[0]; i <= size; ++i) {
	if (ptr[i] != wmap) {
	    ptr[j++] = ptr[i];
	}
    }
    if (j == 1) {
	ruby_sized_xfree(ptr, i * sizeof(VALUE));
	return ST_DELETE;
    }
    if (j < i) {
	ptr = ruby_sized_xrealloc2(ptr, j + 1, sizeof(VALUE), i);
	ptr[0] = j;
	*value = (st_data_t)ptr;
    }
    return ST_CONTINUE;
}

static VALUE
wmap_finalize(VALUE self, VALUE objid)
{
    st_data_t orig, wmap, data;
    VALUE obj, *rids, i, size;
    struct weakmap *w;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    /* Get reference from object id. */
    obj = obj_id_to_ref(objid);

    /* obj is original referenced object and/or weak reference. */
    orig = (st_data_t)obj;
    if (st_delete(w->obj2wmap, &orig, &data)) {
	rids = (VALUE *)data;
	size = *rids++;
	for (i = 0; i < size; ++i) {
	    wmap = (st_data_t)rids[i];
	    st_delete(w->wmap2obj, &wmap, NULL);
	}
	ruby_sized_xfree((VALUE *)data, (size + 1) * sizeof(VALUE));
    }

    wmap = (st_data_t)obj;
    if (st_delete(w->wmap2obj, &wmap, &orig)) {
	wmap = (st_data_t)obj;
	st_update(w->obj2wmap, orig, wmap_final_func, wmap);
    }
    return self;
}

struct wmap_iter_arg {
    rb_objspace_t *objspace;
    VALUE value;
};

static int
wmap_inspect_i(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE str = (VALUE)arg;
    VALUE k = (VALUE)key, v = (VALUE)val;

    if (RSTRING_PTR(str)[0] == '#') {
	rb_str_cat2(str, ", ");
    }
    else {
	rb_str_cat2(str, ": ");
	RSTRING_PTR(str)[0] = '#';
    }
    k = SPECIAL_CONST_P(k) ? rb_inspect(k) : rb_any_to_s(k);
    rb_str_append(str, k);
    rb_str_cat2(str, " => ");
    v = SPECIAL_CONST_P(v) ? rb_inspect(v) : rb_any_to_s(v);
    rb_str_append(str, v);
    OBJ_INFECT(str, k);
    OBJ_INFECT(str, v);

    return ST_CONTINUE;
}

static VALUE
wmap_inspect(VALUE self)
{
    VALUE str;
    VALUE c = rb_class_name(CLASS_OF(self));
    struct weakmap *w;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    str = rb_sprintf("-<%"PRIsVALUE":%p", c, (void *)self);
    if (w->wmap2obj) {
	st_foreach(w->wmap2obj, wmap_inspect_i, str);
    }
    RSTRING_PTR(str)[0] = '#';
    rb_str_cat2(str, ">");
    return str;
}

static int
wmap_each_i(st_data_t key, st_data_t val, st_data_t arg)
{
    rb_objspace_t *objspace = (rb_objspace_t *)arg;
    VALUE obj = (VALUE)val;
    if (is_id_value(objspace, obj) && is_live_object(objspace, obj)) {
	rb_yield_values(2, (VALUE)key, obj);
    }
    return ST_CONTINUE;
}

/* Iterates over keys and objects in a weakly referenced object */
static VALUE
wmap_each(VALUE self)
{
    struct weakmap *w;
    rb_objspace_t *objspace = &rb_objspace;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    st_foreach(w->wmap2obj, wmap_each_i, (st_data_t)objspace);
    return self;
}

static int
wmap_each_key_i(st_data_t key, st_data_t val, st_data_t arg)
{
    rb_objspace_t *objspace = (rb_objspace_t *)arg;
    VALUE obj = (VALUE)val;
    if (is_id_value(objspace, obj) && is_live_object(objspace, obj)) {
	rb_yield((VALUE)key);
    }
    return ST_CONTINUE;
}

/* Iterates over keys and objects in a weakly referenced object */
static VALUE
wmap_each_key(VALUE self)
{
    struct weakmap *w;
    rb_objspace_t *objspace = &rb_objspace;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    st_foreach(w->wmap2obj, wmap_each_key_i, (st_data_t)objspace);
    return self;
}

static int
wmap_each_value_i(st_data_t key, st_data_t val, st_data_t arg)
{
    rb_objspace_t *objspace = (rb_objspace_t *)arg;
    VALUE obj = (VALUE)val;
    if (is_id_value(objspace, obj) && is_live_object(objspace, obj)) {
	rb_yield(obj);
    }
    return ST_CONTINUE;
}

/* Iterates over keys and objects in a weakly referenced object */
static VALUE
wmap_each_value(VALUE self)
{
    struct weakmap *w;
    rb_objspace_t *objspace = &rb_objspace;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    st_foreach(w->wmap2obj, wmap_each_value_i, (st_data_t)objspace);
    return self;
}

static int
wmap_keys_i(st_data_t key, st_data_t val, st_data_t arg)
{
    struct wmap_iter_arg *argp = (struct wmap_iter_arg *)arg;
    rb_objspace_t *objspace = argp->objspace;
    VALUE ary = argp->value;
    VALUE obj = (VALUE)val;
    if (is_id_value(objspace, obj) && is_live_object(objspace, obj)) {
	rb_ary_push(ary, (VALUE)key);
    }
    return ST_CONTINUE;
}

/* Iterates over keys and objects in a weakly referenced object */
static VALUE
wmap_keys(VALUE self)
{
    struct weakmap *w;
    struct wmap_iter_arg args;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    args.objspace = &rb_objspace;
    args.value = rb_ary_new();
    st_foreach(w->wmap2obj, wmap_keys_i, (st_data_t)&args);
    return args.value;
}

static int
wmap_values_i(st_data_t key, st_data_t val, st_data_t arg)
{
    struct wmap_iter_arg *argp = (struct wmap_iter_arg *)arg;
    rb_objspace_t *objspace = argp->objspace;
    VALUE ary = argp->value;
    VALUE obj = (VALUE)val;
    if (is_id_value(objspace, obj) && is_live_object(objspace, obj)) {
	rb_ary_push(ary, obj);
    }
    return ST_CONTINUE;
}

/* Iterates over values and objects in a weakly referenced object */
static VALUE
wmap_values(VALUE self)
{
    struct weakmap *w;
    struct wmap_iter_arg args;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    args.objspace = &rb_objspace;
    args.value = rb_ary_new();
    st_foreach(w->wmap2obj, wmap_values_i, (st_data_t)&args);
    return args.value;
}

static int
wmap_aset_update(st_data_t *key, st_data_t *val, st_data_t arg, int existing)
{
    VALUE size, *ptr, *optr;
    if (existing) {
	size = (ptr = optr = (VALUE *)*val)[0];
	++size;
	ptr = ruby_sized_xrealloc2(ptr, size + 1, sizeof(VALUE), size);
    }
    else {
	optr = 0;
	size = 1;
	ptr = ruby_xmalloc2(2, sizeof(VALUE));
    }
    ptr[0] = size;
    ptr[size] = (VALUE)arg;
    if (ptr == optr) return ST_STOP;
    *val = (st_data_t)ptr;
    return ST_CONTINUE;
}

/* Creates a weak reference from the given key to the given value */
static VALUE
wmap_aset(VALUE self, VALUE wmap, VALUE orig)
{
    struct weakmap *w;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    should_be_finalizable(orig);
    should_be_finalizable(wmap);
    define_final0(orig, w->final);
    define_final0(wmap, w->final);
    st_update(w->obj2wmap, (st_data_t)orig, wmap_aset_update, wmap);
    st_insert(w->wmap2obj, (st_data_t)wmap, (st_data_t)orig);
    return nonspecial_obj_id(orig);
}

/* Retrieves a weakly referenced object with the given key */
static VALUE
wmap_aref(VALUE self, VALUE wmap)
{
    st_data_t data;
    VALUE obj;
    struct weakmap *w;
    rb_objspace_t *objspace = &rb_objspace;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    if (!st_lookup(w->wmap2obj, (st_data_t)wmap, &data)) return Qnil;
    obj = (VALUE)data;
    if (!is_id_value(objspace, obj)) return Qnil;
    if (!is_live_object(objspace, obj)) return Qnil;
    return obj;
}

/* Returns +true+ if +key+ is registered */
static VALUE
wmap_has_key(VALUE self, VALUE key)
{
    return NIL_P(wmap_aref(self, key)) ? Qfalse : Qtrue;
}

static VALUE
wmap_size(VALUE self)
{
    struct weakmap *w;
    st_index_t n;

    TypedData_Get_Struct(self, struct weakmap, &weakmap_type, w);
    n = w->wmap2obj->num_entries;
#if SIZEOF_ST_INDEX_T <= SIZEOF_LONG
    return ULONG2NUM(n);
#else
    return ULL2NUM(n);
#endif
}

/*
  ------------------------------ GC profiler ------------------------------
*/

#define GC_PROFILE_RECORD_DEFAULT_SIZE 100

/* return sec in user time */
static double
getrusage_time(void)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_PROCESS_CPUTIME_ID)
    {
        static int try_clock_gettime = 1;
        struct timespec ts;
        if (try_clock_gettime && clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
            return ts.tv_sec + ts.tv_nsec * 1e-9;
        }
        else {
            try_clock_gettime = 0;
        }
    }
#endif

#ifdef RUSAGE_SELF
    {
        struct rusage usage;
        struct timeval time;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            time = usage.ru_utime;
            return time.tv_sec + time.tv_usec * 1e-6;
        }
    }
#endif

#ifdef _WIN32
    {
	FILETIME creation_time, exit_time, kernel_time, user_time;
	ULARGE_INTEGER ui;
	LONG_LONG q;
	double t;

	if (GetProcessTimes(GetCurrentProcess(),
			    &creation_time, &exit_time, &kernel_time, &user_time) != 0) {
	    memcpy(&ui, &user_time, sizeof(FILETIME));
	    q = ui.QuadPart / 10L;
	    t = (DWORD)(q % 1000000L) * 1e-6;
	    q /= 1000000L;
#ifdef __GNUC__
	    t += q;
#else
	    t += (double)(DWORD)(q >> 16) * (1 << 16);
	    t += (DWORD)q & ~(~0 << 16);
#endif
	    return t;
	}
    }
#endif

    return 0.0;
}

static inline void
gc_prof_setup_new_record(rb_objspace_t *objspace, int reason)
{
    if (objspace->profile.run) {
	size_t index = objspace->profile.next_index;
	gc_profile_record *record;

	/* create new record */
	objspace->profile.next_index++;

	if (!objspace->profile.records) {
	    objspace->profile.size = GC_PROFILE_RECORD_DEFAULT_SIZE;
	    objspace->profile.records = malloc(sizeof(gc_profile_record) * objspace->profile.size);
	}
	if (index >= objspace->profile.size) {
	    objspace->profile.size += 1000;
	    objspace->profile.records = realloc(objspace->profile.records, sizeof(gc_profile_record) * objspace->profile.size);
	}
	if (!objspace->profile.records) {
	    rb_bug("gc_profile malloc or realloc miss");
	}
	record = objspace->profile.current_record = &objspace->profile.records[objspace->profile.next_index - 1];
	MEMZERO(record, gc_profile_record, 1);

	/* setup before-GC parameter */
	record->flags = reason | (ruby_gc_stressful ? GPR_FLAG_STRESS : 0);
#if MALLOC_ALLOCATED_SIZE
	record->allocated_size = malloc_allocated_size;
#endif
#if GC_PROFILE_DETAIL_MEMORY
#ifdef RUSAGE_SELF
	{
	    struct rusage usage;
	    if (getrusage(RUSAGE_SELF, &usage) == 0) {
		record->maxrss = usage.ru_maxrss;
		record->minflt = usage.ru_minflt;
		record->majflt = usage.ru_majflt;
	    }
	}
#endif
#endif
    }
}

static inline void
gc_prof_timer_start(rb_objspace_t *objspace)
{
    if (gc_prof_enabled(objspace)) {
	gc_profile_record *record = gc_prof_record(objspace);
#if GC_PROFILE_MORE_DETAIL
	record->prepare_time = objspace->profile.prepare_time;
#endif
	record->gc_time = 0;
	record->gc_invoke_time = getrusage_time();
    }
}

static double
elapsed_time_from(double time)
{
    double now = getrusage_time();
    if (now > time) {
	return now - time;
    }
    else {
	return 0;
    }
}

static inline void
gc_prof_timer_stop(rb_objspace_t *objspace)
{
    if (gc_prof_enabled(objspace)) {
	gc_profile_record *record = gc_prof_record(objspace);
	record->gc_time = elapsed_time_from(record->gc_invoke_time);
	record->gc_invoke_time -= objspace->profile.invoke_time;
    }
}

static inline void
gc_prof_mark_timer_start(rb_objspace_t *objspace)
{
    if (RUBY_DTRACE_GC_MARK_BEGIN_ENABLED()) {
	RUBY_DTRACE_GC_MARK_BEGIN();
    }
#if GC_PROFILE_MORE_DETAIL
    if (gc_prof_enabled(objspace)) {
	gc_prof_record(objspace)->gc_mark_time = getrusage_time();
    }
#endif
}

static inline void
gc_prof_mark_timer_stop(rb_objspace_t *objspace)
{
    if (RUBY_DTRACE_GC_MARK_END_ENABLED()) {
	RUBY_DTRACE_GC_MARK_END();
    }
#if GC_PROFILE_MORE_DETAIL
    if (gc_prof_enabled(objspace)) {
        gc_profile_record *record = gc_prof_record(objspace);
	record->gc_mark_time = elapsed_time_from(record->gc_mark_time);
    }
#endif
}

static inline void
gc_prof_sweep_timer_start(rb_objspace_t *objspace)
{
    if (RUBY_DTRACE_GC_SWEEP_BEGIN_ENABLED()) {
	RUBY_DTRACE_GC_SWEEP_BEGIN();
    }
    if (gc_prof_enabled(objspace)) {
	gc_profile_record *record = gc_prof_record(objspace);

	if (record->gc_time > 0 || GC_PROFILE_MORE_DETAIL) {
	    objspace->profile.gc_sweep_start_time = getrusage_time();
	}
    }
}

static inline void
gc_prof_sweep_timer_stop(rb_objspace_t *objspace)
{
    if (RUBY_DTRACE_GC_SWEEP_END_ENABLED()) {
	RUBY_DTRACE_GC_SWEEP_END();
    }

    if (gc_prof_enabled(objspace)) {
	double sweep_time;
	gc_profile_record *record = gc_prof_record(objspace);

	if (record->gc_time > 0) {
	    sweep_time = elapsed_time_from(objspace->profile.gc_sweep_start_time);
	    /* need to accumulate GC time for lazy sweep after gc() */
	    record->gc_time += sweep_time;
	}
	else if (GC_PROFILE_MORE_DETAIL) {
	    sweep_time = elapsed_time_from(objspace->profile.gc_sweep_start_time);
	}

#if GC_PROFILE_MORE_DETAIL
	record->gc_sweep_time += sweep_time;
	if (heap_pages_deferred_final) record->flags |= GPR_FLAG_HAVE_FINALIZE;
#endif
	if (heap_pages_deferred_final) objspace->profile.latest_gc_info |= GPR_FLAG_HAVE_FINALIZE;
    }
}

static inline void
gc_prof_set_malloc_info(rb_objspace_t *objspace)
{
#if GC_PROFILE_MORE_DETAIL
    if (gc_prof_enabled(objspace)) {
        gc_profile_record *record = gc_prof_record(objspace);
	record->allocate_increase = malloc_increase;
	record->allocate_limit = malloc_limit;
    }
#endif
}

static inline void
gc_prof_set_heap_info(rb_objspace_t *objspace)
{
    if (gc_prof_enabled(objspace)) {
	gc_profile_record *record = gc_prof_record(objspace);
	size_t live = objspace->profile.total_allocated_objects_at_gc_start - objspace->profile.total_freed_objects;
	size_t total = objspace->profile.heap_used_at_gc_start * HEAP_OBJ_LIMIT;

#if GC_PROFILE_MORE_DETAIL
	record->heap_use_pages = objspace->profile.heap_used_at_gc_start;
	record->heap_live_objects = live;
	record->heap_free_objects = total - live;
#endif

	record->heap_total_objects = total;
	record->heap_use_size = live * sizeof(RVALUE);
	record->heap_total_size = total * sizeof(RVALUE);
    }
}

/*
 *  call-seq:
 *    GC::Profiler.clear          -> nil
 *
 *  Clears the GC profiler data.
 *
 */

static VALUE
gc_profile_clear(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    if (GC_PROFILE_RECORD_DEFAULT_SIZE * 2 < objspace->profile.size) {
        objspace->profile.size = GC_PROFILE_RECORD_DEFAULT_SIZE * 2;
        objspace->profile.records = realloc(objspace->profile.records, sizeof(gc_profile_record) * objspace->profile.size);
        if (!objspace->profile.records) {
            rb_memerror();
        }
    }
    MEMZERO(objspace->profile.records, gc_profile_record, objspace->profile.size);
    objspace->profile.next_index = 0;
    objspace->profile.current_record = 0;
    return Qnil;
}

/*
 *  call-seq:
 *     GC::Profiler.raw_data	-> [Hash, ...]
 *
 *  Returns an Array of individual raw profile data Hashes ordered
 *  from earliest to latest by +:GC_INVOKE_TIME+.
 *
 *  For example:
 *
 *    [
 *	{
 *	   :GC_TIME=>1.3000000000000858e-05,
 *	   :GC_INVOKE_TIME=>0.010634999999999999,
 *	   :HEAP_USE_SIZE=>289640,
 *	   :HEAP_TOTAL_SIZE=>588960,
 *	   :HEAP_TOTAL_OBJECTS=>14724,
 *	   :GC_IS_MARKED=>false
 *	},
 *      # ...
 *    ]
 *
 *  The keys mean:
 *
 *  +:GC_TIME+::
 *	Time elapsed in seconds for this GC run
 *  +:GC_INVOKE_TIME+::
 *	Time elapsed in seconds from startup to when the GC was invoked
 *  +:HEAP_USE_SIZE+::
 *	Total bytes of heap used
 *  +:HEAP_TOTAL_SIZE+::
 *	Total size of heap in bytes
 *  +:HEAP_TOTAL_OBJECTS+::
 *	Total number of objects
 *  +:GC_IS_MARKED+::
 *	Returns +true+ if the GC is in mark phase
 *
 *  If ruby was built with +GC_PROFILE_MORE_DETAIL+, you will also have access
 *  to the following hash keys:
 *
 *  +:GC_MARK_TIME+::
 *  +:GC_SWEEP_TIME+::
 *  +:ALLOCATE_INCREASE+::
 *  +:ALLOCATE_LIMIT+::
 *  +:HEAP_USE_PAGES+::
 *  +:HEAP_LIVE_OBJECTS+::
 *  +:HEAP_FREE_OBJECTS+::
 *  +:HAVE_FINALIZE+::
 *
 */

static VALUE
gc_profile_record_get(void)
{
    VALUE prof;
    VALUE gc_profile = rb_ary_new();
    size_t i;
    rb_objspace_t *objspace = (&rb_objspace);

    if (!objspace->profile.run) {
	return Qnil;
    }

    for (i =0; i < objspace->profile.next_index; i++) {
	gc_profile_record *record = &objspace->profile.records[i];

	prof = rb_hash_new();
	rb_hash_aset(prof, ID2SYM(rb_intern("GC_FLAGS")), gc_info_decode(0, rb_hash_new(), record->flags));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_TIME")), DBL2NUM(record->gc_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_INVOKE_TIME")), DBL2NUM(record->gc_invoke_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_USE_SIZE")), SIZET2NUM(record->heap_use_size));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_TOTAL_SIZE")), SIZET2NUM(record->heap_total_size));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_TOTAL_OBJECTS")), SIZET2NUM(record->heap_total_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_IS_MARKED")), Qtrue);
#if GC_PROFILE_MORE_DETAIL
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_MARK_TIME")), DBL2NUM(record->gc_mark_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_SWEEP_TIME")), DBL2NUM(record->gc_sweep_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("ALLOCATE_INCREASE")), SIZET2NUM(record->allocate_increase));
        rb_hash_aset(prof, ID2SYM(rb_intern("ALLOCATE_LIMIT")), SIZET2NUM(record->allocate_limit));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_USE_PAGES")), SIZET2NUM(record->heap_use_pages));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_LIVE_OBJECTS")), SIZET2NUM(record->heap_live_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_FREE_OBJECTS")), SIZET2NUM(record->heap_free_objects));

	rb_hash_aset(prof, ID2SYM(rb_intern("REMOVING_OBJECTS")), SIZET2NUM(record->removing_objects));
	rb_hash_aset(prof, ID2SYM(rb_intern("EMPTY_OBJECTS")), SIZET2NUM(record->empty_objects));

	rb_hash_aset(prof, ID2SYM(rb_intern("HAVE_FINALIZE")), (record->flags & GPR_FLAG_HAVE_FINALIZE) ? Qtrue : Qfalse);
#endif

#if RGENGC_PROFILE > 0
	rb_hash_aset(prof, ID2SYM(rb_intern("OLD_OBJECTS")), SIZET2NUM(record->old_objects));
	rb_hash_aset(prof, ID2SYM(rb_intern("REMEMBERED_NORMAL_OBJECTS")), SIZET2NUM(record->remembered_normal_objects));
	rb_hash_aset(prof, ID2SYM(rb_intern("REMEMBERED_SHADY_OBJECTS")), SIZET2NUM(record->remembered_shady_objects));
#endif
	rb_ary_push(gc_profile, prof);
    }

    return gc_profile;
}

#if GC_PROFILE_MORE_DETAIL
#define MAJOR_REASON_MAX 0x10

static char *
gc_profile_dump_major_reason(int flags, char *buff)
{
    int reason = flags & GPR_FLAG_MAJOR_MASK;
    int i = 0;

    if (reason == GPR_FLAG_NONE) {
	buff[0] = '-';
	buff[1] = 0;
    }
    else {
#define C(x, s) \
  if (reason & GPR_FLAG_MAJOR_BY_##x) { \
      buff[i++] = #x[0]; \
      if (i >= MAJOR_REASON_MAX) rb_bug("gc_profile_dump_major_reason: overflow"); \
      buff[i] = 0; \
  }
	C(NOFREE, N);
	C(OLDGEN, O);
	C(SHADY,  S);
	C(RESCAN, R);
	C(STRESS, T);
#if RGENGC_ESTIMATE_OLDMALLOC
	C(OLDMALLOC, M);
#endif
#undef C
    }
    return buff;
}
#endif

static void
gc_profile_dump_on(VALUE out, VALUE (*append)(VALUE, VALUE))
{
    rb_objspace_t *objspace = &rb_objspace;
    size_t count = objspace->profile.next_index;
#ifdef MAJOR_REASON_MAX
    char reason_str[MAJOR_REASON_MAX];
#endif

    if (objspace->profile.run && count /* > 1 */) {
	size_t i;
	const gc_profile_record *record;

	append(out, rb_sprintf("GC %"PRIuSIZE" invokes.\n", objspace->profile.count));
	append(out, rb_str_new_cstr("Index    Invoke Time(sec)       Use Size(byte)     Total Size(byte)         Total Object                    GC Time(ms)\n"));

	for (i = 0; i < count; i++) {
	    record = &objspace->profile.records[i];
	    append(out, rb_sprintf("%5"PRIdSIZE" %19.3f %20"PRIuSIZE" %20"PRIuSIZE" %20"PRIuSIZE" %30.20f\n",
				   i+1, record->gc_invoke_time, record->heap_use_size,
				   record->heap_total_size, record->heap_total_objects, record->gc_time*1000));
	}

#if GC_PROFILE_MORE_DETAIL
	append(out, rb_str_new_cstr("\n\n" \
				    "More detail.\n" \
				    "Prepare Time = Previously GC's rest sweep time\n"
				    "Index Flags          Allocate Inc.  Allocate Limit"
#if CALC_EXACT_MALLOC_SIZE
				    "  Allocated Size"
#endif
				    "  Use Page     Mark Time(ms)    Sweep Time(ms)  Prepare Time(ms)  LivingObj    FreeObj RemovedObj   EmptyObj"
#if RGENGC_PROFILE
				    " OldgenObj RemNormObj RemShadObj"
#endif
#if GC_PROFILE_DETAIL_MEMORY
				    " MaxRSS(KB) MinorFLT MajorFLT"
#endif
				    "\n"));

	for (i = 0; i < count; i++) {
	    record = &objspace->profile.records[i];
	    append(out, rb_sprintf("%5"PRIdSIZE" %4s/%c/%6s%c %13"PRIuSIZE" %15"PRIuSIZE
#if CALC_EXACT_MALLOC_SIZE
				   " %15"PRIuSIZE
#endif
				   " %9"PRIuSIZE" %17.12f %17.12f %17.12f %10"PRIuSIZE" %10"PRIuSIZE" %10"PRIuSIZE" %10"PRIuSIZE
#if RGENGC_PROFILE
				   "%10"PRIuSIZE" %10"PRIuSIZE" %10"PRIuSIZE
#endif
#if GC_PROFILE_DETAIL_MEMORY
				   "%11ld %8ld %8ld"
#endif

				   "\n",
				   i+1,
				   gc_profile_dump_major_reason(record->flags, reason_str),
				   (record->flags & GPR_FLAG_HAVE_FINALIZE) ? 'F' : '.',
				   (record->flags & GPR_FLAG_NEWOBJ) ? "NEWOBJ" :
				   (record->flags & GPR_FLAG_MALLOC) ? "MALLOC" :
				   (record->flags & GPR_FLAG_METHOD) ? "METHOD" :
				   (record->flags & GPR_FLAG_CAPI)   ? "CAPI__" : "??????",
				   (record->flags & GPR_FLAG_STRESS) ? '!' : ' ',
				   record->allocate_increase, record->allocate_limit,
#if CALC_EXACT_MALLOC_SIZE
				   record->allocated_size,
#endif
				   record->heap_use_pages,
				   record->gc_mark_time*1000,
				   record->gc_sweep_time*1000,
				   record->prepare_time*1000,

				   record->heap_live_objects,
				   record->heap_free_objects,
				   record->removing_objects,
				   record->empty_objects
#if RGENGC_PROFILE
				   ,
				   record->old_objects,
				   record->remembered_normal_objects,
				   record->remembered_shady_objects
#endif
#if GC_PROFILE_DETAIL_MEMORY
				   ,
				   record->maxrss / 1024,
				   record->minflt,
				   record->majflt
#endif

		       ));
	}
#endif
    }
}

/*
 *  call-seq:
 *     GC::Profiler.result  -> String
 *
 *  Returns a profile data report such as:
 *
 *    GC 1 invokes.
 *    Index    Invoke Time(sec)       Use Size(byte)     Total Size(byte)         Total Object                    GC time(ms)
 *        1               0.012               159240               212940                10647         0.00000000000001530000
 */

static VALUE
gc_profile_result(void)
{
	VALUE str = rb_str_buf_new(0);
	gc_profile_dump_on(str, rb_str_buf_append);
	return str;
}

/*
 *  call-seq:
 *     GC::Profiler.report
 *     GC::Profiler.report(io)
 *
 *  Writes the GC::Profiler.result to <tt>$stdout</tt> or the given IO object.
 *
 */

static VALUE
gc_profile_report(int argc, VALUE *argv, VALUE self)
{
    VALUE out;

    if (argc == 0) {
	out = rb_stdout;
    }
    else {
	rb_scan_args(argc, argv, "01", &out);
    }
    gc_profile_dump_on(out, rb_io_write);

    return Qnil;
}

/*
 *  call-seq:
 *     GC::Profiler.total_time	-> float
 *
 *  The total time used for garbage collection in seconds
 */

static VALUE
gc_profile_total_time(VALUE self)
{
    double time = 0;
    rb_objspace_t *objspace = &rb_objspace;

    if (objspace->profile.run && objspace->profile.next_index > 0) {
	size_t i;
	size_t count = objspace->profile.next_index;

	for (i = 0; i < count; i++) {
	    time += objspace->profile.records[i].gc_time;
	}
    }
    return DBL2NUM(time);
}

/*
 *  call-seq:
 *    GC::Profiler.enabled?	-> true or false
 *
 *  The current status of GC profile mode.
 */

static VALUE
gc_profile_enable_get(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return objspace->profile.run ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *    GC::Profiler.enable	-> nil
 *
 *  Starts the GC profiler.
 *
 */

static VALUE
gc_profile_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    objspace->profile.run = TRUE;
    objspace->profile.current_record = 0;
    return Qnil;
}

/*
 *  call-seq:
 *    GC::Profiler.disable	-> nil
 *
 *  Stops the GC profiler.
 *
 */

static VALUE
gc_profile_disable(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    objspace->profile.run = FALSE;
    objspace->profile.current_record = 0;
    return Qnil;
}

/*
  ------------------------------ DEBUG ------------------------------
*/

static const char *
type_name(int type, VALUE obj)
{
    switch (type) {
#define TYPE_NAME(t) case (t): return #t;
	    TYPE_NAME(T_NONE);
	    TYPE_NAME(T_OBJECT);
	    TYPE_NAME(T_CLASS);
	    TYPE_NAME(T_MODULE);
	    TYPE_NAME(T_FLOAT);
	    TYPE_NAME(T_STRING);
	    TYPE_NAME(T_REGEXP);
	    TYPE_NAME(T_ARRAY);
	    TYPE_NAME(T_HASH);
	    TYPE_NAME(T_STRUCT);
	    TYPE_NAME(T_BIGNUM);
	    TYPE_NAME(T_FILE);
	    TYPE_NAME(T_MATCH);
	    TYPE_NAME(T_COMPLEX);
	    TYPE_NAME(T_RATIONAL);
	    TYPE_NAME(T_NIL);
	    TYPE_NAME(T_TRUE);
	    TYPE_NAME(T_FALSE);
	    TYPE_NAME(T_SYMBOL);
	    TYPE_NAME(T_FIXNUM);
	    TYPE_NAME(T_UNDEF);
	    TYPE_NAME(T_IMEMO);
	    TYPE_NAME(T_NODE);
	    TYPE_NAME(T_ICLASS);
	    TYPE_NAME(T_ZOMBIE);
      case T_DATA:
	if (obj && rb_objspace_data_type_name(obj)) {
	    return rb_objspace_data_type_name(obj);
	}
	return "T_DATA";
#undef TYPE_NAME
    }
    return "unknown";
}

static const char *
obj_type_name(VALUE obj)
{
    return type_name(TYPE(obj), obj);
}

static const char *
method_type_name(rb_method_type_t type)
{
    switch (type) {
      case VM_METHOD_TYPE_ISEQ:           return "iseq";
      case VM_METHOD_TYPE_ATTRSET:        return "attrest";
      case VM_METHOD_TYPE_IVAR:           return "ivar";
      case VM_METHOD_TYPE_BMETHOD:        return "bmethod";
      case VM_METHOD_TYPE_ALIAS:          return "alias";
      case VM_METHOD_TYPE_REFINED:        return "refined";
      case VM_METHOD_TYPE_CFUNC:          return "cfunc";
      case VM_METHOD_TYPE_ZSUPER:         return "zsuper";
      case VM_METHOD_TYPE_MISSING:        return "missing";
      case VM_METHOD_TYPE_OPTIMIZED:      return "optimized";
      case VM_METHOD_TYPE_UNDEF:          return "undef";
      case VM_METHOD_TYPE_NOTIMPLEMENTED: return "notimplemented";
    }
    rb_bug("method_type_name: unreachable (type: %d)", type);
}

/* from array.c */
# define ARY_SHARED_P(ary) \
    (assert(!FL_TEST((ary), ELTS_SHARED) || !FL_TEST((ary), RARRAY_EMBED_FLAG)), \
     FL_TEST((ary),ELTS_SHARED)!=0)
# define ARY_EMBED_P(ary) \
    (assert(!FL_TEST((ary), ELTS_SHARED) || !FL_TEST((ary), RARRAY_EMBED_FLAG)), \
     FL_TEST((ary), RARRAY_EMBED_FLAG)!=0)

const char *
rb_raw_obj_info(char *buff, const int buff_size, VALUE obj)
{
    const int age = RVALUE_FLAGS_AGE(RBASIC(obj)->flags);
    const int type = BUILTIN_TYPE(obj);

#define TF(c) ((c) != 0 ? "true" : "false")
#define C(c, s) ((c) != 0 ? (s) : " ")
    snprintf(buff, buff_size, "%p [%d%s%s%s%s] %s",
	     (void *)obj, age,
	     C(RVALUE_UNCOLLECTIBLE_BITMAP(obj),  "L"),
	     C(RVALUE_MARK_BITMAP(obj),           "M"),
	     C(RVALUE_MARKING_BITMAP(obj),        "R"),
	     C(RVALUE_WB_UNPROTECTED_BITMAP(obj), "U"),
	     obj_type_name(obj));

    if (internal_object_p(obj)) {
	/* ignore */
    }
    else if (RBASIC(obj)->klass == 0) {
	snprintf(buff, buff_size, "%s (temporary internal)", buff);
    }
    else {
	VALUE class_path = rb_class_path_cached(RBASIC(obj)->klass);
	if (!NIL_P(class_path)) {
	    snprintf(buff, buff_size, "%s (%s)", buff, RSTRING_PTR(class_path));
	}
    }

#if GC_DEBUG
    snprintf(buff, buff_size, "%s @%s:%d", buff, RANY(obj)->file, RANY(obj)->line);
#endif

    switch (type) {
      case T_NODE:
	snprintf(buff, buff_size, "%s (%s)", buff,
		 ruby_node_name(nd_type(obj)));
	break;
      case T_ARRAY:
	snprintf(buff, buff_size, "%s [%s%s] len: %d", buff,
		 C(ARY_EMBED_P(obj),  "E"),
		 C(ARY_SHARED_P(obj), "S"),
		 (int)RARRAY_LEN(obj));
	break;
      case T_STRING: {
	  snprintf(buff, buff_size, "%s %s", buff, RSTRING_PTR(obj));
	  break;
      }
      case T_CLASS: {
	  VALUE class_path = rb_class_path_cached(obj);
	  if (!NIL_P(class_path)) {
	      snprintf(buff, buff_size, "%s %s", buff, RSTRING_PTR(class_path));
	  }
	  break;
      }
      case T_DATA: {
	  const char * const type_name = rb_objspace_data_type_name(obj);
	  if (type_name) {
	      snprintf(buff, buff_size, "%s %s", buff, type_name);
	  }
	  break;
      }
      case T_IMEMO: {
	  const char *imemo_name;
	  switch (imemo_type(obj)) {
#define IMEMO_NAME(x) case imemo_##x: imemo_name = #x; break;
	      IMEMO_NAME(none);
	      IMEMO_NAME(cref);
	      IMEMO_NAME(svar);
	      IMEMO_NAME(throw_data);
	      IMEMO_NAME(ifunc);
	      IMEMO_NAME(memo);
	      IMEMO_NAME(ment);
	      IMEMO_NAME(iseq);
	    default: rb_bug("unknown IMEMO");
#undef IMEMO_NAME
	  }
	  snprintf(buff, buff_size, "%s %s", buff, imemo_name);

	  switch (imemo_type(obj)) {
	    case imemo_ment: {
		const rb_method_entry_t *me = &RANY(obj)->as.imemo.ment;
		snprintf(buff, buff_size, "%s (called_id: %s, type: %s, alias: %d, class: %s)", buff,
			 rb_id2name(me->called_id), method_type_name(me->def->type), me->def->alias_count, obj_info(me->defined_class));
		break;
	    }
	    case imemo_iseq: {
		const rb_iseq_t *iseq = (const rb_iseq_t *)obj;

		if (iseq->body->location.label) {
		    snprintf(buff, buff_size, "%s %s@%s:%d", buff,
			     RSTRING_PTR(iseq->body->location.label),
			     RSTRING_PTR(iseq->body->location.path),
			     FIX2INT(iseq->body->location.first_lineno));
		}
		break;
	    }
	    default:
	      break;
	  }
      }
      default:
	break;
    }
#undef TF
#undef C

    return buff;
}

#if RGENGC_OBJ_INFO
#define OBJ_INFO_BUFFERS_NUM  10
#define OBJ_INFO_BUFFERS_SIZE 0x100
static int obj_info_buffers_index = 0;
static char obj_info_buffers[OBJ_INFO_BUFFERS_NUM][OBJ_INFO_BUFFERS_SIZE];

static const char *
obj_info(VALUE obj)
{
    const int index = obj_info_buffers_index++;
    char *const buff = &obj_info_buffers[index][0];

    if (obj_info_buffers_index >= OBJ_INFO_BUFFERS_NUM) {
	obj_info_buffers_index = 0;
    }

    return rb_raw_obj_info(buff, OBJ_INFO_BUFFERS_SIZE, obj);
}
#else
static const char *
obj_info(VALUE obj)
{
    return obj_type_name(obj);
}
#endif

const char *
rb_obj_info(VALUE obj)
{
    if (!rb_special_const_p(obj)) {
	return obj_info(obj);
    }
    else {
	return obj_type_name(obj);
    }
}

#if GC_DEBUG

void
rb_gcdebug_print_obj_condition(VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;

    fprintf(stderr, "created at: %s:%d\n", RANY(obj)->file, RANY(obj)->line);

    if (is_pointer_to_heap(objspace, (void *)obj)) {
        fprintf(stderr, "pointer to heap?: true\n");
    }
    else {
        fprintf(stderr, "pointer to heap?: false\n");
        return;
    }

    fprintf(stderr, "marked?      : %s\n", MARKED_IN_BITMAP(GET_HEAP_MARK_BITS(obj), obj) ? "true" : "false");
#if USE_RGENGC
    fprintf(stderr, "age?         : %d\n", RVALUE_AGE(obj));
    fprintf(stderr, "old?         : %s\n", RVALUE_OLD_P(obj) ? "true" : "false");
    fprintf(stderr, "WB-protected?: %s\n", RVALUE_WB_UNPROTECTED(obj) ? "false" : "true");
    fprintf(stderr, "remembered?  : %s\n", RVALUE_REMEMBERED(obj) ? "true" : "false");
#endif

    if (is_lazy_sweeping(heap_eden)) {
        fprintf(stderr, "lazy sweeping?: true\n");
        fprintf(stderr, "swept?: %s\n", is_swept_object(objspace, obj) ? "done" : "not yet");
    }
    else {
        fprintf(stderr, "lazy sweeping?: false\n");
    }
}

static VALUE
gcdebug_sentinel(VALUE obj, VALUE name)
{
    fprintf(stderr, "WARNING: object %s(%p) is inadvertently collected\n", (char *)name, (void *)obj);
    return Qnil;
}

void
rb_gcdebug_sentinel(VALUE obj, const char *name)
{
    rb_define_finalizer(obj, rb_proc_new(gcdebug_sentinel, (VALUE)name));
}

#endif /* GC_DEBUG */

#if GC_DEBUG_STRESS_TO_CLASS
static VALUE
rb_gcdebug_add_stress_to_class(int argc, VALUE *argv, VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;

    if (!stress_to_class) {
	stress_to_class = rb_ary_tmp_new(argc);
    }
    rb_ary_cat(stress_to_class, argv, argc);
    return self;
}

static VALUE
rb_gcdebug_remove_stress_to_class(int argc, VALUE *argv, VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    int i;

    if (stress_to_class) {
	for (i = 0; i < argc; ++i) {
	    rb_ary_delete_same(stress_to_class, argv[i]);
	}
	if (RARRAY_LEN(stress_to_class) == 0) {
	    stress_to_class = 0;
	}
    }
    return Qnil;
}
#endif

/*
 * Document-module: ObjectSpace
 *
 *  The ObjectSpace module contains a number of routines
 *  that interact with the garbage collection facility and allow you to
 *  traverse all living objects with an iterator.
 *
 *  ObjectSpace also provides support for object finalizers, procs that will be
 *  called when a specific object is about to be destroyed by garbage
 *  collection.
 *
 *     require 'objspace'
 *
 *     a = "A"
 *     b = "B"
 *
 *     ObjectSpace.define_finalizer(a, proc {|id| puts "Finalizer one on #{id}" })
 *     ObjectSpace.define_finalizer(b, proc {|id| puts "Finalizer two on #{id}" })
 *
 *  _produces:_
 *
 *     Finalizer two on 537763470
 *     Finalizer one on 537763480
 */

/*
 *  Document-class: ObjectSpace::WeakMap
 *
 *  An ObjectSpace::WeakMap object holds references to
 *  any objects, but those objects can get garbage collected.
 *
 *  This class is mostly used internally by WeakRef, please use
 *  +lib/weakref.rb+ for the public interface.
 */

/*  Document-class: GC::Profiler
 *
 *  The GC profiler provides access to information on GC runs including time,
 *  length and object space size.
 *
 *  Example:
 *
 *    GC::Profiler.enable
 *
 *    require 'rdoc/rdoc'
 *
 *    GC::Profiler.report
 *
 *    GC::Profiler.disable
 *
 *  See also GC.count, GC.malloc_allocated_size and GC.malloc_allocations
 */

/*
 *  The GC module provides an interface to Ruby's mark and
 *  sweep garbage collection mechanism.
 *
 *  Some of the underlying methods are also available via the ObjectSpace
 *  module.
 *
 *  You may obtain information about the operation of the GC through
 *  GC::Profiler.
 */

void
Init_GC(void)
{
#undef rb_intern
    VALUE rb_mObjSpace;
    VALUE rb_mProfiler;
    VALUE gc_constants;

    rb_mGC = rb_define_module("GC");
    rb_define_singleton_method(rb_mGC, "start", gc_start_internal, -1);
    rb_define_singleton_method(rb_mGC, "enable", rb_gc_enable, 0);
    rb_define_singleton_method(rb_mGC, "disable", rb_gc_disable, 0);
    rb_define_singleton_method(rb_mGC, "stress", gc_stress_get, 0);
    rb_define_singleton_method(rb_mGC, "stress=", gc_stress_set_m, 1);
    rb_define_singleton_method(rb_mGC, "count", gc_count, 0);
    rb_define_singleton_method(rb_mGC, "stat", gc_stat, -1);
    rb_define_singleton_method(rb_mGC, "latest_gc_info", gc_latest_gc_info, -1);
    rb_define_method(rb_mGC, "garbage_collect", gc_start_internal, -1);

    gc_constants = rb_hash_new();
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RVALUE_SIZE")), SIZET2NUM(sizeof(RVALUE)));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("HEAP_OBJ_LIMIT")), SIZET2NUM(HEAP_OBJ_LIMIT));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("HEAP_BITMAP_SIZE")), SIZET2NUM(HEAP_BITMAP_SIZE));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("HEAP_BITMAP_PLANES")), SIZET2NUM(HEAP_BITMAP_PLANES));
    OBJ_FREEZE(gc_constants);
    rb_define_const(rb_mGC, "INTERNAL_CONSTANTS", gc_constants);

    rb_mProfiler = rb_define_module_under(rb_mGC, "Profiler");
    rb_define_singleton_method(rb_mProfiler, "enabled?", gc_profile_enable_get, 0);
    rb_define_singleton_method(rb_mProfiler, "enable", gc_profile_enable, 0);
    rb_define_singleton_method(rb_mProfiler, "raw_data", gc_profile_record_get, 0);
    rb_define_singleton_method(rb_mProfiler, "disable", gc_profile_disable, 0);
    rb_define_singleton_method(rb_mProfiler, "clear", gc_profile_clear, 0);
    rb_define_singleton_method(rb_mProfiler, "result", gc_profile_result, 0);
    rb_define_singleton_method(rb_mProfiler, "report", gc_profile_report, -1);
    rb_define_singleton_method(rb_mProfiler, "total_time", gc_profile_total_time, 0);

    rb_mObjSpace = rb_define_module("ObjectSpace");
    rb_define_module_function(rb_mObjSpace, "each_object", os_each_obj, -1);
    rb_define_module_function(rb_mObjSpace, "garbage_collect", gc_start_internal, -1);

    rb_define_module_function(rb_mObjSpace, "define_finalizer", define_final, -1);
    rb_define_module_function(rb_mObjSpace, "undefine_finalizer", undefine_final, 1);

    rb_define_module_function(rb_mObjSpace, "_id2ref", id2ref, 1);

    rb_vm_register_special_exception(ruby_error_nomemory, rb_eNoMemError, "failed to allocate memory");

    rb_define_method(rb_cBasicObject, "__id__", rb_obj_id, 0);
    rb_define_method(rb_mKernel, "object_id", rb_obj_id, 0);

    rb_define_module_function(rb_mObjSpace, "count_objects", count_objects, -1);

    {
	VALUE rb_cWeakMap = rb_define_class_under(rb_mObjSpace, "WeakMap", rb_cObject);
	rb_define_alloc_func(rb_cWeakMap, wmap_allocate);
	rb_define_method(rb_cWeakMap, "[]=", wmap_aset, 2);
	rb_define_method(rb_cWeakMap, "[]", wmap_aref, 1);
	rb_define_method(rb_cWeakMap, "include?", wmap_has_key, 1);
	rb_define_method(rb_cWeakMap, "member?", wmap_has_key, 1);
	rb_define_method(rb_cWeakMap, "key?", wmap_has_key, 1);
	rb_define_method(rb_cWeakMap, "inspect", wmap_inspect, 0);
	rb_define_method(rb_cWeakMap, "each", wmap_each, 0);
	rb_define_method(rb_cWeakMap, "each_pair", wmap_each, 0);
	rb_define_method(rb_cWeakMap, "each_key", wmap_each_key, 0);
	rb_define_method(rb_cWeakMap, "each_value", wmap_each_value, 0);
	rb_define_method(rb_cWeakMap, "keys", wmap_keys, 0);
	rb_define_method(rb_cWeakMap, "values", wmap_values, 0);
	rb_define_method(rb_cWeakMap, "size", wmap_size, 0);
	rb_define_method(rb_cWeakMap, "length", wmap_size, 0);
	rb_define_private_method(rb_cWeakMap, "finalize", wmap_finalize, 1);
	rb_include_module(rb_cWeakMap, rb_mEnumerable);
    }

    /* internal methods */
    rb_define_singleton_method(rb_mGC, "verify_internal_consistency", gc_verify_internal_consistency, 0);
#if MALLOC_ALLOCATED_SIZE
    rb_define_singleton_method(rb_mGC, "malloc_allocated_size", gc_malloc_allocated_size, 0);
    rb_define_singleton_method(rb_mGC, "malloc_allocations", gc_malloc_allocations, 0);
#endif

#if GC_DEBUG_STRESS_TO_CLASS
    rb_define_singleton_method(rb_mGC, "add_stress_to_class", rb_gcdebug_add_stress_to_class, -1);
    rb_define_singleton_method(rb_mGC, "remove_stress_to_class", rb_gcdebug_remove_stress_to_class, -1);
#endif

    /* ::GC::OPTS, which shows GC build options */
    {
	VALUE opts;
	rb_define_const(rb_mGC, "OPTS", opts = rb_ary_new());
#define OPT(o) if (o) rb_ary_push(opts, rb_fstring_lit(#o))
	OPT(GC_DEBUG);
	OPT(USE_RGENGC);
	OPT(RGENGC_DEBUG);
	OPT(RGENGC_CHECK_MODE);
	OPT(RGENGC_PROFILE);
	OPT(RGENGC_ESTIMATE_OLDMALLOC);
	OPT(GC_PROFILE_MORE_DETAIL);
	OPT(GC_ENABLE_LAZY_SWEEP);
	OPT(CALC_EXACT_MALLOC_SIZE);
	OPT(MALLOC_ALLOCATED_SIZE);
	OPT(MALLOC_ALLOCATED_SIZE_CHECK);
	OPT(GC_PROFILE_DETAIL_MEMORY);
#undef OPT
	OBJ_FREEZE(opts);
    }
}
/**********************************************************************

  string.c -

  $Author$
  created at: Mon Aug  9 17:12:58 JST 1993

  Copyright (C) 1993-2007 Yukihiro Matsumoto
  Copyright (C) 2000  Network Applied Communication Laboratory, Inc.
  Copyright (C) 2000  Information-technology Promotion Agency, Japan

**********************************************************************/

#include "internal.h"
#include "ruby/re.h"
#include "encindex.h"
#include "probes.h"
#include "gc.h"
#include <assert.h>

#define BEG(no) (regs->beg[(no)])
#define END(no) (regs->end[(no)])

#include <math.h>
#include <ctype.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#define STRING_ENUMERATORS_WANTARRAY 0 /* next major */

#undef rb_str_new
#undef rb_usascii_str_new
#undef rb_utf8_str_new
#undef rb_enc_str_new
#undef rb_str_new_cstr
#undef rb_tainted_str_new_cstr
#undef rb_usascii_str_new_cstr
#undef rb_utf8_str_new_cstr
#undef rb_enc_str_new_cstr
#undef rb_external_str_new_cstr
#undef rb_locale_str_new_cstr
#undef rb_str_dup_frozen
#undef rb_str_buf_new_cstr
#undef rb_str_buf_cat
#undef rb_str_buf_cat2
#undef rb_str_cat2
#undef rb_str_cat_cstr
#undef rb_fstring_cstr

static VALUE rb_str_clear(VALUE str);

VALUE rb_cString;
VALUE rb_cSymbol;

/* FLAGS of RString
 *
 * 1:     RSTRING_NOEMBED
 * 2:     STR_SHARED (== ELTS_SHARED)
 * 2-6:   RSTRING_EMBED_LEN (5 bits == 32)
 * 7:     STR_TMPLOCK
 * 8-9:   ENC_CODERANGE (2 bits)
 * 10-16: ENCODING (7 bits == 128)
 * 17:    RSTRING_FSTR
 * 18:    STR_NOFREE
 * 19:    STR_FAKESTR
 */

#define RUBY_MAX_CHAR_LEN 16
#define STR_TMPLOCK FL_USER7
#define STR_NOFREE FL_USER18
#define STR_FAKESTR FL_USER19

#define STR_SET_NOEMBED(str) do {\
    FL_SET((str), STR_NOEMBED);\
    STR_SET_EMBED_LEN((str), 0);\
} while (0)
#define STR_SET_EMBED(str) FL_UNSET((str), (STR_NOEMBED|STR_NOFREE))
#define STR_SET_EMBED_LEN(str, n) do { \
    long tmp_n = (n);\
    RBASIC(str)->flags &= ~RSTRING_EMBED_LEN_MASK;\
    RBASIC(str)->flags |= (tmp_n) << RSTRING_EMBED_LEN_SHIFT;\
} while (0)

#define STR_SET_LEN(str, n) do { \
    if (STR_EMBED_P(str)) {\
	STR_SET_EMBED_LEN((str), (n));\
    }\
    else {\
	RSTRING(str)->as.heap.len = (n);\
    }\
} while (0)

#define STR_DEC_LEN(str) do {\
    if (STR_EMBED_P(str)) {\
	long n = RSTRING_LEN(str);\
	n--;\
	STR_SET_EMBED_LEN((str), n);\
    }\
    else {\
	RSTRING(str)->as.heap.len--;\
    }\
} while (0)

#define TERM_LEN(str) rb_enc_mbminlen(rb_enc_get(str))
#define TERM_FILL(ptr, termlen) do {\
    char *const term_fill_ptr = (ptr);\
    const int term_fill_len = (termlen);\
    *term_fill_ptr = '\0';\
    if (UNLIKELY(term_fill_len > 1))\
	memset(term_fill_ptr, 0, term_fill_len);\
} while (0)

#define RESIZE_CAPA(str,capacity) do {\
    const int termlen = TERM_LEN(str);\
    RESIZE_CAPA_TERM(str,capacity,termlen);\
} while (0)
#define RESIZE_CAPA_TERM(str,capacity,termlen) do {\
    if (STR_EMBED_P(str)) {\
	if ((capacity) > RSTRING_EMBED_LEN_MAX) {\
	    char *const tmp = ALLOC_N(char, (capacity)+termlen);\
	    const long tlen = RSTRING_LEN(str);\
	    memcpy(tmp, RSTRING_PTR(str), tlen);\
	    RSTRING(str)->as.heap.ptr = tmp;\
	    RSTRING(str)->as.heap.len = tlen;\
            STR_SET_NOEMBED(str);\
	    RSTRING(str)->as.heap.aux.capa = (capacity);\
	}\
    }\
    else {\
	assert(!FL_TEST((str), STR_SHARED)); \
	REALLOC_N(RSTRING(str)->as.heap.ptr, char, (capacity)+termlen);\
	RSTRING(str)->as.heap.aux.capa = (capacity);\
    }\
} while (0)

#define STR_SET_SHARED(str, shared_str) do { \
    if (!FL_TEST(str, STR_FAKESTR)) { \
	RB_OBJ_WRITE((str), &RSTRING(str)->as.heap.aux.shared, (shared_str)); \
	FL_SET((str), STR_SHARED); \
    } \
} while (0)

#define STR_HEAP_PTR(str)  (RSTRING(str)->as.heap.ptr)
#define STR_HEAP_SIZE(str) (RSTRING(str)->as.heap.aux.capa + TERM_LEN(str))

#define STR_ENC_GET(str) get_encoding(str)

#if !defined SHARABLE_MIDDLE_SUBSTRING
# define SHARABLE_MIDDLE_SUBSTRING 0
#endif
#if !SHARABLE_MIDDLE_SUBSTRING
#define SHARABLE_SUBSTRING_P(beg, len, end) ((beg) + (len) == (end))
#else
#define SHARABLE_SUBSTRING_P(beg, len, end) 1
#endif

static VALUE str_replace_shared_without_enc(VALUE str2, VALUE str);
static VALUE str_new_shared(VALUE klass, VALUE str);
static VALUE str_new_frozen(VALUE klass, VALUE orig);
static VALUE str_new_static(VALUE klass, const char *ptr, long len, int encindex);
static void str_make_independent_expand(VALUE str, long expand);

static rb_encoding *
get_actual_encoding(const int encidx, VALUE str)
{
    const unsigned char *q;

    switch (encidx) {
      case ENCINDEX_UTF_16:
	if (RSTRING_LEN(str) < 2) break;
	q = (const unsigned char *)RSTRING_PTR(str);
	if (q[0] == 0xFE && q[1] == 0xFF) {
	    return rb_enc_get_from_index(ENCINDEX_UTF_16BE);
	}
	if (q[0] == 0xFF && q[1] == 0xFE) {
	    return rb_enc_get_from_index(ENCINDEX_UTF_16LE);
	}
	return rb_ascii8bit_encoding();
      case ENCINDEX_UTF_32:
	if (RSTRING_LEN(str) < 4) break;
	q = (const unsigned char *)RSTRING_PTR(str);
	if (q[0] == 0 && q[1] == 0 && q[2] == 0xFE && q[3] == 0xFF) {
	    return rb_enc_get_from_index(ENCINDEX_UTF_32BE);
	}
	if (q[3] == 0 && q[2] == 0 && q[1] == 0xFE && q[0] == 0xFF) {
	    return rb_enc_get_from_index(ENCINDEX_UTF_32LE);
	}
	return rb_ascii8bit_encoding();
    }
    return rb_enc_from_index(encidx);
}

static rb_encoding *
get_encoding(VALUE str)
{
    return get_actual_encoding(ENCODING_GET(str), str);
}

static void
mustnot_broken(VALUE str)
{
    if (is_broken_string(str)) {
	rb_raise(rb_eArgError, "invalid byte sequence in %s", rb_enc_name(STR_ENC_GET(str)));
    }
}

static void
mustnot_wchar(VALUE str)
{
    rb_encoding *enc = STR_ENC_GET(str);
    if (rb_enc_mbminlen(enc) > 1) {
	rb_raise(rb_eArgError, "wide char encoding: %s", rb_enc_name(enc));
    }
}

static int fstring_cmp(VALUE a, VALUE b);

static VALUE register_fstring(VALUE str);

st_table *rb_vm_fstring_table(void);

const struct st_hash_type rb_fstring_hash_type = {
    fstring_cmp,
    rb_str_hash,
};

#define BARE_STRING_P(str) (!FL_ANY_RAW(str, FL_TAINT|FL_EXIVAR) && RBASIC_CLASS(str) == rb_cString)

static int
fstr_update_callback(st_data_t *key, st_data_t *value, st_data_t arg, int existing)
{
    VALUE *fstr = (VALUE *)arg;
    VALUE str = (VALUE)*key;

    if (existing) {
	/* because of lazy sweep, str may be unmarked already and swept
	 * at next time */

	if (rb_objspace_garbage_object_p(str)) {
	    *fstr = Qundef;
	    return ST_DELETE;
	}

	*fstr = str;
	return ST_STOP;
    }
    else {
	if (FL_TEST_RAW(str, STR_FAKESTR)) {
	    str = str_new_static(rb_cString, RSTRING(str)->as.heap.ptr,
				 RSTRING(str)->as.heap.len,
				 ENCODING_GET(str));
	    OBJ_FREEZE_RAW(str);
	}
	else {
	    str = str_new_frozen(rb_cString, str);
	    if (STR_SHARED_P(str)) { /* str should not be shared */
		/* shared substring  */
		str_make_independent_expand(str, 0L);
		assert(OBJ_FROZEN(str));
	    }
	    if (!BARE_STRING_P(str)) {
		str = str_new_frozen(rb_cString, str);
	    }
	}
	RBASIC(str)->flags |= RSTRING_FSTR;

	*key = *value = *fstr = str;
	return ST_CONTINUE;
    }
}

RUBY_FUNC_EXPORTED
VALUE
rb_fstring(VALUE str)
{
    VALUE fstr;
    int bare;

    Check_Type(str, T_STRING);

    if (FL_TEST(str, RSTRING_FSTR))
	return str;

    bare = BARE_STRING_P(str);
    if (STR_EMBED_P(str) && !bare) {
	OBJ_FREEZE_RAW(str);
	return str;
    }

    fstr = register_fstring(str);

    if (!bare) {
	str_replace_shared_without_enc(str, fstr);
	OBJ_FREEZE_RAW(str);
	return str;
    }
    return fstr;
}

static VALUE
register_fstring(VALUE str)
{
    VALUE ret;
    st_table *frozen_strings = rb_vm_fstring_table();

    do {
	ret = str;
	st_update(frozen_strings, (st_data_t)str,
		  fstr_update_callback, (st_data_t)&ret);
    } while (ret == Qundef);

    assert(OBJ_FROZEN(ret));
    assert(!FL_TEST_RAW(ret, STR_FAKESTR));
    assert(!FL_TEST_RAW(ret, FL_EXIVAR));
    assert(!FL_TEST_RAW(ret, FL_TAINT));
    assert(RBASIC_CLASS(ret) == rb_cString);
    return ret;
}

static VALUE
setup_fake_str(struct RString *fake_str, const char *name, long len, int encidx)
{
    fake_str->basic.flags = T_STRING|RSTRING_NOEMBED|STR_NOFREE|STR_FAKESTR;
    /* SHARED to be allocated by the callback */

    ENCODING_SET_INLINED((VALUE)fake_str, encidx);

    RBASIC_SET_CLASS_RAW((VALUE)fake_str, rb_cString);
    fake_str->as.heap.len = len;
    fake_str->as.heap.ptr = (char *)name;
    fake_str->as.heap.aux.capa = len;
    return (VALUE)fake_str;
}

VALUE
rb_setup_fake_str(struct RString *fake_str, const char *name, long len, rb_encoding *enc)
{
    return setup_fake_str(fake_str, name, len, rb_enc_to_index(enc));
}

VALUE
rb_fstring_new(const char *ptr, long len)
{
    struct RString fake_str;
    return register_fstring(setup_fake_str(&fake_str, ptr, len, ENCINDEX_US_ASCII));
}

VALUE
rb_fstring_cstr(const char *ptr)
{
    return rb_fstring_new(ptr, strlen(ptr));
}

static int
fstring_set_class_i(st_data_t key, st_data_t val, st_data_t arg)
{
    RBASIC_SET_CLASS((VALUE)key, (VALUE)arg);
    return ST_CONTINUE;
}

static int
fstring_cmp(VALUE a, VALUE b)
{
    long alen, blen;
    const char *aptr, *bptr;
    RSTRING_GETMEM(a, aptr, alen);
    RSTRING_GETMEM(b, bptr, blen);
    return (alen != blen ||
	    ENCODING_GET(a) != ENCODING_GET(b) ||
	    memcmp(aptr, bptr, alen) != 0);
}

static inline int
single_byte_optimizable(VALUE str)
{
    rb_encoding *enc;

    /* Conservative.  It may be ENC_CODERANGE_UNKNOWN. */
    if (ENC_CODERANGE(str) == ENC_CODERANGE_7BIT)
        return 1;

    enc = STR_ENC_GET(str);
    if (rb_enc_mbmaxlen(enc) == 1)
        return 1;

    /* Conservative.  Possibly single byte.
     * "\xa1" in Shift_JIS for example. */
    return 0;
}

VALUE rb_fs;

static inline const char *
search_nonascii(const char *p, const char *e)
{
#if SIZEOF_VOIDP == 8
# define NONASCII_MASK 0x8080808080808080ULL
#elif SIZEOF_VOIDP == 4
# define NONASCII_MASK 0x80808080UL
#endif
#ifdef NONASCII_MASK
    if ((int)SIZEOF_VOIDP * 2 < e - p) {
        const uintptr_t *s, *t;
        const uintptr_t lowbits = SIZEOF_VOIDP - 1;
        s = (const uintptr_t*)(~lowbits & ((uintptr_t)p + lowbits));
        while (p < (const char *)s) {
            if (!ISASCII(*p))
                return p;
            p++;
        }
        t = (const uintptr_t*)(~lowbits & (uintptr_t)e);
        while (s < t) {
            if (*s & NONASCII_MASK) {
                t = s;
                break;
            }
            s++;
        }
        p = (const char *)t;
    }
#endif
    while (p < e) {
        if (!ISASCII(*p))
            return p;
        p++;
    }
    return NULL;
}

static int
coderange_scan(const char *p, long len, rb_encoding *enc)
{
    const char *e = p + len;

    if (rb_enc_to_index(enc) == rb_ascii8bit_encindex()) {
        /* enc is ASCII-8BIT.  ASCII-8BIT string never be broken. */
        p = search_nonascii(p, e);
        return p ? ENC_CODERANGE_VALID : ENC_CODERANGE_7BIT;
    }

    if (rb_enc_asciicompat(enc)) {
        p = search_nonascii(p, e);
        if (!p) return ENC_CODERANGE_7BIT;
        for (;;) {
            int ret = rb_enc_precise_mbclen(p, e, enc);
            if (!MBCLEN_CHARFOUND_P(ret)) return ENC_CODERANGE_BROKEN;
            p += MBCLEN_CHARFOUND_LEN(ret);
            if (p == e) break;
            p = search_nonascii(p, e);
            if (!p) break;
        }
    }
    else {
        while (p < e) {
            int ret = rb_enc_precise_mbclen(p, e, enc);
            if (!MBCLEN_CHARFOUND_P(ret)) return ENC_CODERANGE_BROKEN;
            p += MBCLEN_CHARFOUND_LEN(ret);
        }
    }
    return ENC_CODERANGE_VALID;
}

long
rb_str_coderange_scan_restartable(const char *s, const char *e, rb_encoding *enc, int *cr)
{
    const char *p = s;

    if (*cr == ENC_CODERANGE_BROKEN)
	return e - s;

    if (rb_enc_to_index(enc) == rb_ascii8bit_encindex()) {
	/* enc is ASCII-8BIT.  ASCII-8BIT string never be broken. */
	if (*cr == ENC_CODERANGE_VALID) return e - s;
	p = search_nonascii(p, e);
        *cr = p ? ENC_CODERANGE_VALID : ENC_CODERANGE_7BIT;
	return e - s;
    }
    else if (rb_enc_asciicompat(enc)) {
	p = search_nonascii(p, e);
	if (!p) {
	    if (*cr != ENC_CODERANGE_VALID) *cr = ENC_CODERANGE_7BIT;
	    return e - s;
	}
	for (;;) {
	    int ret = rb_enc_precise_mbclen(p, e, enc);
	    if (!MBCLEN_CHARFOUND_P(ret)) {
		*cr = MBCLEN_INVALID_P(ret) ? ENC_CODERANGE_BROKEN: ENC_CODERANGE_UNKNOWN;
		return p - s;
	    }
	    p += MBCLEN_CHARFOUND_LEN(ret);
	    if (p == e) break;
	    p = search_nonascii(p, e);
	    if (!p) break;
	}
    }
    else {
	while (p < e) {
	    int ret = rb_enc_precise_mbclen(p, e, enc);
	    if (!MBCLEN_CHARFOUND_P(ret)) {
		*cr = MBCLEN_INVALID_P(ret) ? ENC_CODERANGE_BROKEN: ENC_CODERANGE_UNKNOWN;
		return p - s;
	    }
	    p += MBCLEN_CHARFOUND_LEN(ret);
	}
    }
    *cr = ENC_CODERANGE_VALID;
    return e - s;
}

static inline void
str_enc_copy(VALUE str1, VALUE str2)
{
    rb_enc_set_index(str1, ENCODING_GET(str2));
}

static void
rb_enc_cr_str_copy_for_substr(VALUE dest, VALUE src)
{
    /* this function is designed for copying encoding and coderange
     * from src to new string "dest" which is made from the part of src.
     */
    str_enc_copy(dest, src);
    if (RSTRING_LEN(dest) == 0) {
	if (!rb_enc_asciicompat(STR_ENC_GET(src)))
	    ENC_CODERANGE_SET(dest, ENC_CODERANGE_VALID);
	else
	    ENC_CODERANGE_SET(dest, ENC_CODERANGE_7BIT);
	return;
    }
    switch (ENC_CODERANGE(src)) {
      case ENC_CODERANGE_7BIT:
	ENC_CODERANGE_SET(dest, ENC_CODERANGE_7BIT);
	break;
      case ENC_CODERANGE_VALID:
	if (!rb_enc_asciicompat(STR_ENC_GET(src)) ||
	    search_nonascii(RSTRING_PTR(dest), RSTRING_END(dest)))
	    ENC_CODERANGE_SET(dest, ENC_CODERANGE_VALID);
	else
	    ENC_CODERANGE_SET(dest, ENC_CODERANGE_7BIT);
	break;
      default:
	break;
    }
}

static void
rb_enc_cr_str_exact_copy(VALUE dest, VALUE src)
{
    str_enc_copy(dest, src);
    ENC_CODERANGE_SET(dest, ENC_CODERANGE(src));
}

int
rb_enc_str_coderange(VALUE str)
{
    int cr = ENC_CODERANGE(str);

    if (cr == ENC_CODERANGE_UNKNOWN) {
	int encidx = ENCODING_GET(str);
	rb_encoding *enc = rb_enc_from_index(encidx);
	if (rb_enc_mbminlen(enc) > 1 && rb_enc_dummy_p(enc)) {
	    cr = ENC_CODERANGE_BROKEN;
	}
	else {
	    cr = coderange_scan(RSTRING_PTR(str), RSTRING_LEN(str),
				get_actual_encoding(encidx, str));
	}
        ENC_CODERANGE_SET(str, cr);
    }
    return cr;
}

int
rb_enc_str_asciionly_p(VALUE str)
{
    rb_encoding *enc = STR_ENC_GET(str);

    if (!rb_enc_asciicompat(enc))
        return FALSE;
    else if (rb_enc_str_coderange(str) == ENC_CODERANGE_7BIT)
        return TRUE;
    return FALSE;
}

static inline void
str_mod_check(VALUE s, const char *p, long len)
{
    if (RSTRING_PTR(s) != p || RSTRING_LEN(s) != len){
	rb_raise(rb_eRuntimeError, "string modified");
    }
}

size_t
rb_str_capacity(VALUE str)
{
    if (STR_EMBED_P(str)) {
	return RSTRING_EMBED_LEN_MAX;
    }
    else if (FL_TEST(str, STR_SHARED|STR_NOFREE)) {
	return RSTRING(str)->as.heap.len;
    }
    else {
	return RSTRING(str)->as.heap.aux.capa;
    }
}

static inline void
must_not_null(const char *ptr)
{
    if (!ptr) {
	rb_raise(rb_eArgError, "NULL pointer given");
    }
}

static inline VALUE
str_alloc(VALUE klass)
{
    NEWOBJ_OF(str, struct RString, klass, T_STRING | (RGENGC_WB_PROTECTED_STRING ? FL_WB_PROTECTED : 0));
    return (VALUE)str;
}

static inline VALUE
empty_str_alloc(VALUE klass)
{
    if (RUBY_DTRACE_STRING_CREATE_ENABLED()) {
	RUBY_DTRACE_STRING_CREATE(0, rb_sourcefile(), rb_sourceline());
    }
    return str_alloc(klass);
}

static VALUE
str_new0(VALUE klass, const char *ptr, long len, int termlen)
{
    VALUE str;

    if (len < 0) {
	rb_raise(rb_eArgError, "negative string size (or size too big)");
    }

    if (RUBY_DTRACE_STRING_CREATE_ENABLED()) {
	RUBY_DTRACE_STRING_CREATE(len, rb_sourcefile(), rb_sourceline());
    }

    str = str_alloc(klass);
    if (len > RSTRING_EMBED_LEN_MAX) {
	RSTRING(str)->as.heap.aux.capa = len;
	RSTRING(str)->as.heap.ptr = ALLOC_N(char, len + termlen);
	STR_SET_NOEMBED(str);
    }
    else if (len == 0) {
	ENC_CODERANGE_SET(str, ENC_CODERANGE_7BIT);
    }
    if (ptr) {
	memcpy(RSTRING_PTR(str), ptr, len);
    }
    STR_SET_LEN(str, len);
    TERM_FILL(RSTRING_PTR(str) + len, termlen);
    return str;
}

static VALUE
str_new(VALUE klass, const char *ptr, long len)
{
    return str_new0(klass, ptr, len, 1);
}

VALUE
rb_str_new(const char *ptr, long len)
{
    return str_new(rb_cString, ptr, len);
}

VALUE
rb_usascii_str_new(const char *ptr, long len)
{
    VALUE str = rb_str_new(ptr, len);
    ENCODING_CODERANGE_SET(str, rb_usascii_encindex(), ENC_CODERANGE_7BIT);
    return str;
}

VALUE
rb_utf8_str_new(const char *ptr, long len)
{
    VALUE str = str_new(rb_cString, ptr, len);
    rb_enc_associate_index(str, rb_utf8_encindex());
    return str;
}

VALUE
rb_enc_str_new(const char *ptr, long len, rb_encoding *enc)
{
    VALUE str;

    if (!enc) return rb_str_new(ptr, len);

    str = str_new0(rb_cString, ptr, len, rb_enc_mbminlen(enc));
    rb_enc_associate(str, enc);
    return str;
}

VALUE
rb_str_new_cstr(const char *ptr)
{
    must_not_null(ptr);
    return rb_str_new(ptr, strlen(ptr));
}

VALUE
rb_usascii_str_new_cstr(const char *ptr)
{
    VALUE str = rb_str_new_cstr(ptr);
    ENCODING_CODERANGE_SET(str, rb_usascii_encindex(), ENC_CODERANGE_7BIT);
    return str;
}

VALUE
rb_utf8_str_new_cstr(const char *ptr)
{
    VALUE str = rb_str_new_cstr(ptr);
    rb_enc_associate_index(str, rb_utf8_encindex());
    return str;
}

VALUE
rb_enc_str_new_cstr(const char *ptr, rb_encoding *enc)
{
    must_not_null(ptr);
    if (rb_enc_mbminlen(enc) != 1) {
	rb_raise(rb_eArgError, "wchar encoding given");
    }
    return rb_enc_str_new(ptr, strlen(ptr), enc);
}

static VALUE
str_new_static(VALUE klass, const char *ptr, long len, int encindex)
{
    VALUE str;

    if (len < 0) {
	rb_raise(rb_eArgError, "negative string size (or size too big)");
    }

    if (!ptr) {
	str = str_new(klass, ptr, len);
    }
    else {
	if (RUBY_DTRACE_STRING_CREATE_ENABLED()) {
	    RUBY_DTRACE_STRING_CREATE(len, rb_sourcefile(), rb_sourceline());
	}
	str = str_alloc(klass);
	RSTRING(str)->as.heap.len = len;
	RSTRING(str)->as.heap.ptr = (char *)ptr;
	RSTRING(str)->as.heap.aux.capa = len;
	STR_SET_NOEMBED(str);
	RBASIC(str)->flags |= STR_NOFREE;
    }
    rb_enc_associate_index(str, encindex);
    return str;
}

VALUE
rb_str_new_static(const char *ptr, long len)
{
    return str_new_static(rb_cString, ptr, len, 0);
}

VALUE
rb_usascii_str_new_static(const char *ptr, long len)
{
    return str_new_static(rb_cString, ptr, len, ENCINDEX_US_ASCII);
}

VALUE
rb_utf8_str_new_static(const char *ptr, long len)
{
    return str_new_static(rb_cString, ptr, len, ENCINDEX_UTF_8);
}

VALUE
rb_enc_str_new_static(const char *ptr, long len, rb_encoding *enc)
{
    return str_new_static(rb_cString, ptr, len, rb_enc_to_index(enc));
}

VALUE
rb_tainted_str_new(const char *ptr, long len)
{
    VALUE str = rb_str_new(ptr, len);

    OBJ_TAINT(str);
    return str;
}

VALUE
rb_tainted_str_new_cstr(const char *ptr)
{
    VALUE str = rb_str_new_cstr(ptr);

    OBJ_TAINT(str);
    return str;
}

VALUE
rb_str_conv_enc_opts(VALUE str, rb_encoding *from, rb_encoding *to, int ecflags, VALUE ecopts)
{
    rb_econv_t *ec;
    rb_econv_result_t ret;
    long len, olen;
    VALUE econv_wrapper;
    VALUE newstr;
    const unsigned char *start, *sp;
    unsigned char *dest, *dp;
    size_t converted_output = 0;

    if (!to) return str;
    if (!from) from = rb_enc_get(str);
    if (from == to) return str;
    if ((rb_enc_asciicompat(to) && is_ascii_string(str)) ||
	to == rb_ascii8bit_encoding()) {
	if (STR_ENC_GET(str) != to) {
	    str = rb_str_dup(str);
	    rb_enc_associate(str, to);
	}
	return str;
    }

    len = RSTRING_LEN(str);
    newstr = rb_str_new(0, len);
    OBJ_INFECT(newstr, str);
    olen = len;

    econv_wrapper = rb_obj_alloc(rb_cEncodingConverter);
    RBASIC_CLEAR_CLASS(econv_wrapper);
    ec = rb_econv_open_opts(from->name, to->name, ecflags, ecopts);
    if (!ec) return str;
    DATA_PTR(econv_wrapper) = ec;

    sp = (unsigned char*)RSTRING_PTR(str);
    start = sp;
    while ((dest = (unsigned char*)RSTRING_PTR(newstr)),
	   (dp = dest + converted_output),
	   (ret = rb_econv_convert(ec, &sp, start + len, &dp, dest + olen, 0)),
	   ret == econv_destination_buffer_full) {
	/* destination buffer short */
	size_t converted_input = sp - start;
	size_t rest = len - converted_input;
	converted_output = dp - dest;
	rb_str_set_len(newstr, converted_output);
	if (converted_input && converted_output &&
	    rest < (LONG_MAX / converted_output)) {
	    rest = (rest * converted_output) / converted_input;
	}
	else {
	    rest = olen;
	}
	olen += rest < 2 ? 2 : rest;
	rb_str_resize(newstr, olen);
    }
    DATA_PTR(econv_wrapper) = 0;
    rb_econv_close(ec);
    rb_gc_force_recycle(econv_wrapper);
    switch (ret) {
      case econv_finished:
	len = dp - (unsigned char*)RSTRING_PTR(newstr);
	rb_str_set_len(newstr, len);
	rb_enc_associate(newstr, to);
	return newstr;

      default:
	/* some error, return original */
	return str;
    }
}

VALUE
rb_str_conv_enc(VALUE str, rb_encoding *from, rb_encoding *to)
{
    return rb_str_conv_enc_opts(str, from, to, 0, Qnil);
}

VALUE
rb_external_str_new_with_enc(const char *ptr, long len, rb_encoding *eenc)
{
    VALUE str;

    str = rb_tainted_str_new(ptr, len);
    return rb_external_str_with_enc(str, eenc);
}

VALUE
rb_external_str_with_enc(VALUE str, rb_encoding *eenc)
{
    if (eenc == rb_usascii_encoding() &&
	rb_enc_str_coderange(str) != ENC_CODERANGE_7BIT) {
	rb_enc_associate(str, rb_ascii8bit_encoding());
	return str;
    }
    rb_enc_associate(str, eenc);
    return rb_str_conv_enc(str, eenc, rb_default_internal_encoding());
}

VALUE
rb_external_str_new(const char *ptr, long len)
{
    return rb_external_str_new_with_enc(ptr, len, rb_default_external_encoding());
}

VALUE
rb_external_str_new_cstr(const char *ptr)
{
    return rb_external_str_new_with_enc(ptr, strlen(ptr), rb_default_external_encoding());
}

VALUE
rb_locale_str_new(const char *ptr, long len)
{
    return rb_external_str_new_with_enc(ptr, len, rb_locale_encoding());
}

VALUE
rb_locale_str_new_cstr(const char *ptr)
{
    return rb_external_str_new_with_enc(ptr, strlen(ptr), rb_locale_encoding());
}

VALUE
rb_filesystem_str_new(const char *ptr, long len)
{
    return rb_external_str_new_with_enc(ptr, len, rb_filesystem_encoding());
}

VALUE
rb_filesystem_str_new_cstr(const char *ptr)
{
    return rb_external_str_new_with_enc(ptr, strlen(ptr), rb_filesystem_encoding());
}

VALUE
rb_str_export(VALUE str)
{
    return rb_str_conv_enc(str, STR_ENC_GET(str), rb_default_external_encoding());
}

VALUE
rb_str_export_locale(VALUE str)
{
    return rb_str_conv_enc(str, STR_ENC_GET(str), rb_locale_encoding());
}

VALUE
rb_str_export_to_enc(VALUE str, rb_encoding *enc)
{
    return rb_str_conv_enc(str, STR_ENC_GET(str), enc);
}

static VALUE
str_replace_shared_without_enc(VALUE str2, VALUE str)
{
    const int termlen = TERM_LEN(str);
    char *ptr;
    long len;

    RSTRING_GETMEM(str, ptr, len);
    if (len+termlen <= RSTRING_EMBED_LEN_MAX+1) {
	char *ptr2 = RSTRING(str2)->as.ary;
	STR_SET_EMBED(str2);
	memcpy(ptr2, RSTRING_PTR(str), len);
	STR_SET_EMBED_LEN(str2, len);
	TERM_FILL(ptr2+len, termlen);
    }
    else {
	str = rb_str_new_frozen(str);
	FL_SET(str2, STR_NOEMBED);
	RSTRING_GETMEM(str, ptr, len);
	RSTRING(str2)->as.heap.len = len;
	RSTRING(str2)->as.heap.ptr = ptr;
	STR_SET_SHARED(str2, str);
    }
    return str2;
}

static VALUE
str_replace_shared(VALUE str2, VALUE str)
{
    str_replace_shared_without_enc(str2, str);
    rb_enc_cr_str_exact_copy(str2, str);
    return str2;
}

static VALUE
str_new_shared(VALUE klass, VALUE str)
{
    return str_replace_shared(str_alloc(klass), str);
}

VALUE
rb_str_new_shared(VALUE str)
{
    VALUE str2 = str_new_shared(rb_obj_class(str), str);

    OBJ_INFECT(str2, str);
    return str2;
}

VALUE
rb_str_new_frozen(VALUE orig)
{
    VALUE str;

    if (OBJ_FROZEN(orig)) return orig;

    str = str_new_frozen(rb_obj_class(orig), orig);
    OBJ_INFECT(str, orig);
    return str;
}

static VALUE
str_new_frozen(VALUE klass, VALUE orig)
{
    VALUE str;

    if (STR_EMBED_P(orig)) {
	str = str_new(klass, RSTRING_PTR(orig), RSTRING_LEN(orig));
    }
    else {
	if (FL_TEST(orig, STR_SHARED)) {
	    VALUE shared = RSTRING(orig)->as.heap.aux.shared;
	    long ofs = RSTRING(orig)->as.heap.ptr - RSTRING(shared)->as.heap.ptr;
	    long rest = RSTRING(shared)->as.heap.len - ofs - RSTRING(orig)->as.heap.len;
	    assert(!STR_EMBED_P(shared));
	    assert(OBJ_FROZEN(shared));

	    if ((ofs > 0) || (rest > 0) ||
		(klass != RBASIC(shared)->klass) ||
		((RBASIC(shared)->flags ^ RBASIC(orig)->flags) & FL_TAINT) ||
		ENCODING_GET(shared) != ENCODING_GET(orig)) {
		str = str_new_shared(klass, shared);
		RSTRING(str)->as.heap.ptr += ofs;
		RSTRING(str)->as.heap.len -= ofs + rest;
	    }
	    else {
		return shared;
	    }
	}
	else {
	    str = str_alloc(klass);
	    STR_SET_NOEMBED(str);
	    RSTRING(str)->as.heap.len = RSTRING_LEN(orig);
	    RSTRING(str)->as.heap.ptr = RSTRING_PTR(orig);
	    RSTRING(str)->as.heap.aux.capa = RSTRING(orig)->as.heap.aux.capa;
	    RBASIC(str)->flags |= RBASIC(orig)->flags & STR_NOFREE;
	    RBASIC(orig)->flags &= ~STR_NOFREE;
	    STR_SET_SHARED(orig, str);
	}
    }

    rb_enc_cr_str_exact_copy(str, orig);
    OBJ_FREEZE(str);
    return str;
}

VALUE
rb_str_new_with_class(VALUE obj, const char *ptr, long len)
{
    return str_new(rb_obj_class(obj), ptr, len);
}

static VALUE
str_new_empty(VALUE str)
{
    VALUE v = rb_str_new_with_class(str, 0, 0);
    rb_enc_copy(v, str);
    OBJ_INFECT(v, str);
    return v;
}

#define STR_BUF_MIN_SIZE 128

VALUE
rb_str_buf_new(long capa)
{
    VALUE str = str_alloc(rb_cString);

    if (capa < STR_BUF_MIN_SIZE) {
	capa = STR_BUF_MIN_SIZE;
    }
    FL_SET(str, STR_NOEMBED);
    RSTRING(str)->as.heap.aux.capa = capa;
    RSTRING(str)->as.heap.ptr = ALLOC_N(char, capa+1);
    RSTRING(str)->as.heap.ptr[0] = '\0';

    return str;
}

VALUE
rb_str_buf_new_cstr(const char *ptr)
{
    VALUE str;
    long len = strlen(ptr);

    str = rb_str_buf_new(len);
    rb_str_buf_cat(str, ptr, len);

    return str;
}

VALUE
rb_str_tmp_new(long len)
{
    return str_new(0, 0, len);
}

void
rb_str_free(VALUE str)
{
    if (FL_TEST(str, RSTRING_FSTR)) {
	st_data_t fstr = (st_data_t)str;
	st_delete(rb_vm_fstring_table(), &fstr, NULL);
    }

    if (!STR_EMBED_P(str) && !FL_TEST(str, STR_SHARED|STR_NOFREE)) {
	ruby_sized_xfree(STR_HEAP_PTR(str), STR_HEAP_SIZE(str));
    }
}

RUBY_FUNC_EXPORTED size_t
rb_str_memsize(VALUE str)
{
    if (FL_TEST(str, STR_NOEMBED|STR_SHARED|STR_NOFREE) == STR_NOEMBED) {
	return STR_HEAP_SIZE(str);
    }
    else {
	return 0;
    }
}

VALUE
rb_str_to_str(VALUE str)
{
    return rb_convert_type(str, T_STRING, "String", "to_str");
}

static inline void str_discard(VALUE str);
static void str_shared_replace(VALUE str, VALUE str2);

void
rb_str_shared_replace(VALUE str, VALUE str2)
{
    if (str != str2) str_shared_replace(str, str2);
}

static void
str_shared_replace(VALUE str, VALUE str2)
{
    rb_encoding *enc;
    int cr;

    ASSUME(str2 != str);
    enc = STR_ENC_GET(str2);
    cr = ENC_CODERANGE(str2);
    str_discard(str);
    OBJ_INFECT(str, str2);

    if (RSTRING_LEN(str2) <= RSTRING_EMBED_LEN_MAX) {
	STR_SET_EMBED(str);
	memcpy(RSTRING_PTR(str), RSTRING_PTR(str2), RSTRING_LEN(str2)+1);
	STR_SET_EMBED_LEN(str, RSTRING_LEN(str2));
        rb_enc_associate(str, enc);
        ENC_CODERANGE_SET(str, cr);
    }
    else {
	STR_SET_NOEMBED(str);
	FL_UNSET(str, STR_SHARED);
	RSTRING(str)->as.heap.ptr = RSTRING_PTR(str2);
	RSTRING(str)->as.heap.len = RSTRING_LEN(str2);

	if (FL_TEST(str2, STR_SHARED)) {
	    VALUE shared = RSTRING(str2)->as.heap.aux.shared;
	    STR_SET_SHARED(str, shared);
	}
	else {
	    RSTRING(str)->as.heap.aux.capa = RSTRING(str2)->as.heap.aux.capa;
	}

	/* abandon str2 */
	STR_SET_EMBED(str2);
	RSTRING_PTR(str2)[0] = 0;
	STR_SET_EMBED_LEN(str2, 0);
	rb_enc_associate(str, enc);
	ENC_CODERANGE_SET(str, cr);
    }
}

static ID id_to_s;

VALUE
rb_obj_as_string(VALUE obj)
{
    VALUE str;

    if (RB_TYPE_P(obj, T_STRING)) {
	return obj;
    }
    str = rb_funcall(obj, id_to_s, 0);
    if (!RB_TYPE_P(str, T_STRING))
	return rb_any_to_s(obj);
    OBJ_INFECT(str, obj);
    return str;
}

static VALUE
str_replace(VALUE str, VALUE str2)
{
    long len;

    len = RSTRING_LEN(str2);
    if (STR_SHARED_P(str2)) {
	VALUE shared = RSTRING(str2)->as.heap.aux.shared;
	assert(OBJ_FROZEN(shared));
	STR_SET_NOEMBED(str);
	RSTRING(str)->as.heap.len = len;
	RSTRING(str)->as.heap.ptr = RSTRING_PTR(str2);
	STR_SET_SHARED(str, shared);
	rb_enc_cr_str_exact_copy(str, str2);
    }
    else {
	str_replace_shared(str, str2);
    }

    OBJ_INFECT(str, str2);
    return str;
}

static VALUE
str_duplicate(VALUE klass, VALUE str)
{
    VALUE dup = str_alloc(klass);
    str_replace(dup, str);
    return dup;
}

VALUE
rb_str_dup(VALUE str)
{
    return str_duplicate(rb_obj_class(str), str);
}

VALUE
rb_str_resurrect(VALUE str)
{
    VALUE dup;
    VALUE flags = FL_TEST_RAW(str,
			      RSTRING_NOEMBED |
			      RSTRING_EMBED_LEN_MASK |
			      ENC_CODERANGE_MASK |
			      ENCODING_MASK |
			      FL_TAINT | FL_FREEZE);

    if (RUBY_DTRACE_STRING_CREATE_ENABLED()) {
	RUBY_DTRACE_STRING_CREATE(RSTRING_LEN(str),
				  rb_sourcefile(), rb_sourceline());
    }
    if (!(flags & STR_NOEMBED) && LIKELY((flags & FL_FREEZE))) {
        dup = newobj_of(rb_cString,
        			  T_STRING | (RGENGC_WB_PROTECTED_STRING ? FL_WB_PROTECTED : 0) |
			  (RBASIC(str)->flags & (RSTRING_NOEMBED | RSTRING_EMBED_LEN_MASK |
						 RUBY_ENC_CODERANGE_MASK | RUBY_ENCODING_MASK)),
			  RANY(str)->as.values.v1, RANY(str)->as.values.v2, RANY(str)->as.values.v3);
        return dup;
    }

        
    dup = str_alloc(rb_cString);
    MEMCPY(RSTRING(dup)->as.ary, RSTRING(str)->as.ary, VALUE, 3);
    if (flags & STR_NOEMBED) {
	if (UNLIKELY(!(flags & FL_FREEZE))) {
	    str = str_new_frozen(rb_cString, str);
	    FL_SET_RAW(str, flags & FL_TAINT);
	}
	RB_OBJ_WRITE(dup, &RSTRING(dup)->as.heap.aux.shared, str);
	flags |= STR_SHARED;
    }
    FL_SET_RAW(dup, flags & ~FL_FREEZE);
    return dup;
}

/*
 *  call-seq:
 *     String.new(str="")   -> new_str
 *
 *  Returns a new string object containing a copy of <i>str</i>.
 */

static VALUE
rb_str_init(int argc, VALUE *argv, VALUE str)
{
    VALUE orig;

    if (argc > 0 && rb_scan_args(argc, argv, "01", &orig) == 1)
	rb_str_replace(str, orig);
    return str;
}

#ifdef NONASCII_MASK
#define is_utf8_lead_byte(c) (((c)&0xC0) != 0x80)

/*
 * UTF-8 leading bytes have either 0xxxxxxx or 11xxxxxx
 * bit representation. (see http://en.wikipedia.org/wiki/UTF-8)
 * Therefore, the following pseudocode can detect UTF-8 leading bytes.
 *
 * if (!(byte & 0x80))
 *   byte |= 0x40;          // turn on bit6
 * return ((byte>>6) & 1);  // bit6 represent whether this byte is leading or not.
 *
 * This function calculates whether a byte is leading or not for all bytes
 * in the argument word by concurrently using the above logic, and then
 * adds up the number of leading bytes in the word.
 */
static inline uintptr_t
count_utf8_lead_bytes_with_word(const uintptr_t *s)
{
    uintptr_t d = *s;

    /* Transform so that bit0 indicates whether we have a UTF-8 leading byte or not. */
    d |= ~(d>>1);
    d >>= 6;
    d &= NONASCII_MASK >> 7;

    /* Gather all bytes. */
    d += (d>>8);
    d += (d>>16);
#if SIZEOF_VOIDP == 8
    d += (d>>32);
#endif
    return (d&0xF);
}
#endif

static inline long
enc_strlen(const char *p, const char *e, rb_encoding *enc, int cr)
{
    long c;
    const char *q;

    if (rb_enc_mbmaxlen(enc) == rb_enc_mbminlen(enc)) {
        return (e - p + rb_enc_mbminlen(enc) - 1) / rb_enc_mbminlen(enc);
    }
#ifdef NONASCII_MASK
    else if (cr == ENC_CODERANGE_VALID && enc == rb_utf8_encoding()) {
	uintptr_t len = 0;
	if ((int)sizeof(uintptr_t) * 2 < e - p) {
	    const uintptr_t *s, *t;
	    const uintptr_t lowbits = sizeof(uintptr_t) - 1;
	    s = (const uintptr_t*)(~lowbits & ((uintptr_t)p + lowbits));
	    t = (const uintptr_t*)(~lowbits & (uintptr_t)e);
	    while (p < (const char *)s) {
		if (is_utf8_lead_byte(*p)) len++;
		p++;
	    }
	    while (s < t) {
		len += count_utf8_lead_bytes_with_word(s);
		s++;
	    }
	    p = (const char *)s;
	}
	while (p < e) {
	    if (is_utf8_lead_byte(*p)) len++;
	    p++;
	}
	return (long)len;
    }
#endif
    else if (rb_enc_asciicompat(enc)) {
        c = 0;
	if (ENC_CODERANGE_CLEAN_P(cr)) {
	    while (p < e) {
		if (ISASCII(*p)) {
		    q = search_nonascii(p, e);
		    if (!q)
			return c + (e - p);
		    c += q - p;
		    p = q;
		}
		p += rb_enc_fast_mbclen(p, e, enc);
		c++;
	    }
	}
	else {
	    while (p < e) {
		if (ISASCII(*p)) {
		    q = search_nonascii(p, e);
		    if (!q)
			return c + (e - p);
		    c += q - p;
		    p = q;
		}
		p += rb_enc_mbclen(p, e, enc);
		c++;
	    }
	}
        return c;
    }

    for (c=0; p<e; c++) {
        p += rb_enc_mbclen(p, e, enc);
    }
    return c;
}

long
rb_enc_strlen(const char *p, const char *e, rb_encoding *enc)
{
    return enc_strlen(p, e, enc, ENC_CODERANGE_UNKNOWN);
}

/* To get strlen with cr
 * Note that given cr is not used.
 */
long
rb_enc_strlen_cr(const char *p, const char *e, rb_encoding *enc, int *cr)
{
    long c;
    const char *q;
    int ret;

    *cr = 0;
    if (rb_enc_mbmaxlen(enc) == rb_enc_mbminlen(enc)) {
	return (e - p + rb_enc_mbminlen(enc) - 1) / rb_enc_mbminlen(enc);
    }
    else if (rb_enc_asciicompat(enc)) {
	c = 0;
	while (p < e) {
	    if (ISASCII(*p)) {
		q = search_nonascii(p, e);
		if (!q) {
		    if (!*cr) *cr = ENC_CODERANGE_7BIT;
		    return c + (e - p);
		}
		c += q - p;
		p = q;
	    }
	    ret = rb_enc_precise_mbclen(p, e, enc);
	    if (MBCLEN_CHARFOUND_P(ret)) {
		*cr |= ENC_CODERANGE_VALID;
		p += MBCLEN_CHARFOUND_LEN(ret);
	    }
	    else {
		*cr = ENC_CODERANGE_BROKEN;
		p++;
	    }
	    c++;
	}
	if (!*cr) *cr = ENC_CODERANGE_7BIT;
	return c;
    }

    for (c=0; p<e; c++) {
	ret = rb_enc_precise_mbclen(p, e, enc);
	if (MBCLEN_CHARFOUND_P(ret)) {
	    *cr |= ENC_CODERANGE_VALID;
	    p += MBCLEN_CHARFOUND_LEN(ret);
	}
	else {
	    *cr = ENC_CODERANGE_BROKEN;
            if (p + rb_enc_mbminlen(enc) <= e)
                p += rb_enc_mbminlen(enc);
            else
                p = e;
	}
    }
    if (!*cr) *cr = ENC_CODERANGE_7BIT;
    return c;
}

/* enc must be str's enc or rb_enc_check(str, str2) */
static long
str_strlen(VALUE str, rb_encoding *enc)
{
    const char *p, *e;
    int cr;

    if (single_byte_optimizable(str)) return RSTRING_LEN(str);
    if (!enc) enc = STR_ENC_GET(str);
    p = RSTRING_PTR(str);
    e = RSTRING_END(str);
    cr = ENC_CODERANGE(str);

    if (cr == ENC_CODERANGE_UNKNOWN) {
	long n = rb_enc_strlen_cr(p, e, enc, &cr);
	if (cr) ENC_CODERANGE_SET(str, cr);
	return n;
    }
    else {
	return enc_strlen(p, e, enc, cr);
    }
}

long
rb_str_strlen(VALUE str)
{
    return str_strlen(str, NULL);
}

/*
 *  call-seq:
 *     str.length   -> integer
 *     str.size     -> integer
 *
 *  Returns the character length of <i>str</i>.
 */

VALUE
rb_str_length(VALUE str)
{
    return LONG2NUM(str_strlen(str, NULL));
}

/*
 *  call-seq:
 *     str.bytesize  -> integer
 *
 *  Returns the length of +str+ in bytes.
 *
 *    "\x80\u3042".bytesize  #=> 4
 *    "hello".bytesize       #=> 5
 */

static VALUE
rb_str_bytesize(VALUE str)
{
    return LONG2NUM(RSTRING_LEN(str));
}

/*
 *  call-seq:
 *     str.empty?   -> true or false
 *
 *  Returns <code>true</code> if <i>str</i> has a length of zero.
 *
 *     "hello".empty?   #=> false
 *     " ".empty?       #=> false
 *     "".empty?        #=> true
 */

static VALUE
rb_str_empty(VALUE str)
{
    if (RSTRING_LEN(str) == 0)
	return Qtrue;
    return Qfalse;
}

/*
 *  call-seq:
 *     str + other_str   -> new_str
 *
 *  Concatenation---Returns a new <code>String</code> containing
 *  <i>other_str</i> concatenated to <i>str</i>.
 *
 *     "Hello from " + self.to_s   #=> "Hello from main"
 */

VALUE
rb_str_plus(VALUE str1, VALUE str2)
{
    VALUE str3;
    rb_encoding *enc;
    char *ptr1, *ptr2, *ptr3;
    long len1, len2;

    StringValue(str2);
    enc = rb_enc_check(str1, str2);
    RSTRING_GETMEM(str1, ptr1, len1);
    RSTRING_GETMEM(str2, ptr2, len2);
    str3 = rb_str_new(0, len1+len2);
    ptr3 = RSTRING_PTR(str3);
    memcpy(ptr3, ptr1, len1);
    memcpy(ptr3+len1, ptr2, len2);
    TERM_FILL(&ptr3[len1+len2], rb_enc_mbminlen(enc));

    FL_SET_RAW(str3, OBJ_TAINTED_RAW(str1) | OBJ_TAINTED_RAW(str2));
    ENCODING_CODERANGE_SET(str3, rb_enc_to_index(enc),
			   ENC_CODERANGE_AND(ENC_CODERANGE(str1), ENC_CODERANGE(str2)));
    RB_GC_GUARD(str1);
    RB_GC_GUARD(str2);
    return str3;
}

/*
 *  call-seq:
 *     str * integer   -> new_str
 *
 *  Copy --- Returns a new String containing +integer+ copies of the receiver.
 *  +integer+ must be greater than or equal to 0.
 *
 *     "Ho! " * 3   #=> "Ho! Ho! Ho! "
 *     "Ho! " * 0   #=> ""
 */

VALUE
rb_str_times(VALUE str, VALUE times)
{
    VALUE str2;
    long n, len;
    char *ptr2;
    int termlen;

    len = NUM2LONG(times);
    if (len < 0) {
	rb_raise(rb_eArgError, "negative argument");
    }
    if (len && LONG_MAX/len <  RSTRING_LEN(str)) {
	rb_raise(rb_eArgError, "argument too big");
    }

    len *= RSTRING_LEN(str);
    termlen = TERM_LEN(str);
    str2 = rb_str_new_with_class(str, 0, (len + termlen - 1));
    ptr2 = RSTRING_PTR(str2);
    if (len) {
        n = RSTRING_LEN(str);
        memcpy(ptr2, RSTRING_PTR(str), n);
        while (n <= len/2) {
            memcpy(ptr2 + n, ptr2, n);
            n *= 2;
        }
        memcpy(ptr2 + n, ptr2, len-n);
    }
    STR_SET_LEN(str2, len);
    TERM_FILL(&ptr2[len], termlen);
    OBJ_INFECT(str2, str);
    rb_enc_cr_str_copy_for_substr(str2, str);

    return str2;
}

/*
 *  call-seq:
 *     str % arg   -> new_str
 *
 *  Format---Uses <i>str</i> as a format specification, and returns the result
 *  of applying it to <i>arg</i>. If the format specification contains more than
 *  one substitution, then <i>arg</i> must be an <code>Array</code> or <code>Hash</code>
 *  containing the values to be substituted. See <code>Kernel::sprintf</code> for
 *  details of the format string.
 *
 *     "%05d" % 123                              #=> "00123"
 *     "%-5s: %08x" % [ "ID", self.object_id ]   #=> "ID   : 200e14d6"
 *     "foo = %{foo}" % { :foo => 'bar' }        #=> "foo = bar"
 */

static VALUE
rb_str_format_m(VALUE str, VALUE arg)
{
    VALUE tmp = rb_check_array_type(arg);

    if (!NIL_P(tmp)) {
	VALUE rv = rb_str_format(RARRAY_LENINT(tmp), RARRAY_CONST_PTR(tmp), str);
	RB_GC_GUARD(tmp);
	return rv;
    }
    return rb_str_format(1, &arg, str);
}

static inline void
str_modifiable(VALUE str)
{
    if (FL_TEST(str, STR_TMPLOCK)) {
	rb_raise(rb_eRuntimeError, "can't modify string; temporarily locked");
    }
    rb_check_frozen(str);
}

static inline int
str_independent(VALUE str)
{
    str_modifiable(str);
    if (STR_EMBED_P(str) || !FL_TEST(str, STR_SHARED|STR_NOFREE)) {
	return 1;
    }
    else {
	return 0;
    }
}

static void
str_make_independent_expand(VALUE str, long expand)
{
    char *ptr;
    const char *oldptr;
    long len = RSTRING_LEN(str);
    const int termlen = TERM_LEN(str);
    long capa = len + expand;

    if (len > capa) len = capa;

    if (capa + termlen - 1 <= RSTRING_EMBED_LEN_MAX && !STR_EMBED_P(str)) {
	ptr = RSTRING(str)->as.heap.ptr;
	STR_SET_EMBED(str);
	memcpy(RSTRING(str)->as.ary, ptr, len);
	TERM_FILL(RSTRING(str)->as.ary + len, termlen);
	STR_SET_EMBED_LEN(str, len);
	return;
    }

    ptr = ALLOC_N(char, capa + termlen);
    oldptr = RSTRING_PTR(str);
    if (oldptr) {
	memcpy(ptr, oldptr, len);
    }
    STR_SET_NOEMBED(str);
    FL_UNSET(str, STR_SHARED|STR_NOFREE);
    TERM_FILL(ptr + len, termlen);
    RSTRING(str)->as.heap.ptr = ptr;
    RSTRING(str)->as.heap.len = len;
    RSTRING(str)->as.heap.aux.capa = capa;
}

#define str_make_independent(str) str_make_independent_expand((str), 0L)

void
rb_str_modify(VALUE str)
{
    if (!str_independent(str))
	str_make_independent(str);
    ENC_CODERANGE_CLEAR(str);
}

void
rb_str_modify_expand(VALUE str, long expand)
{
    if (expand < 0) {
	rb_raise(rb_eArgError, "negative expanding string size");
    }
    if (!str_independent(str)) {
	str_make_independent_expand(str, expand);
    }
    else if (expand > 0) {
	long len = RSTRING_LEN(str);
	long capa = len + expand;
	int termlen = TERM_LEN(str);
	if (!STR_EMBED_P(str)) {
	    REALLOC_N(RSTRING(str)->as.heap.ptr, char, capa + termlen);
	    RSTRING(str)->as.heap.aux.capa = capa;
	}
	else if (capa + termlen > RSTRING_EMBED_LEN_MAX + 1) {
	    str_make_independent_expand(str, expand);
	}
    }
    ENC_CODERANGE_CLEAR(str);
}

/* As rb_str_modify(), but don't clear coderange */
static void
str_modify_keep_cr(VALUE str)
{
    if (!str_independent(str))
	str_make_independent(str);
    if (ENC_CODERANGE(str) == ENC_CODERANGE_BROKEN)
	/* Force re-scan later */
	ENC_CODERANGE_CLEAR(str);
}

static inline void
str_discard(VALUE str)
{
    str_modifiable(str);
    if (!STR_EMBED_P(str) && !FL_TEST(str, STR_SHARED|STR_NOFREE)) {
	ruby_sized_xfree(STR_HEAP_PTR(str), STR_HEAP_SIZE(str));
	RSTRING(str)->as.heap.ptr = 0;
	RSTRING(str)->as.heap.len = 0;
    }
}

void
rb_must_asciicompat(VALUE str)
{
    rb_encoding *enc = rb_enc_get(str);
    if (!rb_enc_asciicompat(enc)) {
	rb_raise(rb_eEncCompatError, "ASCII incompatible encoding: %s", rb_enc_name(enc));
    }
}

VALUE
rb_string_value(volatile VALUE *ptr)
{
    VALUE s = *ptr;
    if (!RB_TYPE_P(s, T_STRING)) {
	s = rb_str_to_str(s);
	*ptr = s;
    }
    return s;
}

char *
rb_string_value_ptr(volatile VALUE *ptr)
{
    VALUE str = rb_string_value(ptr);
    return RSTRING_PTR(str);
}

static int
zero_filled(const char *s, int n)
{
    for (; n > 0; --n) {
	if (*s++) return 0;
    }
    return 1;
}

static const char *
str_null_char(const char *s, long len, const int minlen, rb_encoding *enc)
{
    const char *e = s + len;

    for (; s + minlen <= e; s += rb_enc_mbclen(s, e, enc)) {
	if (zero_filled(s, minlen)) return s;
    }
    return 0;
}

static char *
str_fill_term(VALUE str, char *s, long len, int oldtermlen, int termlen)
{
    long capa = rb_str_capacity(str) + 1;

    if (capa < len + termlen) {
	rb_str_modify_expand(str, termlen);
    }
    else if (!str_independent(str)) {
	if (zero_filled(s + len, termlen)) return s;
	str_make_independent(str);
    }
    s = RSTRING_PTR(str);
    TERM_FILL(s + len, termlen);
    return s;
}

char *
rb_string_value_cstr(volatile VALUE *ptr)
{
    VALUE str = rb_string_value(ptr);
    char *s = RSTRING_PTR(str);
    long len = RSTRING_LEN(str);
    rb_encoding *enc = rb_enc_get(str);
    const int minlen = rb_enc_mbminlen(enc);

    if (minlen > 1) {
	if (str_null_char(s, len, minlen, enc)) {
	    rb_raise(rb_eArgError, "string contains null char");
	}
	return str_fill_term(str, s, len, minlen, minlen);
    }
    if (!s || memchr(s, 0, len)) {
	rb_raise(rb_eArgError, "string contains null byte");
    }
    if (s[len]) {
	rb_str_modify(str);
	s = RSTRING_PTR(str);
	s[RSTRING_LEN(str)] = 0;
    }
    return s;
}

void
rb_str_fill_terminator(VALUE str, const int newminlen)
{
    char *s = RSTRING_PTR(str);
    long len = RSTRING_LEN(str);
    rb_encoding *enc = rb_enc_get(str);
    str_fill_term(str, s, len, rb_enc_mbminlen(enc), newminlen);
}

VALUE
rb_check_string_type(VALUE str)
{
    str = rb_check_convert_type(str, T_STRING, "String", "to_str");
    return str;
}

/*
 *  call-seq:
 *     String.try_convert(obj) -> string or nil
 *
 *  Try to convert <i>obj</i> into a String, using to_str method.
 *  Returns converted string or nil if <i>obj</i> cannot be converted
 *  for any reason.
 *
 *     String.try_convert("str")     #=> "str"
 *     String.try_convert(/re/)      #=> nil
 */
static VALUE
rb_str_s_try_convert(VALUE dummy, VALUE str)
{
    return rb_check_string_type(str);
}

static char*
str_nth_len(const char *p, const char *e, long *nthp, rb_encoding *enc)
{
    long nth = *nthp;
    if (rb_enc_mbmaxlen(enc) == 1) {
        p += nth;
    }
    else if (rb_enc_mbmaxlen(enc) == rb_enc_mbminlen(enc)) {
        p += nth * rb_enc_mbmaxlen(enc);
    }
    else if (rb_enc_asciicompat(enc)) {
        const char *p2, *e2;
        int n;

        while (p < e && 0 < nth) {
            e2 = p + nth;
            if (e < e2) {
                *nthp = nth;
                return (char *)e;
            }
            if (ISASCII(*p)) {
                p2 = search_nonascii(p, e2);
                if (!p2) {
		    nth -= e2 - p;
		    *nthp = nth;
                    return (char *)e2;
                }
                nth -= p2 - p;
                p = p2;
            }
            n = rb_enc_mbclen(p, e, enc);
            p += n;
            nth--;
        }
        *nthp = nth;
        if (nth != 0) {
            return (char *)e;
        }
        return (char *)p;
    }
    else {
        while (p < e && nth--) {
            p += rb_enc_mbclen(p, e, enc);
        }
    }
    if (p > e) p = e;
    *nthp = nth;
    return (char*)p;
}

char*
rb_enc_nth(const char *p, const char *e, long nth, rb_encoding *enc)
{
    return str_nth_len(p, e, &nth, enc);
}

static char*
str_nth(const char *p, const char *e, long nth, rb_encoding *enc, int singlebyte)
{
    if (singlebyte)
	p += nth;
    else {
	p = str_nth_len(p, e, &nth, enc);
    }
    if (!p) return 0;
    if (p > e) p = e;
    return (char *)p;
}

/* char offset to byte offset */
static long
str_offset(const char *p, const char *e, long nth, rb_encoding *enc, int singlebyte)
{
    const char *pp = str_nth(p, e, nth, enc, singlebyte);
    if (!pp) return e - p;
    return pp - p;
}

long
rb_str_offset(VALUE str, long pos)
{
    return str_offset(RSTRING_PTR(str), RSTRING_END(str), pos,
		      STR_ENC_GET(str), single_byte_optimizable(str));
}

#ifdef NONASCII_MASK
static char *
str_utf8_nth(const char *p, const char *e, long *nthp)
{
    long nth = *nthp;
    if ((int)SIZEOF_VOIDP * 2 < e - p && (int)SIZEOF_VOIDP * 2 < nth) {
	const uintptr_t *s, *t;
	const uintptr_t lowbits = SIZEOF_VOIDP - 1;
	s = (const uintptr_t*)(~lowbits & ((uintptr_t)p + lowbits));
	t = (const uintptr_t*)(~lowbits & (uintptr_t)e);
	while (p < (const char *)s) {
	    if (is_utf8_lead_byte(*p)) nth--;
	    p++;
	}
	do {
	    nth -= count_utf8_lead_bytes_with_word(s);
	    s++;
	} while (s < t && (int)SIZEOF_VOIDP <= nth);
	p = (char *)s;
    }
    while (p < e) {
	if (is_utf8_lead_byte(*p)) {
	    if (nth == 0) break;
	    nth--;
	}
	p++;
    }
    *nthp = nth;
    return (char *)p;
}

static long
str_utf8_offset(const char *p, const char *e, long nth)
{
    const char *pp = str_utf8_nth(p, e, &nth);
    return pp - p;
}
#endif

/* byte offset to char offset */
long
rb_str_sublen(VALUE str, long pos)
{
    if (single_byte_optimizable(str) || pos < 0)
        return pos;
    else {
	char *p = RSTRING_PTR(str);
        return enc_strlen(p, p + pos, STR_ENC_GET(str), ENC_CODERANGE(str));
    }
}

VALUE
rb_str_subseq(VALUE str, long beg, long len)
{
    VALUE str2;

    if (RSTRING_EMBED_LEN_MAX < len && SHARABLE_SUBSTRING_P(beg, len, RSTRING_LEN(str))) {
	long olen;
	str2 = rb_str_new_shared(rb_str_new_frozen(str));
	RSTRING(str2)->as.heap.ptr += beg;
	olen = RSTRING(str2)->as.heap.len;
	if (olen > len) RSTRING(str2)->as.heap.len = len;
    }
    else {
        str2 = rb_str_new_with_class(str, RSTRING_PTR(str)+beg, len);
	RB_GC_GUARD(str);
    }

    rb_enc_cr_str_copy_for_substr(str2, str);
    OBJ_INFECT(str2, str);

    return str2;
}

char *
rb_str_subpos(VALUE str, long beg, long *lenp)
{
    long len = *lenp;
    long slen = -1L;
    long blen = RSTRING_LEN(str);
    rb_encoding *enc = STR_ENC_GET(str);
    char *p, *s = RSTRING_PTR(str), *e = s + blen;

    if (len < 0) return 0;
    if (!blen) {
	len = 0;
    }
    if (single_byte_optimizable(str)) {
	if (beg > blen) return 0;
	if (beg < 0) {
	    beg += blen;
	    if (beg < 0) return 0;
	}
	if (beg + len > blen)
	    len = blen - beg;
	if (len < 0) return 0;
	p = s + beg;
	goto end;
    }
    if (beg < 0) {
	if (len > -beg) len = -beg;
	if (-beg * rb_enc_mbmaxlen(enc) < RSTRING_LEN(str) / 8) {
	    beg = -beg;
	    while (beg-- > len && (e = rb_enc_prev_char(s, e, e, enc)) != 0);
	    p = e;
	    if (!p) return 0;
	    while (len-- > 0 && (p = rb_enc_prev_char(s, p, e, enc)) != 0);
	    if (!p) return 0;
	    len = e - p;
	    goto end;
	}
	else {
	    slen = str_strlen(str, enc);
	    beg += slen;
	    if (beg < 0) return 0;
	    p = s + beg;
	    if (len == 0) goto end;
	}
    }
    else if (beg > 0 && beg > RSTRING_LEN(str)) {
	return 0;
    }
    if (len == 0) {
	if (beg > str_strlen(str, enc)) return 0; /* str's enc */
	p = s + beg;
    }
#ifdef NONASCII_MASK
    else if (ENC_CODERANGE(str) == ENC_CODERANGE_VALID &&
        enc == rb_utf8_encoding()) {
        p = str_utf8_nth(s, e, &beg);
        if (beg > 0) return 0;
        len = str_utf8_offset(p, e, len);
    }
#endif
    else if (rb_enc_mbmaxlen(enc) == rb_enc_mbminlen(enc)) {
	int char_sz = rb_enc_mbmaxlen(enc);

	p = s + beg * char_sz;
	if (p > e) {
	    return 0;
	}
        else if (len * char_sz > e - p)
            len = e - p;
        else
	    len *= char_sz;
    }
    else if ((p = str_nth_len(s, e, &beg, enc)) == e) {
	if (beg > 0) return 0;
	len = 0;
    }
    else {
	len = str_offset(p, e, len, enc, 0);
    }
  end:
    *lenp = len;
    RB_GC_GUARD(str);
    return p;
}

VALUE
rb_str_substr(VALUE str, long beg, long len)
{
    VALUE str2;
    char *p = rb_str_subpos(str, beg, &len);

    if (!p) return Qnil;
    if (len > RSTRING_EMBED_LEN_MAX && SHARABLE_SUBSTRING_P(p, len, RSTRING_END(str))) {
	long ofs = p - RSTRING_PTR(str);
	str2 = rb_str_new_frozen(str);
	str2 = str_new_shared(rb_obj_class(str2), str2);
	RSTRING(str2)->as.heap.ptr += ofs;
	RSTRING(str2)->as.heap.len = len;
    }
    else {
	str2 = rb_str_new_with_class(str, p, len);
	OBJ_INFECT(str2, str);
	RB_GC_GUARD(str);
    }
    rb_enc_cr_str_copy_for_substr(str2, str);

    return str2;
}

VALUE
rb_str_freeze(VALUE str)
{
    if (OBJ_FROZEN(str)) return str;
    rb_str_resize(str, RSTRING_LEN(str));
    return rb_obj_freeze(str);
}

RUBY_ALIAS_FUNCTION(rb_str_dup_frozen(VALUE str), rb_str_new_frozen, (str))
#define rb_str_dup_frozen rb_str_new_frozen

VALUE
rb_str_locktmp(VALUE str)
{
    if (FL_TEST(str, STR_TMPLOCK)) {
	rb_raise(rb_eRuntimeError, "temporal locking already locked string");
    }
    FL_SET(str, STR_TMPLOCK);
    return str;
}

VALUE
rb_str_unlocktmp(VALUE str)
{
    if (!FL_TEST(str, STR_TMPLOCK)) {
	rb_raise(rb_eRuntimeError, "temporal unlocking already unlocked string");
    }
    FL_UNSET(str, STR_TMPLOCK);
    return str;
}

RUBY_FUNC_EXPORTED VALUE
rb_str_locktmp_ensure(VALUE str, VALUE (*func)(VALUE), VALUE arg)
{
    rb_str_locktmp(str);
    return rb_ensure(func, arg, rb_str_unlocktmp, str);
}

void
rb_str_set_len(VALUE str, long len)
{
    long capa;
    const int termlen = TERM_LEN(str);

    str_modifiable(str);
    if (STR_SHARED_P(str)) {
	rb_raise(rb_eRuntimeError, "can't set length of shared string");
    }
    if (len + termlen - 1 > (capa = (long)rb_str_capacity(str))) {
	rb_bug("probable buffer overflow: %ld for %ld", len, capa);
    }
    STR_SET_LEN(str, len);
    TERM_FILL(&RSTRING_PTR(str)[len], termlen);
}

VALUE
rb_str_resize(VALUE str, long len)
{
    long slen;
    int independent;

    if (len < 0) {
	rb_raise(rb_eArgError, "negative string size (or size too big)");
    }

    independent = str_independent(str);
    ENC_CODERANGE_CLEAR(str);
    slen = RSTRING_LEN(str);

    {
	long capa;
	const int termlen = TERM_LEN(str);
	if (STR_EMBED_P(str)) {
	    if (len == slen) return str;
	    if (len + termlen <= RSTRING_EMBED_LEN_MAX + 1) {
		STR_SET_EMBED_LEN(str, len);
		TERM_FILL(RSTRING(str)->as.ary + len, termlen);
		return str;
	    }
	    str_make_independent_expand(str, len - slen);
	}
	else if (len + termlen <= RSTRING_EMBED_LEN_MAX + 1) {
	    char *ptr = STR_HEAP_PTR(str);
	    STR_SET_EMBED(str);
	    if (slen > len) slen = len;
	    if (slen > 0) MEMCPY(RSTRING(str)->as.ary, ptr, char, slen);
	    TERM_FILL(RSTRING(str)->as.ary + len, termlen);
	    STR_SET_EMBED_LEN(str, len);
	    if (independent) ruby_xfree(ptr);
	    return str;
	}
	else if (!independent) {
	    if (len == slen) return str;
	    str_make_independent_expand(str, len - slen);
	}
	else if ((capa = RSTRING(str)->as.heap.aux.capa) < len ||
		 (capa - len) > (len < 1024 ? len : 1024)) {
	    REALLOC_N(RSTRING(str)->as.heap.ptr, char, len + termlen);
	    RSTRING(str)->as.heap.aux.capa = len;
	}
	else if (len == slen) return str;
	RSTRING(str)->as.heap.len = len;
	TERM_FILL(RSTRING(str)->as.heap.ptr + len, termlen); /* sentinel */
    }
    return str;
}

static VALUE
str_buf_cat(VALUE str, const char *ptr, long len)
{
    long capa, total, olen, off = -1;
    char *sptr;
    const int termlen = TERM_LEN(str);

    RSTRING_GETMEM(str, sptr, olen);
    if (ptr >= sptr && ptr <= sptr + olen) {
        off = ptr - sptr;
    }
    rb_str_modify(str);
    if (len == 0) return 0;
    if (STR_EMBED_P(str)) {
	capa = RSTRING_EMBED_LEN_MAX;
	sptr = RSTRING(str)->as.ary;
	olen = RSTRING_EMBED_LEN(str);
    }
    else {
	capa = RSTRING(str)->as.heap.aux.capa;
	sptr = RSTRING(str)->as.heap.ptr;
	olen = RSTRING(str)->as.heap.len;
    }
    if (olen >= LONG_MAX - len) {
	rb_raise(rb_eArgError, "string sizes too big");
    }
    total = olen + len;
    if (capa <= total) {
	if (LIKELY(capa > 0)) {
	    while (total > capa) {
		if (capa > LONG_MAX / 2) {
		    capa = (total + 4095) / 4096 * 4096;
		    break;
		}
		capa = 2 * capa;
	    }
	}
	else {
	    capa = total;
	}
	RESIZE_CAPA_TERM(str, capa, termlen);
	sptr = RSTRING_PTR(str);
    }
    if (off != -1) {
        ptr = sptr + off;
    }
    memcpy(sptr + olen, ptr, len);
    STR_SET_LEN(str, total);
    TERM_FILL(sptr + total, termlen); /* sentinel */

    return str;
}

#define str_buf_cat2(str, ptr) str_buf_cat((str), (ptr), strlen(ptr))

VALUE
rb_str_cat(VALUE str, const char *ptr, long len)
{
    if (len == 0) return str;
    if (len < 0) {
	rb_raise(rb_eArgError, "negative string size (or size too big)");
    }
    return str_buf_cat(str, ptr, len);
}

VALUE
rb_str_cat_cstr(VALUE str, const char *ptr)
{
    must_not_null(ptr);
    return rb_str_buf_cat(str, ptr, strlen(ptr));
}

RUBY_ALIAS_FUNCTION(rb_str_buf_cat(VALUE str, const char *ptr, long len), rb_str_cat, (str, ptr, len))
RUBY_ALIAS_FUNCTION(rb_str_buf_cat2(VALUE str, const char *ptr), rb_str_cat_cstr, (str, ptr))
RUBY_ALIAS_FUNCTION(rb_str_cat2(VALUE str, const char *ptr), rb_str_cat_cstr, (str, ptr))

static VALUE
rb_enc_cr_str_buf_cat(VALUE str, const char *ptr, long len,
    int ptr_encindex, int ptr_cr, int *ptr_cr_ret)
{
    int str_encindex = ENCODING_GET(str);
    int res_encindex;
    int str_cr, res_cr;
    rb_encoding *str_enc, *ptr_enc;

    str_cr = RSTRING_LEN(str) ? ENC_CODERANGE(str) : ENC_CODERANGE_7BIT;

    if (str_encindex == ptr_encindex) {
	if (str_cr != ENC_CODERANGE_UNKNOWN && ptr_cr == ENC_CODERANGE_UNKNOWN) {
            ptr_cr = coderange_scan(ptr, len, rb_enc_from_index(ptr_encindex));
        }
    }
    else {
	str_enc = rb_enc_from_index(str_encindex);
	ptr_enc = rb_enc_from_index(ptr_encindex);
        if (!rb_enc_asciicompat(str_enc) || !rb_enc_asciicompat(ptr_enc)) {
            if (len == 0)
                return str;
            if (RSTRING_LEN(str) == 0) {
                rb_str_buf_cat(str, ptr, len);
                ENCODING_CODERANGE_SET(str, ptr_encindex, ptr_cr);
                return str;
            }
            goto incompatible;
        }
	if (ptr_cr == ENC_CODERANGE_UNKNOWN) {
	    ptr_cr = coderange_scan(ptr, len, ptr_enc);
	}
        if (str_cr == ENC_CODERANGE_UNKNOWN) {
            if (ENCODING_IS_ASCII8BIT(str) || ptr_cr != ENC_CODERANGE_7BIT) {
                str_cr = rb_enc_str_coderange(str);
            }
        }
    }
    if (ptr_cr_ret)
        *ptr_cr_ret = ptr_cr;

    if (str_encindex != ptr_encindex &&
        str_cr != ENC_CODERANGE_7BIT &&
        ptr_cr != ENC_CODERANGE_7BIT) {
	str_enc = rb_enc_from_index(str_encindex);
	ptr_enc = rb_enc_from_index(ptr_encindex);
      incompatible:
        rb_raise(rb_eEncCompatError, "incompatible character encodings: %s and %s",
		 rb_enc_name(str_enc), rb_enc_name(ptr_enc));
    }

    if (str_cr == ENC_CODERANGE_UNKNOWN) {
        res_encindex = str_encindex;
        res_cr = ENC_CODERANGE_UNKNOWN;
    }
    else if (str_cr == ENC_CODERANGE_7BIT) {
        if (ptr_cr == ENC_CODERANGE_7BIT) {
            res_encindex = str_encindex;
            res_cr = ENC_CODERANGE_7BIT;
        }
        else {
            res_encindex = ptr_encindex;
            res_cr = ptr_cr;
        }
    }
    else if (str_cr == ENC_CODERANGE_VALID) {
        res_encindex = str_encindex;
	if (ENC_CODERANGE_CLEAN_P(ptr_cr))
	    res_cr = str_cr;
	else
	    res_cr = ptr_cr;
    }
    else { /* str_cr == ENC_CODERANGE_BROKEN */
        res_encindex = str_encindex;
        res_cr = str_cr;
        if (0 < len) res_cr = ENC_CODERANGE_UNKNOWN;
    }

    if (len < 0) {
	rb_raise(rb_eArgError, "negative string size (or size too big)");
    }
    str_buf_cat(str, ptr, len);
    ENCODING_CODERANGE_SET(str, res_encindex, res_cr);
    return str;
}

VALUE
rb_enc_str_buf_cat(VALUE str, const char *ptr, long len, rb_encoding *ptr_enc)
{
    return rb_enc_cr_str_buf_cat(str, ptr, len,
        rb_enc_to_index(ptr_enc), ENC_CODERANGE_UNKNOWN, NULL);
}

VALUE
rb_str_buf_cat_ascii(VALUE str, const char *ptr)
{
    /* ptr must reference NUL terminated ASCII string. */
    int encindex = ENCODING_GET(str);
    rb_encoding *enc = rb_enc_from_index(encindex);
    if (rb_enc_asciicompat(enc)) {
        return rb_enc_cr_str_buf_cat(str, ptr, strlen(ptr),
            encindex, ENC_CODERANGE_7BIT, 0);
    }
    else {
        char *buf = ALLOCA_N(char, rb_enc_mbmaxlen(enc));
        while (*ptr) {
            unsigned int c = (unsigned char)*ptr;
            int len = rb_enc_codelen(c, enc);
            rb_enc_mbcput(c, buf, enc);
            rb_enc_cr_str_buf_cat(str, buf, len,
                encindex, ENC_CODERANGE_VALID, 0);
            ptr++;
        }
        return str;
    }
}

VALUE
rb_str_buf_append(VALUE str, VALUE str2)
{
    int str2_cr;

    str2_cr = ENC_CODERANGE(str2);

    rb_enc_cr_str_buf_cat(str, RSTRING_PTR(str2), RSTRING_LEN(str2),
        ENCODING_GET(str2), str2_cr, &str2_cr);

    OBJ_INFECT(str, str2);
    ENC_CODERANGE_SET(str2, str2_cr);

    return str;
}

VALUE
rb_str_append(VALUE str, VALUE str2)
{
    StringValue(str2);
    return rb_str_buf_append(str, str2);
}

VALUE
rb_str_append_literal(VALUE str, VALUE str2)
{
    int encidx = rb_enc_get_index(str2);
    rb_str_buf_append(str, str2);
    if (encidx != ENCINDEX_US_ASCII) {
	if (rb_enc_get_index(str) == ENCINDEX_US_ASCII)
	    rb_enc_associate_index(str, encidx);
    }
    return str;
}

/*
 *  call-seq:
 *     str << integer       -> str
 *     str.concat(integer)  -> str
 *     str << obj           -> str
 *     str.concat(obj)      -> str
 *
 *  Append---Concatenates the given object to <i>str</i>. If the object is a
 *  <code>Integer</code>, it is considered as a codepoint, and is converted
 *  to a character before concatenation.
 *
 *     a = "hello "
 *     a << "world"   #=> "hello world"
 *     a.concat(33)   #=> "hello world!"
 */

VALUE
rb_str_concat(VALUE str1, VALUE str2)
{
    unsigned int code;
    rb_encoding *enc = STR_ENC_GET(str1);

    if (FIXNUM_P(str2) || RB_TYPE_P(str2, T_BIGNUM)) {
	if (rb_num_to_uint(str2, &code) == 0) {
	}
	else if (FIXNUM_P(str2)) {
	    rb_raise(rb_eRangeError, "%ld out of char range", FIX2LONG(str2));
	}
	else {
	    rb_raise(rb_eRangeError, "bignum out of char range");
	}
    }
    else {
	return rb_str_append(str1, str2);
    }

    if (enc == rb_usascii_encoding()) {
	/* US-ASCII automatically extended to ASCII-8BIT */
	char buf[1];
	buf[0] = (char)code;
	if (code > 0xFF) {
	    rb_raise(rb_eRangeError, "%u out of char range", code);
	}
	rb_str_cat(str1, buf, 1);
	if (code > 127) {
	    rb_enc_associate(str1, rb_ascii8bit_encoding());
	    ENC_CODERANGE_SET(str1, ENC_CODERANGE_VALID);
	}
    }
    else {
	long pos = RSTRING_LEN(str1);
	int cr = ENC_CODERANGE(str1);
	int len;
	char *buf;

	switch (len = rb_enc_codelen(code, enc)) {
	  case ONIGERR_INVALID_CODE_POINT_VALUE:
	    rb_raise(rb_eRangeError, "invalid codepoint 0x%X in %s", code, rb_enc_name(enc));
	    break;
	  case ONIGERR_TOO_BIG_WIDE_CHAR_VALUE:
	  case 0:
	    rb_raise(rb_eRangeError, "%u out of char range", code);
	    break;
	}
	buf = ALLOCA_N(char, len + 1);
	rb_enc_mbcput(code, buf, enc);
	if (rb_enc_precise_mbclen(buf, buf + len + 1, enc) != len) {
	    rb_raise(rb_eRangeError, "invalid codepoint 0x%X in %s", code, rb_enc_name(enc));
	}
	rb_str_resize(str1, pos+len);
	memcpy(RSTRING_PTR(str1) + pos, buf, len);
	if (cr == ENC_CODERANGE_7BIT && code > 127)
	    cr = ENC_CODERANGE_VALID;
	ENC_CODERANGE_SET(str1, cr);
    }
    return str1;
}

/*
 *  call-seq:
 *     str.prepend(other_str)  -> str
 *
 *  Prepend---Prepend the given string to <i>str</i>.
 *
 *     a = "world"
 *     a.prepend("hello ") #=> "hello world"
 *     a                   #=> "hello world"
 */

static VALUE
rb_str_prepend(VALUE str, VALUE str2)
{
    StringValue(str2);
    StringValue(str);
    rb_str_update(str, 0L, 0L, str2);
    return str;
}

st_index_t
rb_str_hash(VALUE str)
{
    int e = ENCODING_GET(str);
    if (e && rb_enc_str_coderange(str) == ENC_CODERANGE_7BIT) {
	e = 0;
    }
    return rb_memhash((const void *)RSTRING_PTR(str), RSTRING_LEN(str)) ^ e;
}

int
rb_str_hash_cmp(VALUE str1, VALUE str2)
{
    long len1, len2;
    const char *ptr1, *ptr2;
    RSTRING_GETMEM(str1, ptr1, len1);
    RSTRING_GETMEM(str2, ptr2, len2);
    return (len1 != len2 ||
	    !rb_str_comparable(str1, str2) ||
	    memcmp(ptr1, ptr2, len1) != 0);
}

/*
 * call-seq:
 *    str.hash   -> fixnum
 *
 * Return a hash based on the string's length, content and encoding.
 *
 * See also Object#hash.
 */

static VALUE
rb_str_hash_m(VALUE str)
{
    st_index_t hval = rb_str_hash(str);
    return INT2FIX(hval);
}

#define lesser(a,b) (((a)>(b))?(b):(a))

int
rb_str_comparable(VALUE str1, VALUE str2)
{
    int idx1, idx2;
    int rc1, rc2;

    if (RSTRING_LEN(str1) == 0) return TRUE;
    if (RSTRING_LEN(str2) == 0) return TRUE;
    idx1 = ENCODING_GET(str1);
    idx2 = ENCODING_GET(str2);
    if (idx1 == idx2) return TRUE;
    rc1 = rb_enc_str_coderange(str1);
    rc2 = rb_enc_str_coderange(str2);
    if (rc1 == ENC_CODERANGE_7BIT) {
	if (rc2 == ENC_CODERANGE_7BIT) return TRUE;
	if (rb_enc_asciicompat(rb_enc_from_index(idx2)))
	    return TRUE;
    }
    if (rc2 == ENC_CODERANGE_7BIT) {
	if (rb_enc_asciicompat(rb_enc_from_index(idx1)))
	    return TRUE;
    }
    return FALSE;
}

int
rb_str_cmp(VALUE str1, VALUE str2)
{
    long len1, len2;
    const char *ptr1, *ptr2;
    int retval;

    if (str1 == str2) return 0;
    RSTRING_GETMEM(str1, ptr1, len1);
    RSTRING_GETMEM(str2, ptr2, len2);
    if (ptr1 == ptr2 || (retval = memcmp(ptr1, ptr2, lesser(len1, len2))) == 0) {
	if (len1 == len2) {
	    if (!rb_str_comparable(str1, str2)) {
		if (ENCODING_GET(str1) > ENCODING_GET(str2))
		    return 1;
		return -1;
	    }
	    return 0;
	}
	if (len1 > len2) return 1;
	return -1;
    }
    if (retval > 0) return 1;
    return -1;
}

/* expect tail call optimization */
static VALUE
str_eql(const VALUE str1, const VALUE str2)
{
    const long len = RSTRING_LEN(str1);
    const char *ptr1, *ptr2;

    if (len != RSTRING_LEN(str2)) return Qfalse;
    if (!rb_str_comparable(str1, str2)) return Qfalse;
    if ((ptr1 = RSTRING_PTR(str1)) == (ptr2 = RSTRING_PTR(str2)))
	return Qtrue;
    if (memcmp(ptr1, ptr2, len) == 0)
	return Qtrue;
    return Qfalse;
}

/*
 *  call-seq:
 *     str == obj    -> true or false
 *     str === obj   -> true or false
 *
 *  === Equality
 *
 *  Returns whether +str+ == +obj+, similar to Object#==.
 *
 *  If +obj+ is not an instance of String but responds to +to_str+, then the
 *  two strings are compared using case equality Object#===.
 *
 *  Otherwise, returns similarly to String#eql?, comparing length and content.
 */

VALUE
rb_str_equal(VALUE str1, VALUE str2)
{
    if (str1 == str2) return Qtrue;
    if (!RB_TYPE_P(str2, T_STRING)) {
	if (!rb_respond_to(str2, rb_intern("to_str"))) {
	    return Qfalse;
	}
	return rb_equal(str2, str1);
    }
    return str_eql(str1, str2);
}

/*
 * call-seq:
 *   str.eql?(other)   -> true or false
 *
 * Two strings are equal if they have the same length and content.
 */

static VALUE
rb_str_eql(VALUE str1, VALUE str2)
{
    if (str1 == str2) return Qtrue;
    if (!RB_TYPE_P(str2, T_STRING)) return Qfalse;
    return str_eql(str1, str2);
}

/*
 *  call-seq:
 *     string <=> other_string   -> -1, 0, +1 or nil
 *
 *
 *  Comparison---Returns -1, 0, +1 or nil depending on whether +string+ is less
 *  than, equal to, or greater than +other_string+.
 *
 *  +nil+ is returned if the two values are incomparable.
 *
 *  If the strings are of different lengths, and the strings are equal when
 *  compared up to the shortest length, then the longer string is considered
 *  greater than the shorter one.
 *
 *  <code><=></code> is the basis for the methods <code><</code>,
 *  <code><=</code>, <code>></code>, <code>>=</code>, and
 *  <code>between?</code>, included from module Comparable. The method
 *  String#== does not use Comparable#==.
 *
 *     "abcdef" <=> "abcde"     #=> 1
 *     "abcdef" <=> "abcdef"    #=> 0
 *     "abcdef" <=> "abcdefg"   #=> -1
 *     "abcdef" <=> "ABCDEF"    #=> 1
 *     "abcdef" <=> 1           #=> nil
 */

static VALUE
rb_str_cmp_m(VALUE str1, VALUE str2)
{
    int result;

    if (!RB_TYPE_P(str2, T_STRING)) {
	VALUE tmp = rb_check_funcall(str2, rb_intern("to_str"), 0, 0);
	if (RB_TYPE_P(tmp, T_STRING)) {
	    result = rb_str_cmp(str1, tmp);
	}
	else {
	    return rb_invcmp(str1, str2);
	}
    }
    else {
	result = rb_str_cmp(str1, str2);
    }
    return INT2FIX(result);
}

/*
 *  call-seq:
 *     str.casecmp(other_str)   -> -1, 0, +1 or nil
 *
 *  Case-insensitive version of <code>String#<=></code>.
 *
 *     "abcdef".casecmp("abcde")     #=> 1
 *     "aBcDeF".casecmp("abcdef")    #=> 0
 *     "abcdef".casecmp("abcdefg")   #=> -1
 *     "abcdef".casecmp("ABCDEF")    #=> 0
 */

static VALUE
rb_str_casecmp(VALUE str1, VALUE str2)
{
    long len;
    rb_encoding *enc;
    char *p1, *p1end, *p2, *p2end;

    StringValue(str2);
    enc = rb_enc_compatible(str1, str2);
    if (!enc) {
	return Qnil;
    }

    p1 = RSTRING_PTR(str1); p1end = RSTRING_END(str1);
    p2 = RSTRING_PTR(str2); p2end = RSTRING_END(str2);
    if (single_byte_optimizable(str1) && single_byte_optimizable(str2)) {
	while (p1 < p1end && p2 < p2end) {
	    if (*p1 != *p2) {
		unsigned int c1 = TOUPPER(*p1 & 0xff);
		unsigned int c2 = TOUPPER(*p2 & 0xff);
                if (c1 != c2)
                    return INT2FIX(c1 < c2 ? -1 : 1);
	    }
	    p1++;
	    p2++;
	}
    }
    else {
	while (p1 < p1end && p2 < p2end) {
            int l1, c1 = rb_enc_ascget(p1, p1end, &l1, enc);
            int l2, c2 = rb_enc_ascget(p2, p2end, &l2, enc);

            if (0 <= c1 && 0 <= c2) {
                c1 = TOUPPER(c1);
                c2 = TOUPPER(c2);
                if (c1 != c2)
                    return INT2FIX(c1 < c2 ? -1 : 1);
            }
            else {
                int r;
                l1 = rb_enc_mbclen(p1, p1end, enc);
                l2 = rb_enc_mbclen(p2, p2end, enc);
                len = l1 < l2 ? l1 : l2;
                r = memcmp(p1, p2, len);
                if (r != 0)
                    return INT2FIX(r < 0 ? -1 : 1);
                if (l1 != l2)
                    return INT2FIX(l1 < l2 ? -1 : 1);
            }
	    p1 += l1;
	    p2 += l2;
	}
    }
    if (RSTRING_LEN(str1) == RSTRING_LEN(str2)) return INT2FIX(0);
    if (RSTRING_LEN(str1) > RSTRING_LEN(str2)) return INT2FIX(1);
    return INT2FIX(-1);
}

#define rb_str_index(str, sub, offset) rb_strseq_index(str, sub, offset, 0)

static long
rb_strseq_index(VALUE str, VALUE sub, long offset, int in_byte)
{
    const char *s, *sptr, *e;
    long pos, len, slen;
    int single_byte = single_byte_optimizable(str);
    rb_encoding *enc;

    enc = rb_enc_check(str, sub);
    if (is_broken_string(sub)) return -1;

    len = (in_byte || single_byte) ? RSTRING_LEN(str) : str_strlen(str, enc); /* rb_enc_check */
    slen = in_byte ? RSTRING_LEN(sub) : str_strlen(sub, enc); /* rb_enc_check */
    if (offset < 0) {
	offset += len;
	if (offset < 0) return -1;
    }
    if (len - offset < slen) return -1;

    s = RSTRING_PTR(str);
    e = RSTRING_END(str);
    if (offset) {
	if (!in_byte) offset = str_offset(s, e, offset, enc, single_byte);
	s += offset;
    }
    if (slen == 0) return offset;
    /* need proceed one character at a time */
    sptr = RSTRING_PTR(sub);
    slen = RSTRING_LEN(sub);
    len = RSTRING_LEN(str) - offset;
    for (;;) {
	const char *t;
	pos = rb_memsearch(sptr, slen, s, len, enc);
	if (pos < 0) return pos;
	t = rb_enc_right_char_head(s, s+pos, e, enc);
	if (t == s + pos) break;
	len -= t - s;
	if (len <= 0) return -1;
	offset += t - s;
	s = t;
    }
    return pos + offset;
}


/*
 *  call-seq:
 *     str.index(substring [, offset])   -> fixnum or nil
 *     str.index(regexp [, offset])      -> fixnum or nil
 *
 *  Returns the index of the first occurrence of the given <i>substring</i> or
 *  pattern (<i>regexp</i>) in <i>str</i>. Returns <code>nil</code> if not
 *  found. If the second parameter is present, it specifies the position in the
 *  string to begin the search.
 *
 *     "hello".index('e')             #=> 1
 *     "hello".index('lo')            #=> 3
 *     "hello".index('a')             #=> nil
 *     "hello".index(?e)              #=> 1
 *     "hello".index(/[aeiou]/, -3)   #=> 4
 */

static VALUE
rb_str_index_m(int argc, VALUE *argv, VALUE str)
{
    VALUE sub;
    VALUE initpos;
    long pos;

    if (rb_scan_args(argc, argv, "11", &sub, &initpos) == 2) {
	pos = NUM2LONG(initpos);
    }
    else {
	pos = 0;
    }
    if (pos < 0) {
	pos += str_strlen(str, NULL);
	if (pos < 0) {
	    if (RB_TYPE_P(sub, T_REGEXP)) {
		rb_backref_set(Qnil);
	    }
	    return Qnil;
	}
    }

    if (SPECIAL_CONST_P(sub)) goto generic;
    switch (BUILTIN_TYPE(sub)) {
      case T_REGEXP:
	if (pos > str_strlen(str, NULL))
	    return Qnil;
	pos = str_offset(RSTRING_PTR(str), RSTRING_END(str), pos,
			 rb_enc_check(str, sub), single_byte_optimizable(str));

	pos = rb_reg_search(sub, str, pos, 0);
	pos = rb_str_sublen(str, pos);
	break;

      generic:
      default: {
	VALUE tmp;

	tmp = rb_check_string_type(sub);
	if (NIL_P(tmp)) {
	    rb_raise(rb_eTypeError, "type mismatch: %s given",
		     rb_obj_classname(sub));
	}
	sub = tmp;
      }
	/* fall through */
      case T_STRING:
	pos = rb_str_index(str, sub, pos);
	pos = rb_str_sublen(str, pos);
	break;
    }

    if (pos == -1) return Qnil;
    return LONG2NUM(pos);
}

#ifdef HAVE_MEMRCHR
static long
str_rindex(VALUE str, VALUE sub, const char *s, long pos, rb_encoding *enc)
{
    char *hit, *adjusted;
    int c;
    long slen, searchlen;
    char *sbeg, *e, *t;

    slen = RSTRING_LEN(sub);
    if (slen == 0) return pos;
    sbeg = RSTRING_PTR(str);
    e = RSTRING_END(str);
    t = RSTRING_PTR(sub);
    c = *t & 0xff;
    searchlen = s - sbeg + 1;

    do {
	hit = memrchr(sbeg, c, searchlen);
	if (!hit) break;
	adjusted = rb_enc_left_char_head(sbeg, hit, e, enc);
	if (hit != adjusted) {
	    searchlen = adjusted - sbeg;
	    continue;
	}
	if (memcmp(hit, t, slen) == 0)
	    return rb_str_sublen(str, hit - sbeg);
	searchlen = adjusted - sbeg;
    } while (searchlen > 0);

    return -1;
}
#else
static long
str_rindex(VALUE str, VALUE sub, const char *s, long pos, rb_encoding *enc)
{
    long slen;
    char *sbeg, *e, *t;

    sbeg = RSTRING_PTR(str);
    e = RSTRING_END(str);
    t = RSTRING_PTR(sub);
    slen = RSTRING_LEN(sub);

    while (s) {
	if (memcmp(s, t, slen) == 0) {
	    return pos;
	}
	if (pos == 0) break;
	pos--;
	s = rb_enc_prev_char(sbeg, s, e, enc);
    }

    return -1;
}
#endif

static long
rb_str_rindex(VALUE str, VALUE sub, long pos)
{
    long len, slen;
    char *sbeg, *s;
    rb_encoding *enc;
    int singlebyte;

    enc = rb_enc_check(str, sub);
    if (is_broken_string(sub)) return -1;
    singlebyte = single_byte_optimizable(str);
    len = singlebyte ? RSTRING_LEN(str) : str_strlen(str, enc); /* rb_enc_check */
    slen = str_strlen(sub, enc); /* rb_enc_check */

    /* substring longer than string */
    if (len < slen) return -1;
    if (len - pos < slen) pos = len - slen;
    if (len == 0) return pos;

    sbeg = RSTRING_PTR(str);

    if (pos == 0) {
	if (memcmp(sbeg, RSTRING_PTR(sub), RSTRING_LEN(sub)) == 0)
	    return 0;
	else
	    return -1;
    }

    s = str_nth(sbeg, RSTRING_END(str), pos, enc, singlebyte);
    return str_rindex(str, sub, s, pos, enc);
}


/*
 *  call-seq:
 *     str.rindex(substring [, fixnum])   -> fixnum or nil
 *     str.rindex(regexp [, fixnum])   -> fixnum or nil
 *
 *  Returns the index of the last occurrence of the given <i>substring</i> or
 *  pattern (<i>regexp</i>) in <i>str</i>. Returns <code>nil</code> if not
 *  found. If the second parameter is present, it specifies the position in the
 *  string to end the search---characters beyond this point will not be
 *  considered.
 *
 *     "hello".rindex('e')             #=> 1
 *     "hello".rindex('l')             #=> 3
 *     "hello".rindex('a')             #=> nil
 *     "hello".rindex(?e)              #=> 1
 *     "hello".rindex(/[aeiou]/, -2)   #=> 1
 */

static VALUE
rb_str_rindex_m(int argc, VALUE *argv, VALUE str)
{
    VALUE sub;
    VALUE vpos;
    rb_encoding *enc = STR_ENC_GET(str);
    long pos, len = str_strlen(str, enc); /* str's enc */

    if (rb_scan_args(argc, argv, "11", &sub, &vpos) == 2) {
	pos = NUM2LONG(vpos);
	if (pos < 0) {
	    pos += len;
	    if (pos < 0) {
		if (RB_TYPE_P(sub, T_REGEXP)) {
		    rb_backref_set(Qnil);
		}
		return Qnil;
	    }
	}
	if (pos > len) pos = len;
    }
    else {
	pos = len;
    }

    if (SPECIAL_CONST_P(sub)) goto generic;
    switch (BUILTIN_TYPE(sub)) {
      case T_REGEXP:
	/* enc = rb_get_check(str, sub); */
	pos = str_offset(RSTRING_PTR(str), RSTRING_END(str), pos,
			 enc, single_byte_optimizable(str));

	if (!RREGEXP(sub)->ptr || RREGEXP_SRC_LEN(sub)) {
	    pos = rb_reg_search(sub, str, pos, 1);
	    pos = rb_str_sublen(str, pos);
	}
	if (pos >= 0) return LONG2NUM(pos);
	break;

      generic:
      default: {
	VALUE tmp;

	tmp = rb_check_string_type(sub);
	if (NIL_P(tmp)) {
	    rb_raise(rb_eTypeError, "type mismatch: %s given",
		     rb_obj_classname(sub));
	}
	sub = tmp;
      }
	/* fall through */
      case T_STRING:
	pos = rb_str_rindex(str, sub, pos);
	if (pos >= 0) return LONG2NUM(pos);
	break;
    }
    return Qnil;
}

/*
 *  call-seq:
 *     str =~ obj   -> fixnum or nil
 *
 *  Match---If <i>obj</i> is a <code>Regexp</code>, use it as a pattern to match
 *  against <i>str</i>,and returns the position the match starts, or
 *  <code>nil</code> if there is no match. Otherwise, invokes
 *  <i>obj.=~</i>, passing <i>str</i> as an argument. The default
 *  <code>=~</code> in <code>Object</code> returns <code>nil</code>.
 *
 *  Note: <code>str =~ regexp</code> is not the same as
 *  <code>regexp =~ str</code>. Strings captured from named capture groups
 *  are assigned to local variables only in the second case.
 *
 *     "cat o' 9 tails" =~ /\d/   #=> 7
 *     "cat o' 9 tails" =~ 9      #=> nil
 */

static VALUE
rb_str_match(VALUE x, VALUE y)
{
    if (SPECIAL_CONST_P(y)) goto generic;
    switch (BUILTIN_TYPE(y)) {
      case T_STRING:
	rb_raise(rb_eTypeError, "type mismatch: String given");

      case T_REGEXP:
	return rb_reg_match(y, x);

      generic:
      default:
	return rb_funcall(y, rb_intern("=~"), 1, x);
    }
}


static VALUE get_pat(VALUE);


/*
 *  call-seq:
 *     str.match(pattern)        -> matchdata or nil
 *     str.match(pattern, pos)   -> matchdata or nil
 *
 *  Converts <i>pattern</i> to a <code>Regexp</code> (if it isn't already one),
 *  then invokes its <code>match</code> method on <i>str</i>.  If the second
 *  parameter is present, it specifies the position in the string to begin the
 *  search.
 *
 *     'hello'.match('(.)\1')      #=> #<MatchData "ll" 1:"l">
 *     'hello'.match('(.)\1')[0]   #=> "ll"
 *     'hello'.match(/(.)\1/)[0]   #=> "ll"
 *     'hello'.match('xx')         #=> nil
 *
 *  If a block is given, invoke the block with MatchData if match succeed, so
 *  that you can write
 *
 *     str.match(pat) {|m| ...}
 *
 *  instead of
 *
 *     if m = str.match(pat)
 *       ...
 *     end
 *
 *  The return value is a value from block execution in this case.
 */

static VALUE
rb_str_match_m(int argc, VALUE *argv, VALUE str)
{
    VALUE re, result;
    if (argc < 1)
	rb_check_arity(argc, 1, 2);
    re = argv[0];
    argv[0] = str;
    result = rb_funcall2(get_pat(re), rb_intern("match"), argc, argv);
    if (!NIL_P(result) && rb_block_given_p()) {
	return rb_yield(result);
    }
    return result;
}

enum neighbor_char {
    NEIGHBOR_NOT_CHAR,
    NEIGHBOR_FOUND,
    NEIGHBOR_WRAPPED
};

static enum neighbor_char
enc_succ_char(char *p, long len, rb_encoding *enc)
{
    long i;
    int l;

    if (rb_enc_mbminlen(enc) > 1) {
	/* wchar, trivial case */
	int r = rb_enc_precise_mbclen(p, p + len, enc), c;
	if (!MBCLEN_CHARFOUND_P(r)) {
	    return NEIGHBOR_NOT_CHAR;
	}
	c = rb_enc_mbc_to_codepoint(p, p + len, enc) + 1;
	l = rb_enc_code_to_mbclen(c, enc);
	if (!l) return NEIGHBOR_NOT_CHAR;
	if (l != len) return NEIGHBOR_WRAPPED;
	rb_enc_mbcput(c, p, enc);
	r = rb_enc_precise_mbclen(p, p + len, enc);
	if (!MBCLEN_CHARFOUND_P(r)) {
	    return NEIGHBOR_NOT_CHAR;
	}
	return NEIGHBOR_FOUND;
    }
    while (1) {
        for (i = len-1; 0 <= i && (unsigned char)p[i] == 0xff; i--)
            p[i] = '\0';
        if (i < 0)
            return NEIGHBOR_WRAPPED;
        ++((unsigned char*)p)[i];
        l = rb_enc_precise_mbclen(p, p+len, enc);
        if (MBCLEN_CHARFOUND_P(l)) {
            l = MBCLEN_CHARFOUND_LEN(l);
            if (l == len) {
                return NEIGHBOR_FOUND;
            }
            else {
                memset(p+l, 0xff, len-l);
            }
        }
        if (MBCLEN_INVALID_P(l) && i < len-1) {
            long len2;
            int l2;
            for (len2 = len-1; 0 < len2; len2--) {
                l2 = rb_enc_precise_mbclen(p, p+len2, enc);
                if (!MBCLEN_INVALID_P(l2))
                    break;
            }
            memset(p+len2+1, 0xff, len-(len2+1));
        }
    }
}

static enum neighbor_char
enc_pred_char(char *p, long len, rb_encoding *enc)
{
    long i;
    int l;
    if (rb_enc_mbminlen(enc) > 1) {
	/* wchar, trivial case */
	int r = rb_enc_precise_mbclen(p, p + len, enc), c;
	if (!MBCLEN_CHARFOUND_P(r)) {
	    return NEIGHBOR_NOT_CHAR;
	}
	c = rb_enc_mbc_to_codepoint(p, p + len, enc);
	if (!c) return NEIGHBOR_NOT_CHAR;
	--c;
	l = rb_enc_code_to_mbclen(c, enc);
	if (!l) return NEIGHBOR_NOT_CHAR;
	if (l != len) return NEIGHBOR_WRAPPED;
	rb_enc_mbcput(c, p, enc);
	r = rb_enc_precise_mbclen(p, p + len, enc);
	if (!MBCLEN_CHARFOUND_P(r)) {
	    return NEIGHBOR_NOT_CHAR;
	}
	return NEIGHBOR_FOUND;
    }
    while (1) {
        for (i = len-1; 0 <= i && (unsigned char)p[i] == 0; i--)
            p[i] = '\xff';
        if (i < 0)
            return NEIGHBOR_WRAPPED;
        --((unsigned char*)p)[i];
        l = rb_enc_precise_mbclen(p, p+len, enc);
        if (MBCLEN_CHARFOUND_P(l)) {
            l = MBCLEN_CHARFOUND_LEN(l);
            if (l == len) {
                return NEIGHBOR_FOUND;
            }
            else {
                memset(p+l, 0, len-l);
            }
        }
        if (MBCLEN_INVALID_P(l) && i < len-1) {
            long len2;
            int l2;
            for (len2 = len-1; 0 < len2; len2--) {
                l2 = rb_enc_precise_mbclen(p, p+len2, enc);
                if (!MBCLEN_INVALID_P(l2))
                    break;
            }
            memset(p+len2+1, 0, len-(len2+1));
        }
    }
}

/*
  overwrite +p+ by succeeding letter in +enc+ and returns
  NEIGHBOR_FOUND or NEIGHBOR_WRAPPED.
  When NEIGHBOR_WRAPPED, carried-out letter is stored into carry.
  assuming each ranges are successive, and mbclen
  never change in each ranges.
  NEIGHBOR_NOT_CHAR is returned if invalid character or the range has only one
  character.
 */
static enum neighbor_char
enc_succ_alnum_char(char *p, long len, rb_encoding *enc, char *carry)
{
    enum neighbor_char ret;
    unsigned int c;
    int ctype;
    int range;
    char save[ONIGENC_CODE_TO_MBC_MAXLEN];

    c = rb_enc_mbc_to_codepoint(p, p+len, enc);
    if (rb_enc_isctype(c, ONIGENC_CTYPE_DIGIT, enc))
        ctype = ONIGENC_CTYPE_DIGIT;
    else if (rb_enc_isctype(c, ONIGENC_CTYPE_ALPHA, enc))
        ctype = ONIGENC_CTYPE_ALPHA;
    else
        return NEIGHBOR_NOT_CHAR;

    MEMCPY(save, p, char, len);
    ret = enc_succ_char(p, len, enc);
    if (ret == NEIGHBOR_FOUND) {
        c = rb_enc_mbc_to_codepoint(p, p+len, enc);
        if (rb_enc_isctype(c, ctype, enc))
            return NEIGHBOR_FOUND;
    }
    MEMCPY(p, save, char, len);
    range = 1;
    while (1) {
        MEMCPY(save, p, char, len);
        ret = enc_pred_char(p, len, enc);
        if (ret == NEIGHBOR_FOUND) {
            c = rb_enc_mbc_to_codepoint(p, p+len, enc);
            if (!rb_enc_isctype(c, ctype, enc)) {
                MEMCPY(p, save, char, len);
                break;
            }
        }
        else {
            MEMCPY(p, save, char, len);
            break;
        }
        range++;
    }
    if (range == 1) {
        return NEIGHBOR_NOT_CHAR;
    }

    if (ctype != ONIGENC_CTYPE_DIGIT) {
        MEMCPY(carry, p, char, len);
        return NEIGHBOR_WRAPPED;
    }

    MEMCPY(carry, p, char, len);
    enc_succ_char(carry, len, enc);
    return NEIGHBOR_WRAPPED;
}


static VALUE str_succ(VALUE str);

/*
 *  call-seq:
 *     str.succ   -> new_str
 *     str.next   -> new_str
 *
 *  Returns the successor to <i>str</i>. The successor is calculated by
 *  incrementing characters starting from the rightmost alphanumeric (or
 *  the rightmost character if there are no alphanumerics) in the
 *  string. Incrementing a digit always results in another digit, and
 *  incrementing a letter results in another letter of the same case.
 *  Incrementing nonalphanumerics uses the underlying character set's
 *  collating sequence.
 *
 *  If the increment generates a ``carry,'' the character to the left of
 *  it is incremented. This process repeats until there is no carry,
 *  adding an additional character if necessary.
 *
 *     "abcd".succ        #=> "abce"
 *     "THX1138".succ     #=> "THX1139"
 *     "<<koala>>".succ   #=> "<<koalb>>"
 *     "1999zzz".succ     #=> "2000aaa"
 *     "ZZZ9999".succ     #=> "AAAA0000"
 *     "***".succ         #=> "**+"
 */

VALUE
rb_str_succ(VALUE orig)
{
    VALUE str;
    str = rb_str_new_with_class(orig, RSTRING_PTR(orig), RSTRING_LEN(orig));
    rb_enc_cr_str_copy_for_substr(str, orig);
    OBJ_INFECT(str, orig);
    return str_succ(str);
}

static VALUE
str_succ(VALUE str)
{
    rb_encoding *enc;
    char *sbeg, *s, *e, *last_alnum = 0;
    int c = -1;
    long l, slen;
    char carry[ONIGENC_CODE_TO_MBC_MAXLEN] = "\1";
    long carry_pos = 0, carry_len = 1;
    enum neighbor_char neighbor = NEIGHBOR_FOUND;

    slen = RSTRING_LEN(str);
    if (slen == 0) return str;

    enc = STR_ENC_GET(str);
    sbeg = RSTRING_PTR(str);
    s = e = sbeg + slen;

    while ((s = rb_enc_prev_char(sbeg, s, e, enc)) != 0) {
	if (neighbor == NEIGHBOR_NOT_CHAR && last_alnum) {
	    if (ISALPHA(*last_alnum) ? ISDIGIT(*s) :
		ISDIGIT(*last_alnum) ? ISALPHA(*s) : 0) {
		s = last_alnum;
		break;
	    }
	}
	l = rb_enc_precise_mbclen(s, e, enc);
	if (!ONIGENC_MBCLEN_CHARFOUND_P(l)) continue;
	l = ONIGENC_MBCLEN_CHARFOUND_LEN(l);
        neighbor = enc_succ_alnum_char(s, l, enc, carry);
        switch (neighbor) {
	  case NEIGHBOR_NOT_CHAR:
	    continue;
	  case NEIGHBOR_FOUND:
	    return str;
	  case NEIGHBOR_WRAPPED:
	    last_alnum = s;
	    break;
	}
        c = 1;
        carry_pos = s - sbeg;
        carry_len = l;
    }
    if (c == -1) {		/* str contains no alnum */
	s = e;
	while ((s = rb_enc_prev_char(sbeg, s, e, enc)) != 0) {
            enum neighbor_char neighbor;
	    char tmp[ONIGENC_CODE_TO_MBC_MAXLEN];
	    l = rb_enc_precise_mbclen(s, e, enc);
	    if (!ONIGENC_MBCLEN_CHARFOUND_P(l)) continue;
	    l = ONIGENC_MBCLEN_CHARFOUND_LEN(l);
	    MEMCPY(tmp, s, char, l);
	    neighbor = enc_succ_char(tmp, l, enc);
	    switch (neighbor) {
	      case NEIGHBOR_FOUND:
		MEMCPY(s, tmp, char, l);
                return str;
		break;
	      case NEIGHBOR_WRAPPED:
		MEMCPY(s, tmp, char, l);
		break;
	      case NEIGHBOR_NOT_CHAR:
		break;
	    }
            if (rb_enc_precise_mbclen(s, s+l, enc) != l) {
                /* wrapped to \0...\0.  search next valid char. */
                enc_succ_char(s, l, enc);
            }
            if (!rb_enc_asciicompat(enc)) {
                MEMCPY(carry, s, char, l);
                carry_len = l;
            }
            carry_pos = s - sbeg;
	}
    }
    RESIZE_CAPA(str, slen + carry_len);
    sbeg = RSTRING_PTR(str);
    s = sbeg + carry_pos;
    memmove(s + carry_len, s, slen - carry_pos);
    memmove(s, carry, carry_len);
    slen += carry_len;
    STR_SET_LEN(str, slen);
    TERM_FILL(&sbeg[slen], rb_enc_mbminlen(enc));
    rb_enc_str_coderange(str);
    return str;
}


/*
 *  call-seq:
 *     str.succ!   -> str
 *     str.next!   -> str
 *
 *  Equivalent to <code>String#succ</code>, but modifies the receiver in
 *  place.
 */

static VALUE
rb_str_succ_bang(VALUE str)
{
    rb_str_modify(str);
    str_succ(str);
    return str;
}

static int
all_digits_p(const char *s, long len)
{
    while (len-- > 0) {
	if (!ISDIGIT(*s)) return 0;
	s++;
    }
    return 1;
}

static VALUE str_upto_each(VALUE beg, VALUE end, int excl, int (*each)(VALUE, VALUE), VALUE);

static int
str_upto_i(VALUE str, VALUE arg)
{
    rb_yield(str);
    return 0;
}

/*
 *  call-seq:
 *     str.upto(other_str, exclusive=false) {|s| block }   -> str
 *     str.upto(other_str, exclusive=false)                -> an_enumerator
 *
 *  Iterates through successive values, starting at <i>str</i> and
 *  ending at <i>other_str</i> inclusive, passing each value in turn to
 *  the block. The <code>String#succ</code> method is used to generate
 *  each value.  If optional second argument exclusive is omitted or is false,
 *  the last value will be included; otherwise it will be excluded.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     "a8".upto("b6") {|s| print s, ' ' }
 *     for s in "a8".."b6"
 *       print s, ' '
 *     end
 *
 *  <em>produces:</em>
 *
 *     a8 a9 b0 b1 b2 b3 b4 b5 b6
 *     a8 a9 b0 b1 b2 b3 b4 b5 b6
 *
 *  If <i>str</i> and <i>other_str</i> contains only ascii numeric characters,
 *  both are recognized as decimal numbers. In addition, the width of
 *  string (e.g. leading zeros) is handled appropriately.
 *
 *     "9".upto("11").to_a   #=> ["9", "10", "11"]
 *     "25".upto("5").to_a   #=> []
 *     "07".upto("11").to_a  #=> ["07", "08", "09", "10", "11"]
 */

static VALUE
rb_str_upto(int argc, VALUE *argv, VALUE beg)
{
    VALUE end, exclusive;

    rb_scan_args(argc, argv, "11", &end, &exclusive);
    RETURN_ENUMERATOR(beg, argc, argv);
    return str_upto_each(beg, end, RTEST(exclusive), str_upto_i, Qnil);
}

static VALUE
str_upto_each(VALUE beg, VALUE end, int excl, int (*each)(VALUE, VALUE), VALUE arg)
{
    VALUE current, after_end;
    ID succ;
    int n, ascii;
    rb_encoding *enc;

    CONST_ID(succ, "succ");
    StringValue(end);
    enc = rb_enc_check(beg, end);
    ascii = (is_ascii_string(beg) && is_ascii_string(end));
    /* single character */
    if (RSTRING_LEN(beg) == 1 && RSTRING_LEN(end) == 1 && ascii) {
	char c = RSTRING_PTR(beg)[0];
	char e = RSTRING_PTR(end)[0];

	if (c > e || (excl && c == e)) return beg;
	for (;;) {
	    if ((*each)(rb_enc_str_new(&c, 1, enc), arg)) break;
	    if (!excl && c == e) break;
	    c++;
	    if (excl && c == e) break;
	}
	return beg;
    }
    /* both edges are all digits */
    if (ascii && ISDIGIT(RSTRING_PTR(beg)[0]) && ISDIGIT(RSTRING_PTR(end)[0]) &&
	all_digits_p(RSTRING_PTR(beg), RSTRING_LEN(beg)) &&
	all_digits_p(RSTRING_PTR(end), RSTRING_LEN(end))) {
	VALUE b, e;
	int width;

	width = RSTRING_LENINT(beg);
	b = rb_str_to_inum(beg, 10, FALSE);
	e = rb_str_to_inum(end, 10, FALSE);
	if (FIXNUM_P(b) && FIXNUM_P(e)) {
	    long bi = FIX2LONG(b);
	    long ei = FIX2LONG(e);
	    rb_encoding *usascii = rb_usascii_encoding();

	    while (bi <= ei) {
		if (excl && bi == ei) break;
		if ((*each)(rb_enc_sprintf(usascii, "%.*ld", width, bi), arg)) break;
		bi++;
	    }
	}
	else {
	    ID op = excl ? '<' : rb_intern("<=");
	    VALUE args[2], fmt = rb_obj_freeze(rb_usascii_str_new_cstr("%.*d"));

	    args[0] = INT2FIX(width);
	    while (rb_funcall(b, op, 1, e)) {
		args[1] = b;
		if ((*each)(rb_str_format(numberof(args), args, fmt), arg)) break;
		b = rb_funcallv(b, succ, 0, 0);
	    }
	}
	return beg;
    }
    /* normal case */
    n = rb_str_cmp(beg, end);
    if (n > 0 || (excl && n == 0)) return beg;

    after_end = rb_funcallv(end, succ, 0, 0);
    current = rb_str_dup(beg);
    while (!rb_str_equal(current, after_end)) {
	VALUE next = Qnil;
	if (excl || !rb_str_equal(current, end))
	    next = rb_funcallv(current, succ, 0, 0);
	if ((*each)(current, arg)) break;
	if (NIL_P(next)) break;
	current = next;
	StringValue(current);
	if (excl && rb_str_equal(current, end)) break;
	if (RSTRING_LEN(current) > RSTRING_LEN(end) || RSTRING_LEN(current) == 0)
	    break;
    }

    return beg;
}

static int
include_range_i(VALUE str, VALUE arg)
{
    VALUE *argp = (VALUE *)arg;
    if (!rb_equal(str, *argp)) return 0;
    *argp = Qnil;
    return 1;
}

VALUE
rb_str_include_range_p(VALUE beg, VALUE end, VALUE val, VALUE exclusive)
{
    beg = rb_str_new_frozen(beg);
    StringValue(end);
    end = rb_str_new_frozen(end);
    if (NIL_P(val)) return Qfalse;
    val = rb_check_string_type(val);
    if (NIL_P(val)) return Qfalse;
    if (rb_enc_asciicompat(STR_ENC_GET(beg)) &&
	rb_enc_asciicompat(STR_ENC_GET(end)) &&
	rb_enc_asciicompat(STR_ENC_GET(val))) {
	const char *bp = RSTRING_PTR(beg);
	const char *ep = RSTRING_PTR(end);
	const char *vp = RSTRING_PTR(val);
	if (RSTRING_LEN(beg) == 1 && RSTRING_LEN(end) == 1) {
	    if (RSTRING_LEN(val) == 0 || RSTRING_LEN(val) > 1)
		return Qfalse;
	    else {
		char b = *bp;
		char e = *ep;
		char v = *vp;

		if (ISASCII(b) && ISASCII(e) && ISASCII(v)) {
		    if (b <= v && v < e) return Qtrue;
		    if (!RTEST(exclusive) && v == e) return Qtrue;
		    return Qfalse;
		}
	    }
	}
#if 0
	/* both edges are all digits */
	if (ISDIGIT(*bp) && ISDIGIT(*ep) &&
	    all_digits_p(bp, RSTRING_LEN(beg)) &&
	    all_digits_p(ep, RSTRING_LEN(end))) {
	    /* TODO */
	}
#endif
    }
    str_upto_each(beg, end, RTEST(exclusive), include_range_i, (VALUE)&val);

    return NIL_P(val) ? Qtrue : Qfalse;
}

static VALUE
rb_str_subpat(VALUE str, VALUE re, VALUE backref)
{
    if (rb_reg_search(re, str, 0, 0) >= 0) {
        VALUE match = rb_backref_get();
        int nth = rb_reg_backref_number(match, backref);
	return rb_reg_nth_match(nth, match);
    }
    return Qnil;
}

static VALUE
rb_str_aref(VALUE str, VALUE indx)
{
    long idx;

    if (FIXNUM_P(indx)) {
	idx = FIX2LONG(indx);

      num_index:
	str = rb_str_substr(str, idx, 1);
	if (!NIL_P(str) && RSTRING_LEN(str) == 0) return Qnil;
	return str;
    }

    if (SPECIAL_CONST_P(indx)) goto generic;
    switch (BUILTIN_TYPE(indx)) {
      case T_REGEXP:
	return rb_str_subpat(str, indx, INT2FIX(0));

      case T_STRING:
	if (rb_str_index(str, indx, 0) != -1)
	    return rb_str_dup(indx);
	return Qnil;

      generic:
      default:
	/* check if indx is Range */
	{
	    long beg, len;
	    VALUE tmp;

	    len = str_strlen(str, NULL);
	    switch (rb_range_beg_len(indx, &beg, &len, len, 0)) {
	      case Qfalse:
		break;
	      case Qnil:
		return Qnil;
	      default:
		tmp = rb_str_substr(str, beg, len);
		return tmp;
	    }
	}
	idx = NUM2LONG(indx);
	goto num_index;
    }

    UNREACHABLE;
}


/*
 *  call-seq:
 *     str[index]                 -> new_str or nil
 *     str[start, length]         -> new_str or nil
 *     str[range]                 -> new_str or nil
 *     str[regexp]                -> new_str or nil
 *     str[regexp, capture]       -> new_str or nil
 *     str[match_str]             -> new_str or nil
 *     str.slice(index)           -> new_str or nil
 *     str.slice(start, length)   -> new_str or nil
 *     str.slice(range)           -> new_str or nil
 *     str.slice(regexp)          -> new_str or nil
 *     str.slice(regexp, capture) -> new_str or nil
 *     str.slice(match_str)       -> new_str or nil
 *
 *  Element Reference --- If passed a single +index+, returns a substring of
 *  one character at that index. If passed a +start+ index and a +length+,
 *  returns a substring containing +length+ characters starting at the
 *  +start+ index. If passed a +range+, its beginning and end are interpreted as
 *  offsets delimiting the substring to be returned.
 *
 *  In these three cases, if an index is negative, it is counted from the end
 *  of the string.  For the +start+ and +range+ cases the starting index
 *  is just before a character and an index matching the string's size.
 *  Additionally, an empty string is returned when the starting index for a
 *  character range is at the end of the string.
 *
 *  Returns +nil+ if the initial index falls outside the string or the length
 *  is negative.
 *
 *  If a +Regexp+ is supplied, the matching portion of the string is
 *  returned.  If a +capture+ follows the regular expression, which may be a
 *  capture group index or name, follows the regular expression that component
 *  of the MatchData is returned instead.
 *
 *  If a +match_str+ is given, that string is returned if it occurs in
 *  the string.
 *
 *  Returns +nil+ if the regular expression does not match or the match string
 *  cannot be found.
 *
 *     a = "hello there"
 *
 *     a[1]                   #=> "e"
 *     a[2, 3]                #=> "llo"
 *     a[2..3]                #=> "ll"
 *
 *     a[-3, 2]               #=> "er"
 *     a[7..-2]               #=> "her"
 *     a[-4..-2]              #=> "her"
 *     a[-2..-4]              #=> ""
 *
 *     a[11, 0]               #=> ""
 *     a[11]                  #=> nil
 *     a[12, 0]               #=> nil
 *     a[12..-1]              #=> nil
 *
 *     a[/[aeiou](.)\1/]      #=> "ell"
 *     a[/[aeiou](.)\1/, 0]   #=> "ell"
 *     a[/[aeiou](.)\1/, 1]   #=> "l"
 *     a[/[aeiou](.)\1/, 2]   #=> nil
 *
 *     a[/(?<vowel>[aeiou])(?<non_vowel>[^aeiou])/, "non_vowel"] #=> "l"
 *     a[/(?<vowel>[aeiou])(?<non_vowel>[^aeiou])/, "vowel"]     #=> "e"
 *
 *     a["lo"]                #=> "lo"
 *     a["bye"]               #=> nil
 */

static VALUE
rb_str_aref_m(int argc, VALUE *argv, VALUE str)
{
    if (argc == 2) {
	if (RB_TYPE_P(argv[0], T_REGEXP)) {
	    return rb_str_subpat(str, argv[0], argv[1]);
	}
	return rb_str_substr(str, NUM2LONG(argv[0]), NUM2LONG(argv[1]));
    }
    rb_check_arity(argc, 1, 2);
    return rb_str_aref(str, argv[0]);
}

VALUE
rb_str_drop_bytes(VALUE str, long len)
{
    char *ptr = RSTRING_PTR(str);
    long olen = RSTRING_LEN(str), nlen;

    str_modifiable(str);
    if (len > olen) len = olen;
    nlen = olen - len;
    if (nlen <= RSTRING_EMBED_LEN_MAX) {
	char *oldptr = ptr;
	int fl = (int)(RBASIC(str)->flags & (STR_NOEMBED|STR_SHARED|STR_NOFREE));
	STR_SET_EMBED(str);
	STR_SET_EMBED_LEN(str, nlen);
	ptr = RSTRING(str)->as.ary;
	memmove(ptr, oldptr + len, nlen);
	if (fl == STR_NOEMBED) xfree(oldptr);
    }
    else {
	if (!STR_SHARED_P(str)) rb_str_new_frozen(str);
	ptr = RSTRING(str)->as.heap.ptr += len;
	RSTRING(str)->as.heap.len = nlen;
    }
    ptr[nlen] = 0;
    ENC_CODERANGE_CLEAR(str);
    return str;
}

static void
rb_str_splice_0(VALUE str, long beg, long len, VALUE val)
{
    char *sptr;
    long slen, vlen = RSTRING_LEN(val);

    if (beg == 0 && vlen == 0) {
	rb_str_drop_bytes(str, len);
	OBJ_INFECT(str, val);
	return;
    }

    rb_str_modify(str);
    RSTRING_GETMEM(str, sptr, slen);
    if (len < vlen) {
	/* expand string */
	RESIZE_CAPA(str, slen + vlen - len);
	sptr = RSTRING_PTR(str);
    }

    if (vlen != len) {
	memmove(sptr + beg + vlen,
		sptr + beg + len,
		slen - (beg + len));
    }
    if (vlen < beg && len < 0) {
	MEMZERO(sptr + slen, char, -len);
    }
    if (vlen > 0) {
	memmove(sptr + beg, RSTRING_PTR(val), vlen);
    }
    slen += vlen - len;
    STR_SET_LEN(str, slen);
    TERM_FILL(&sptr[slen], TERM_LEN(str));
    OBJ_INFECT(str, val);
}

void
rb_str_update(VALUE str, long beg, long len, VALUE val)
{
    long slen;
    char *p, *e;
    rb_encoding *enc;
    int singlebyte = single_byte_optimizable(str);
    int cr;

    if (len < 0) rb_raise(rb_eIndexError, "negative length %ld", len);

    StringValue(val);
    enc = rb_enc_check(str, val);
    slen = str_strlen(str, enc); /* rb_enc_check */

    if (slen < beg) {
      out_of_range:
	rb_raise(rb_eIndexError, "index %ld out of string", beg);
    }
    if (beg < 0) {
	if (-beg > slen) {
	    goto out_of_range;
	}
	beg += slen;
    }
    if (slen < len || slen < beg + len) {
	len = slen - beg;
    }
    str_modify_keep_cr(str);
    p = str_nth(RSTRING_PTR(str), RSTRING_END(str), beg, enc, singlebyte);
    if (!p) p = RSTRING_END(str);
    e = str_nth(p, RSTRING_END(str), len, enc, singlebyte);
    if (!e) e = RSTRING_END(str);
    /* error check */
    beg = p - RSTRING_PTR(str);	/* physical position */
    len = e - p;		/* physical length */
    rb_str_splice_0(str, beg, len, val);
    rb_enc_associate(str, enc);
    cr = ENC_CODERANGE_AND(ENC_CODERANGE(str), ENC_CODERANGE(val));
    if (cr != ENC_CODERANGE_BROKEN)
	ENC_CODERANGE_SET(str, cr);
}

#define rb_str_splice(str, beg, len, val) rb_str_update(str, beg, len, val)

static void
rb_str_subpat_set(VALUE str, VALUE re, VALUE backref, VALUE val)
{
    int nth;
    VALUE match;
    long start, end, len;
    rb_encoding *enc;
    struct re_registers *regs;

    if (rb_reg_search(re, str, 0, 0) < 0) {
	rb_raise(rb_eIndexError, "regexp not matched");
    }
    match = rb_backref_get();
    nth = rb_reg_backref_number(match, backref);
    regs = RMATCH_REGS(match);
    if (nth >= regs->num_regs) {
      out_of_range:
	rb_raise(rb_eIndexError, "index %d out of regexp", nth);
    }
    if (nth < 0) {
	if (-nth >= regs->num_regs) {
	    goto out_of_range;
	}
	nth += regs->num_regs;
    }

    start = BEG(nth);
    if (start == -1) {
	rb_raise(rb_eIndexError, "regexp group %d not matched", nth);
    }
    end = END(nth);
    len = end - start;
    StringValue(val);
    enc = rb_enc_check(str, val);
    rb_str_splice_0(str, start, len, val);
    rb_enc_associate(str, enc);
}

static VALUE
rb_str_aset(VALUE str, VALUE indx, VALUE val)
{
    long idx, beg;

    if (FIXNUM_P(indx)) {
	idx = FIX2LONG(indx);
      num_index:
	rb_str_splice(str, idx, 1, val);
	return val;
    }

    if (SPECIAL_CONST_P(indx)) goto generic;
    switch (TYPE(indx)) {
      case T_REGEXP:
	rb_str_subpat_set(str, indx, INT2FIX(0), val);
	return val;

      case T_STRING:
	beg = rb_str_index(str, indx, 0);
	if (beg < 0) {
	    rb_raise(rb_eIndexError, "string not matched");
	}
	beg = rb_str_sublen(str, beg);
	rb_str_splice(str, beg, str_strlen(indx, NULL), val);
	return val;

      generic:
      default:
	/* check if indx is Range */
	{
	    long beg, len;
	    if (rb_range_beg_len(indx, &beg, &len, str_strlen(str, NULL), 2)) {
		rb_str_splice(str, beg, len, val);
		return val;
	    }
	}
	idx = NUM2LONG(indx);
	goto num_index;
    }
}

/*
 *  call-seq:
 *     str[fixnum] = new_str
 *     str[fixnum, fixnum] = new_str
 *     str[range] = aString
 *     str[regexp] = new_str
 *     str[regexp, fixnum] = new_str
 *     str[regexp, name] = new_str
 *     str[other_str] = new_str
 *
 *  Element Assignment---Replaces some or all of the content of <i>str</i>. The
 *  portion of the string affected is determined using the same criteria as
 *  <code>String#[]</code>. If the replacement string is not the same length as
 *  the text it is replacing, the string will be adjusted accordingly. If the
 *  regular expression or string is used as the index doesn't match a position
 *  in the string, <code>IndexError</code> is raised. If the regular expression
 *  form is used, the optional second <code>Fixnum</code> allows you to specify
 *  which portion of the match to replace (effectively using the
 *  <code>MatchData</code> indexing rules. The forms that take a
 *  <code>Fixnum</code> will raise an <code>IndexError</code> if the value is
 *  out of range; the <code>Range</code> form will raise a
 *  <code>RangeError</code>, and the <code>Regexp</code> and <code>String</code>
 *  will raise an <code>IndexError</code> on negative match.
 */

static VALUE
rb_str_aset_m(int argc, VALUE *argv, VALUE str)
{
    if (argc == 3) {
	if (RB_TYPE_P(argv[0], T_REGEXP)) {
	    rb_str_subpat_set(str, argv[0], argv[1], argv[2]);
	}
	else {
	    rb_str_splice(str, NUM2LONG(argv[0]), NUM2LONG(argv[1]), argv[2]);
	}
	return argv[2];
    }
    rb_check_arity(argc, 2, 3);
    return rb_str_aset(str, argv[0], argv[1]);
}

/*
 *  call-seq:
 *     str.insert(index, other_str)   -> str
 *
 *  Inserts <i>other_str</i> before the character at the given
 *  <i>index</i>, modifying <i>str</i>. Negative indices count from the
 *  end of the string, and insert <em>after</em> the given character.
 *  The intent is insert <i>aString</i> so that it starts at the given
 *  <i>index</i>.
 *
 *     "abcd".insert(0, 'X')    #=> "Xabcd"
 *     "abcd".insert(3, 'X')    #=> "abcXd"
 *     "abcd".insert(4, 'X')    #=> "abcdX"
 *     "abcd".insert(-3, 'X')   #=> "abXcd"
 *     "abcd".insert(-1, 'X')   #=> "abcdX"
 */

static VALUE
rb_str_insert(VALUE str, VALUE idx, VALUE str2)
{
    long pos = NUM2LONG(idx);

    if (pos == -1) {
	return rb_str_append(str, str2);
    }
    else if (pos < 0) {
	pos++;
    }
    rb_str_splice(str, pos, 0, str2);
    return str;
}


/*
 *  call-seq:
 *     str.slice!(fixnum)           -> new_str or nil
 *     str.slice!(fixnum, fixnum)   -> new_str or nil
 *     str.slice!(range)            -> new_str or nil
 *     str.slice!(regexp)           -> new_str or nil
 *     str.slice!(other_str)        -> new_str or nil
 *
 *  Deletes the specified portion from <i>str</i>, and returns the portion
 *  deleted.
 *
 *     string = "this is a string"
 *     string.slice!(2)        #=> "i"
 *     string.slice!(3..6)     #=> " is "
 *     string.slice!(/s.*t/)   #=> "sa st"
 *     string.slice!("r")      #=> "r"
 *     string                  #=> "thing"
 */

static VALUE
rb_str_slice_bang(int argc, VALUE *argv, VALUE str)
{
    VALUE result;
    VALUE buf[3];
    int i;

    rb_check_arity(argc, 1, 2);
    for (i=0; i<argc; i++) {
	buf[i] = argv[i];
    }
    str_modify_keep_cr(str);
    result = rb_str_aref_m(argc, buf, str);
    if (!NIL_P(result)) {
	buf[i] = rb_str_new(0,0);
	rb_str_aset_m(argc+1, buf, str);
    }
    return result;
}

static VALUE
get_pat(VALUE pat)
{
    VALUE val;

    if (SPECIAL_CONST_P(pat)) goto to_string;
    switch (BUILTIN_TYPE(pat)) {
      case T_REGEXP:
	return pat;

      case T_STRING:
	break;

      default:
      to_string:
	val = rb_check_string_type(pat);
	if (NIL_P(val)) {
	    Check_Type(pat, T_REGEXP);
	}
	pat = val;
    }

    return rb_reg_regcomp(pat);
}

static VALUE
get_pat_quoted(VALUE pat, int check)
{
    VALUE val;

    if (SPECIAL_CONST_P(pat)) goto to_string;
    switch (BUILTIN_TYPE(pat)) {
      case T_REGEXP:
	return pat;

      case T_STRING:
	break;

      default:
      to_string:
	val = rb_check_string_type(pat);
	if (NIL_P(val)) {
	    Check_Type(pat, T_REGEXP);
	}
	pat = val;
    }
    if (check && is_broken_string(pat)) {
	rb_exc_raise(rb_reg_check_preprocess(pat));
    }
    return pat;
}

static long
rb_pat_search(VALUE pat, VALUE str, long pos, int set_backref_str)
{
    if (BUILTIN_TYPE(pat) == T_STRING) {
	pos = rb_strseq_index(str, pat, pos, 1);
	if (set_backref_str) {
	    if (pos >= 0) {
		VALUE match;
		str = rb_str_new_frozen(str);
		rb_backref_set_string(str, pos, RSTRING_LEN(pat));
		match = rb_backref_get();
		OBJ_INFECT(match, pat);
	    }
	    else {
		rb_backref_set(Qnil);
	    }
	}
	return pos;
    }
    else {
	return rb_reg_search0(pat, str, pos, 0, set_backref_str);
    }
}


/*
 *  call-seq:
 *     str.sub!(pattern, replacement)          -> str or nil
 *     str.sub!(pattern) {|match| block }      -> str or nil
 *
 *  Performs the same substitution as String#sub in-place.
 *
 *  Returns +str+ if a substitution was performed or +nil+ if no substitution
 *  was performed.
 */

static VALUE
rb_str_sub_bang(int argc, VALUE *argv, VALUE str)
{
    VALUE pat, repl, hash = Qnil;
    int iter = 0;
    int tainted = 0;
    long plen;
    int min_arity = rb_block_given_p() ? 1 : 2;
    long beg;

    rb_check_arity(argc, min_arity, 2);
    if (argc == 1) {
	iter = 1;
    }
    else {
	repl = argv[1];
	hash = rb_check_hash_type(argv[1]);
	if (NIL_P(hash)) {
	    StringValue(repl);
	}
	tainted = OBJ_TAINTED_RAW(repl);
    }

    pat = get_pat_quoted(argv[0], 1);

    str_modifiable(str);
    beg = rb_pat_search(pat, str, 0, 1);
    if (beg >= 0) {
	rb_encoding *enc;
	int cr = ENC_CODERANGE(str);
	long beg0, end0;
	VALUE match, match0 = Qnil;
	struct re_registers *regs;
	char *p, *rp;
	long len, rlen;

	match = rb_backref_get();
	regs = RMATCH_REGS(match);
	if (RB_TYPE_P(pat, T_STRING)) {
	    beg0 = beg;
	    end0 = beg0 + RSTRING_LEN(pat);
	    match0 = pat;
	}
	else {
	    beg0 = BEG(0);
	    end0 = END(0);
	    if (iter) match0 = rb_reg_nth_match(0, match);
	}

	if (iter || !NIL_P(hash)) {
	    p = RSTRING_PTR(str); len = RSTRING_LEN(str);

            if (iter) {
                repl = rb_obj_as_string(rb_yield(match0));
            }
            else {
                repl = rb_hash_aref(hash, rb_str_subseq(str, beg0, end0 - beg0));
                repl = rb_obj_as_string(repl);
            }
	    str_mod_check(str, p, len);
	    rb_check_frozen(str);
	}
	else {
	    repl = rb_reg_regsub(repl, str, regs, RB_TYPE_P(pat, T_STRING) ? Qnil : pat);
	}

        enc = rb_enc_compatible(str, repl);
        if (!enc) {
            rb_encoding *str_enc = STR_ENC_GET(str);
	    p = RSTRING_PTR(str); len = RSTRING_LEN(str);
	    if (coderange_scan(p, beg0, str_enc) != ENC_CODERANGE_7BIT ||
		coderange_scan(p+end0, len-end0, str_enc) != ENC_CODERANGE_7BIT) {
                rb_raise(rb_eEncCompatError, "incompatible character encodings: %s and %s",
			 rb_enc_name(str_enc),
			 rb_enc_name(STR_ENC_GET(repl)));
            }
            enc = STR_ENC_GET(repl);
        }
	rb_str_modify(str);
	rb_enc_associate(str, enc);
	tainted |= OBJ_TAINTED_RAW(repl);
	if (ENC_CODERANGE_UNKNOWN < cr && cr < ENC_CODERANGE_BROKEN) {
	    int cr2 = ENC_CODERANGE(repl);
            if (cr2 == ENC_CODERANGE_BROKEN ||
                (cr == ENC_CODERANGE_VALID && cr2 == ENC_CODERANGE_7BIT))
                cr = ENC_CODERANGE_UNKNOWN;
            else
                cr = cr2;
	}
	plen = end0 - beg0;
	rp = RSTRING_PTR(repl); rlen = RSTRING_LEN(repl);
	len = RSTRING_LEN(str);
	if (rlen > plen) {
	    RESIZE_CAPA(str, len + rlen - plen);
	}
	p = RSTRING_PTR(str);
	if (rlen != plen) {
	    memmove(p + beg0 + rlen, p + beg0 + plen, len - beg0 - plen);
	}
	memcpy(p + beg0, rp, rlen);
	len += rlen - plen;
	STR_SET_LEN(str, len);
	TERM_FILL(&RSTRING_PTR(str)[len], TERM_LEN(str));
	ENC_CODERANGE_SET(str, cr);
	FL_SET_RAW(str, tainted);

	return str;
    }
    return Qnil;
}


/*
 *  call-seq:
 *     str.sub(pattern, replacement)         -> new_str
 *     str.sub(pattern, hash)                -> new_str
 *     str.sub(pattern) {|match| block }     -> new_str
 *
 *  Returns a copy of +str+ with the _first_ occurrence of +pattern+
 *  replaced by the second argument. The +pattern+ is typically a Regexp; if
 *  given as a String, any regular expression metacharacters it contains will
 *  be interpreted literally, e.g. <code>'\\\d'</code> will match a backlash
 *  followed by 'd', instead of a digit.
 *
 *  If +replacement+ is a String it will be substituted for the matched text.
 *  It may contain back-references to the pattern's capture groups of the form
 *  <code>"\\d"</code>, where <i>d</i> is a group number, or
 *  <code>"\\k<n>"</code>, where <i>n</i> is a group name. If it is a
 *  double-quoted string, both back-references must be preceded by an
 *  additional backslash. However, within +replacement+ the special match
 *  variables, such as <code>&$</code>, will not refer to the current match.
 *  If +replacement+ is a String that looks like a pattern's capture group but
 *  is actaully not a pattern capture group e.g. <code>"\\'"</code>, then it
 *  will have to be preceded by two backslashes like so <code>"\\\\'"</code>.
 *
 *  If the second argument is a Hash, and the matched text is one of its keys,
 *  the corresponding value is the replacement string.
 *
 *  In the block form, the current match string is passed in as a parameter,
 *  and variables such as <code>$1</code>, <code>$2</code>, <code>$`</code>,
 *  <code>$&</code>, and <code>$'</code> will be set appropriately. The value
 *  returned by the block will be substituted for the match on each call.
 *
 *  The result inherits any tainting in the original string or any supplied
 *  replacement string.
 *
 *     "hello".sub(/[aeiou]/, '*')                  #=> "h*llo"
 *     "hello".sub(/([aeiou])/, '<\1>')             #=> "h<e>llo"
 *     "hello".sub(/./) {|s| s.ord.to_s + ' ' }     #=> "104 ello"
 *     "hello".sub(/(?<foo>[aeiou])/, '*\k<foo>*')  #=> "h*e*llo"
 *     'Is SHELL your preferred shell?'.sub(/[[:upper:]]{2,}/, ENV)
 *      #=> "Is /bin/bash your preferred shell?"
 */

static VALUE
rb_str_sub(int argc, VALUE *argv, VALUE str)
{
    str = rb_str_dup(str);
    rb_str_sub_bang(argc, argv, str);
    return str;
}

static VALUE
str_gsub(int argc, VALUE *argv, VALUE str, int bang)
{
    VALUE pat, val = Qnil, repl, match, match0 = Qnil, dest, hash = Qnil;
    struct re_registers *regs;
    long beg, n;
    long beg0, end0;
    long offset, blen, slen, len, last;
    enum {STR, ITER, MAP} mode = STR;
    char *sp, *cp;
    int tainted = 0;
    int need_backref = -1;
    rb_encoding *str_enc;

    switch (argc) {
      case 1:
	RETURN_ENUMERATOR(str, argc, argv);
	mode = ITER;
	break;
      case 2:
	repl = argv[1];
	hash = rb_check_hash_type(argv[1]);
	if (NIL_P(hash)) {
	    StringValue(repl);
	}
	else {
	    mode = MAP;
	}
	tainted = OBJ_TAINTED_RAW(repl);
	break;
      default:
	rb_check_arity(argc, 1, 2);
    }

    pat = get_pat_quoted(argv[0], 1);
    beg = rb_pat_search(pat, str, 0, need_backref);
    if (beg < 0) {
	if (bang) return Qnil;	/* no match, no substitution */
	return rb_str_dup(str);
    }

    offset = 0;
    n = 0;
    blen = RSTRING_LEN(str) + 30; /* len + margin */
    dest = rb_str_buf_new(blen);
    sp = RSTRING_PTR(str);
    slen = RSTRING_LEN(str);
    cp = sp;
    str_enc = STR_ENC_GET(str);
    rb_enc_associate(dest, str_enc);
    ENC_CODERANGE_SET(dest, rb_enc_asciicompat(str_enc) ? ENC_CODERANGE_7BIT : ENC_CODERANGE_VALID);

    do {
	n++;

	match = rb_backref_get();
	regs = RMATCH_REGS(match);
	if (RB_TYPE_P(pat, T_STRING)) {
	    beg0 = beg;
	    end0 = beg0 + RSTRING_LEN(pat);
	    match0 = pat;
	}
	else {
	    beg0 = BEG(0);
	    end0 = END(0);
	    if (mode == ITER) match0 = rb_reg_nth_match(0, match);
	}

	if (mode) {
            if (mode == ITER) {
                val = rb_obj_as_string(rb_yield(match0));
            }
            else {
                val = rb_hash_aref(hash, rb_str_subseq(str, beg0, end0 - beg0));
                val = rb_obj_as_string(val);
            }
	    str_mod_check(str, sp, slen);
	    if (val == dest) { 	/* paranoid check [ruby-dev:24827] */
		rb_raise(rb_eRuntimeError, "block should not cheat");
	    }
	}
	else if (need_backref) {
	    val = rb_reg_regsub(repl, str, regs, RB_TYPE_P(pat, T_STRING) ? Qnil : pat);
	    if (need_backref < 0) {
		need_backref = val != repl;
	    }
	}
	else {
	    val = repl;
	}

	tainted |= OBJ_TAINTED_RAW(val);

	len = beg0 - offset;	/* copy pre-match substr */
        if (len) {
            rb_enc_str_buf_cat(dest, cp, len, str_enc);
        }

        rb_str_buf_append(dest, val);

	last = offset;
	offset = end0;
	if (beg0 == end0) {
	    /*
	     * Always consume at least one character of the input string
	     * in order to prevent infinite loops.
	     */
	    if (RSTRING_LEN(str) <= end0) break;
	    len = rb_enc_fast_mbclen(RSTRING_PTR(str)+end0, RSTRING_END(str), str_enc);
            rb_enc_str_buf_cat(dest, RSTRING_PTR(str)+end0, len, str_enc);
	    offset = end0 + len;
	}
	cp = RSTRING_PTR(str) + offset;
	if (offset > RSTRING_LEN(str)) break;
	beg = rb_pat_search(pat, str, offset, need_backref);
    } while (beg >= 0);
    if (RSTRING_LEN(str) > offset) {
        rb_enc_str_buf_cat(dest, cp, RSTRING_LEN(str) - offset, str_enc);
    }
    rb_pat_search(pat, str, last, 1);
    if (bang) {
        str_shared_replace(str, dest);
    }
    else {
	RBASIC_SET_CLASS(dest, rb_obj_class(str));
	tainted |= OBJ_TAINTED_RAW(str);
	str = dest;
    }

    FL_SET_RAW(str, tainted);
    return str;
}


/*
 *  call-seq:
 *     str.gsub!(pattern, replacement)        -> str or nil
 *     str.gsub!(pattern) {|match| block }    -> str or nil
 *     str.gsub!(pattern)                     -> an_enumerator
 *
 *  Performs the substitutions of <code>String#gsub</code> in place, returning
 *  <i>str</i>, or <code>nil</code> if no substitutions were performed.
 *  If no block and no <i>replacement</i> is given, an enumerator is returned instead.
 */

static VALUE
rb_str_gsub_bang(int argc, VALUE *argv, VALUE str)
{
    str_modify_keep_cr(str);
    return str_gsub(argc, argv, str, 1);
}


/*
 *  call-seq:
 *     str.gsub(pattern, replacement)       -> new_str
 *     str.gsub(pattern, hash)              -> new_str
 *     str.gsub(pattern) {|match| block }   -> new_str
 *     str.gsub(pattern)                    -> enumerator
 *
 *  Returns a copy of <i>str</i> with the <em>all</em> occurrences of
 *  <i>pattern</i> substituted for the second argument. The <i>pattern</i> is
 *  typically a <code>Regexp</code>; if given as a <code>String</code>, any
 *  regular expression metacharacters it contains will be interpreted
 *  literally, e.g. <code>'\\\d'</code> will match a backlash followed by 'd',
 *  instead of a digit.
 *
 *  If <i>replacement</i> is a <code>String</code> it will be substituted for
 *  the matched text. It may contain back-references to the pattern's capture
 *  groups of the form <code>\\\d</code>, where <i>d</i> is a group number, or
 *  <code>\\\k<n></code>, where <i>n</i> is a group name. If it is a
 *  double-quoted string, both back-references must be preceded by an
 *  additional backslash. However, within <i>replacement</i> the special match
 *  variables, such as <code>$&</code>, will not refer to the current match.
 *
 *  If the second argument is a <code>Hash</code>, and the matched text is one
 *  of its keys, the corresponding value is the replacement string.
 *
 *  In the block form, the current match string is passed in as a parameter,
 *  and variables such as <code>$1</code>, <code>$2</code>, <code>$`</code>,
 *  <code>$&</code>, and <code>$'</code> will be set appropriately. The value
 *  returned by the block will be substituted for the match on each call.
 *
 *  The result inherits any tainting in the original string or any supplied
 *  replacement string.
 *
 *  When neither a block nor a second argument is supplied, an
 *  <code>Enumerator</code> is returned.
 *
 *     "hello".gsub(/[aeiou]/, '*')                  #=> "h*ll*"
 *     "hello".gsub(/([aeiou])/, '<\1>')             #=> "h<e>ll<o>"
 *     "hello".gsub(/./) {|s| s.ord.to_s + ' '}      #=> "104 101 108 108 111 "
 *     "hello".gsub(/(?<foo>[aeiou])/, '{\k<foo>}')  #=> "h{e}ll{o}"
 *     'hello'.gsub(/[eo]/, 'e' => 3, 'o' => '*')    #=> "h3ll*"
 */

static VALUE
rb_str_gsub(int argc, VALUE *argv, VALUE str)
{
    return str_gsub(argc, argv, str, 0);
}


/*
 *  call-seq:
 *     str.replace(other_str)   -> str
 *
 *  Replaces the contents and taintedness of <i>str</i> with the corresponding
 *  values in <i>other_str</i>.
 *
 *     s = "hello"         #=> "hello"
 *     s.replace "world"   #=> "world"
 */

VALUE
rb_str_replace(VALUE str, VALUE str2)
{
    str_modifiable(str);
    if (str == str2) return str;

    StringValue(str2);
    str_discard(str);
    return str_replace(str, str2);
}

/*
 *  call-seq:
 *     string.clear    ->  string
 *
 *  Makes string empty.
 *
 *     a = "abcde"
 *     a.clear    #=> ""
 */

static VALUE
rb_str_clear(VALUE str)
{
    str_discard(str);
    STR_SET_EMBED(str);
    STR_SET_EMBED_LEN(str, 0);
    RSTRING_PTR(str)[0] = 0;
    if (rb_enc_asciicompat(STR_ENC_GET(str)))
	ENC_CODERANGE_SET(str, ENC_CODERANGE_7BIT);
    else
	ENC_CODERANGE_SET(str, ENC_CODERANGE_VALID);
    return str;
}

/*
 *  call-seq:
 *     string.chr    ->  string
 *
 *  Returns a one-character string at the beginning of the string.
 *
 *     a = "abcde"
 *     a.chr    #=> "a"
 */

static VALUE
rb_str_chr(VALUE str)
{
    return rb_str_substr(str, 0, 1);
}

/*
 *  call-seq:
 *     str.getbyte(index)          -> 0 .. 255
 *
 *  returns the <i>index</i>th byte as an integer.
 */
static VALUE
rb_str_getbyte(VALUE str, VALUE index)
{
    long pos = NUM2LONG(index);

    if (pos < 0)
        pos += RSTRING_LEN(str);
    if (pos < 0 ||  RSTRING_LEN(str) <= pos)
        return Qnil;

    return INT2FIX((unsigned char)RSTRING_PTR(str)[pos]);
}

/*
 *  call-seq:
 *     str.setbyte(index, integer) -> integer
 *
 *  modifies the <i>index</i>th byte as <i>integer</i>.
 */
static VALUE
rb_str_setbyte(VALUE str, VALUE index, VALUE value)
{
    long pos = NUM2LONG(index);
    int byte = NUM2INT(value);
    long len = RSTRING_LEN(str);
    char *head, *ptr, *left = 0;
    rb_encoding *enc;
    int cr = ENC_CODERANGE_UNKNOWN, width, nlen;

    if (pos < -len || len <= pos)
        rb_raise(rb_eIndexError, "index %ld out of string", pos);
    if (pos < 0)
        pos += len;

    if (!str_independent(str))
	str_make_independent(str);
    enc = STR_ENC_GET(str);
    head = RSTRING_PTR(str);
    ptr = &head[pos];
    if (len > RSTRING_EMBED_LEN_MAX) {
	cr = ENC_CODERANGE(str);
	switch (cr) {
	  case ENC_CODERANGE_7BIT:
	    left = ptr;
	    *ptr = byte;
	    if (ISASCII(byte)) break;
	    nlen = rb_enc_precise_mbclen(left, head+len, enc);
	    if (!MBCLEN_CHARFOUND_P(nlen))
		ENC_CODERANGE_SET(str, ENC_CODERANGE_BROKEN);
	    else
		ENC_CODERANGE_SET(str, ENC_CODERANGE_VALID);
	    goto end;
	  case ENC_CODERANGE_VALID:
	    left = rb_enc_left_char_head(head, ptr, head+len, enc);
	    width = rb_enc_precise_mbclen(left, head+len, enc);
	    *ptr = byte;
	    nlen = rb_enc_precise_mbclen(left, head+len, enc);
	    if (!MBCLEN_CHARFOUND_P(nlen))
		ENC_CODERANGE_SET(str, ENC_CODERANGE_BROKEN);
	    else if (MBCLEN_CHARFOUND_LEN(nlen) != width || ISASCII(byte))
		ENC_CODERANGE_CLEAR(str);
	    goto end;
	}
    }
    ENC_CODERANGE_CLEAR(str);
    *ptr = byte;

  end:
    return value;
}

static VALUE
str_byte_substr(VALUE str, long beg, long len)
{
    char *p, *s = RSTRING_PTR(str);
    long n = RSTRING_LEN(str);
    VALUE str2;

    if (beg > n || len < 0) return Qnil;
    if (beg < 0) {
	beg += n;
	if (beg < 0) return Qnil;
    }
    if (beg + len > n)
	len = n - beg;
    if (len <= 0) {
	len = 0;
	p = 0;
    }
    else
	p = s + beg;

    if (len > RSTRING_EMBED_LEN_MAX && SHARABLE_SUBSTRING_P(beg, len, n)) {
	str2 = rb_str_new_frozen(str);
	str2 = str_new_shared(rb_obj_class(str2), str2);
	RSTRING(str2)->as.heap.ptr += beg;
	RSTRING(str2)->as.heap.len = len;
    }
    else {
	str2 = rb_str_new_with_class(str, p, len);
    }

    str_enc_copy(str2, str);

    if (RSTRING_LEN(str2) == 0) {
	if (!rb_enc_asciicompat(STR_ENC_GET(str)))
	    ENC_CODERANGE_SET(str2, ENC_CODERANGE_VALID);
	else
	    ENC_CODERANGE_SET(str2, ENC_CODERANGE_7BIT);
    }
    else {
	switch (ENC_CODERANGE(str)) {
	  case ENC_CODERANGE_7BIT:
	    ENC_CODERANGE_SET(str2, ENC_CODERANGE_7BIT);
	    break;
	  default:
	    ENC_CODERANGE_SET(str2, ENC_CODERANGE_UNKNOWN);
	    break;
	}
    }

    OBJ_INFECT_RAW(str2, str);

    return str2;
}

static VALUE
str_byte_aref(VALUE str, VALUE indx)
{
    long idx;
    switch (TYPE(indx)) {
      case T_FIXNUM:
	idx = FIX2LONG(indx);

      num_index:
	str = str_byte_substr(str, idx, 1);
	if (NIL_P(str) || RSTRING_LEN(str) == 0) return Qnil;
	return str;

      default:
	/* check if indx is Range */
	{
	    long beg, len = RSTRING_LEN(str);

	    switch (rb_range_beg_len(indx, &beg, &len, len, 0)) {
	      case Qfalse:
		break;
	      case Qnil:
		return Qnil;
	      default:
		return str_byte_substr(str, beg, len);
	    }
	}
	idx = NUM2LONG(indx);
	goto num_index;
    }

    UNREACHABLE;
}

/*
 *  call-seq:
 *     str.byteslice(fixnum)           -> new_str or nil
 *     str.byteslice(fixnum, fixnum)   -> new_str or nil
 *     str.byteslice(range)            -> new_str or nil
 *
 *  Byte Reference---If passed a single <code>Fixnum</code>, returns a
 *  substring of one byte at that position. If passed two <code>Fixnum</code>
 *  objects, returns a substring starting at the offset given by the first, and
 *  a length given by the second. If given a <code>Range</code>, a substring containing
 *  bytes at offsets given by the range is returned. In all three cases, if
 *  an offset is negative, it is counted from the end of <i>str</i>. Returns
 *  <code>nil</code> if the initial offset falls outside the string, the length
 *  is negative, or the beginning of the range is greater than the end.
 *  The encoding of the resulted string keeps original encoding.
 *
 *     "hello".byteslice(1)     #=> "e"
 *     "hello".byteslice(-1)    #=> "o"
 *     "hello".byteslice(1, 2)  #=> "el"
 *     "\x80\u3042".byteslice(1, 3) #=> "\u3042"
 *     "\x03\u3042\xff".byteslice(1..3) #=> "\u3042"
 */

static VALUE
rb_str_byteslice(int argc, VALUE *argv, VALUE str)
{
    if (argc == 2) {
	return str_byte_substr(str, NUM2LONG(argv[0]), NUM2LONG(argv[1]));
    }
    rb_check_arity(argc, 1, 2);
    return str_byte_aref(str, argv[0]);
}

/*
 *  call-seq:
 *     str.reverse   -> new_str
 *
 *  Returns a new string with the characters from <i>str</i> in reverse order.
 *
 *     "stressed".reverse   #=> "desserts"
 */

static VALUE
rb_str_reverse(VALUE str)
{
    rb_encoding *enc;
    VALUE rev;
    char *s, *e, *p;
    int cr;

    if (RSTRING_LEN(str) <= 1) return rb_str_dup(str);
    enc = STR_ENC_GET(str);
    rev = rb_str_new_with_class(str, 0, RSTRING_LEN(str));
    s = RSTRING_PTR(str); e = RSTRING_END(str);
    p = RSTRING_END(rev);
    cr = ENC_CODERANGE(str);

    if (RSTRING_LEN(str) > 1) {
	if (single_byte_optimizable(str)) {
	    while (s < e) {
		*--p = *s++;
	    }
	}
	else if (cr == ENC_CODERANGE_VALID) {
	    while (s < e) {
		int clen = rb_enc_fast_mbclen(s, e, enc);

		p -= clen;
		memcpy(p, s, clen);
		s += clen;
	    }
	}
	else {
	    cr = rb_enc_asciicompat(enc) ?
		ENC_CODERANGE_7BIT : ENC_CODERANGE_VALID;
	    while (s < e) {
		int clen = rb_enc_mbclen(s, e, enc);

		if (clen > 1 || (*s & 0x80)) cr = ENC_CODERANGE_UNKNOWN;
		p -= clen;
		memcpy(p, s, clen);
		s += clen;
	    }
	}
    }
    STR_SET_LEN(rev, RSTRING_LEN(str));
    OBJ_INFECT_RAW(rev, str);
    str_enc_copy(rev, str);
    ENC_CODERANGE_SET(rev, cr);

    return rev;
}


/*
 *  call-seq:
 *     str.reverse!   -> str
 *
 *  Reverses <i>str</i> in place.
 */

static VALUE
rb_str_reverse_bang(VALUE str)
{
    if (RSTRING_LEN(str) > 1) {
	if (single_byte_optimizable(str)) {
	    char *s, *e, c;

	    str_modify_keep_cr(str);
	    s = RSTRING_PTR(str);
	    e = RSTRING_END(str) - 1;
	    while (s < e) {
		c = *s;
		*s++ = *e;
		*e-- = c;
	    }
	}
	else {
	    str_shared_replace(str, rb_str_reverse(str));
	}
    }
    else {
	str_modify_keep_cr(str);
    }
    return str;
}


/*
 *  call-seq:
 *     str.include? other_str   -> true or false
 *
 *  Returns <code>true</code> if <i>str</i> contains the given string or
 *  character.
 *
 *     "hello".include? "lo"   #=> true
 *     "hello".include? "ol"   #=> false
 *     "hello".include? ?h     #=> true
 */

static VALUE
rb_str_include(VALUE str, VALUE arg)
{
    long i;

    StringValue(arg);
    i = rb_str_index(str, arg, 0);

    if (i == -1) return Qfalse;
    return Qtrue;
}


/*
 *  call-seq:
 *     str.to_i(base=10)   -> integer
 *
 *  Returns the result of interpreting leading characters in <i>str</i> as an
 *  integer base <i>base</i> (between 2 and 36). Extraneous characters past the
 *  end of a valid number are ignored. If there is not a valid number at the
 *  start of <i>str</i>, <code>0</code> is returned. This method never raises an
 *  exception when <i>base</i> is valid.
 *
 *     "12345".to_i             #=> 12345
 *     "99 red balloons".to_i   #=> 99
 *     "0a".to_i                #=> 0
 *     "0a".to_i(16)            #=> 10
 *     "hello".to_i             #=> 0
 *     "1100101".to_i(2)        #=> 101
 *     "1100101".to_i(8)        #=> 294977
 *     "1100101".to_i(10)       #=> 1100101
 *     "1100101".to_i(16)       #=> 17826049
 */

static VALUE
rb_str_to_i(int argc, VALUE *argv, VALUE str)
{
    int base;

    if (argc == 0) base = 10;
    else {
	VALUE b;

	rb_scan_args(argc, argv, "01", &b);
	base = NUM2INT(b);
    }
    if (base < 0) {
	rb_raise(rb_eArgError, "invalid radix %d", base);
    }
    return rb_str_to_inum(str, base, FALSE);
}


/*
 *  call-seq:
 *     str.to_f   -> float
 *
 *  Returns the result of interpreting leading characters in <i>str</i> as a
 *  floating point number. Extraneous characters past the end of a valid number
 *  are ignored. If there is not a valid number at the start of <i>str</i>,
 *  <code>0.0</code> is returned. This method never raises an exception.
 *
 *     "123.45e1".to_f        #=> 1234.5
 *     "45.67 degrees".to_f   #=> 45.67
 *     "thx1138".to_f         #=> 0.0
 */

static VALUE
rb_str_to_f(VALUE str)
{
    return DBL2NUM(rb_str_to_dbl(str, FALSE));
}


/*
 *  call-seq:
 *     str.to_s     -> str
 *     str.to_str   -> str
 *
 *  Returns +self+.
 *
 *  If called on a subclass of String, converts the receiver to a String object.
 */

static VALUE
rb_str_to_s(VALUE str)
{
    if (rb_obj_class(str) != rb_cString) {
	return str_duplicate(rb_cString, str);
    }
    return str;
}

#if 0
static void
str_cat_char(VALUE str, unsigned int c, rb_encoding *enc)
{
    char s[RUBY_MAX_CHAR_LEN];
    int n = rb_enc_codelen(c, enc);

    rb_enc_mbcput(c, s, enc);
    rb_enc_str_buf_cat(str, s, n, enc);
}
#endif

#define CHAR_ESC_LEN 13 /* sizeof(\x{ hex of 32bit unsigned int } \0) */

int
rb_str_buf_cat_escaped_char(VALUE result, unsigned int c, int unicode_p)
{
    char buf[CHAR_ESC_LEN + 1];
    int l;

#if SIZEOF_INT > 4
    c &= 0xffffffff;
#endif
    if (unicode_p) {
	if (c < 0x7F && ISPRINT(c)) {
	    snprintf(buf, CHAR_ESC_LEN, "%c", c);
	}
	else if (c < 0x10000) {
	    snprintf(buf, CHAR_ESC_LEN, "\\u%04X", c);
	}
	else {
	    snprintf(buf, CHAR_ESC_LEN, "\\u{%X}", c);
	}
    }
    else {
	if (c < 0x100) {
	    snprintf(buf, CHAR_ESC_LEN, "\\x%02X", c);
	}
	else {
	    snprintf(buf, CHAR_ESC_LEN, "\\x{%X}", c);
	}
    }
    l = (int)strlen(buf);	/* CHAR_ESC_LEN cannot exceed INT_MAX */
    rb_str_buf_cat(result, buf, l);
    return l;
}

/*
 * call-seq:
 *   str.inspect   -> string
 *
 * Returns a printable version of _str_, surrounded by quote marks,
 * with special characters escaped.
 *
 *    str = "hello"
 *    str[3] = "\b"
 *    str.inspect       #=> "\"hel\\bo\""
 */

VALUE
rb_str_inspect(VALUE str)
{
    int encidx = ENCODING_GET(str);
    rb_encoding *enc = rb_enc_from_index(encidx), *actenc;
    const char *p, *pend, *prev;
    char buf[CHAR_ESC_LEN + 1];
    VALUE result = rb_str_buf_new(0);
    rb_encoding *resenc = rb_default_internal_encoding();
    int unicode_p = rb_enc_unicode_p(enc);
    int asciicompat = rb_enc_asciicompat(enc);

    if (resenc == NULL) resenc = rb_default_external_encoding();
    if (!rb_enc_asciicompat(resenc)) resenc = rb_usascii_encoding();
    rb_enc_associate(result, resenc);
    str_buf_cat2(result, "\"");

    p = RSTRING_PTR(str); pend = RSTRING_END(str);
    prev = p;
    actenc = get_actual_encoding(encidx, str);
    if (actenc != enc) {
	enc = actenc;
	if (unicode_p) unicode_p = rb_enc_unicode_p(enc);
    }
    while (p < pend) {
	unsigned int c, cc;
	int n;

        n = rb_enc_precise_mbclen(p, pend, enc);
        if (!MBCLEN_CHARFOUND_P(n)) {
	    if (p > prev) str_buf_cat(result, prev, p - prev);
            n = rb_enc_mbminlen(enc);
            if (pend < p + n)
                n = (int)(pend - p);
            while (n--) {
                snprintf(buf, CHAR_ESC_LEN, "\\x%02X", *p & 0377);
                str_buf_cat(result, buf, strlen(buf));
                prev = ++p;
            }
	    continue;
	}
        n = MBCLEN_CHARFOUND_LEN(n);
	c = rb_enc_mbc_to_codepoint(p, pend, enc);
	p += n;
	if ((asciicompat || unicode_p) &&
	  (c == '"'|| c == '\\' ||
	    (c == '#' &&
             p < pend &&
             MBCLEN_CHARFOUND_P(rb_enc_precise_mbclen(p,pend,enc)) &&
             (cc = rb_enc_codepoint(p,pend,enc),
              (cc == '$' || cc == '@' || cc == '{'))))) {
	    if (p - n > prev) str_buf_cat(result, prev, p - n - prev);
	    str_buf_cat2(result, "\\");
	    if (asciicompat || enc == resenc) {
		prev = p - n;
		continue;
	    }
	}
	switch (c) {
	  case '\n': cc = 'n'; break;
	  case '\r': cc = 'r'; break;
	  case '\t': cc = 't'; break;
	  case '\f': cc = 'f'; break;
	  case '\013': cc = 'v'; break;
	  case '\010': cc = 'b'; break;
	  case '\007': cc = 'a'; break;
	  case 033: cc = 'e'; break;
	  default: cc = 0; break;
	}
	if (cc) {
	    if (p - n > prev) str_buf_cat(result, prev, p - n - prev);
	    buf[0] = '\\';
	    buf[1] = (char)cc;
	    str_buf_cat(result, buf, 2);
	    prev = p;
	    continue;
	}
	if ((enc == resenc && rb_enc_isprint(c, enc)) ||
	    (asciicompat && rb_enc_isascii(c, enc) && ISPRINT(c))) {
	    continue;
	}
	else {
	    if (p - n > prev) str_buf_cat(result, prev, p - n - prev);
	    rb_str_buf_cat_escaped_char(result, c, unicode_p);
	    prev = p;
	    continue;
	}
    }
    if (p > prev) str_buf_cat(result, prev, p - prev);
    str_buf_cat2(result, "\"");

    OBJ_INFECT_RAW(result, str);
    return result;
}

#define IS_EVSTR(p,e) ((p) < (e) && (*(p) == '$' || *(p) == '@' || *(p) == '{'))

/*
 *  call-seq:
 *     str.dump   -> new_str
 *
 *  Produces a version of +str+ with all non-printing characters replaced by
 *  <code>\nnn</code> notation and all special characters escaped.
 *
 *    "hello \n ''".dump  #=> "\"hello \\n ''\"
 */

VALUE
rb_str_dump(VALUE str)
{
    rb_encoding *enc = rb_enc_get(str);
    long len;
    const char *p, *pend;
    char *q, *qend;
    VALUE result;
    int u8 = (enc == rb_utf8_encoding());

    len = 2;			/* "" */
    p = RSTRING_PTR(str); pend = p + RSTRING_LEN(str);
    while (p < pend) {
	unsigned char c = *p++;
	switch (c) {
	  case '"':  case '\\':
	  case '\n': case '\r':
	  case '\t': case '\f':
	  case '\013': case '\010': case '\007': case '\033':
	    len += 2;
	    break;

	  case '#':
	    len += IS_EVSTR(p, pend) ? 2 : 1;
	    break;

	  default:
	    if (ISPRINT(c)) {
		len++;
	    }
	    else {
		if (u8 && c > 0x7F) {	/* \u{NN} */
		    int n = rb_enc_precise_mbclen(p-1, pend, enc);
		    if (MBCLEN_CHARFOUND_P(n)) {
			unsigned int cc = rb_enc_mbc_to_codepoint(p-1, pend, enc);
			while (cc >>= 4) len++;
			len += 5;
			p += MBCLEN_CHARFOUND_LEN(n)-1;
			break;
		    }
		}
		len += 4;	/* \xNN */
	    }
	    break;
	}
    }
    if (!rb_enc_asciicompat(enc)) {
	len += 19;		/* ".force_encoding('')" */
	len += strlen(enc->name);
    }

    result = rb_str_new_with_class(str, 0, len);
    p = RSTRING_PTR(str); pend = p + RSTRING_LEN(str);
    q = RSTRING_PTR(result); qend = q + len + 1;

    *q++ = '"';
    while (p < pend) {
	unsigned char c = *p++;

	if (c == '"' || c == '\\') {
	    *q++ = '\\';
	    *q++ = c;
	}
	else if (c == '#') {
	    if (IS_EVSTR(p, pend)) *q++ = '\\';
	    *q++ = '#';
	}
	else if (c == '\n') {
	    *q++ = '\\';
	    *q++ = 'n';
	}
	else if (c == '\r') {
	    *q++ = '\\';
	    *q++ = 'r';
	}
	else if (c == '\t') {
	    *q++ = '\\';
	    *q++ = 't';
	}
	else if (c == '\f') {
	    *q++ = '\\';
	    *q++ = 'f';
	}
	else if (c == '\013') {
	    *q++ = '\\';
	    *q++ = 'v';
	}
	else if (c == '\010') {
	    *q++ = '\\';
	    *q++ = 'b';
	}
	else if (c == '\007') {
	    *q++ = '\\';
	    *q++ = 'a';
	}
	else if (c == '\033') {
	    *q++ = '\\';
	    *q++ = 'e';
	}
	else if (ISPRINT(c)) {
	    *q++ = c;
	}
	else {
	    *q++ = '\\';
	    if (u8) {
		int n = rb_enc_precise_mbclen(p-1, pend, enc) - 1;
		if (MBCLEN_CHARFOUND_P(n)) {
		    int cc = rb_enc_mbc_to_codepoint(p-1, pend, enc);
		    p += n;
		    snprintf(q, qend-q, "u{%x}", cc);
		    q += strlen(q);
		    continue;
		}
	    }
	    snprintf(q, qend-q, "x%02X", c);
	    q += 3;
	}
    }
    *q++ = '"';
    *q = '\0';
    if (!rb_enc_asciicompat(enc)) {
	snprintf(q, qend-q, ".force_encoding(\"%s\")", enc->name);
	enc = rb_ascii8bit_encoding();
    }
    OBJ_INFECT_RAW(result, str);
    /* result from dump is ASCII */
    rb_enc_associate(result, enc);
    ENC_CODERANGE_SET(result, ENC_CODERANGE_7BIT);
    return result;
}


static void
rb_str_check_dummy_enc(rb_encoding *enc)
{
    if (rb_enc_dummy_p(enc)) {
	rb_raise(rb_eEncCompatError, "incompatible encoding with this operation: %s",
		 rb_enc_name(enc));
    }
}

/*
 *  call-seq:
 *     str.upcase!   -> str or nil
 *
 *  Upcases the contents of <i>str</i>, returning <code>nil</code> if no changes
 *  were made.
 *  Note: case replacement is effective only in ASCII region.
 */

static VALUE
rb_str_upcase_bang(VALUE str)
{
    rb_encoding *enc;
    char *s, *send;
    int modify = 0;
    int n;

    str_modify_keep_cr(str);
    enc = STR_ENC_GET(str);
    rb_str_check_dummy_enc(enc);
    s = RSTRING_PTR(str); send = RSTRING_END(str);
    if (single_byte_optimizable(str)) {
	while (s < send) {
	    unsigned int c = *(unsigned char*)s;

	    if (rb_enc_isascii(c, enc) && 'a' <= c && c <= 'z') {
		*s = 'A' + (c - 'a');
		modify = 1;
	    }
	    s++;
	}
    }
    else {
	int ascompat = rb_enc_asciicompat(enc);

	while (s < send) {
	    unsigned int c;

	    if (ascompat && (c = *(unsigned char*)s) < 0x80) {
		if (rb_enc_isascii(c, enc) && 'a' <= c && c <= 'z') {
		    *s = 'A' + (c - 'a');
		    modify = 1;
		}
		s++;
	    }
	    else {
		c = rb_enc_codepoint_len(s, send, &n, enc);
		if (rb_enc_islower(c, enc)) {
		    /* assuming toupper returns codepoint with same size */
		    rb_enc_mbcput(rb_enc_toupper(c, enc), s, enc);
		    modify = 1;
		}
		s += n;
	    }
	}
    }

    if (modify) return str;
    return Qnil;
}


/*
 *  call-seq:
 *     str.upcase   -> new_str
 *
 *  Returns a copy of <i>str</i> with all lowercase letters replaced with their
 *  uppercase counterparts. The operation is locale insensitive---only
 *  characters ``a'' to ``z'' are affected.
 *  Note: case replacement is effective only in ASCII region.
 *
 *     "hEllO".upcase   #=> "HELLO"
 */

static VALUE
rb_str_upcase(VALUE str)
{
    str = rb_str_dup(str);
    rb_str_upcase_bang(str);
    return str;
}


/*
 *  call-seq:
 *     str.downcase!   -> str or nil
 *
 *  Downcases the contents of <i>str</i>, returning <code>nil</code> if no
 *  changes were made.
 *  Note: case replacement is effective only in ASCII region.
 */

static VALUE
rb_str_downcase_bang(VALUE str)
{
    rb_encoding *enc;
    char *s, *send;
    int modify = 0;

    str_modify_keep_cr(str);
    enc = STR_ENC_GET(str);
    rb_str_check_dummy_enc(enc);
    s = RSTRING_PTR(str); send = RSTRING_END(str);
    if (single_byte_optimizable(str)) {
	while (s < send) {
	    unsigned int c = *(unsigned char*)s;

	    if (rb_enc_isascii(c, enc) && 'A' <= c && c <= 'Z') {
		*s = 'a' + (c - 'A');
		modify = 1;
	    }
	    s++;
	}
    }
    else {
	int ascompat = rb_enc_asciicompat(enc);

	while (s < send) {
	    unsigned int c;
	    int n;

	    if (ascompat && (c = *(unsigned char*)s) < 0x80) {
		if (rb_enc_isascii(c, enc) && 'A' <= c && c <= 'Z') {
		    *s = 'a' + (c - 'A');
		    modify = 1;
		}
		s++;
	    }
	    else {
		c = rb_enc_codepoint_len(s, send, &n, enc);
		if (rb_enc_isupper(c, enc)) {
		    /* assuming toupper returns codepoint with same size */
		    rb_enc_mbcput(rb_enc_tolower(c, enc), s, enc);
		    modify = 1;
		}
		s += n;
	    }
	}
    }

    if (modify) return str;
    return Qnil;
}


/*
 *  call-seq:
 *     str.downcase   -> new_str
 *
 *  Returns a copy of <i>str</i> with all uppercase letters replaced with their
 *  lowercase counterparts. The operation is locale insensitive---only
 *  characters ``A'' to ``Z'' are affected.
 *  Note: case replacement is effective only in ASCII region.
 *
 *     "hEllO".downcase   #=> "hello"
 */

static VALUE
rb_str_downcase(VALUE str)
{
    str = rb_str_dup(str);
    rb_str_downcase_bang(str);
    return str;
}


/*
 *  call-seq:
 *     str.capitalize!   -> str or nil
 *
 *  Modifies <i>str</i> by converting the first character to uppercase and the
 *  remainder to lowercase. Returns <code>nil</code> if no changes are made.
 *  Note: case conversion is effective only in ASCII region.
 *
 *     a = "hello"
 *     a.capitalize!   #=> "Hello"
 *     a               #=> "Hello"
 *     a.capitalize!   #=> nil
 */

static VALUE
rb_str_capitalize_bang(VALUE str)
{
    rb_encoding *enc;
    char *s, *send;
    int modify = 0;
    unsigned int c;
    int n;

    str_modify_keep_cr(str);
    enc = STR_ENC_GET(str);
    rb_str_check_dummy_enc(enc);
    if (RSTRING_LEN(str) == 0 || !RSTRING_PTR(str)) return Qnil;
    s = RSTRING_PTR(str); send = RSTRING_END(str);

    c = rb_enc_codepoint_len(s, send, &n, enc);
    if (rb_enc_islower(c, enc)) {
	rb_enc_mbcput(rb_enc_toupper(c, enc), s, enc);
	modify = 1;
    }
    s += n;
    while (s < send) {
	c = rb_enc_codepoint_len(s, send, &n, enc);
	if (rb_enc_isupper(c, enc)) {
	    rb_enc_mbcput(rb_enc_tolower(c, enc), s, enc);
	    modify = 1;
	}
	s += n;
    }

    if (modify) return str;
    return Qnil;
}


/*
 *  call-seq:
 *     str.capitalize   -> new_str
 *
 *  Returns a copy of <i>str</i> with the first character converted to uppercase
 *  and the remainder to lowercase.
 *  Note: case conversion is effective only in ASCII region.
 *
 *     "hello".capitalize    #=> "Hello"
 *     "HELLO".capitalize    #=> "Hello"
 *     "123ABC".capitalize   #=> "123abc"
 */

static VALUE
rb_str_capitalize(VALUE str)
{
    str = rb_str_dup(str);
    rb_str_capitalize_bang(str);
    return str;
}


/*
 *  call-seq:
 *     str.swapcase!   -> str or nil
 *
 *  Equivalent to <code>String#swapcase</code>, but modifies the receiver in
 *  place, returning <i>str</i>, or <code>nil</code> if no changes were made.
 *  Note: case conversion is effective only in ASCII region.
 */

static VALUE
rb_str_swapcase_bang(VALUE str)
{
    rb_encoding *enc;
    char *s, *send;
    int modify = 0;
    int n;

    str_modify_keep_cr(str);
    enc = STR_ENC_GET(str);
    rb_str_check_dummy_enc(enc);
    s = RSTRING_PTR(str); send = RSTRING_END(str);
    while (s < send) {
	unsigned int c = rb_enc_codepoint_len(s, send, &n, enc);

	if (rb_enc_isupper(c, enc)) {
	    /* assuming toupper returns codepoint with same size */
	    rb_enc_mbcput(rb_enc_tolower(c, enc), s, enc);
	    modify = 1;
	}
	else if (rb_enc_islower(c, enc)) {
	    /* assuming tolower returns codepoint with same size */
	    rb_enc_mbcput(rb_enc_toupper(c, enc), s, enc);
	    modify = 1;
	}
	s += n;
    }

    if (modify) return str;
    return Qnil;
}


/*
 *  call-seq:
 *     str.swapcase   -> new_str
 *
 *  Returns a copy of <i>str</i> with uppercase alphabetic characters converted
 *  to lowercase and lowercase characters converted to uppercase.
 *  Note: case conversion is effective only in ASCII region.
 *
 *     "Hello".swapcase          #=> "hELLO"
 *     "cYbEr_PuNk11".swapcase   #=> "CyBeR_pUnK11"
 */

static VALUE
rb_str_swapcase(VALUE str)
{
    str = rb_str_dup(str);
    rb_str_swapcase_bang(str);
    return str;
}

typedef unsigned char *USTR;

struct tr {
    int gen;
    unsigned int now, max;
    char *p, *pend;
};

static unsigned int
trnext(struct tr *t, rb_encoding *enc)
{
    int n;

    for (;;) {
	if (!t->gen) {
nextpart:
	    if (t->p == t->pend) return -1;
	    if (rb_enc_ascget(t->p, t->pend, &n, enc) == '\\' && t->p + n < t->pend) {
		t->p += n;
	    }
	    t->now = rb_enc_codepoint_len(t->p, t->pend, &n, enc);
	    t->p += n;
	    if (rb_enc_ascget(t->p, t->pend, &n, enc) == '-' && t->p + n < t->pend) {
		t->p += n;
		if (t->p < t->pend) {
		    unsigned int c = rb_enc_codepoint_len(t->p, t->pend, &n, enc);
		    t->p += n;
		    if (t->now > c) {
			if (t->now < 0x80 && c < 0x80) {
			    rb_raise(rb_eArgError,
				     "invalid range \"%c-%c\" in string transliteration",
				     t->now, c);
			}
			else {
			    rb_raise(rb_eArgError, "invalid range in string transliteration");
			}
			continue; /* not reached */
		    }
		    t->gen = 1;
		    t->max = c;
		}
	    }
	    return t->now;
	}
	else {
	    while (ONIGENC_CODE_TO_MBCLEN(enc, ++t->now) <= 0) {
		if (t->now == t->max) {
		    t->gen = 0;
		    goto nextpart;
		}
	    }
	    if (t->now < t->max) {
		return t->now;
	    }
	    else {
		t->gen = 0;
		return t->max;
	    }
	}
    }
}

static VALUE rb_str_delete_bang(int,VALUE*,VALUE);

static VALUE
tr_trans(VALUE str, VALUE src, VALUE repl, int sflag)
{
    const unsigned int errc = -1;
    unsigned int trans[256];
    rb_encoding *enc, *e1, *e2;
    struct tr trsrc, trrepl;
    int cflag = 0;
    unsigned int c, c0, last = 0;
    int modify = 0, i, l;
    char *s, *send;
    VALUE hash = 0;
    int singlebyte = single_byte_optimizable(str);
    int cr;

#define CHECK_IF_ASCII(c) \
    (void)((cr == ENC_CODERANGE_7BIT && !rb_isascii(c)) ? \
	   (cr = ENC_CODERANGE_VALID) : 0)

    StringValue(src);
    StringValue(repl);
    if (RSTRING_LEN(str) == 0 || !RSTRING_PTR(str)) return Qnil;
    if (RSTRING_LEN(repl) == 0) {
	return rb_str_delete_bang(1, &src, str);
    }

    cr = ENC_CODERANGE(str);
    e1 = rb_enc_check(str, src);
    e2 = rb_enc_check(str, repl);
    if (e1 == e2) {
	enc = e1;
    }
    else {
	enc = rb_enc_check(src, repl);
    }
    trsrc.p = RSTRING_PTR(src); trsrc.pend = trsrc.p + RSTRING_LEN(src);
    if (RSTRING_LEN(src) > 1 &&
	rb_enc_ascget(trsrc.p, trsrc.pend, &l, enc) == '^' &&
	trsrc.p + l < trsrc.pend) {
	cflag = 1;
	trsrc.p += l;
    }
    trrepl.p = RSTRING_PTR(repl);
    trrepl.pend = trrepl.p + RSTRING_LEN(repl);
    trsrc.gen = trrepl.gen = 0;
    trsrc.now = trrepl.now = 0;
    trsrc.max = trrepl.max = 0;

    if (cflag) {
	for (i=0; i<256; i++) {
	    trans[i] = 1;
	}
	while ((c = trnext(&trsrc, enc)) != errc) {
	    if (c < 256) {
		trans[c] = errc;
	    }
	    else {
		if (!hash) hash = rb_hash_new();
		rb_hash_aset(hash, UINT2NUM(c), Qtrue);
	    }
	}
	while ((c = trnext(&trrepl, enc)) != errc)
	    /* retrieve last replacer */;
	last = trrepl.now;
	for (i=0; i<256; i++) {
	    if (trans[i] != errc) {
		trans[i] = last;
	    }
	}
    }
    else {
	unsigned int r;

	for (i=0; i<256; i++) {
	    trans[i] = errc;
	}
	while ((c = trnext(&trsrc, enc)) != errc) {
	    r = trnext(&trrepl, enc);
	    if (r == errc) r = trrepl.now;
	    if (c < 256) {
		trans[c] = r;
		if (rb_enc_codelen(r, enc) != 1) singlebyte = 0;
	    }
	    else {
		if (!hash) hash = rb_hash_new();
		rb_hash_aset(hash, UINT2NUM(c), UINT2NUM(r));
	    }
	}
    }

    if (cr == ENC_CODERANGE_VALID)
	cr = ENC_CODERANGE_7BIT;
    str_modify_keep_cr(str);
    s = RSTRING_PTR(str); send = RSTRING_END(str);
    if (sflag) {
	int clen, tlen;
	long offset, max = RSTRING_LEN(str);
	unsigned int save = -1;
	char *buf = ALLOC_N(char, max), *t = buf;

	while (s < send) {
	    int may_modify = 0;

	    c0 = c = rb_enc_codepoint_len(s, send, &clen, e1);
	    tlen = enc == e1 ? clen : rb_enc_codelen(c, enc);

	    s += clen;
	    if (c < 256) {
		c = trans[c];
	    }
	    else if (hash) {
		VALUE tmp = rb_hash_lookup(hash, UINT2NUM(c));
		if (NIL_P(tmp)) {
		    if (cflag) c = last;
		    else c = errc;
		}
		else if (cflag) c = errc;
		else c = NUM2INT(tmp);
	    }
	    else {
		c = errc;
	    }
	    if (c != (unsigned int)-1) {
		if (save == c) {
		    CHECK_IF_ASCII(c);
		    continue;
		}
		save = c;
		tlen = rb_enc_codelen(c, enc);
		modify = 1;
	    }
	    else {
		save = -1;
		c = c0;
		if (enc != e1) may_modify = 1;
	    }
	    while (t - buf + tlen >= max) {
		offset = t - buf;
		max *= 2;
		REALLOC_N(buf, char, max);
		t = buf + offset;
	    }
	    rb_enc_mbcput(c, t, enc);
	    if (may_modify && memcmp(s, t, tlen) != 0) {
		modify = 1;
	    }
	    CHECK_IF_ASCII(c);
	    t += tlen;
	}
	if (!STR_EMBED_P(str)) {
	    ruby_sized_xfree(STR_HEAP_PTR(str), STR_HEAP_SIZE(str));
	}
	TERM_FILL(t, rb_enc_mbminlen(enc));
	RSTRING(str)->as.heap.ptr = buf;
	RSTRING(str)->as.heap.len = t - buf;
	STR_SET_NOEMBED(str);
	RSTRING(str)->as.heap.aux.capa = max;
    }
    else if (rb_enc_mbmaxlen(enc) == 1 || (singlebyte && !hash)) {
	while (s < send) {
	    c = (unsigned char)*s;
	    if (trans[c] != errc) {
		if (!cflag) {
		    c = trans[c];
		    *s = c;
		    modify = 1;
		}
		else {
		    *s = last;
		    modify = 1;
		}
	    }
	    CHECK_IF_ASCII(c);
	    s++;
	}
    }
    else {
	int clen, tlen, max = (int)(RSTRING_LEN(str) * 1.2);
	long offset;
	char *buf = ALLOC_N(char, max), *t = buf;

	while (s < send) {
	    int may_modify = 0;
	    c0 = c = rb_enc_codepoint_len(s, send, &clen, e1);
	    tlen = enc == e1 ? clen : rb_enc_codelen(c, enc);

	    if (c < 256) {
		c = trans[c];
	    }
	    else if (hash) {
		VALUE tmp = rb_hash_lookup(hash, UINT2NUM(c));
		if (NIL_P(tmp)) {
		    if (cflag) c = last;
		    else c = errc;
		}
		else if (cflag) c = errc;
		else c = NUM2INT(tmp);
	    }
	    else {
		c = cflag ? last : errc;
	    }
	    if (c != errc) {
		tlen = rb_enc_codelen(c, enc);
		modify = 1;
	    }
	    else {
		c = c0;
		if (enc != e1) may_modify = 1;
	    }
	    while (t - buf + tlen >= max) {
		offset = t - buf;
		max *= 2;
		REALLOC_N(buf, char, max);
		t = buf + offset;
	    }
	    if (s != t) {
		rb_enc_mbcput(c, t, enc);
		if (may_modify && memcmp(s, t, tlen) != 0) {
		    modify = 1;
		}
	    }
	    CHECK_IF_ASCII(c);
	    s += clen;
	    t += tlen;
	}
	if (!STR_EMBED_P(str)) {
	    ruby_sized_xfree(STR_HEAP_PTR(str), STR_HEAP_SIZE(str));
	}
	TERM_FILL(t, rb_enc_mbminlen(enc));
	RSTRING(str)->as.heap.ptr = buf;
	RSTRING(str)->as.heap.len = t - buf;
	STR_SET_NOEMBED(str);
	RSTRING(str)->as.heap.aux.capa = max;
    }

    if (modify) {
	if (cr != ENC_CODERANGE_BROKEN)
	    ENC_CODERANGE_SET(str, cr);
	rb_enc_associate(str, enc);
	return str;
    }
    return Qnil;
}


/*
 *  call-seq:
 *     str.tr!(from_str, to_str)   -> str or nil
 *
 *  Translates <i>str</i> in place, using the same rules as
 *  <code>String#tr</code>. Returns <i>str</i>, or <code>nil</code> if no
 *  changes were made.
 */

static VALUE
rb_str_tr_bang(VALUE str, VALUE src, VALUE repl)
{
    return tr_trans(str, src, repl, 0);
}


/*
 *  call-seq:
 *     str.tr(from_str, to_str)   => new_str
 *
 *  Returns a copy of +str+ with the characters in +from_str+ replaced by the
 *  corresponding characters in +to_str+.  If +to_str+ is shorter than
 *  +from_str+, it is padded with its last character in order to maintain the
 *  correspondence.
 *
 *     "hello".tr('el', 'ip')      #=> "hippo"
 *     "hello".tr('aeiou', '*')    #=> "h*ll*"
 *     "hello".tr('aeiou', 'AA*')  #=> "hAll*"
 *
 *  Both strings may use the <code>c1-c2</code> notation to denote ranges of
 *  characters, and +from_str+ may start with a <code>^</code>, which denotes
 *  all characters except those listed.
 *
 *     "hello".tr('a-y', 'b-z')    #=> "ifmmp"
 *     "hello".tr('^aeiou', '*')   #=> "*e**o"
 *
 *  The backslash character <code>\</code> can be used to escape
 *  <code>^</code> or <code>-</code> and is otherwise ignored unless it
 *  appears at the end of a range or the end of the +from_str+ or +to_str+:
 *
 *     "hello^world".tr("\\^aeiou", "*") #=> "h*ll**w*rld"
 *     "hello-world".tr("a\\-eo", "*")   #=> "h*ll**w*rld"
 *
 *     "hello\r\nworld".tr("\r", "")   #=> "hello\nworld"
 *     "hello\r\nworld".tr("\\r", "")  #=> "hello\r\nwold"
 *     "hello\r\nworld".tr("\\\r", "") #=> "hello\nworld"
 *
 *     "X['\\b']".tr("X\\", "")   #=> "['b']"
 *     "X['\\b']".tr("X-\\]", "") #=> "'b'"
 */

static VALUE
rb_str_tr(VALUE str, VALUE src, VALUE repl)
{
    str = rb_str_dup(str);
    tr_trans(str, src, repl, 0);
    return str;
}

#define TR_TABLE_SIZE 257
static void
tr_setup_table(VALUE str, char stable[TR_TABLE_SIZE], int first,
	       VALUE *tablep, VALUE *ctablep, rb_encoding *enc)
{
    const unsigned int errc = -1;
    char buf[256];
    struct tr tr;
    unsigned int c;
    VALUE table = 0, ptable = 0;
    int i, l, cflag = 0;

    tr.p = RSTRING_PTR(str); tr.pend = tr.p + RSTRING_LEN(str);
    tr.gen = tr.now = tr.max = 0;

    if (RSTRING_LEN(str) > 1 && rb_enc_ascget(tr.p, tr.pend, &l, enc) == '^') {
	cflag = 1;
	tr.p += l;
    }
    if (first) {
	for (i=0; i<256; i++) {
	    stable[i] = 1;
	}
	stable[256] = cflag;
    }
    else if (stable[256] && !cflag) {
	stable[256] = 0;
    }
    for (i=0; i<256; i++) {
	buf[i] = cflag;
    }

    while ((c = trnext(&tr, enc)) != errc) {
	if (c < 256) {
	    buf[c & 0xff] = !cflag;
	}
	else {
	    VALUE key = UINT2NUM(c);

	    if (!table && (first || *tablep || stable[256])) {
		if (cflag) {
		    ptable = *ctablep;
		    table = ptable ? ptable : rb_hash_new();
		    *ctablep = table;
		}
		else {
		    table = rb_hash_new();
		    ptable = *tablep;
		    *tablep = table;
		}
	    }
	    if (table && (!ptable || (cflag ^ !NIL_P(rb_hash_aref(ptable, key))))) {
		rb_hash_aset(table, key, Qtrue);
	    }
	}
    }
    for (i=0; i<256; i++) {
	stable[i] = stable[i] && buf[i];
    }
    if (!table && !cflag) {
	*tablep = 0;
    }
}


static int
tr_find(unsigned int c, const char table[TR_TABLE_SIZE], VALUE del, VALUE nodel)
{
    if (c < 256) {
	return table[c] != 0;
    }
    else {
	VALUE v = UINT2NUM(c);

	if (del) {
	    if (!NIL_P(rb_hash_lookup(del, v)) &&
		    (!nodel || NIL_P(rb_hash_lookup(nodel, v)))) {
		return TRUE;
	    }
	}
	else if (nodel && !NIL_P(rb_hash_lookup(nodel, v))) {
	    return FALSE;
	}
	return table[256] ? TRUE : FALSE;
    }
}

/*
 *  call-seq:
 *     str.delete!([other_str]+)   -> str or nil
 *
 *  Performs a <code>delete</code> operation in place, returning <i>str</i>, or
 *  <code>nil</code> if <i>str</i> was not modified.
 */

static VALUE
rb_str_delete_bang(int argc, VALUE *argv, VALUE str)
{
    char squeez[TR_TABLE_SIZE];
    rb_encoding *enc = 0;
    char *s, *send, *t;
    VALUE del = 0, nodel = 0;
    int modify = 0;
    int i, ascompat, cr;

    if (RSTRING_LEN(str) == 0 || !RSTRING_PTR(str)) return Qnil;
    rb_check_arity(argc, 1, UNLIMITED_ARGUMENTS);
    for (i=0; i<argc; i++) {
	VALUE s = argv[i];

	StringValue(s);
	enc = rb_enc_check(str, s);
	tr_setup_table(s, squeez, i==0, &del, &nodel, enc);
    }

    str_modify_keep_cr(str);
    ascompat = rb_enc_asciicompat(enc);
    s = t = RSTRING_PTR(str);
    send = RSTRING_END(str);
    cr = ascompat ? ENC_CODERANGE_7BIT : ENC_CODERANGE_VALID;
    while (s < send) {
	unsigned int c;
	int clen;

	if (ascompat && (c = *(unsigned char*)s) < 0x80) {
	    if (squeez[c]) {
		modify = 1;
	    }
	    else {
		if (t != s) *t = c;
		t++;
	    }
	    s++;
	}
	else {
	    c = rb_enc_codepoint_len(s, send, &clen, enc);

	    if (tr_find(c, squeez, del, nodel)) {
		modify = 1;
	    }
	    else {
		if (t != s) rb_enc_mbcput(c, t, enc);
		t += clen;
		if (cr == ENC_CODERANGE_7BIT) cr = ENC_CODERANGE_VALID;
	    }
	    s += clen;
	}
    }
    TERM_FILL(t, TERM_LEN(str));
    STR_SET_LEN(str, t - RSTRING_PTR(str));
    ENC_CODERANGE_SET(str, cr);

    if (modify) return str;
    return Qnil;
}


/*
 *  call-seq:
 *     str.delete([other_str]+)   -> new_str
 *
 *  Returns a copy of <i>str</i> with all characters in the intersection of its
 *  arguments deleted. Uses the same rules for building the set of characters as
 *  <code>String#count</code>.
 *
 *     "hello".delete "l","lo"        #=> "heo"
 *     "hello".delete "lo"            #=> "he"
 *     "hello".delete "aeiou", "^e"   #=> "hell"
 *     "hello".delete "ej-m"          #=> "ho"
 */

static VALUE
rb_str_delete(int argc, VALUE *argv, VALUE str)
{
    str = rb_str_dup(str);
    rb_str_delete_bang(argc, argv, str);
    return str;
}


/*
 *  call-seq:
 *     str.squeeze!([other_str]*)   -> str or nil
 *
 *  Squeezes <i>str</i> in place, returning either <i>str</i>, or
 *  <code>nil</code> if no changes were made.
 */

static VALUE
rb_str_squeeze_bang(int argc, VALUE *argv, VALUE str)
{
    char squeez[TR_TABLE_SIZE];
    rb_encoding *enc = 0;
    VALUE del = 0, nodel = 0;
    char *s, *send, *t;
    int i, modify = 0;
    int ascompat, singlebyte = single_byte_optimizable(str);
    unsigned int save;

    if (argc == 0) {
	enc = STR_ENC_GET(str);
    }
    else {
	for (i=0; i<argc; i++) {
	    VALUE s = argv[i];

	    StringValue(s);
	    enc = rb_enc_check(str, s);
	    if (singlebyte && !single_byte_optimizable(s))
		singlebyte = 0;
	    tr_setup_table(s, squeez, i==0, &del, &nodel, enc);
	}
    }

    str_modify_keep_cr(str);
    s = t = RSTRING_PTR(str);
    if (!s || RSTRING_LEN(str) == 0) return Qnil;
    send = RSTRING_END(str);
    save = -1;
    ascompat = rb_enc_asciicompat(enc);

    if (singlebyte) {
        while (s < send) {
	    unsigned int c = *(unsigned char*)s++;
	    if (c != save || (argc > 0 && !squeez[c])) {
	        *t++ = save = c;
	    }
	}
    }
    else {
	while (s < send) {
	    unsigned int c;
	    int clen;

	    if (ascompat && (c = *(unsigned char*)s) < 0x80) {
		if (c != save || (argc > 0 && !squeez[c])) {
		    *t++ = save = c;
		}
		s++;
	    }
	    else {
		c = rb_enc_codepoint_len(s, send, &clen, enc);

		if (c != save || (argc > 0 && !tr_find(c, squeez, del, nodel))) {
		    if (t != s) rb_enc_mbcput(c, t, enc);
		    save = c;
		    t += clen;
		}
		s += clen;
	    }
	}
    }

    TERM_FILL(t, TERM_LEN(str));
    if (t - RSTRING_PTR(str) != RSTRING_LEN(str)) {
	STR_SET_LEN(str, t - RSTRING_PTR(str));
	modify = 1;
    }

    if (modify) return str;
    return Qnil;
}


/*
 *  call-seq:
 *     str.squeeze([other_str]*)    -> new_str
 *
 *  Builds a set of characters from the <i>other_str</i> parameter(s) using the
 *  procedure described for <code>String#count</code>. Returns a new string
 *  where runs of the same character that occur in this set are replaced by a
 *  single character. If no arguments are given, all runs of identical
 *  characters are replaced by a single character.
 *
 *     "yellow moon".squeeze                  #=> "yelow mon"
 *     "  now   is  the".squeeze(" ")         #=> " now is the"
 *     "putters shoot balls".squeeze("m-z")   #=> "puters shot balls"
 */

static VALUE
rb_str_squeeze(int argc, VALUE *argv, VALUE str)
{
    str = rb_str_dup(str);
    rb_str_squeeze_bang(argc, argv, str);
    return str;
}


/*
 *  call-seq:
 *     str.tr_s!(from_str, to_str)   -> str or nil
 *
 *  Performs <code>String#tr_s</code> processing on <i>str</i> in place,
 *  returning <i>str</i>, or <code>nil</code> if no changes were made.
 */

static VALUE
rb_str_tr_s_bang(VALUE str, VALUE src, VALUE repl)
{
    return tr_trans(str, src, repl, 1);
}


/*
 *  call-seq:
 *     str.tr_s(from_str, to_str)   -> new_str
 *
 *  Processes a copy of <i>str</i> as described under <code>String#tr</code>,
 *  then removes duplicate characters in regions that were affected by the
 *  translation.
 *
 *     "hello".tr_s('l', 'r')     #=> "hero"
 *     "hello".tr_s('el', '*')    #=> "h*o"
 *     "hello".tr_s('el', 'hx')   #=> "hhxo"
 */

static VALUE
rb_str_tr_s(VALUE str, VALUE src, VALUE repl)
{
    str = rb_str_dup(str);
    tr_trans(str, src, repl, 1);
    return str;
}


/*
 *  call-seq:
 *     str.count([other_str]+)   -> fixnum
 *
 *  Each +other_str+ parameter defines a set of characters to count.  The
 *  intersection of these sets defines the characters to count in +str+.  Any
 *  +other_str+ that starts with a caret <code>^</code> is negated.  The
 *  sequence <code>c1-c2</code> means all characters between c1 and c2.  The
 *  backslash character <code>\\</code> can be used to escape <code>^</code> or
 *  <code>-</code> and is otherwise ignored unless it appears at the end of a
 *  sequence or the end of a +other_str+.
 *
 *     a = "hello world"
 *     a.count "lo"                   #=> 5
 *     a.count "lo", "o"              #=> 2
 *     a.count "hello", "^l"          #=> 4
 *     a.count "ej-m"                 #=> 4
 *
 *     "hello^world".count "\\^aeiou" #=> 4
 *     "hello-world".count "a\\-eo"   #=> 4
 *
 *     c = "hello world\\r\\n"
 *     c.count "\\"                   #=> 2
 *     c.count "\\A"                  #=> 0
 *     c.count "X-\\w"                #=> 3
 */

static VALUE
rb_str_count(int argc, VALUE *argv, VALUE str)
{
    char table[TR_TABLE_SIZE];
    rb_encoding *enc = 0;
    VALUE del = 0, nodel = 0, tstr;
    char *s, *send;
    int i;
    int ascompat;

    rb_check_arity(argc, 1, UNLIMITED_ARGUMENTS);

    tstr = argv[0];
    StringValue(tstr);
    enc = rb_enc_check(str, tstr);
    if (argc == 1) {
	const char *ptstr;
	if (RSTRING_LEN(tstr) == 1 && rb_enc_asciicompat(enc) &&
	    (ptstr = RSTRING_PTR(tstr),
	     ONIGENC_IS_ALLOWED_REVERSE_MATCH(enc, (const unsigned char *)ptstr, (const unsigned char *)ptstr+1)) &&
	    !is_broken_string(str)) {
	    int n = 0;
	    int clen;
	    unsigned char c = rb_enc_codepoint_len(ptstr, ptstr+1, &clen, enc);

	    s = RSTRING_PTR(str);
	    if (!s || RSTRING_LEN(str) == 0) return INT2FIX(0);
	    send = RSTRING_END(str);
	    while (s < send) {
		if (*(unsigned char*)s++ == c) n++;
	    }
	    return INT2NUM(n);
	}
    }

    tr_setup_table(tstr, table, TRUE, &del, &nodel, enc);
    for (i=1; i<argc; i++) {
	tstr = argv[i];
	StringValue(tstr);
	enc = rb_enc_check(str, tstr);
	tr_setup_table(tstr, table, FALSE, &del, &nodel, enc);
    }

    s = RSTRING_PTR(str);
    if (!s || RSTRING_LEN(str) == 0) return INT2FIX(0);
    send = RSTRING_END(str);
    ascompat = rb_enc_asciicompat(enc);
    i = 0;
    while (s < send) {
	unsigned int c;

	if (ascompat && (c = *(unsigned char*)s) < 0x80) {
	    if (table[c]) {
		i++;
	    }
	    s++;
	}
	else {
	    int clen;
	    c = rb_enc_codepoint_len(s, send, &clen, enc);
	    if (tr_find(c, table, del, nodel)) {
		i++;
	    }
	    s += clen;
	}
    }

    return INT2NUM(i);
}

static const char isspacetable[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

#define ascii_isspace(c) isspacetable[(unsigned char)(c)]

/*
 *  call-seq:
 *     str.split(pattern=$;, [limit])   -> anArray
 *
 *  Divides <i>str</i> into substrings based on a delimiter, returning an array
 *  of these substrings.
 *
 *  If <i>pattern</i> is a <code>String</code>, then its contents are used as
 *  the delimiter when splitting <i>str</i>. If <i>pattern</i> is a single
 *  space, <i>str</i> is split on whitespace, with leading whitespace and runs
 *  of contiguous whitespace characters ignored.
 *
 *  If <i>pattern</i> is a <code>Regexp</code>, <i>str</i> is divided where the
 *  pattern matches. Whenever the pattern matches a zero-length string,
 *  <i>str</i> is split into individual characters. If <i>pattern</i> contains
 *  groups, the respective matches will be returned in the array as well.
 *
 *  If <i>pattern</i> is omitted, the value of <code>$;</code> is used.  If
 *  <code>$;</code> is <code>nil</code> (which is the default), <i>str</i> is
 *  split on whitespace as if ` ' were specified.
 *
 *  If the <i>limit</i> parameter is omitted, trailing null fields are
 *  suppressed. If <i>limit</i> is a positive number, at most that number of
 *  fields will be returned (if <i>limit</i> is <code>1</code>, the entire
 *  string is returned as the only entry in an array). If negative, there is no
 *  limit to the number of fields returned, and trailing null fields are not
 *  suppressed.
 *
 *  When the input +str+ is empty an empty Array is returned as the string is
 *  considered to have no fields to split.
 *
 *     " now's  the time".split        #=> ["now's", "the", "time"]
 *     " now's  the time".split(' ')   #=> ["now's", "the", "time"]
 *     " now's  the time".split(/ /)   #=> ["", "now's", "", "the", "time"]
 *     "1, 2.34,56, 7".split(%r{,\s*}) #=> ["1", "2.34", "56", "7"]
 *     "hello".split(//)               #=> ["h", "e", "l", "l", "o"]
 *     "hello".split(//, 3)            #=> ["h", "e", "llo"]
 *     "hi mom".split(%r{\s*})         #=> ["h", "i", "m", "o", "m"]
 *
 *     "mellow yellow".split("ello")   #=> ["m", "w y", "w"]
 *     "1,2,,3,4,,".split(',')         #=> ["1", "2", "", "3", "4"]
 *     "1,2,,3,4,,".split(',', 4)      #=> ["1", "2", "", "3,4,,"]
 *     "1,2,,3,4,,".split(',', -4)     #=> ["1", "2", "", "3", "4", "", ""]
 *
 *     "".split(',', -1)               #=> []
 */

static VALUE
rb_str_split_m(int argc, VALUE *argv, VALUE str)
{
    rb_encoding *enc;
    VALUE spat;
    VALUE limit;
    enum {awk, string, regexp} split_type;
    long beg, end, i = 0;
    int lim = 0;
    VALUE result, tmp;

    if (rb_scan_args(argc, argv, "02", &spat, &limit) == 2) {
	lim = NUM2INT(limit);
	if (lim <= 0) limit = Qnil;
	else if (lim == 1) {
	    if (RSTRING_LEN(str) == 0)
		return rb_ary_new2(0);
	    return rb_ary_new3(1, str);
	}
	i = 1;
    }

    enc = STR_ENC_GET(str);
    if (NIL_P(spat) && NIL_P(spat = rb_fs)) {
	split_type = awk;
    }
    else {
	spat = get_pat_quoted(spat, 0);
	if (BUILTIN_TYPE(spat) == T_STRING) {
	    rb_encoding *enc2 = STR_ENC_GET(spat);

	    mustnot_broken(spat);
	    split_type = string;
	    if (RSTRING_LEN(spat) == 0) {
		/* Special case - split into chars */
		spat = rb_reg_regcomp(spat);
		split_type = regexp;
	    }
	    else if (rb_enc_asciicompat(enc2) == 1) {
		if (RSTRING_LEN(spat) == 1 && RSTRING_PTR(spat)[0] == ' '){
		    split_type = awk;
		}
	    }
	    else {
		int l;
		if (rb_enc_ascget(RSTRING_PTR(spat), RSTRING_END(spat), &l, enc2) == ' ' &&
		    RSTRING_LEN(spat) == l) {
		    split_type = awk;
		}
	    }
	}
	else {
	    split_type = regexp;
	}
    }

    result = rb_ary_new();
    beg = 0;
    if (split_type == awk) {
	char *ptr = RSTRING_PTR(str);
	char *eptr = RSTRING_END(str);
	char *bptr = ptr;
	int skip = 1;
	unsigned int c;

	end = beg;
	if (is_ascii_string(str)) {
	    while (ptr < eptr) {
		c = (unsigned char)*ptr++;
		if (skip) {
		    if (ascii_isspace(c)) {
			beg = ptr - bptr;
		    }
		    else {
			end = ptr - bptr;
			skip = 0;
			if (!NIL_P(limit) && lim <= i) break;
		    }
		}
		else if (ascii_isspace(c)) {
		    rb_ary_push(result, rb_str_subseq(str, beg, end-beg));
		    skip = 1;
		    beg = ptr - bptr;
		    if (!NIL_P(limit)) ++i;
		}
		else {
		    end = ptr - bptr;
		}
	    }
	}
	else {
	    while (ptr < eptr) {
		int n;

		c = rb_enc_codepoint_len(ptr, eptr, &n, enc);
		ptr += n;
		if (skip) {
		    if (rb_isspace(c)) {
			beg = ptr - bptr;
		    }
		    else {
			end = ptr - bptr;
			skip = 0;
			if (!NIL_P(limit) && lim <= i) break;
		    }
		}
		else if (rb_isspace(c)) {
		    rb_ary_push(result, rb_str_subseq(str, beg, end-beg));
		    skip = 1;
		    beg = ptr - bptr;
		    if (!NIL_P(limit)) ++i;
		}
		else {
		    end = ptr - bptr;
		}
	    }
	}
    }
    else if (split_type == string) {
	char *ptr = RSTRING_PTR(str);
	char *temp = ptr;
	char *eptr = RSTRING_END(str);
	char *sptr = RSTRING_PTR(spat);
	long slen = RSTRING_LEN(spat);

	mustnot_broken(str);
	enc = rb_enc_check(str, spat);
	while (ptr < eptr &&
	       (end = rb_memsearch(sptr, slen, ptr, eptr - ptr, enc)) >= 0) {
	    /* Check we are at the start of a char */
	    char *t = rb_enc_right_char_head(ptr, ptr + end, eptr, enc);
	    if (t != ptr + end) {
		ptr = t;
		continue;
	    }
	    rb_ary_push(result, rb_str_subseq(str, ptr - temp, end));
	    ptr += end + slen;
	    if (!NIL_P(limit) && lim <= ++i) break;
	}
	beg = ptr - temp;
    }
    else {
	char *ptr = RSTRING_PTR(str);
	long len = RSTRING_LEN(str);
	long start = beg;
	long idx;
	int last_null = 0;
	struct re_registers *regs;

	while ((end = rb_reg_search(spat, str, start, 0)) >= 0) {
	    regs = RMATCH_REGS(rb_backref_get());
	    if (start == end && BEG(0) == END(0)) {
		if (!ptr) {
		    rb_ary_push(result, str_new_empty(str));
		    break;
		}
		else if (last_null == 1) {
		    rb_ary_push(result, rb_str_subseq(str, beg,
						      rb_enc_fast_mbclen(ptr+beg,
									 ptr+len,
									 enc)));
		    beg = start;
		}
		else {
                    if (ptr+start == ptr+len)
                        start++;
                    else
                        start += rb_enc_fast_mbclen(ptr+start,ptr+len,enc);
		    last_null = 1;
		    continue;
		}
	    }
	    else {
		rb_ary_push(result, rb_str_subseq(str, beg, end-beg));
		beg = start = END(0);
	    }
	    last_null = 0;

	    for (idx=1; idx < regs->num_regs; idx++) {
		if (BEG(idx) == -1) continue;
		if (BEG(idx) == END(idx))
		    tmp = str_new_empty(str);
		else
		    tmp = rb_str_subseq(str, BEG(idx), END(idx)-BEG(idx));
		rb_ary_push(result, tmp);
	    }
	    if (!NIL_P(limit) && lim <= ++i) break;
	}
    }
    if (RSTRING_LEN(str) > 0 && (!NIL_P(limit) || RSTRING_LEN(str) > beg || lim < 0)) {
	if (RSTRING_LEN(str) == beg)
	    tmp = str_new_empty(str);
	else
	    tmp = rb_str_subseq(str, beg, RSTRING_LEN(str)-beg);
	rb_ary_push(result, tmp);
    }
    if (NIL_P(limit) && lim == 0) {
	long len;
	while ((len = RARRAY_LEN(result)) > 0 &&
	       (tmp = RARRAY_AREF(result, len-1), RSTRING_LEN(tmp) == 0))
	    rb_ary_pop(result);
    }

    return result;
}

VALUE
rb_str_split(VALUE str, const char *sep0)
{
    VALUE sep;

    StringValue(str);
    sep = rb_str_new_cstr(sep0);
    return rb_str_split_m(1, &sep, str);
}


static VALUE
rb_str_enumerate_lines(int argc, VALUE *argv, VALUE str, int wantarray)
{
    rb_encoding *enc;
    VALUE line, rs, orig = str;
    const char *ptr, *pend, *subptr, *subend, *rsptr, *hit, *adjusted;
    long pos, len, rslen;
    int paragraph_mode = 0;

    VALUE UNINITIALIZED_VAR(ary);

    if (argc == 0)
	rs = rb_rs;
    else
	rb_scan_args(argc, argv, "01", &rs);

    if (rb_block_given_p()) {
	if (wantarray) {
#if STRING_ENUMERATORS_WANTARRAY
	    rb_warn("given block not used");
	    ary = rb_ary_new();
#else
	    rb_warning("passing a block to String#lines is deprecated");
	    wantarray = 0;
#endif
	}
    }
    else {
	if (wantarray)
	    ary = rb_ary_new();
	else
	    return SIZED_ENUMERATOR(str, argc, argv, 0);
    }

    if (NIL_P(rs)) {
	if (wantarray) {
	    rb_ary_push(ary, str);
	    return ary;
	}
	else {
	    rb_yield(str);
	    return orig;
	}
    }

    str = rb_str_new_frozen(str);
    ptr = subptr = RSTRING_PTR(str);
    pend = RSTRING_END(str);
    len = RSTRING_LEN(str);
    StringValue(rs);
    rslen = RSTRING_LEN(rs);

    if (rs == rb_default_rs)
	enc = rb_enc_get(str);
    else
	enc = rb_enc_check(str, rs);

    if (rslen == 0) {
	rsptr = "\n\n";
	rslen = 2;
	paragraph_mode = 1;
    }
    else {
	rsptr = RSTRING_PTR(rs);
    }

    if ((rs == rb_default_rs || paragraph_mode) && !rb_enc_asciicompat(enc)) {
	rs = rb_str_new(rsptr, rslen);
	rs = rb_str_encode(rs, rb_enc_from_encoding(enc), 0, Qnil);
	rsptr = RSTRING_PTR(rs);
	rslen = RSTRING_LEN(rs);
    }

    while (subptr < pend) {
	pos = rb_memsearch(rsptr, rslen, subptr, pend - subptr, enc);
	if (pos < 0) break;
	hit = subptr + pos;
	adjusted = rb_enc_right_char_head(subptr, hit, pend, enc);
	if (hit != adjusted) {
	    subptr = adjusted;
	    continue;
	}
	subend = hit + rslen;
	if (paragraph_mode) {
	    while (subend < pend && rb_enc_is_newline(subend, pend, enc)) {
		subend += rb_enc_mbclen(subend, pend, enc);
	    }
	}
	line = rb_str_subseq(str, subptr - ptr, subend - subptr);
	if (wantarray) {
	    rb_ary_push(ary, line);
	}
	else {
	    rb_yield(line);
	    str_mod_check(str, ptr, len);
	}
	subptr = subend;
    }

    if (subptr != pend) {
	line = rb_str_subseq(str, subptr - ptr, pend - subptr);
	if (wantarray)
	    rb_ary_push(ary, line);
	else
	    rb_yield(line);
	RB_GC_GUARD(str);
    }

    if (wantarray)
	return ary;
    else
	return orig;
}

/*
 *  call-seq:
 *     str.each_line(separator=$/) {|substr| block }   -> str
 *     str.each_line(separator=$/)                     -> an_enumerator
 *
 *  Splits <i>str</i> using the supplied parameter as the record
 *  separator (<code>$/</code> by default), passing each substring in
 *  turn to the supplied block.  If a zero-length record separator is
 *  supplied, the string is split into paragraphs delimited by
 *  multiple successive newlines.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     print "Example one\n"
 *     "hello\nworld".each_line {|s| p s}
 *     print "Example two\n"
 *     "hello\nworld".each_line('l') {|s| p s}
 *     print "Example three\n"
 *     "hello\n\n\nworld".each_line('') {|s| p s}
 *
 *  <em>produces:</em>
 *
 *     Example one
 *     "hello\n"
 *     "world"
 *     Example two
 *     "hel"
 *     "l"
 *     "o\nworl"
 *     "d"
 *     Example three
 *     "hello\n\n\n"
 *     "world"
 */

static VALUE
rb_str_each_line(int argc, VALUE *argv, VALUE str)
{
    return rb_str_enumerate_lines(argc, argv, str, 0);
}

/*
 *  call-seq:
 *     str.lines(separator=$/)  -> an_array
 *
 *  Returns an array of lines in <i>str</i> split using the supplied
 *  record separator (<code>$/</code> by default).  This is a
 *  shorthand for <code>str.each_line(separator).to_a</code>.
 *
 *  If a block is given, which is a deprecated form, works the same as
 *  <code>each_line</code>.
 */

static VALUE
rb_str_lines(int argc, VALUE *argv, VALUE str)
{
    return rb_str_enumerate_lines(argc, argv, str, 1);
}

static VALUE
rb_str_each_byte_size(VALUE str, VALUE args, VALUE eobj)
{
    return LONG2FIX(RSTRING_LEN(str));
}

static VALUE
rb_str_enumerate_bytes(VALUE str, int wantarray)
{
    long i;
    VALUE UNINITIALIZED_VAR(ary);

    if (rb_block_given_p()) {
	if (wantarray) {
#if STRING_ENUMERATORS_WANTARRAY
	    rb_warn("given block not used");
	    ary = rb_ary_new();
#else
	    rb_warning("passing a block to String#bytes is deprecated");
	    wantarray = 0;
#endif
	}
    }
    else {
	if (wantarray)
	    ary = rb_ary_new2(RSTRING_LEN(str));
	else
	    return SIZED_ENUMERATOR(str, 0, 0, rb_str_each_byte_size);
    }

    for (i=0; i<RSTRING_LEN(str); i++) {
	if (wantarray)
	    rb_ary_push(ary, INT2FIX(RSTRING_PTR(str)[i] & 0xff));
	else
	    rb_yield(INT2FIX(RSTRING_PTR(str)[i] & 0xff));
    }
    if (wantarray)
	return ary;
    else
	return str;
}

/*
 *  call-seq:
 *     str.each_byte {|fixnum| block }    -> str
 *     str.each_byte                      -> an_enumerator
 *
 *  Passes each byte in <i>str</i> to the given block, or returns an
 *  enumerator if no block is given.
 *
 *     "hello".each_byte {|c| print c, ' ' }
 *
 *  <em>produces:</em>
 *
 *     104 101 108 108 111
 */

static VALUE
rb_str_each_byte(VALUE str)
{
    return rb_str_enumerate_bytes(str, 0);
}

/*
 *  call-seq:
 *     str.bytes    -> an_array
 *
 *  Returns an array of bytes in <i>str</i>.  This is a shorthand for
 *  <code>str.each_byte.to_a</code>.
 *
 *  If a block is given, which is a deprecated form, works the same as
 *  <code>each_byte</code>.
 */

static VALUE
rb_str_bytes(VALUE str)
{
    return rb_str_enumerate_bytes(str, 1);
}

static VALUE
rb_str_each_char_size(VALUE str, VALUE args, VALUE eobj)
{
    return rb_str_length(str);
}

static VALUE
rb_str_enumerate_chars(VALUE str, int wantarray)
{
    VALUE orig = str;
    VALUE substr;
    long i, len, n;
    const char *ptr;
    rb_encoding *enc;
    VALUE UNINITIALIZED_VAR(ary);

    str = rb_str_new_frozen(str);
    ptr = RSTRING_PTR(str);
    len = RSTRING_LEN(str);
    enc = rb_enc_get(str);

    if (rb_block_given_p()) {
	if (wantarray) {
#if STRING_ENUMERATORS_WANTARRAY
	    rb_warn("given block not used");
	    ary = rb_ary_new_capa(str_strlen(str, enc)); /* str's enc*/
#else
	    rb_warning("passing a block to String#chars is deprecated");
	    wantarray = 0;
#endif
	}
    }
    else {
	if (wantarray)
	    ary = rb_ary_new_capa(str_strlen(str, enc)); /* str's enc*/
	else
	    return SIZED_ENUMERATOR(str, 0, 0, rb_str_each_char_size);
    }

    if (ENC_CODERANGE_CLEAN_P(ENC_CODERANGE(str))) {
	for (i = 0; i < len; i += n) {
	    n = rb_enc_fast_mbclen(ptr + i, ptr + len, enc);
	    substr = rb_str_subseq(str, i, n);
	    if (wantarray)
		rb_ary_push(ary, substr);
	    else
		rb_yield(substr);
	}
    }
    else {
	for (i = 0; i < len; i += n) {
	    n = rb_enc_mbclen(ptr + i, ptr + len, enc);
	    substr = rb_str_subseq(str, i, n);
	    if (wantarray)
		rb_ary_push(ary, substr);
	    else
		rb_yield(substr);
	}
    }
    RB_GC_GUARD(str);
    if (wantarray)
	return ary;
    else
	return orig;
}

/*
 *  call-seq:
 *     str.each_char {|cstr| block }    -> str
 *     str.each_char                    -> an_enumerator
 *
 *  Passes each character in <i>str</i> to the given block, or returns
 *  an enumerator if no block is given.
 *
 *     "hello".each_char {|c| print c, ' ' }
 *
 *  <em>produces:</em>
 *
 *     h e l l o
 */

static VALUE
rb_str_each_char(VALUE str)
{
    return rb_str_enumerate_chars(str, 0);
}

/*
 *  call-seq:
 *     str.chars    -> an_array
 *
 *  Returns an array of characters in <i>str</i>.  This is a shorthand
 *  for <code>str.each_char.to_a</code>.
 *
 *  If a block is given, which is a deprecated form, works the same as
 *  <code>each_char</code>.
 */

static VALUE
rb_str_chars(VALUE str)
{
    return rb_str_enumerate_chars(str, 1);
}


static VALUE
rb_str_enumerate_codepoints(VALUE str, int wantarray)
{
    VALUE orig = str;
    int n;
    unsigned int c;
    const char *ptr, *end;
    rb_encoding *enc;
    VALUE UNINITIALIZED_VAR(ary);

    if (single_byte_optimizable(str))
	return rb_str_enumerate_bytes(str, wantarray);

    str = rb_str_new_frozen(str);
    ptr = RSTRING_PTR(str);
    end = RSTRING_END(str);
    enc = STR_ENC_GET(str);

    if (rb_block_given_p()) {
	if (wantarray) {
#if STRING_ENUMERATORS_WANTARRAY
	    rb_warn("given block not used");
	    ary = rb_ary_new_capa(str_strlen(str, enc)); /* str's enc*/
#else
	    rb_warning("passing a block to String#codepoints is deprecated");
	    wantarray = 0;
#endif
	}
    }
    else {
	if (wantarray)
	    ary = rb_ary_new_capa(str_strlen(str, enc)); /* str's enc*/
	else
	    return SIZED_ENUMERATOR(str, 0, 0, rb_str_each_char_size);
    }

    while (ptr < end) {
	c = rb_enc_codepoint_len(ptr, end, &n, enc);
	if (wantarray)
	    rb_ary_push(ary, UINT2NUM(c));
	else
	    rb_yield(UINT2NUM(c));
	ptr += n;
    }
    RB_GC_GUARD(str);
    if (wantarray)
	return ary;
    else
	return orig;
}

/*
 *  call-seq:
 *     str.each_codepoint {|integer| block }    -> str
 *     str.each_codepoint                       -> an_enumerator
 *
 *  Passes the <code>Integer</code> ordinal of each character in <i>str</i>,
 *  also known as a <i>codepoint</i> when applied to Unicode strings to the
 *  given block.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     "hello\u0639".each_codepoint {|c| print c, ' ' }
 *
 *  <em>produces:</em>
 *
 *     104 101 108 108 111 1593
 */

static VALUE
rb_str_each_codepoint(VALUE str)
{
    return rb_str_enumerate_codepoints(str, 0);
}

/*
 *  call-seq:
 *     str.codepoints   -> an_array
 *
 *  Returns an array of the <code>Integer</code> ordinals of the
 *  characters in <i>str</i>.  This is a shorthand for
 *  <code>str.each_codepoint.to_a</code>.
 *
 *  If a block is given, which is a deprecated form, works the same as
 *  <code>each_codepoint</code>.
 */

static VALUE
rb_str_codepoints(VALUE str)
{
    return rb_str_enumerate_codepoints(str, 1);
}


static long
chopped_length(VALUE str)
{
    rb_encoding *enc = STR_ENC_GET(str);
    const char *p, *p2, *beg, *end;

    beg = RSTRING_PTR(str);
    end = beg + RSTRING_LEN(str);
    if (beg > end) return 0;
    p = rb_enc_prev_char(beg, end, end, enc);
    if (!p) return 0;
    if (p > beg && rb_enc_ascget(p, end, 0, enc) == '\n') {
	p2 = rb_enc_prev_char(beg, p, end, enc);
	if (p2 && rb_enc_ascget(p2, end, 0, enc) == '\r') p = p2;
    }
    return p - beg;
}

/*
 *  call-seq:
 *     str.chop!   -> str or nil
 *
 *  Processes <i>str</i> as for <code>String#chop</code>, returning <i>str</i>,
 *  or <code>nil</code> if <i>str</i> is the empty string.  See also
 *  <code>String#chomp!</code>.
 */

static VALUE
rb_str_chop_bang(VALUE str)
{
    str_modify_keep_cr(str);
    if (RSTRING_LEN(str) > 0) {
	long len;
	len = chopped_length(str);
	STR_SET_LEN(str, len);
	TERM_FILL(&RSTRING_PTR(str)[len], TERM_LEN(str));
	if (ENC_CODERANGE(str) != ENC_CODERANGE_7BIT) {
	    ENC_CODERANGE_CLEAR(str);
	}
	return str;
    }
    return Qnil;
}


/*
 *  call-seq:
 *     str.chop   -> new_str
 *
 *  Returns a new <code>String</code> with the last character removed.  If the
 *  string ends with <code>\r\n</code>, both characters are removed. Applying
 *  <code>chop</code> to an empty string returns an empty
 *  string. <code>String#chomp</code> is often a safer alternative, as it leaves
 *  the string unchanged if it doesn't end in a record separator.
 *
 *     "string\r\n".chop   #=> "string"
 *     "string\n\r".chop   #=> "string\n"
 *     "string\n".chop     #=> "string"
 *     "string".chop       #=> "strin"
 *     "x".chop.chop       #=> ""
 */

static VALUE
rb_str_chop(VALUE str)
{
    return rb_str_subseq(str, 0, chopped_length(str));
}


static long
chompped_length(VALUE str, VALUE rs)
{
    rb_encoding *enc;
    int newline;
    char *pp, *e, *rsptr;
    long rslen;
    char *const p = RSTRING_PTR(str);
    long len = RSTRING_LEN(str);

    if (len == 0) return 0;
    e = p + len;
    if (rs == rb_default_rs) {
      smart_chomp:
	enc = rb_enc_get(str);
	if (rb_enc_mbminlen(enc) > 1) {
	    pp = rb_enc_left_char_head(p, e-rb_enc_mbminlen(enc), e, enc);
	    if (rb_enc_is_newline(pp, e, enc)) {
		e = pp;
	    }
	    pp = e - rb_enc_mbminlen(enc);
	    if (pp >= p) {
		pp = rb_enc_left_char_head(p, pp, e, enc);
		if (rb_enc_ascget(pp, e, 0, enc) == '\r') {
		    e = pp;
		}
	    }
	}
	else {
	    switch (*(e-1)) { /* not e[-1] to get rid of VC bug */
	      case '\n':
		if (--e > p && *(e-1) == '\r') {
		    --e;
		}
		break;
	      case '\r':
		--e;
		break;
	    }
	}
	return e - p;
    }

    enc = rb_enc_get(str);
    RSTRING_GETMEM(rs, rsptr, rslen);
    if (rslen == 0) {
	if (rb_enc_mbminlen(enc) > 1) {
	    while (e > p) {
		pp = rb_enc_left_char_head(p, e-rb_enc_mbminlen(enc), e, enc);
		if (!rb_enc_is_newline(pp, e, enc)) break;
		e = pp;
		pp -= rb_enc_mbminlen(enc);
		if (pp >= p) {
		    pp = rb_enc_left_char_head(p, pp, e, enc);
		    if (rb_enc_ascget(pp, e, 0, enc) == '\r') {
			e = pp;
		    }
		}
	    }
	}
	else {
	    while (e > p && *(e-1) == '\n') {
		--e;
		if (e > p && *(e-1) == '\r')
		    --e;
	    }
	}
	return e - p;
    }
    if (rslen > len) return len;

    enc = rb_enc_get(rs);
    newline = rsptr[rslen-1];
    if (rslen == rb_enc_mbminlen(enc)) {
	if (rslen == 1) {
	    if (newline == '\n')
		goto smart_chomp;
	}
	else {
	    if (rb_enc_is_newline(rsptr, rsptr+rslen, enc))
		goto smart_chomp;
	}
    }

    enc = rb_enc_check(str, rs);
    if (is_broken_string(rs)) {
	return len;
    }
    pp = e - rslen;
    if (p[len-1] == newline &&
	(rslen <= 1 ||
	 memcmp(rsptr, pp, rslen) == 0)) {
	if (rb_enc_left_char_head(p, pp, e, enc) == pp)
	    return len - rslen;
	RB_GC_GUARD(rs);
    }
    return len;
}

static VALUE
chomp_rs(int argc, const VALUE *argv)
{
    rb_check_arity(argc, 0, 1);
    if (argc > 0) {
	VALUE rs = argv[0];
	if (!NIL_P(rs)) StringValue(rs);
	return rs;
    }
    else {
	return rb_rs;
    }
}

/*
 *  call-seq:
 *     str.chomp!(separator=$/)   -> str or nil
 *
 *  Modifies <i>str</i> in place as described for <code>String#chomp</code>,
 *  returning <i>str</i>, or <code>nil</code> if no modifications were made.
 */

static VALUE
rb_str_chomp_bang(int argc, VALUE *argv, VALUE str)
{
    VALUE rs;
    long olen;
    str_modify_keep_cr(str);
    if ((olen = RSTRING_LEN(str)) > 0 && !NIL_P(rs = chomp_rs(argc, argv))) {
	long len;
	len = chompped_length(str, rs);
	if (len < olen) {
	    STR_SET_LEN(str, len);
	    TERM_FILL(&RSTRING_PTR(str)[len], TERM_LEN(str));
	    if (ENC_CODERANGE(str) != ENC_CODERANGE_7BIT) {
		ENC_CODERANGE_CLEAR(str);
	    }
	    return str;
	}
    }
    return Qnil;
}


/*
 *  call-seq:
 *     str.chomp(separator=$/)   -> new_str
 *
 *  Returns a new <code>String</code> with the given record separator removed
 *  from the end of <i>str</i> (if present). If <code>$/</code> has not been
 *  changed from the default Ruby record separator, then <code>chomp</code> also
 *  removes carriage return characters (that is it will remove <code>\n</code>,
 *  <code>\r</code>, and <code>\r\n</code>). If <code>$/</code> is an empty string,
 *  it will remove all trailing newlines from the string.
 *
 *     "hello".chomp                #=> "hello"
 *     "hello\n".chomp              #=> "hello"
 *     "hello\r\n".chomp            #=> "hello"
 *     "hello\n\r".chomp            #=> "hello\n"
 *     "hello\r".chomp              #=> "hello"
 *     "hello \n there".chomp       #=> "hello \n there"
 *     "hello".chomp("llo")         #=> "he"
 *     "hello\r\n\r\n".chomp('')    #=> "hello"
 *     "hello\r\n\r\r\n".chomp('')  #=> "hello\r\n\r"
 */

static VALUE
rb_str_chomp(int argc, VALUE *argv, VALUE str)
{
    VALUE rs = chomp_rs(argc, argv);
    if (NIL_P(rs)) return rb_str_dup(str);
    return rb_str_subseq(str, 0, chompped_length(str, rs));
}

static long
lstrip_offset(VALUE str, const char *s, const char *e, rb_encoding *enc)
{
    const char *const start = s;

    if (!s || s >= e) return 0;
    /* remove spaces at head */
    while (s < e) {
	int n;
	unsigned int cc = rb_enc_codepoint_len(s, e, &n, enc);

	if (!rb_isspace(cc)) break;
	s += n;
    }
    return s - start;
}

/*
 *  call-seq:
 *     str.lstrip!   -> self or nil
 *
 *  Removes leading whitespace from <i>str</i>, returning <code>nil</code> if no
 *  change was made. See also <code>String#rstrip!</code> and
 *  <code>String#strip!</code>.
 *
 *  Refer to <code>strip</code> for the definition of whitespace.
 *
 *     "  hello  ".lstrip   #=> "hello  "
 *     "hello".lstrip!      #=> nil
 */

static VALUE
rb_str_lstrip_bang(VALUE str)
{
    rb_encoding *enc;
    char *start, *s;
    long olen, loffset;

    str_modify_keep_cr(str);
    enc = STR_ENC_GET(str);
    RSTRING_GETMEM(str, start, olen);
    loffset = lstrip_offset(str, start, start+olen, enc);
    if (loffset > 0) {
	long len = olen-loffset;
	s = start + loffset;
	memmove(start, s, len);
	STR_SET_LEN(str, len);
#if !SHARABLE_MIDDLE_SUBSTRING
	TERM_FILL(start+len, rb_enc_mbminlen(enc));
#endif
	return str;
    }
    return Qnil;
}


/*
 *  call-seq:
 *     str.lstrip   -> new_str
 *
 *  Returns a copy of <i>str</i> with leading whitespace removed. See also
 *  <code>String#rstrip</code> and <code>String#strip</code>.
 *
 *  Refer to <code>strip</code> for the definition of whitespace.
 *
 *     "  hello  ".lstrip   #=> "hello  "
 *     "hello".lstrip       #=> "hello"
 */

static VALUE
rb_str_lstrip(VALUE str)
{
    char *start;
    long len, loffset;
    RSTRING_GETMEM(str, start, len);
    loffset = lstrip_offset(str, start, start+len, STR_ENC_GET(str));
    if (loffset <= 0) return rb_str_dup(str);
    return rb_str_subseq(str, loffset, len - loffset);
}

static long
rstrip_offset(VALUE str, const char *s, const char *e, rb_encoding *enc)
{
    const char *t;

    rb_str_check_dummy_enc(enc);
    if (!s || s >= e) return 0;
    t = e;

    /* remove trailing spaces or '\0's */
    if (single_byte_optimizable(str)) {
	unsigned char c;
	while (s < t && ((c = *(t-1)) == '\0' || ascii_isspace(c))) t--;
    }
    else {
	char *tp;

        while ((tp = rb_enc_prev_char(s, t, e, enc)) != NULL) {
	    unsigned int c = rb_enc_codepoint(tp, e, enc);
	    if (c && !rb_isspace(c)) break;
	    t = tp;
	}
    }
    return e - t;
}

/*
 *  call-seq:
 *     str.rstrip!   -> self or nil
 *
 *  Removes trailing whitespace from <i>str</i>, returning <code>nil</code> if
 *  no change was made. See also <code>String#lstrip!</code> and
 *  <code>String#strip!</code>.
 *
 *  Refer to <code>strip</code> for the definition of whitespace.
 *
 *     "  hello  ".rstrip   #=> "  hello"
 *     "hello".rstrip!      #=> nil
 */

static VALUE
rb_str_rstrip_bang(VALUE str)
{
    rb_encoding *enc;
    char *start;
    long olen, roffset;

    str_modify_keep_cr(str);
    enc = STR_ENC_GET(str);
    RSTRING_GETMEM(str, start, olen);
    roffset = rstrip_offset(str, start, start+olen, enc);
    if (roffset > 0) {
	long len = olen - roffset;

	STR_SET_LEN(str, len);
#if !SHARABLE_MIDDLE_SUBSTRING
	TERM_FILL(start+len, rb_enc_mbminlen(enc));
#endif
	return str;
    }
    return Qnil;
}


/*
 *  call-seq:
 *     str.rstrip   -> new_str
 *
 *  Returns a copy of <i>str</i> with trailing whitespace removed. See also
 *  <code>String#lstrip</code> and <code>String#strip</code>.
 *
 *  Refer to <code>strip</code> for the definition of whitespace.
 *
 *     "  hello  ".rstrip   #=> "  hello"
 *     "hello".rstrip       #=> "hello"
 */

static VALUE
rb_str_rstrip(VALUE str)
{
    rb_encoding *enc;
    char *start;
    long olen, roffset;

    enc = STR_ENC_GET(str);
    RSTRING_GETMEM(str, start, olen);
    roffset = rstrip_offset(str, start, start+olen, enc);

    if (roffset <= 0) return rb_str_dup(str);
    return rb_str_subseq(str, 0, olen-roffset);
}


/*
 *  call-seq:
 *     str.strip!   -> str or nil
 *
 *  Removes leading and trailing whitespace from <i>str</i>. Returns
 *  <code>nil</code> if <i>str</i> was not altered.
 *
 *  Refer to <code>strip</code> for the definition of whitespace.
 */

static VALUE
rb_str_strip_bang(VALUE str)
{
    char *start;
    long olen, loffset, roffset;
    rb_encoding *enc;

    str_modify_keep_cr(str);
    enc = STR_ENC_GET(str);
    RSTRING_GETMEM(str, start, olen);
    loffset = lstrip_offset(str, start, start+olen, enc);
    roffset = rstrip_offset(str, start+loffset, start+olen, enc);

    if (loffset > 0 || roffset > 0) {
	long len = olen-roffset;
	if (loffset > 0) {
	    len -= loffset;
	    memmove(start, start + loffset, len);
	}
	STR_SET_LEN(str, len);
#if !SHARABLE_MIDDLE_SUBSTRING
	TERM_FILL(start+len, rb_enc_mbminlen(enc));
#endif
	return str;
    }
    return Qnil;
}


/*
 *  call-seq:
 *     str.strip   -> new_str
 *
 *  Returns a copy of <i>str</i> with leading and trailing whitespace removed.
 *
 *  Whitespace is defined as any of the following characters:
 *  null, horizontal tab, line feed, vertical tab, form feed, carriage return, space.
 *
 *     "    hello    ".strip   #=> "hello"
 *     "\tgoodbye\r\n".strip   #=> "goodbye"
 *     "\x00\t\n\v\f\r ".strip #=> ""
 */

static VALUE
rb_str_strip(VALUE str)
{
    char *start;
    long olen, loffset, roffset;
    rb_encoding *enc = STR_ENC_GET(str);

    RSTRING_GETMEM(str, start, olen);
    loffset = lstrip_offset(str, start, start+olen, enc);
    roffset = rstrip_offset(str, start+loffset, start+olen, enc);

    if (loffset <= 0 && roffset <= 0) return rb_str_dup(str);
    return rb_str_subseq(str, loffset, olen-loffset-roffset);
}

static VALUE
scan_once(VALUE str, VALUE pat, long *start)
{
    VALUE result, match;
    struct re_registers *regs;
    int i;

    if (rb_pat_search(pat, str, *start, 1) >= 0) {
	match = rb_backref_get();
	regs = RMATCH_REGS(match);
	if (BEG(0) == END(0)) {
	    rb_encoding *enc = STR_ENC_GET(str);
	    /*
	     * Always consume at least one character of the input string
	     */
	    if (RSTRING_LEN(str) > END(0))
		*start = END(0)+rb_enc_fast_mbclen(RSTRING_PTR(str)+END(0),
						   RSTRING_END(str), enc);
	    else
		*start = END(0)+1;
	}
	else {
	    *start = END(0);
	}
	if (regs->num_regs == 1) {
	    return rb_reg_nth_match(0, match);
	}
	result = rb_ary_new2(regs->num_regs);
	for (i=1; i < regs->num_regs; i++) {
	    rb_ary_push(result, rb_reg_nth_match(i, match));
	}

	return result;
    }
    return Qnil;
}


/*
 *  call-seq:
 *     str.scan(pattern)                         -> array
 *     str.scan(pattern) {|match, ...| block }   -> str
 *
 *  Both forms iterate through <i>str</i>, matching the pattern (which may be a
 *  <code>Regexp</code> or a <code>String</code>). For each match, a result is
 *  generated and either added to the result array or passed to the block. If
 *  the pattern contains no groups, each individual result consists of the
 *  matched string, <code>$&</code>.  If the pattern contains groups, each
 *  individual result is itself an array containing one entry per group.
 *
 *     a = "cruel world"
 *     a.scan(/\w+/)        #=> ["cruel", "world"]
 *     a.scan(/.../)        #=> ["cru", "el ", "wor"]
 *     a.scan(/(...)/)      #=> [["cru"], ["el "], ["wor"]]
 *     a.scan(/(..)(..)/)   #=> [["cr", "ue"], ["l ", "wo"]]
 *
 *  And the block form:
 *
 *     a.scan(/\w+/) {|w| print "<<#{w}>> " }
 *     print "\n"
 *     a.scan(/(.)(.)/) {|x,y| print y, x }
 *     print "\n"
 *
 *  <em>produces:</em>
 *
 *     <<cruel>> <<world>>
 *     rceu lowlr
 */

static VALUE
rb_str_scan(VALUE str, VALUE pat)
{
    VALUE result;
    long start = 0;
    long last = -1, prev = 0;
    char *p = RSTRING_PTR(str); long len = RSTRING_LEN(str);

    pat = get_pat_quoted(pat, 1);
    mustnot_broken(str);
    if (!rb_block_given_p()) {
	VALUE ary = rb_ary_new();

	while (!NIL_P(result = scan_once(str, pat, &start))) {
	    last = prev;
	    prev = start;
	    rb_ary_push(ary, result);
	}
	if (last >= 0) rb_pat_search(pat, str, last, 1);
	return ary;
    }

    while (!NIL_P(result = scan_once(str, pat, &start))) {
	last = prev;
	prev = start;
	rb_yield(result);
	str_mod_check(str, p, len);
    }
    if (last >= 0) rb_pat_search(pat, str, last, 1);
    return str;
}


/*
 *  call-seq:
 *     str.hex   -> integer
 *
 *  Treats leading characters from <i>str</i> as a string of hexadecimal digits
 *  (with an optional sign and an optional <code>0x</code>) and returns the
 *  corresponding number. Zero is returned on error.
 *
 *     "0x0a".hex     #=> 10
 *     "-1234".hex    #=> -4660
 *     "0".hex        #=> 0
 *     "wombat".hex   #=> 0
 */

static VALUE
rb_str_hex(VALUE str)
{
    return rb_str_to_inum(str, 16, FALSE);
}


/*
 *  call-seq:
 *     str.oct   -> integer
 *
 *  Treats leading characters of <i>str</i> as a string of octal digits (with an
 *  optional sign) and returns the corresponding number.  Returns 0 if the
 *  conversion fails.
 *
 *     "123".oct       #=> 83
 *     "-377".oct      #=> -255
 *     "bad".oct       #=> 0
 *     "0377bad".oct   #=> 255
 */

static VALUE
rb_str_oct(VALUE str)
{
    return rb_str_to_inum(str, -8, FALSE);
}


/*
 *  call-seq:
 *     str.crypt(salt_str)   -> new_str
 *
 *  Applies a one-way cryptographic hash to <i>str</i> by invoking the
 *  standard library function <code>crypt(3)</code> with the given
 *  salt string.  While the format and the result are system and
 *  implementation dependent, using a salt matching the regular
 *  expression <code>\A[a-zA-Z0-9./]{2}</code> should be valid and
 *  safe on any platform, in which only the first two characters are
 *  significant.
 *
 *  This method is for use in system specific scripts, so if you want
 *  a cross-platform hash function consider using Digest or OpenSSL
 *  instead.
 */

static VALUE
rb_str_crypt(VALUE str, VALUE salt)
{
    extern char *crypt(const char *, const char *);
    VALUE result;
    const char *s, *saltp;
    char *res;
#ifdef BROKEN_CRYPT
    char salt_8bit_clean[3];
#endif

    StringValue(salt);
    mustnot_wchar(str);
    mustnot_wchar(salt);
    if (RSTRING_LEN(salt) < 2) {
      short_salt:
	rb_raise(rb_eArgError, "salt too short (need >=2 bytes)");
    }

    s = StringValueCStr(str);
    saltp = RSTRING_PTR(salt);
    if (!saltp[0] || !saltp[1]) goto short_salt;
#ifdef BROKEN_CRYPT
    if (!ISASCII((unsigned char)saltp[0]) || !ISASCII((unsigned char)saltp[1])) {
	salt_8bit_clean[0] = saltp[0] & 0x7f;
	salt_8bit_clean[1] = saltp[1] & 0x7f;
	salt_8bit_clean[2] = '\0';
	saltp = salt_8bit_clean;
    }
#endif
    res = crypt(s, saltp);
    if (!res) {
	rb_sys_fail("crypt");
    }
    result = rb_str_new_cstr(res);
    FL_SET_RAW(result, OBJ_TAINTED_RAW(str) | OBJ_TAINTED_RAW(salt));
    return result;
}


/*
 *  call-seq:
 *     str.ord   -> integer
 *
 *  Return the <code>Integer</code> ordinal of a one-character string.
 *
 *     "a".ord         #=> 97
 */

VALUE
rb_str_ord(VALUE s)
{
    unsigned int c;

    c = rb_enc_codepoint(RSTRING_PTR(s), RSTRING_END(s), STR_ENC_GET(s));
    return UINT2NUM(c);
}
/*
 *  call-seq:
 *     str.sum(n=16)   -> integer
 *
 *  Returns a basic <em>n</em>-bit checksum of the characters in <i>str</i>,
 *  where <em>n</em> is the optional <code>Fixnum</code> parameter, defaulting
 *  to 16. The result is simply the sum of the binary value of each byte in
 *  <i>str</i> modulo <code>2**n - 1</code>. This is not a particularly good
 *  checksum.
 */

static VALUE
rb_str_sum(int argc, VALUE *argv, VALUE str)
{
    VALUE vbits;
    int bits;
    char *ptr, *p, *pend;
    long len;
    VALUE sum = INT2FIX(0);
    unsigned long sum0 = 0;

    if (argc == 0) {
	bits = 16;
    }
    else {
	rb_scan_args(argc, argv, "01", &vbits);
	bits = NUM2INT(vbits);
        if (bits < 0)
            bits = 0;
    }
    ptr = p = RSTRING_PTR(str);
    len = RSTRING_LEN(str);
    pend = p + len;

    while (p < pend) {
        if (FIXNUM_MAX - UCHAR_MAX < sum0) {
            sum = rb_funcall(sum, '+', 1, LONG2FIX(sum0));
            str_mod_check(str, ptr, len);
            sum0 = 0;
        }
        sum0 += (unsigned char)*p;
        p++;
    }

    if (bits == 0) {
        if (sum0) {
            sum = rb_funcall(sum, '+', 1, LONG2FIX(sum0));
        }
    }
    else {
        if (sum == INT2FIX(0)) {
            if (bits < (int)sizeof(long)*CHAR_BIT) {
                sum0 &= (((unsigned long)1)<<bits)-1;
            }
            sum = LONG2FIX(sum0);
        }
        else {
            VALUE mod;

            if (sum0) {
                sum = rb_funcall(sum, '+', 1, LONG2FIX(sum0));
            }

            mod = rb_funcall(INT2FIX(1), rb_intern("<<"), 1, INT2FIX(bits));
            mod = rb_funcall(mod, '-', 1, INT2FIX(1));
            sum = rb_funcall(sum, '&', 1, mod);
        }
    }
    return sum;
}

static VALUE
rb_str_justify(int argc, VALUE *argv, VALUE str, char jflag)
{
    rb_encoding *enc;
    VALUE w;
    long width, len, flen = 1, fclen = 1;
    VALUE res;
    char *p;
    const char *f = " ";
    long n, size, llen, rlen, llen2 = 0, rlen2 = 0;
    VALUE pad;
    int singlebyte = 1, cr;

    rb_scan_args(argc, argv, "11", &w, &pad);
    enc = STR_ENC_GET(str);
    width = NUM2LONG(w);
    if (argc == 2) {
	StringValue(pad);
	enc = rb_enc_check(str, pad);
	f = RSTRING_PTR(pad);
	flen = RSTRING_LEN(pad);
	fclen = str_strlen(pad, enc); /* rb_enc_check */
	singlebyte = single_byte_optimizable(pad);
	if (flen == 0 || fclen == 0) {
	    rb_raise(rb_eArgError, "zero width padding");
	}
    }
    len = str_strlen(str, enc); /* rb_enc_check */
    if (width < 0 || len >= width) return rb_str_dup(str);
    n = width - len;
    llen = (jflag == 'l') ? 0 : ((jflag == 'r') ? n : n/2);
    rlen = n - llen;
    cr = ENC_CODERANGE(str);
    if (flen > 1) {
       llen2 = str_offset(f, f + flen, llen % fclen, enc, singlebyte);
       rlen2 = str_offset(f, f + flen, rlen % fclen, enc, singlebyte);
    }
    size = RSTRING_LEN(str);
    if ((len = llen / fclen + rlen / fclen) >= LONG_MAX / flen ||
       (len *= flen) >= LONG_MAX - llen2 - rlen2 ||
       (len += llen2 + rlen2) >= LONG_MAX - size) {
       rb_raise(rb_eArgError, "argument too big");
    }
    len += size;
    res = rb_str_new_with_class(str, 0, len);
    p = RSTRING_PTR(res);
    if (flen <= 1) {
       memset(p, *f, llen);
       p += llen;
    }
    else {
       while (llen >= fclen) {
	    memcpy(p,f,flen);
	    p += flen;
	    llen -= fclen;
	}
       if (llen > 0) {
           memcpy(p, f, llen2);
           p += llen2;
	}
    }
    memcpy(p, RSTRING_PTR(str), size);
    p += size;
    if (flen <= 1) {
       memset(p, *f, rlen);
       p += rlen;
    }
    else {
       while (rlen >= fclen) {
	    memcpy(p,f,flen);
	    p += flen;
	    rlen -= fclen;
	}
       if (rlen > 0) {
           memcpy(p, f, rlen2);
           p += rlen2;
	}
    }
    TERM_FILL(p, rb_enc_mbminlen(enc));
    STR_SET_LEN(res, p-RSTRING_PTR(res));
    OBJ_INFECT_RAW(res, str);
    if (!NIL_P(pad)) OBJ_INFECT_RAW(res, pad);
    rb_enc_associate(res, enc);
    if (argc == 2)
	cr = ENC_CODERANGE_AND(cr, ENC_CODERANGE(pad));
    if (cr != ENC_CODERANGE_BROKEN)
	ENC_CODERANGE_SET(res, cr);

    RB_GC_GUARD(pad);
    return res;
}


/*
 *  call-seq:
 *     str.ljust(integer, padstr=' ')   -> new_str
 *
 *  If <i>integer</i> is greater than the length of <i>str</i>, returns a new
 *  <code>String</code> of length <i>integer</i> with <i>str</i> left justified
 *  and padded with <i>padstr</i>; otherwise, returns <i>str</i>.
 *
 *     "hello".ljust(4)            #=> "hello"
 *     "hello".ljust(20)           #=> "hello               "
 *     "hello".ljust(20, '1234')   #=> "hello123412341234123"
 */

static VALUE
rb_str_ljust(int argc, VALUE *argv, VALUE str)
{
    return rb_str_justify(argc, argv, str, 'l');
}


/*
 *  call-seq:
 *     str.rjust(integer, padstr=' ')   -> new_str
 *
 *  If <i>integer</i> is greater than the length of <i>str</i>, returns a new
 *  <code>String</code> of length <i>integer</i> with <i>str</i> right justified
 *  and padded with <i>padstr</i>; otherwise, returns <i>str</i>.
 *
 *     "hello".rjust(4)            #=> "hello"
 *     "hello".rjust(20)           #=> "               hello"
 *     "hello".rjust(20, '1234')   #=> "123412341234123hello"
 */

static VALUE
rb_str_rjust(int argc, VALUE *argv, VALUE str)
{
    return rb_str_justify(argc, argv, str, 'r');
}


/*
 *  call-seq:
 *     str.center(width, padstr=' ')   -> new_str
 *
 *  Centers +str+ in +width+.  If +width+ is greater than the length of +str+,
 *  returns a new String of length +width+ with +str+ centered and padded with
 *  +padstr+; otherwise, returns +str+.
 *
 *     "hello".center(4)         #=> "hello"
 *     "hello".center(20)        #=> "       hello        "
 *     "hello".center(20, '123') #=> "1231231hello12312312"
 */

static VALUE
rb_str_center(int argc, VALUE *argv, VALUE str)
{
    return rb_str_justify(argc, argv, str, 'c');
}

/*
 *  call-seq:
 *     str.partition(sep)              -> [head, sep, tail]
 *     str.partition(regexp)           -> [head, match, tail]
 *
 *  Searches <i>sep</i> or pattern (<i>regexp</i>) in the string
 *  and returns the part before it, the match, and the part
 *  after it.
 *  If it is not found, returns two empty strings and <i>str</i>.
 *
 *     "hello".partition("l")         #=> ["he", "l", "lo"]
 *     "hello".partition("x")         #=> ["hello", "", ""]
 *     "hello".partition(/.l/)        #=> ["h", "el", "lo"]
 */

static VALUE
rb_str_partition(VALUE str, VALUE sep)
{
    long pos;

    sep = get_pat_quoted(sep, 0);
    if (RB_TYPE_P(sep, T_REGEXP)) {
	pos = rb_reg_search(sep, str, 0, 0);
	if (pos < 0) {
	  failed:
	    return rb_ary_new3(3, str, str_new_empty(str), str_new_empty(str));
	}
	sep = rb_str_subpat(str, sep, INT2FIX(0));
	if (pos == 0 && RSTRING_LEN(sep) == 0) goto failed;
    }
    else {
	pos = rb_str_index(str, sep, 0);
	if (pos < 0) goto failed;
    }
    return rb_ary_new3(3, rb_str_subseq(str, 0, pos),
		          sep,
		          rb_str_subseq(str, pos+RSTRING_LEN(sep),
					     RSTRING_LEN(str)-pos-RSTRING_LEN(sep)));
}

/*
 *  call-seq:
 *     str.rpartition(sep)             -> [head, sep, tail]
 *     str.rpartition(regexp)          -> [head, match, tail]
 *
 *  Searches <i>sep</i> or pattern (<i>regexp</i>) in the string from the end
 *  of the string, and returns the part before it, the match, and the part
 *  after it.
 *  If it is not found, returns two empty strings and <i>str</i>.
 *
 *     "hello".rpartition("l")         #=> ["hel", "l", "o"]
 *     "hello".rpartition("x")         #=> ["", "", "hello"]
 *     "hello".rpartition(/.l/)        #=> ["he", "ll", "o"]
 */

static VALUE
rb_str_rpartition(VALUE str, VALUE sep)
{
    long pos = RSTRING_LEN(str);
    int regex = FALSE;

    if (RB_TYPE_P(sep, T_REGEXP)) {
	pos = rb_reg_search(sep, str, pos, 1);
	regex = TRUE;
    }
    else {
	VALUE tmp;

	tmp = rb_check_string_type(sep);
	if (NIL_P(tmp)) {
	    rb_raise(rb_eTypeError, "type mismatch: %s given",
		     rb_obj_classname(sep));
	}
	sep = tmp;
	pos = rb_str_sublen(str, pos);
	pos = rb_str_rindex(str, sep, pos);
    }
    if (pos < 0) {
	return rb_ary_new3(3, str_new_empty(str), str_new_empty(str), str);
    }
    if (regex) {
	sep = rb_reg_nth_match(0, rb_backref_get());
    }
    else {
	pos = rb_str_offset(str, pos);
    }
    return rb_ary_new3(3, rb_str_subseq(str, 0, pos),
		          sep,
		          rb_str_subseq(str, pos+RSTRING_LEN(sep),
					RSTRING_LEN(str)-pos-RSTRING_LEN(sep)));
}

/*
 *  call-seq:
 *     str.start_with?([prefixes]+)   -> true or false
 *
 *  Returns true if +str+ starts with one of the +prefixes+ given.
 *
 *    "hello".start_with?("hell")               #=> true
 *
 *    # returns true if one of the prefixes matches.
 *    "hello".start_with?("heaven", "hell")     #=> true
 *    "hello".start_with?("heaven", "paradise") #=> false
 */

static VALUE
rb_str_start_with(int argc, VALUE *argv, VALUE str)
{
    int i;

    for (i=0; i<argc; i++) {
	VALUE tmp = argv[i];
	StringValue(tmp);
	rb_enc_check(str, tmp);
	if (RSTRING_LEN(str) < RSTRING_LEN(tmp)) continue;
	if (memcmp(RSTRING_PTR(str), RSTRING_PTR(tmp), RSTRING_LEN(tmp)) == 0)
	    return Qtrue;
    }
    return Qfalse;
}

/*
 *  call-seq:
 *     str.end_with?([suffixes]+)   -> true or false
 *
 *  Returns true if +str+ ends with one of the +suffixes+ given.
 *
 *    "hello".end_with?("ello")               #=> true
 *
 *    # returns true if one of the +suffixes+ matches.
 *    "hello".end_with?("heaven", "ello")     #=> true
 *    "hello".end_with?("heaven", "paradise") #=> false
 */

static VALUE
rb_str_end_with(int argc, VALUE *argv, VALUE str)
{
    int i;
    char *p, *s, *e;
    rb_encoding *enc;

    for (i=0; i<argc; i++) {
	VALUE tmp = argv[i];
	StringValue(tmp);
	enc = rb_enc_check(str, tmp);
	if (RSTRING_LEN(str) < RSTRING_LEN(tmp)) continue;
	p = RSTRING_PTR(str);
        e = p + RSTRING_LEN(str);
	s = e - RSTRING_LEN(tmp);
	if (rb_enc_left_char_head(p, s, e, enc) != s)
	    continue;
	if (memcmp(s, RSTRING_PTR(tmp), RSTRING_LEN(tmp)) == 0)
	    return Qtrue;
    }
    return Qfalse;
}

void
rb_str_setter(VALUE val, ID id, VALUE *var)
{
    if (!NIL_P(val) && !RB_TYPE_P(val, T_STRING)) {
	rb_raise(rb_eTypeError, "value of %"PRIsVALUE" must be String", rb_id2str(id));
    }
    *var = val;
}


/*
 *  call-seq:
 *     str.force_encoding(encoding)   -> str
 *
 *  Changes the encoding to +encoding+ and returns self.
 */

static VALUE
rb_str_force_encoding(VALUE str, VALUE enc)
{
    str_modifiable(str);
    rb_enc_associate(str, rb_to_encoding(enc));
    ENC_CODERANGE_CLEAR(str);
    return str;
}

/*
 *  call-seq:
 *     str.b   -> str
 *
 *  Returns a copied string whose encoding is ASCII-8BIT.
 */

static VALUE
rb_str_b(VALUE str)
{
    VALUE str2 = str_alloc(rb_cString);
    str_replace_shared_without_enc(str2, str);
    OBJ_INFECT_RAW(str2, str);
    ENC_CODERANGE_CLEAR(str2);
    return str2;
}

/*
 *  call-seq:
 *     str.valid_encoding?  -> true or false
 *
 *  Returns true for a string which encoded correctly.
 *
 *    "\xc2\xa1".force_encoding("UTF-8").valid_encoding?  #=> true
 *    "\xc2".force_encoding("UTF-8").valid_encoding?      #=> false
 *    "\x80".force_encoding("UTF-8").valid_encoding?      #=> false
 */

static VALUE
rb_str_valid_encoding_p(VALUE str)
{
    int cr = rb_enc_str_coderange(str);

    return cr == ENC_CODERANGE_BROKEN ? Qfalse : Qtrue;
}

/*
 *  call-seq:
 *     str.ascii_only?  -> true or false
 *
 *  Returns true for a string which has only ASCII characters.
 *
 *    "abc".force_encoding("UTF-8").ascii_only?          #=> true
 *    "abc\u{6666}".force_encoding("UTF-8").ascii_only?  #=> false
 */

static VALUE
rb_str_is_ascii_only_p(VALUE str)
{
    int cr = rb_enc_str_coderange(str);

    return cr == ENC_CODERANGE_7BIT ? Qtrue : Qfalse;
}

/**
 * Shortens _str_ and adds three dots, an ellipsis, if it is longer
 * than _len_ characters.
 *
 * \param str	the string to ellipsize.
 * \param len	the maximum string length.
 * \return	the ellipsized string.
 * \pre 	_len_ must not be negative.
 * \post	the length of the returned string in characters is less than or equal to _len_.
 * \post	If the length of _str_ is less than or equal _len_, returns _str_ itself.
 * \post	the encoding of returned string is equal to the encoding of _str_.
 * \post	the class of returned string is equal to the class of _str_.
 * \note	the length is counted in characters.
 */
VALUE
rb_str_ellipsize(VALUE str, long len)
{
    static const char ellipsis[] = "...";
    const long ellipsislen = sizeof(ellipsis) - 1;
    rb_encoding *const enc = rb_enc_get(str);
    const long blen = RSTRING_LEN(str);
    const char *const p = RSTRING_PTR(str), *e = p + blen;
    VALUE estr, ret = 0;

    if (len < 0) rb_raise(rb_eIndexError, "negative length %ld", len);
    if (len * rb_enc_mbminlen(enc) >= blen ||
	(e = rb_enc_nth(p, e, len, enc)) - p == blen) {
	ret = str;
    }
    else if (len <= ellipsislen ||
	     !(e = rb_enc_step_back(p, e, e, len = ellipsislen, enc))) {
	if (rb_enc_asciicompat(enc)) {
	    ret = rb_str_new_with_class(str, ellipsis, len);
	    rb_enc_associate(ret, enc);
	}
	else {
	    estr = rb_usascii_str_new(ellipsis, len);
	    ret = rb_str_encode(estr, rb_enc_from_encoding(enc), 0, Qnil);
	}
    }
    else if (ret = rb_str_subseq(str, 0, e - p), rb_enc_asciicompat(enc)) {
	rb_str_cat(ret, ellipsis, ellipsislen);
    }
    else {
	estr = rb_str_encode(rb_usascii_str_new(ellipsis, ellipsislen),
			     rb_enc_from_encoding(enc), 0, Qnil);
	rb_str_append(ret, estr);
    }
    return ret;
}

static VALUE
str_compat_and_valid(VALUE str, rb_encoding *enc)
{
    int cr;
    str = StringValue(str);
    cr = rb_enc_str_coderange(str);
    if (cr == ENC_CODERANGE_BROKEN) {
	rb_raise(rb_eArgError, "replacement must be valid byte sequence '%+"PRIsVALUE"'", str);
    }
    else if (cr == ENC_CODERANGE_7BIT) {
	rb_encoding *e = STR_ENC_GET(str);
	if (!rb_enc_asciicompat(enc)) {
	    rb_raise(rb_eEncCompatError, "incompatible character encodings: %s and %s",
		    rb_enc_name(enc), rb_enc_name(e));
	}
    }
    else { /* ENC_CODERANGE_VALID */
	rb_encoding *e = STR_ENC_GET(str);
	if (enc != e) {
	    rb_raise(rb_eEncCompatError, "incompatible character encodings: %s and %s",
		    rb_enc_name(enc), rb_enc_name(e));
	}
    }
    return str;
}

/**
 * @param str the string to be scrubbed
 * @param repl the replacement character
 * @return If given string is invalid, returns a new string. Otherwise, returns Qnil.
 */
VALUE
rb_str_scrub(VALUE str, VALUE repl)
{
    int cr = ENC_CODERANGE(str);
    rb_encoding *enc;
    int encidx;

    if (ENC_CODERANGE_CLEAN_P(cr))
	return Qnil;

    enc = STR_ENC_GET(str);
    if (!NIL_P(repl)) {
	repl = str_compat_and_valid(repl, enc);
    }

    if (rb_enc_dummy_p(enc)) {
	return Qnil;
    }
    encidx = rb_enc_to_index(enc);

#define DEFAULT_REPLACE_CHAR(str) do { \
	static const char replace[sizeof(str)-1] = str; \
	rep = replace; replen = (int)sizeof(replace); \
    } while (0)

    if (rb_enc_asciicompat(enc)) {
	const char *p = RSTRING_PTR(str);
	const char *e = RSTRING_END(str);
	const char *p1 = p;
	const char *rep;
	long replen;
	int rep7bit_p;
	VALUE buf = Qnil;
	if (rb_block_given_p()) {
	    rep = NULL;
	    replen = 0;
	    rep7bit_p = FALSE;
	}
	else if (!NIL_P(repl)) {
	    rep = RSTRING_PTR(repl);
	    replen = RSTRING_LEN(repl);
	    rep7bit_p = (ENC_CODERANGE(repl) == ENC_CODERANGE_7BIT);
	}
	else if (encidx == rb_utf8_encindex()) {
	    DEFAULT_REPLACE_CHAR("\xEF\xBF\xBD");
	    rep7bit_p = FALSE;
	}
	else {
	    DEFAULT_REPLACE_CHAR("?");
	    rep7bit_p = TRUE;
	}
	cr = ENC_CODERANGE_7BIT;

	p = search_nonascii(p, e);
	if (!p) {
	    p = e;
	}
	while (p < e) {
	    int ret = rb_enc_precise_mbclen(p, e, enc);
	    if (MBCLEN_NEEDMORE_P(ret)) {
		break;
	    }
	    else if (MBCLEN_CHARFOUND_P(ret)) {
		cr = ENC_CODERANGE_VALID;
		p += MBCLEN_CHARFOUND_LEN(ret);
	    }
	    else if (MBCLEN_INVALID_P(ret)) {
		/*
		 * p1~p: valid ascii/multibyte chars
		 * p ~e: invalid bytes + unknown bytes
		 */
		long clen = rb_enc_mbmaxlen(enc);
		if (NIL_P(buf)) buf = rb_str_buf_new(RSTRING_LEN(str));
		if (p > p1) {
		    rb_str_buf_cat(buf, p1, p - p1);
		}

		if (e - p < clen) clen = e - p;
		if (clen <= 2) {
		    clen = 1;
		}
		else {
		    const char *q = p;
		    clen--;
		    for (; clen > 1; clen--) {
			ret = rb_enc_precise_mbclen(q, q + clen, enc);
			if (MBCLEN_NEEDMORE_P(ret)) break;
			if (MBCLEN_INVALID_P(ret)) continue;
			UNREACHABLE;
		    }
		}
		if (rep) {
		    rb_str_buf_cat(buf, rep, replen);
		    if (!rep7bit_p) cr = ENC_CODERANGE_VALID;
		}
		else {
		    repl = rb_yield(rb_enc_str_new(p, clen, enc));
		    repl = str_compat_and_valid(repl, enc);
		    rb_str_buf_cat(buf, RSTRING_PTR(repl), RSTRING_LEN(repl));
		    if (ENC_CODERANGE(repl) == ENC_CODERANGE_VALID)
			cr = ENC_CODERANGE_VALID;
		}
		p += clen;
		p1 = p;
		p = search_nonascii(p, e);
		if (!p) {
		    p = e;
		    break;
		}
	    }
	    else {
		UNREACHABLE;
	    }
	}
	if (NIL_P(buf)) {
	    if (p == e) {
		ENC_CODERANGE_SET(str, cr);
		return Qnil;
	    }
	    buf = rb_str_buf_new(RSTRING_LEN(str));
	}
	if (p1 < p) {
	    rb_str_buf_cat(buf, p1, p - p1);
	}
	if (p < e) {
	    if (rep) {
		rb_str_buf_cat(buf, rep, replen);
		if (!rep7bit_p) cr = ENC_CODERANGE_VALID;
	    }
	    else {
		repl = rb_yield(rb_enc_str_new(p, e-p, enc));
		repl = str_compat_and_valid(repl, enc);
		rb_str_buf_cat(buf, RSTRING_PTR(repl), RSTRING_LEN(repl));
		if (ENC_CODERANGE(repl) == ENC_CODERANGE_VALID)
		    cr = ENC_CODERANGE_VALID;
	    }
	}
	ENCODING_CODERANGE_SET(buf, rb_enc_to_index(enc), cr);
	return buf;
    }
    else {
	/* ASCII incompatible */
	const char *p = RSTRING_PTR(str);
	const char *e = RSTRING_END(str);
	const char *p1 = p;
	VALUE buf = Qnil;
	const char *rep;
	long replen;
	long mbminlen = rb_enc_mbminlen(enc);
	if (!NIL_P(repl)) {
	    rep = RSTRING_PTR(repl);
	    replen = RSTRING_LEN(repl);
	}
	else if (encidx == ENCINDEX_UTF_16BE) {
	    DEFAULT_REPLACE_CHAR("\xFF\xFD");
	}
	else if (encidx == ENCINDEX_UTF_16LE) {
	    DEFAULT_REPLACE_CHAR("\xFD\xFF");
	}
	else if (encidx == ENCINDEX_UTF_32BE) {
	    DEFAULT_REPLACE_CHAR("\x00\x00\xFF\xFD");
	}
	else if (encidx == ENCINDEX_UTF_32LE) {
	    DEFAULT_REPLACE_CHAR("\xFD\xFF\x00\x00");
	}
	else {
	    DEFAULT_REPLACE_CHAR("?");
	}

	while (p < e) {
	    int ret = rb_enc_precise_mbclen(p, e, enc);
	    if (MBCLEN_NEEDMORE_P(ret)) {
		break;
	    }
	    else if (MBCLEN_CHARFOUND_P(ret)) {
		p += MBCLEN_CHARFOUND_LEN(ret);
	    }
	    else if (MBCLEN_INVALID_P(ret)) {
		const char *q = p;
		long clen = rb_enc_mbmaxlen(enc);
		if (NIL_P(buf)) buf = rb_str_buf_new(RSTRING_LEN(str));
		if (p > p1) rb_str_buf_cat(buf, p1, p - p1);

		if (e - p < clen) clen = e - p;
		if (clen <= mbminlen * 2) {
		    clen = mbminlen;
		}
		else {
		    clen -= mbminlen;
		    for (; clen > mbminlen; clen-=mbminlen) {
			ret = rb_enc_precise_mbclen(q, q + clen, enc);
			if (MBCLEN_NEEDMORE_P(ret)) break;
			if (MBCLEN_INVALID_P(ret)) continue;
			UNREACHABLE;
		    }
		}
		if (rep) {
		    rb_str_buf_cat(buf, rep, replen);
		}
		else {
		    repl = rb_yield(rb_enc_str_new(p, e-p, enc));
		    repl = str_compat_and_valid(repl, enc);
		    rb_str_buf_cat(buf, RSTRING_PTR(repl), RSTRING_LEN(repl));
		}
		p += clen;
		p1 = p;
	    }
	    else {
		UNREACHABLE;
	    }
	}
	if (NIL_P(buf)) {
	    if (p == e) {
		ENC_CODERANGE_SET(str, ENC_CODERANGE_VALID);
		return Qnil;
	    }
	    buf = rb_str_buf_new(RSTRING_LEN(str));
	}
	if (p1 < p) {
	    rb_str_buf_cat(buf, p1, p - p1);
	}
	if (p < e) {
	    if (rep) {
		rb_str_buf_cat(buf, rep, replen);
	    }
	    else {
		repl = rb_yield(rb_enc_str_new(p, e-p, enc));
		repl = str_compat_and_valid(repl, enc);
		rb_str_buf_cat(buf, RSTRING_PTR(repl), RSTRING_LEN(repl));
	    }
	}
	ENCODING_CODERANGE_SET(buf, rb_enc_to_index(enc), ENC_CODERANGE_VALID);
	return buf;
    }
}

/*
 *  call-seq:
 *    str.scrub -> new_str
 *    str.scrub(repl) -> new_str
 *    str.scrub{|bytes|} -> new_str
 *
 *  If the string is invalid byte sequence then replace invalid bytes with given replacement
 *  character, else returns self.
 *  If block is given, replace invalid bytes with returned value of the block.
 *
 *     "abc\u3042\x81".scrub #=> "abc\u3042\uFFFD"
 *     "abc\u3042\x81".scrub("*") #=> "abc\u3042*"
 *     "abc\u3042\xE3\x80".scrub{|bytes| '<'+bytes.unpack('H*')[0]+'>' } #=> "abc\u3042<e380>"
 */
static VALUE
str_scrub(int argc, VALUE *argv, VALUE str)
{
    VALUE repl = argc ? (rb_check_arity(argc, 0, 1), argv[0]) : Qnil;
    VALUE new = rb_str_scrub(str, repl);
    return NIL_P(new) ? rb_str_dup(str): new;
}

/*
 *  call-seq:
 *    str.scrub! -> str
 *    str.scrub!(repl) -> str
 *    str.scrub!{|bytes|} -> str
 *
 *  If the string is invalid byte sequence then replace invalid bytes with given replacement
 *  character, else returns self.
 *  If block is given, replace invalid bytes with returned value of the block.
 *
 *     "abc\u3042\x81".scrub! #=> "abc\u3042\uFFFD"
 *     "abc\u3042\x81".scrub!("*") #=> "abc\u3042*"
 *     "abc\u3042\xE3\x80".scrub!{|bytes| '<'+bytes.unpack('H*')[0]+'>' } #=> "abc\u3042<e380>"
 */
static VALUE
str_scrub_bang(int argc, VALUE *argv, VALUE str)
{
    VALUE repl = argc ? (rb_check_arity(argc, 0, 1), argv[0]) : Qnil;
    VALUE new = rb_str_scrub(str, repl);
    if (!NIL_P(new)) rb_str_replace(str, new);
    return str;
}

/**********************************************************************
 * Document-class: Symbol
 *
 *  <code>Symbol</code> objects represent names and some strings
 *  inside the Ruby
 *  interpreter. They are generated using the <code>:name</code> and
 *  <code>:"string"</code> literals
 *  syntax, and by the various <code>to_sym</code> methods. The same
 *  <code>Symbol</code> object will be created for a given name or string
 *  for the duration of a program's execution, regardless of the context
 *  or meaning of that name. Thus if <code>Fred</code> is a constant in
 *  one context, a method in another, and a class in a third, the
 *  <code>Symbol</code> <code>:Fred</code> will be the same object in
 *  all three contexts.
 *
 *     module One
 *       class Fred
 *       end
 *       $f1 = :Fred
 *     end
 *     module Two
 *       Fred = 1
 *       $f2 = :Fred
 *     end
 *     def Fred()
 *     end
 *     $f3 = :Fred
 *     $f1.object_id   #=> 2514190
 *     $f2.object_id   #=> 2514190
 *     $f3.object_id   #=> 2514190
 *
 */


/*
 *  call-seq:
 *     sym == obj   -> true or false
 *
 *  Equality---If <i>sym</i> and <i>obj</i> are exactly the same
 *  symbol, returns <code>true</code>.
 */

#define sym_equal rb_obj_equal

static int
sym_printable(const char *s, const char *send, rb_encoding *enc)
{
    while (s < send) {
	int n;
	int c = rb_enc_precise_mbclen(s, send, enc);

	if (!MBCLEN_CHARFOUND_P(c)) return FALSE;
	n = MBCLEN_CHARFOUND_LEN(c);
	c = rb_enc_mbc_to_codepoint(s, send, enc);
	if (!rb_enc_isprint(c, enc)) return FALSE;
	s += n;
    }
    return TRUE;
}

int
rb_str_symname_p(VALUE sym)
{
    rb_encoding *enc;
    const char *ptr;
    long len;
    rb_encoding *resenc = rb_default_internal_encoding();

    if (resenc == NULL) resenc = rb_default_external_encoding();
    enc = STR_ENC_GET(sym);
    ptr = RSTRING_PTR(sym);
    len = RSTRING_LEN(sym);
    if ((resenc != enc && !rb_str_is_ascii_only_p(sym)) || len != (long)strlen(ptr) ||
	!rb_enc_symname_p(ptr, enc) || !sym_printable(ptr, ptr + len, enc)) {
	return FALSE;
    }
    return TRUE;
}

VALUE
rb_str_quote_unprintable(VALUE str)
{
    rb_encoding *enc;
    const char *ptr;
    long len;
    rb_encoding *resenc;

    Check_Type(str, T_STRING);
    resenc = rb_default_internal_encoding();
    if (resenc == NULL) resenc = rb_default_external_encoding();
    enc = STR_ENC_GET(str);
    ptr = RSTRING_PTR(str);
    len = RSTRING_LEN(str);
    if ((resenc != enc && !rb_str_is_ascii_only_p(str)) ||
	!sym_printable(ptr, ptr + len, enc)) {
	return rb_str_inspect(str);
    }
    return str;
}

VALUE
rb_id_quote_unprintable(ID id)
{
    return rb_str_quote_unprintable(rb_id2str(id));
}

/*
 *  call-seq:
 *     sym.inspect    -> string
 *
 *  Returns the representation of <i>sym</i> as a symbol literal.
 *
 *     :fred.inspect   #=> ":fred"
 */

static VALUE
sym_inspect(VALUE sym)
{
    VALUE str = rb_sym2str(sym);
    const char *ptr;
    long len;
    char *dest;

    if (!rb_str_symname_p(str)) {
	str = rb_str_inspect(str);
	len = RSTRING_LEN(str);
	rb_str_resize(str, len + 1);
	dest = RSTRING_PTR(str);
	memmove(dest + 1, dest, len);
    }
    else {
	rb_encoding *enc = STR_ENC_GET(str);
	RSTRING_GETMEM(str, ptr, len);
	str = rb_enc_str_new(0, len + 1, enc);
	dest = RSTRING_PTR(str);
	memcpy(dest + 1, ptr, len);
    }
    dest[0] = ':';
    return str;
}


/*
 *  call-seq:
 *     sym.id2name   -> string
 *     sym.to_s      -> string
 *
 *  Returns the name or string corresponding to <i>sym</i>.
 *
 *     :fred.id2name   #=> "fred"
 */


VALUE
rb_sym_to_s(VALUE sym)
{
    return str_new_shared(rb_cString, rb_sym2str(sym));
}


/*
 * call-seq:
 *   sym.to_sym   -> sym
 *   sym.intern   -> sym
 *
 * In general, <code>to_sym</code> returns the <code>Symbol</code> corresponding
 * to an object. As <i>sym</i> is already a symbol, <code>self</code> is returned
 * in this case.
 */

static VALUE
sym_to_sym(VALUE sym)
{
    return sym;
}

VALUE
rb_sym_proc_call(VALUE args, VALUE sym, int argc, const VALUE *argv, VALUE passed_proc)
{
    VALUE obj;

    if (argc < 1) {
	rb_raise(rb_eArgError, "no receiver given");
    }
    obj = argv[0];
    return rb_funcall_with_block(obj, (ID)sym, argc - 1, argv + 1, passed_proc);
}

#define sym_to_proc rb_sym_to_proc
/*
 * call-seq:
 *   sym.to_proc
 *
 * Returns a _Proc_ object which respond to the given method by _sym_.
 *
 *   (1..3).collect(&:to_s)  #=> ["1", "2", "3"]
 */

VALUE
sym_to_proc(VALUE sym)
{
    static VALUE sym_proc_cache = Qfalse;
    enum {SYM_PROC_CACHE_SIZE = 67};
    VALUE proc;
    long index;
    ID id;
    VALUE *aryp;

    if (!sym_proc_cache) {
	sym_proc_cache = rb_ary_tmp_new(SYM_PROC_CACHE_SIZE * 2);
	rb_gc_register_mark_object(sym_proc_cache);
	rb_ary_store(sym_proc_cache, SYM_PROC_CACHE_SIZE*2 - 1, Qnil);
    }

    id = SYM2ID(sym);
    index = (id % SYM_PROC_CACHE_SIZE) << 1;

    aryp = RARRAY_PTR(sym_proc_cache);
    if (aryp[index] == sym) {
	return aryp[index + 1];
    }
    else {
	proc = rb_proc_new(rb_sym_proc_call, (VALUE)id);
	rb_block_clear_env_self(proc);
	aryp[index] = sym;
	aryp[index + 1] = proc;
	return proc;
    }
}

/*
 * call-seq:
 *
 *   sym.succ
 *
 * Same as <code>sym.to_s.succ.intern</code>.
 */

static VALUE
sym_succ(VALUE sym)
{
    return rb_str_intern(rb_str_succ(rb_sym2str(sym)));
}

/*
 * call-seq:
 *
 *   symbol <=> other_symbol       -> -1, 0, +1 or nil
 *
 * Compares +symbol+ with +other_symbol+ after calling #to_s on each of the
 * symbols. Returns -1, 0, +1 or nil depending on whether +symbol+ is less
 * than, equal to, or greater than +other_symbol+.
 *
 *  +nil+ is returned if the two values are incomparable.
 *
 * See String#<=> for more information.
 */

static VALUE
sym_cmp(VALUE sym, VALUE other)
{
    if (!SYMBOL_P(other)) {
	return Qnil;
    }
    return rb_str_cmp_m(rb_sym2str(sym), rb_sym2str(other));
}

/*
 * call-seq:
 *
 *   sym.casecmp(other)  -> -1, 0, +1 or nil
 *
 * Case-insensitive version of <code>Symbol#<=></code>.
 */

static VALUE
sym_casecmp(VALUE sym, VALUE other)
{
    if (!SYMBOL_P(other)) {
	return Qnil;
    }
    return rb_str_casecmp(rb_sym2str(sym), rb_sym2str(other));
}

/*
 * call-seq:
 *   sym =~ obj   -> fixnum or nil
 *   sym.match(obj)   -> fixnum or nil
 *
 * Returns <code>sym.to_s =~ obj</code>.
 */

static VALUE
sym_match(VALUE sym, VALUE other)
{
    return rb_str_match(rb_sym2str(sym), other);
}

/*
 * call-seq:
 *   sym[idx]      -> char
 *   sym[b, n]     -> string
 *   sym.slice(idx)      -> char
 *   sym.slice(b, n)     -> string
 *
 * Returns <code>sym.to_s[]</code>.
 */

static VALUE
sym_aref(int argc, VALUE *argv, VALUE sym)
{
    return rb_str_aref_m(argc, argv, rb_sym2str(sym));
}

/*
 * call-seq:
 *   sym.length    -> integer
 *   sym.size    -> integer
 *
 * Same as <code>sym.to_s.length</code>.
 */

static VALUE
sym_length(VALUE sym)
{
    return rb_str_length(rb_sym2str(sym));
}

/*
 * call-seq:
 *   sym.empty?   -> true or false
 *
 * Returns that _sym_ is :"" or not.
 */

static VALUE
sym_empty(VALUE sym)
{
    return rb_str_empty(rb_sym2str(sym));
}

/*
 * call-seq:
 *   sym.upcase    -> symbol
 *
 * Same as <code>sym.to_s.upcase.intern</code>.
 */

static VALUE
sym_upcase(VALUE sym)
{
    return rb_str_intern(rb_str_upcase(rb_sym2str(sym)));
}

/*
 * call-seq:
 *   sym.downcase  -> symbol
 *
 * Same as <code>sym.to_s.downcase.intern</code>.
 */

static VALUE
sym_downcase(VALUE sym)
{
    return rb_str_intern(rb_str_downcase(rb_sym2str(sym)));
}

/*
 * call-seq:
 *   sym.capitalize  -> symbol
 *
 * Same as <code>sym.to_s.capitalize.intern</code>.
 */

static VALUE
sym_capitalize(VALUE sym)
{
    return rb_str_intern(rb_str_capitalize(rb_sym2str(sym)));
}

/*
 * call-seq:
 *   sym.swapcase  -> symbol
 *
 * Same as <code>sym.to_s.swapcase.intern</code>.
 */

static VALUE
sym_swapcase(VALUE sym)
{
    return rb_str_intern(rb_str_swapcase(rb_sym2str(sym)));
}

/*
 * call-seq:
 *   sym.encoding   -> encoding
 *
 * Returns the Encoding object that represents the encoding of _sym_.
 */

static VALUE
sym_encoding(VALUE sym)
{
    return rb_obj_encoding(rb_sym2str(sym));
}

static VALUE
string_for_symbol(VALUE name)
{
    if (!RB_TYPE_P(name, T_STRING)) {
	VALUE tmp = rb_check_string_type(name);
	if (NIL_P(tmp)) {
	    rb_raise(rb_eTypeError, "%+"PRIsVALUE" is not a symbol",
		     name);
	}
	name = tmp;
    }
    return name;
}

ID
rb_to_id(VALUE name)
{
    if (SYMBOL_P(name)) {
	return SYM2ID(name);
    }
    name = string_for_symbol(name);
    return rb_intern_str(name);
}

VALUE
rb_to_symbol(VALUE name)
{
    if (SYMBOL_P(name)) {
	return name;
    }
    name = string_for_symbol(name);
    return rb_str_intern(name);
}

/*
 *  A <code>String</code> object holds and manipulates an arbitrary sequence of
 *  bytes, typically representing characters. String objects may be created
 *  using <code>String::new</code> or as literals.
 *
 *  Because of aliasing issues, users of strings should be aware of the methods
 *  that modify the contents of a <code>String</code> object.  Typically,
 *  methods with names ending in ``!'' modify their receiver, while those
 *  without a ``!'' return a new <code>String</code>.  However, there are
 *  exceptions, such as <code>String#[]=</code>.
 *
 */

void
Init_String(void)
{
#undef rb_intern
#define rb_intern(str) rb_intern_const(str)

    rb_cString  = rb_define_class("String", rb_cObject);
    rb_include_module(rb_cString, rb_mComparable);
    rb_define_alloc_func(rb_cString, empty_str_alloc);
    rb_define_singleton_method(rb_cString, "try_convert", rb_str_s_try_convert, 1);
    rb_define_method(rb_cString, "initialize", rb_str_init, -1);
    rb_define_method(rb_cString, "initialize_copy", rb_str_replace, 1);
    rb_define_method(rb_cString, "<=>", rb_str_cmp_m, 1);
    rb_define_method(rb_cString, "==", rb_str_equal, 1);
    rb_define_method(rb_cString, "===", rb_str_equal, 1);
    rb_define_method(rb_cString, "eql?", rb_str_eql, 1);
    rb_define_method(rb_cString, "hash", rb_str_hash_m, 0);
    rb_define_method(rb_cString, "casecmp", rb_str_casecmp, 1);
    rb_define_method(rb_cString, "+", rb_str_plus, 1);
    rb_define_method(rb_cString, "*", rb_str_times, 1);
    rb_define_method(rb_cString, "%", rb_str_format_m, 1);
    rb_define_method(rb_cString, "[]", rb_str_aref_m, -1);
    rb_define_method(rb_cString, "[]=", rb_str_aset_m, -1);
    rb_define_method(rb_cString, "insert", rb_str_insert, 2);
    rb_define_method(rb_cString, "length", rb_str_length, 0);
    rb_define_method(rb_cString, "size", rb_str_length, 0);
    rb_define_method(rb_cString, "bytesize", rb_str_bytesize, 0);
    rb_define_method(rb_cString, "empty?", rb_str_empty, 0);
    rb_define_method(rb_cString, "=~", rb_str_match, 1);
    rb_define_method(rb_cString, "match", rb_str_match_m, -1);
    rb_define_method(rb_cString, "succ", rb_str_succ, 0);
    rb_define_method(rb_cString, "succ!", rb_str_succ_bang, 0);
    rb_define_method(rb_cString, "next", rb_str_succ, 0);
    rb_define_method(rb_cString, "next!", rb_str_succ_bang, 0);
    rb_define_method(rb_cString, "upto", rb_str_upto, -1);
    rb_define_method(rb_cString, "index", rb_str_index_m, -1);
    rb_define_method(rb_cString, "rindex", rb_str_rindex_m, -1);
    rb_define_method(rb_cString, "replace", rb_str_replace, 1);
    rb_define_method(rb_cString, "clear", rb_str_clear, 0);
    rb_define_method(rb_cString, "chr", rb_str_chr, 0);
    rb_define_method(rb_cString, "getbyte", rb_str_getbyte, 1);
    rb_define_method(rb_cString, "setbyte", rb_str_setbyte, 2);
    rb_define_method(rb_cString, "byteslice", rb_str_byteslice, -1);
    rb_define_method(rb_cString, "scrub", str_scrub, -1);
    rb_define_method(rb_cString, "scrub!", str_scrub_bang, -1);
    rb_define_method(rb_cString, "freeze", rb_str_freeze, 0);

    rb_define_method(rb_cString, "to_i", rb_str_to_i, -1);
    rb_define_method(rb_cString, "to_f", rb_str_to_f, 0);
    rb_define_method(rb_cString, "to_s", rb_str_to_s, 0);
    rb_define_method(rb_cString, "to_str", rb_str_to_s, 0);
    rb_define_method(rb_cString, "inspect", rb_str_inspect, 0);
    rb_define_method(rb_cString, "dump", rb_str_dump, 0);

    rb_define_method(rb_cString, "upcase", rb_str_upcase, 0);
    rb_define_method(rb_cString, "downcase", rb_str_downcase, 0);
    rb_define_method(rb_cString, "capitalize", rb_str_capitalize, 0);
    rb_define_method(rb_cString, "swapcase", rb_str_swapcase, 0);

    rb_define_method(rb_cString, "upcase!", rb_str_upcase_bang, 0);
    rb_define_method(rb_cString, "downcase!", rb_str_downcase_bang, 0);
    rb_define_method(rb_cString, "capitalize!", rb_str_capitalize_bang, 0);
    rb_define_method(rb_cString, "swapcase!", rb_str_swapcase_bang, 0);

    rb_define_method(rb_cString, "hex", rb_str_hex, 0);
    rb_define_method(rb_cString, "oct", rb_str_oct, 0);
    rb_define_method(rb_cString, "split", rb_str_split_m, -1);
    rb_define_method(rb_cString, "lines", rb_str_lines, -1);
    rb_define_method(rb_cString, "bytes", rb_str_bytes, 0);
    rb_define_method(rb_cString, "chars", rb_str_chars, 0);
    rb_define_method(rb_cString, "codepoints", rb_str_codepoints, 0);
    rb_define_method(rb_cString, "reverse", rb_str_reverse, 0);
    rb_define_method(rb_cString, "reverse!", rb_str_reverse_bang, 0);
    rb_define_method(rb_cString, "concat", rb_str_concat, 1);
    rb_define_method(rb_cString, "<<", rb_str_concat, 1);
    rb_define_method(rb_cString, "prepend", rb_str_prepend, 1);
    rb_define_method(rb_cString, "crypt", rb_str_crypt, 1);
    rb_define_method(rb_cString, "intern", rb_str_intern, 0); /* in symbol.c */
    rb_define_method(rb_cString, "to_sym", rb_str_intern, 0); /* in symbol.c */
    rb_define_method(rb_cString, "ord", rb_str_ord, 0);

    rb_define_method(rb_cString, "include?", rb_str_include, 1);
    rb_define_method(rb_cString, "start_with?", rb_str_start_with, -1);
    rb_define_method(rb_cString, "end_with?", rb_str_end_with, -1);

    rb_define_method(rb_cString, "scan", rb_str_scan, 1);

    rb_define_method(rb_cString, "ljust", rb_str_ljust, -1);
    rb_define_method(rb_cString, "rjust", rb_str_rjust, -1);
    rb_define_method(rb_cString, "center", rb_str_center, -1);

    rb_define_method(rb_cString, "sub", rb_str_sub, -1);
    rb_define_method(rb_cString, "gsub", rb_str_gsub, -1);
    rb_define_method(rb_cString, "chop", rb_str_chop, 0);
    rb_define_method(rb_cString, "chomp", rb_str_chomp, -1);
    rb_define_method(rb_cString, "strip", rb_str_strip, 0);
    rb_define_method(rb_cString, "lstrip", rb_str_lstrip, 0);
    rb_define_method(rb_cString, "rstrip", rb_str_rstrip, 0);

    rb_define_method(rb_cString, "sub!", rb_str_sub_bang, -1);
    rb_define_method(rb_cString, "gsub!", rb_str_gsub_bang, -1);
    rb_define_method(rb_cString, "chop!", rb_str_chop_bang, 0);
    rb_define_method(rb_cString, "chomp!", rb_str_chomp_bang, -1);
    rb_define_method(rb_cString, "strip!", rb_str_strip_bang, 0);
    rb_define_method(rb_cString, "lstrip!", rb_str_lstrip_bang, 0);
    rb_define_method(rb_cString, "rstrip!", rb_str_rstrip_bang, 0);

    rb_define_method(rb_cString, "tr", rb_str_tr, 2);
    rb_define_method(rb_cString, "tr_s", rb_str_tr_s, 2);
    rb_define_method(rb_cString, "delete", rb_str_delete, -1);
    rb_define_method(rb_cString, "squeeze", rb_str_squeeze, -1);
    rb_define_method(rb_cString, "count", rb_str_count, -1);

    rb_define_method(rb_cString, "tr!", rb_str_tr_bang, 2);
    rb_define_method(rb_cString, "tr_s!", rb_str_tr_s_bang, 2);
    rb_define_method(rb_cString, "delete!", rb_str_delete_bang, -1);
    rb_define_method(rb_cString, "squeeze!", rb_str_squeeze_bang, -1);

    rb_define_method(rb_cString, "each_line", rb_str_each_line, -1);
    rb_define_method(rb_cString, "each_byte", rb_str_each_byte, 0);
    rb_define_method(rb_cString, "each_char", rb_str_each_char, 0);
    rb_define_method(rb_cString, "each_codepoint", rb_str_each_codepoint, 0);

    rb_define_method(rb_cString, "sum", rb_str_sum, -1);

    rb_define_method(rb_cString, "slice", rb_str_aref_m, -1);
    rb_define_method(rb_cString, "slice!", rb_str_slice_bang, -1);

    rb_define_method(rb_cString, "partition", rb_str_partition, 1);
    rb_define_method(rb_cString, "rpartition", rb_str_rpartition, 1);

    rb_define_method(rb_cString, "encoding", rb_obj_encoding, 0); /* in encoding.c */
    rb_define_method(rb_cString, "force_encoding", rb_str_force_encoding, 1);
    rb_define_method(rb_cString, "b", rb_str_b, 0);
    rb_define_method(rb_cString, "valid_encoding?", rb_str_valid_encoding_p, 0);
    rb_define_method(rb_cString, "ascii_only?", rb_str_is_ascii_only_p, 0);

    id_to_s = rb_intern("to_s");

    rb_fs = Qnil;
    rb_define_variable("$;", &rb_fs);
    rb_define_variable("$-F", &rb_fs);

    rb_cSymbol = rb_define_class("Symbol", rb_cObject);
    rb_include_module(rb_cSymbol, rb_mComparable);
    rb_undef_alloc_func(rb_cSymbol);
    rb_undef_method(CLASS_OF(rb_cSymbol), "new");
    rb_define_singleton_method(rb_cSymbol, "all_symbols", rb_sym_all_symbols, 0); /* in symbol.c */

    rb_define_method(rb_cSymbol, "==", sym_equal, 1);
    rb_define_method(rb_cSymbol, "===", sym_equal, 1);
    rb_define_method(rb_cSymbol, "inspect", sym_inspect, 0);
    rb_define_method(rb_cSymbol, "to_s", rb_sym_to_s, 0);
    rb_define_method(rb_cSymbol, "id2name", rb_sym_to_s, 0);
    rb_define_method(rb_cSymbol, "intern", sym_to_sym, 0);
    rb_define_method(rb_cSymbol, "to_sym", sym_to_sym, 0);
    rb_define_method(rb_cSymbol, "to_proc", sym_to_proc, 0);
    rb_define_method(rb_cSymbol, "succ", sym_succ, 0);
    rb_define_method(rb_cSymbol, "next", sym_succ, 0);

    rb_define_method(rb_cSymbol, "<=>", sym_cmp, 1);
    rb_define_method(rb_cSymbol, "casecmp", sym_casecmp, 1);
    rb_define_method(rb_cSymbol, "=~", sym_match, 1);

    rb_define_method(rb_cSymbol, "[]", sym_aref, -1);
    rb_define_method(rb_cSymbol, "slice", sym_aref, -1);
    rb_define_method(rb_cSymbol, "length", sym_length, 0);
    rb_define_method(rb_cSymbol, "size", sym_length, 0);
    rb_define_method(rb_cSymbol, "empty?", sym_empty, 0);
    rb_define_method(rb_cSymbol, "match", sym_match, 1);

    rb_define_method(rb_cSymbol, "upcase", sym_upcase, 0);
    rb_define_method(rb_cSymbol, "downcase", sym_downcase, 0);
    rb_define_method(rb_cSymbol, "capitalize", sym_capitalize, 0);
    rb_define_method(rb_cSymbol, "swapcase", sym_swapcase, 0);

    rb_define_method(rb_cSymbol, "encoding", sym_encoding, 0);

    assert(rb_vm_fstring_table());
    st_foreach(rb_vm_fstring_table(), fstring_set_class_i, rb_cString);
}
