/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "linker.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/dlext.h>
#include <android/api-level.h>

#include <bionic/pthread_internal.h>
#include "private/bionic_tls.h"
#include "private/ScopedPthreadMutexLocker.h"
#include "private/ThreadLocalBuffer.h"

/* This file hijacks the symbols stubbed out in libdl.so. */

extern NativeHookTable* nht;

static pthread_mutex_t g_dl_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static const char* __bionic_set_dlerror(char* new_value) {
  char** dlerror_slot = &reinterpret_cast<char**>(__get_tls())[TLS_SLOT_DLERROR];

  const char* old_value = *dlerror_slot;
  *dlerror_slot = new_value;
  return old_value;
}

static void __bionic_format_dlerror(const char* msg, const char* detail) {
  char* buffer = __get_thread()->dlerror_buffer;
  strlcpy(buffer, msg, __BIONIC_DLERROR_BUFFER_SIZE);
  if (detail != nullptr) {
    strlcat(buffer, ": ", __BIONIC_DLERROR_BUFFER_SIZE);
    strlcat(buffer, detail, __BIONIC_DLERROR_BUFFER_SIZE);
  }

  __bionic_set_dlerror(buffer);
}

const char* dlerror() {
  const char* old_value = __bionic_set_dlerror(nullptr);
  return old_value;
}

void android_get_LD_LIBRARY_PATH(char* buffer, size_t buffer_size) {
  ScopedPthreadMutexLocker locker(&g_dl_mutex);
  do_android_get_LD_LIBRARY_PATH(buffer, buffer_size);
}

void android_update_LD_LIBRARY_PATH(const char* ld_library_path) {
  ScopedPthreadMutexLocker locker(&g_dl_mutex);
  do_android_update_LD_LIBRARY_PATH(ld_library_path);
}

static void* dlopen_ext(const char* filename, int flags, const android_dlextinfo* extinfo) {
  ScopedPthreadMutexLocker locker(&g_dl_mutex);
  DL_WARN("[NATIVE HOOK] %s : %s\n", __func__, filename);
  soinfo* result = do_dlopen(filename, flags, extinfo);
  if (result == nullptr) {
    DL_WARN("[NATIVE HOOK] dlopen failed : %s\n", filename);
    __bionic_format_dlerror("dlopen failed", linker_get_error_buffer());
    return nullptr;
  }
  return result;
}

void* android_dlopen_ext(const char* filename, int flags, const android_dlextinfo* extinfo) {
  return dlopen_ext(filename, flags, extinfo);
}

void* dlopen(const char* filename, int flags) {
  return dlopen_ext(filename, flags, nullptr);
}

void* dlsym(void* handle, const char* symbol) {
  ScopedPthreadMutexLocker locker(&g_dl_mutex);

#if !defined(__LP64__)
  if (handle == nullptr) {
    __bionic_format_dlerror("dlsym library handle is null", nullptr);
    return nullptr;
  }
#endif

  if (symbol == nullptr) {
    __bionic_format_dlerror("dlsym symbol name is null", nullptr);
    return nullptr;
  }

  soinfo* found = nullptr;
  const ElfW(Sym)* sym = nullptr;
  void* caller_addr = __builtin_return_address(0);
  soinfo* caller = find_containing_library(caller_addr);

  if (handle == RTLD_DEFAULT || handle == RTLD_NEXT) {
    sym = dlsym_linear_lookup(symbol, &found, caller, handle);
    DL_WARN("[NATIVE HOOK] %s : symbol = %s, handle == RTLD_DEFAULT or RTLD_NEXT", __func__, symbol);
  } else {
    soinfo* so = reinterpret_cast<soinfo*>(handle);
    if (nht &&
        is_the_same_lib(so->get_realpath(), nht->get_hooked_lib_name()) &&
        nht->is_hooked_symbol(symbol)) {
      sym = dlsym_handle_lookup(nht->get_hooking_so(), &found, nht->get_hooking_symbol());
      DL_WARN("[NATIVE HOOK] %s : symbol = %s, handle == others, (lib_from_nht=%p)\n", __func__, symbol, reinterpret_cast<void*>(nht->get_hooking_so()));
    } else {
      sym = dlsym_handle_lookup(so, &found, symbol);
    }
  }

  if (sym != nullptr) {
    unsigned bind = ELF_ST_BIND(sym->st_info);

    // st_shndx != 0 means that this symbol is not undefined.
    if ((bind == STB_GLOBAL || bind == STB_WEAK) && sym->st_shndx != 0) {
        DL_WARN("[NATIVE HOOK] %s : returned successfully, sym_offset=%p, symbol_addr=%p\n", __func__, reinterpret_cast<void*>(sym->st_value), reinterpret_cast<void*>(found->resolve_symbol_address(sym)));
      return reinterpret_cast<void*>(found->resolve_symbol_address(sym));
    }

    __bionic_format_dlerror("symbol found but not global", symbol);
    return nullptr;
  } else {
    __bionic_format_dlerror("undefined symbol", symbol);
    return nullptr;
  }
}

