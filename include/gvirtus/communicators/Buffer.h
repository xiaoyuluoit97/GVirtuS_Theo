/*
 * gVirtuS -- A GPGPU transparent virtualization component.
 * (Original license header retained)
 *
 * This file has been modified for pipeline compatibility by decoupling it
 * from the synchronous Communicator interface.
 */

/**
 * @file   Buffer.h
 * @author Giuseppe Coviello <giuseppe.coviello@uniparthenope.it>
 * @date   Sun Oct 18 13:16:46 2009
 *
 * @brief
 *
 *
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <typeinfo>
#include <type_traits>
#include <cstddef>
#include <vector> // REQUIRED: For DataChunk definition.

#include <gvirtus/common/gvirtus-type.h>

#include "Communicator.h" // REMOVED: This is the primary decoupling change.
#include "IAsyncCommunicator.h" // ADDED: Provides DataChunk, the new way to interact.

#define BLOCK_SIZE 4096

namespace gvirtus::communicators {
/**
 * Buffer is a general purpose for marshalling and unmarshalling data. It's used
 * for exchanging data beetwen Frontend and Backend.
 */

// UNCHANGED
template <typename T>
constexpr std::size_t safe_sizeof() {
    if constexpr (std::is_void_v<T>) {
        return 1;
    } else if constexpr (std::is_function_v<T>) {
        throw std::runtime_error("safe_sizeof<T> cannot be used with function types");
    } else {
        return sizeof(T);
    }
}

class Buffer {
 public:
  // UNCHANGED (with one addition)
  Buffer(size_t initial_size = 0, size_t block_size = BLOCK_SIZE);
  Buffer(const Buffer &orig);
  Buffer(std::istream &in);
  Buffer(const DataChunk &data); // NEW: Necessary bridge to receive data from the async world.
  Buffer(char *buffer, size_t buffer_size, size_t block_size = BLOCK_SIZE);
  virtual ~Buffer();

  // --- ALL SERIALIZATION/DESERIALIZATION METHODS ARE UNCHANGED ---

  template <class T>
  void Add(T item) {
    if ((mLength + safe_sizeof<T>()) >= mSize) {
      mSize = ((mLength + safe_sizeof<T>()) / mBlockSize + 1) * mBlockSize;
      if ((mpBuffer = (char *)realloc(mpBuffer, mSize)) == NULL)
        throw std::runtime_error("Buffer::Add(item): Can't reallocate memory.");
    }
    memmove(mpBuffer + mLength, (char *)&item, safe_sizeof<T>());
    mLength += safe_sizeof<T>();
    mBackOffset = mLength;
  }

  template <class T>
  void Add(T *item, size_t n = 1) {
    if (item == NULL) {
      Add((size_t)0);
      return;
    }
    size_t size = safe_sizeof<T>() * n;
    Add(size);
    if ((mLength + size) >= mSize) {
      mSize = ((mLength + size) / mBlockSize + 1) * mBlockSize;
      if ((mpBuffer = (char *)realloc(mpBuffer, mSize)) == NULL)
        throw std::runtime_error("Buffer::Add(item, n): Can't reallocate memory.");
    }
    memmove(mpBuffer + mLength, (char *)item, size);
    mLength += size;
    mBackOffset = mLength;
  }

  template <class T>
  void AddConst(const T item) {
    if ((mLength + safe_sizeof<T>()) >= mSize) {
      mSize = ((mLength + safe_sizeof<T>()) / mBlockSize + 1) * mBlockSize;
      if ((mpBuffer = (char *)realloc(mpBuffer, mSize)) == NULL)
        throw std::runtime_error("Buffer::AddConst(item): Can't reallocate memory.");
    }
    memmove(mpBuffer + mLength, (char *)&item, safe_sizeof<T>());
    mLength += safe_sizeof<T>();
    mBackOffset = mLength;
  }

  template <class T>
  void AddConst(const T *item, size_t n = 1) {
    if (item == NULL) {
      Add((size_t)0);
      return;
    }
    size_t size = safe_sizeof<T>() * n;
    Add(size);
    if ((mLength + size) >= mSize) {
      mSize = ((mLength + size) / mBlockSize + 1) * mBlockSize;
      if ((mpBuffer = (char *)realloc(mpBuffer, mSize)) == NULL)
        throw std::runtime_error("Buffer::AddConst(item, n): Can't reallocate memory.");
    }
    memmove(mpBuffer + mLength, (char *)item, size);
    mLength += size;
    mBackOffset = mLength;
  }

