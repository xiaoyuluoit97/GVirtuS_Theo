/*
 * gVirtuS -- A GPGPU transparent virtualization component.
 * (Original license header retained)
 *
 * This file has been modified for pipeline compatibility by decoupling the Buffer class
 * from the synchronous Communicator interface.
 */

/**
 * @file   Buffer.cpp
 * @author Giuseppe Coviello <giuseppe.coviello@uniparthenope.it>
 * @date   Sun Oct 18 13:16:46 2009
 *
 * @brief
 *
 *
 */
#define DEBUG
#include "gvirtus/communicators/Buffer.h"

using namespace std;
using gvirtus::communicators::Buffer;
using gvirtus::communicators::DataChunk; // For the new methods

// --- UNCHANGED CONSTRUCTORS & DESTRUCTOR ---

Buffer::Buffer(size_t initial_size, size_t block_size) {
  mSize = initial_size;
  mBlockSize = block_size;
  mLength = 0;
  mOffset = 0;
  mpBuffer = NULL;
  mOwnBuffer = true;
  if (mSize == 0) mSize = 0;
  if ((mSize = (mSize / mBlockSize) * mBlockSize) == 0) mSize = mBlockSize;
  if ((mpBuffer = (char *)malloc(mSize)) == NULL)
    throw runtime_error("Can't allocate memory.");
  mBackOffset = mLength;
}

Buffer::Buffer(const Buffer &orig) {
  mBlockSize = orig.mBlockSize;
  mLength = orig.mLength;
  // Corrected: mSize should reflect the allocated memory, not just the used length.
  mSize = orig.mSize; 
  mOffset = orig.mOffset;
  mOwnBuffer = true;
  if ((mpBuffer = (char *)malloc(mSize)) == NULL)
    throw runtime_error("Can't allocate memory.");
  memmove(mpBuffer, orig.mpBuffer, mLength);
  mBackOffset = mLength;
}

Buffer::Buffer(istream &in) {
  in.read((char *)&mSize, sizeof(size_t));
  mBlockSize = BLOCK_SIZE;
  mLength = mSize;
  mOffset = 0;
  mOwnBuffer = true;
  if ((mpBuffer = (char *)malloc(mSize)) == NULL)
    throw runtime_error("Can't allocate memory.");
  in.read(mpBuffer, mSize);
  mBackOffset = mLength;
}

Buffer::Buffer(char *buffer, size_t buffer_size, size_t block_size) {
  mSize = buffer_size;
  mBlockSize = block_size;
  mLength = buffer_size; // Corrected: length should be the full size of the provided buffer
  mOffset = 0;
  mpBuffer = buffer;
  mOwnBuffer = false;
  mBackOffset = mLength;
}

Buffer::~Buffer() {
  if (mOwnBuffer) free(mpBuffer);
}

// --- NEW METHODS FOR ASYNC PIPELINE COMPATIBILITY ---

/**
 * @brief Constructs a Buffer from a DataChunk (std::vector<char>).
 *
 * This is a crucial bridge for creating a Buffer object from data received
 * by the asynchronous communicator.
 */
Buffer::Buffer(const DataChunk &data) {
    mBlockSize = BLOCK_SIZE;
    mLength = data.size();
    // Allocate a bit more space to match the block size logic
    mSize = ((mLength / mBlockSize) + 1) * mBlockSize;
    if (mLength == 0) mSize = mBlockSize;

    mOffset = 0;
    mOwnBuffer = true;
    if ((mpBuffer = (char *)malloc(mSize)) == NULL)
        throw std::runtime_error("Buffer(DataChunk): Can't allocate memory.");
    
    if (mLength > 0) {
        memcpy(mpBuffer, data.data(), mLength);
    }
    mBackOffset = mLength;
}

/**
 * @brief Converts the Buffer's content into a DataChunk.
 *
 * This is the bridge for sending the Buffer's data via the asynchronous
 * communicator's AsyncWrite method.
 * @return A std::vector<char> containing the buffer's data.
 */
DataChunk Buffer::to_datachunk() const {
    if (mLength == 0) return {};
    return DataChunk(mpBuffer, mpBuffer + mLength);
}

// --- UNCHANGED METHODS ---

void Buffer::Reset() {
  mLength = 0;
  mOffset = 0;
  mBackOffset = 0;
}

const char *const Buffer::GetBuffer() const { return mpBuffer; }

size_t Buffer::GetBufferSize() const { return mLength; }