int dladdr(const void* addr, Dl_info* info) {
  ScopedPthreadMutexLocker locker(&g_dl_mutex);

  // Determine if this address can be found in any library currently mapped.
  soinfo* si = find_containing_library(addr);
  if (si == nullptr) {
    return 0;
  }

  memset(info, 0, sizeof(Dl_info));

  info->dli_fname = si->get_realpath();
  // Address at which the shared object is loaded.
  info->dli_fbase = reinterpret_cast<void*>(si->base);

  // Determine if any symbol in the library contains the specified address.
  ElfW(Sym)* sym = si->find_symbol_by_address(addr);
  if (sym != nullptr) {
    info->dli_sname = si->get_string(sym->st_name);
    info->dli_saddr = reinterpret_cast<void*>(si->resolve_symbol_address(sym));
  }

  return 1;
}

int dlclose(void* handle) {
  ScopedPthreadMutexLocker locker(&g_dl_mutex);
  do_dlclose(reinterpret_cast<soinfo*>(handle));
  // dlclose has no defined errors.
  return 0;
}

int dl_iterate_phdr(int (*cb)(dl_phdr_info* info, size_t size, void* data), void* data) {
  ScopedPthreadMutexLocker locker(&g_dl_mutex);
  return do_dl_iterate_phdr(cb, data);
}

void android_set_application_target_sdk_version(uint32_t target) {
  // lock to avoid modification in the middle of dlopen.
  ScopedPthreadMutexLocker locker(&g_dl_mutex);
  set_application_target_sdk_version(target);
}

uint32_t android_get_application_target_sdk_version() {
  return get_application_target_sdk_version();
}

// name_offset: starting index of the name in libdl_info.strtab
#define ELF32_SYM_INITIALIZER(name_offset, value, shndx) \
    { name_offset, \
      reinterpret_cast<Elf32_Addr>(value), \
      /* st_size */ 0, \
      (shndx == 0) ? 0 : (STB_GLOBAL << 4), \
      /* st_other */ 0, \
      shndx, \
    }

#define ELF64_SYM_INITIALIZER(name_offset, value, shndx) \
    { name_offset, \
      (shndx == 0) ? 0 : (STB_GLOBAL << 4), \
      /* st_other */ 0, \
      shndx, \
      reinterpret_cast<Elf64_Addr>(value), \
      /* st_size */ 0, \
    }

static const char ANDROID_LIBDL_STRTAB[] =
  // 0000000 00011111 111112 22222222 2333333 3333444444444455555555556666666 6667777777777888888888899999 99999
  // 0123456 78901234 567890 12345678 9012345 6789012345678901234567890123456 7890123456789012345678901234 56789
    "dlopen\0dlclose\0dlsym\0dlerror\0dladdr\0android_update_LD_LIBRARY_PATH\0android_get_LD_LIBRARY_PATH\0dl_it"
  // 00000000001 1111111112222222222 3333333333444444444455555555556666666666777 777777788888888889999999999
  // 01234567890 1234567890123456789 0123456789012345678901234567890123456789012 345678901234567890123456789
    "erate_phdr\0android_dlopen_ext\0android_set_application_target_sdk_version\0android_get_application_tar"
  // 0000000000111111
  // 0123456789012345
    "get_sdk_version\0"