  void AddString(const char *s) {
    size_t size = strlen(s) + 1;
    Add(size);
    Add(s, size);
  }

  template <class T>
  void AddMarshal(T item) {
    Add((gvirtus::common::pointer_t)item);
  }

  // --- Communicator-dependent Read methods are REMOVED ---
  // template <class T> void Read(Communicator *c) { ... }
  // template <class T> void Read(Communicator *c, size_t n = 1) { ... }

  template <class T>
  T Get() {
    if (mOffset + safe_sizeof<T>() > mLength)
      throw std::runtime_error(std::string("Buffer::Get(): Can't read any ") + typeid(T).name());
    T result = *((T *)(mpBuffer + mOffset));
    mOffset += safe_sizeof<T>();
    return result;
  }

  template <class T>
  T BackGet() {
    if (mOffset > mBackOffset || mBackOffset < safe_sizeof<T>()) // Boundary check
      throw std::runtime_error(std::string("Buffer::BackGet(): Can't read ") + typeid(T).name());
    T result = *((T *)(mpBuffer + mBackOffset - safe_sizeof<T>()));
    mBackOffset -= safe_sizeof<T>();
    return result;
  }

  template <class T>
  T *Get(size_t n) {
    if (Get<size_t>() == 0) return NULL;
    if (mOffset + safe_sizeof<T>() * n > mLength)
      throw std::runtime_error(std::string("Buffer::Get(n): Can't read  ") + typeid(T).name());
    T *result = new T[n];
    memmove((char *)result, mpBuffer + mOffset, safe_sizeof<T>() * n);
    mOffset += safe_sizeof<T>() * n;
    return result;
  }

  template <class T>
  T *Delegate(size_t n = 1) {
    size_t size = safe_sizeof<T>() * n;
    Add(size);
    if ((mLength + size) >= mSize) {
      mSize = ((mLength + size) / mBlockSize + 1) * mBlockSize;
      if ((mpBuffer = (char *)realloc(mpBuffer, mSize)) == NULL)
        throw std::runtime_error("Buffer::Delegate(n): Can't reallocate memory.");
    }
    T *dst = (T *)(mpBuffer + mLength);
    mLength += size;
    mBackOffset = mLength;
    return dst;
  }

  template <class T>
  T *Assign(size_t n = 1) {
    if (Get<size_t>() == 0) return NULL;

    if (mOffset + safe_sizeof<T>() * n > mLength) {
      throw std::runtime_error(std::string("Buffer::Assign(n): Can't read  ") + typeid(T).name());
    }
    T *result = (T *)(mpBuffer + mOffset);
    mOffset += safe_sizeof<T>() * n;
    return result;
  }

  template <class T>
  T *AssignAll() {
      size_t size = Get<size_t>();
      if (size == 0) return NULL;
      size_t n = size / safe_sizeof<T>();
      if (mOffset + safe_sizeof<T>() * n > mLength)
          throw std::runtime_error(std::string("Buffer::AssignAll(): Can't read ") + typeid(T).name());
      T *result = (T *)(mpBuffer + mOffset);
      mOffset += safe_sizeof<T>() * n;
      return result;
  }

  char *AssignString() {
    size_t size = Get<size_t>();
    if (size == 0) return nullptr;
    return Assign<char>(size);
  }

  template <class T>
  T *BackAssign(size_t n = 1) {
    if (mBackOffset < (safe_sizeof<T>()*n + sizeof(size_t)))
      throw std::runtime_error(std::string("Buffer::BackAssign(n): Can't read ") + typeid(T).name());
    T *result = (T *)(mpBuffer + mBackOffset - safe_sizeof<T>() * n);
    mBackOffset -= safe_sizeof<T>() * n + sizeof(size_t);
    return result;
  }

  template <class T>
  T GetFromMarshal() {
    return (T)Get<gvirtus::common::pointer_t>();
  }

  inline bool Empty() { return mOffset == mLength; }

  // UNCHANGED (kept the non-communicator version)
  void Reset();

  // REMOVED
  // void Reset(Communicator *c);
  
  // UNCHANGED
  const char *const GetBuffer() const;
  size_t GetBufferSize() const;
  
  // REMOVED
  // void Dump(Communicator *c) const;

  // NEW: Necessary bridge to send data into the async world.
  DataChunk to_datachunk() const;


 private:
  // UNCHANGED
  size_t mBlockSize;
  size_t mSize;
  size_t mLength;
  size_t mOffset;
  size_t mBackOffset;
  char *mpBuffer;
  bool mOwnBuffer;
};
}  // namespace gvirtus::communicators