#if defined(__arm__)
  // 216
    "dl_unwind_find_exidx\0"
#endif
    ;

static ElfW(Sym) g_libdl_symtab[] = {
  // Total length of libdl_info.strtab, including trailing 0.
  // This is actually the STH_UNDEF entry. Technically, it's
  // supposed to have st_name == 0, but instead, it points to an index
  // in the strtab with a \0 to make iterating through the symtab easier.
  ELFW(SYM_INITIALIZER)(sizeof(ANDROID_LIBDL_STRTAB) - 1, nullptr, 0),
  ELFW(SYM_INITIALIZER)(  0, &dlopen, 1),
  ELFW(SYM_INITIALIZER)(  7, &dlclose, 1),
  ELFW(SYM_INITIALIZER)( 15, &dlsym, 1),
  ELFW(SYM_INITIALIZER)( 21, &dlerror, 1),
  ELFW(SYM_INITIALIZER)( 29, &dladdr, 1),
  ELFW(SYM_INITIALIZER)( 36, &android_update_LD_LIBRARY_PATH, 1),
  ELFW(SYM_INITIALIZER)( 67, &android_get_LD_LIBRARY_PATH, 1),
  ELFW(SYM_INITIALIZER)( 95, &dl_iterate_phdr, 1),
  ELFW(SYM_INITIALIZER)(111, &android_dlopen_ext, 1),
  ELFW(SYM_INITIALIZER)(130, &android_set_application_target_sdk_version, 1),
  ELFW(SYM_INITIALIZER)(173, &android_get_application_target_sdk_version, 1),
#if defined(__arm__)
  ELFW(SYM_INITIALIZER)(216, &dl_unwind_find_exidx, 1),
#endif
};

// Fake out a hash table with a single bucket.
//
// A search of the hash table will look through g_libdl_symtab starting with index 1, then
// use g_libdl_chains to find the next index to look at. g_libdl_chains should be set up to
// walk through every element in g_libdl_symtab, and then end with 0 (sentinel value).
//
// That is, g_libdl_chains should look like { 0, 2, 3, ... N, 0 } where N is the number
// of actual symbols, or nelems(g_libdl_symtab)-1 (since the first element of g_libdl_symtab is not
// a real symbol). (See soinfo_elf_lookup().)
//
// Note that adding any new symbols here requires stubbing them out in libdl.
static unsigned g_libdl_buckets[1] = { 1 };
#if defined(__arm__)
static unsigned g_libdl_chains[] = { 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0 };
#else
static unsigned g_libdl_chains[] = { 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0 };
#endif

static uint8_t __libdl_info_buf[sizeof(soinfo)] __attribute__((aligned(8)));
static soinfo* __libdl_info = nullptr;

// This is used by the dynamic linker. Every process gets these symbols for free.
soinfo* get_libdl_info() {
  if (__libdl_info == nullptr) {
    __libdl_info = new (__libdl_info_buf) soinfo("libdl.so", nullptr, 0, RTLD_GLOBAL);
    __libdl_info->flags_ |= FLAG_LINKED;
    __libdl_info->strtab_ = ANDROID_LIBDL_STRTAB;
    __libdl_info->symtab_ = g_libdl_symtab;
    __libdl_info->nbucket_ = sizeof(g_libdl_buckets)/sizeof(unsigned);
    __libdl_info->nchain_ = sizeof(g_libdl_chains)/sizeof(unsigned);
    __libdl_info->bucket_ = g_libdl_buckets;
    __libdl_info->chain_ = g_libdl_chains;
    __libdl_info->ref_count_ = 1;
    __libdl_info->strtab_size_ = sizeof(ANDROID_LIBDL_STRTAB);
    __libdl_info->local_group_root_ = __libdl_info;
    __libdl_info->soname_ = "libdl.so";
    __libdl_info->target_sdk_version_ = __ANDROID_API__;
#if defined(__arm__)
    strlcpy(__libdl_info->old_name_, __libdl_info->soname_, sizeof(__libdl_info->old_name_));
#endif
  }

  return __libdl_info;
}
