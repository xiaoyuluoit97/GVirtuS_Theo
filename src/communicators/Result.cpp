#include "gvirtus/communicators/Result.h"
#include "gvirtus/communicators/Buffer.h"
using gvirtus::communicators::Result;
using gvirtus::communicators::Buffer;
Result::Result(int exit_code) {
  mExitCode = exit_code;
  mpOutputBuffer = NULL;
}

Result::Result(int exit_code, const std::shared_ptr<Buffer> output_buffer) {
  mExitCode = exit_code;
  mpOutputBuffer = (output_buffer);
}

int Result::GetExitCode() { return mExitCode; }

// --- RE-IMPLEMENTED SYNCHRONOUS DUMP (SOLUTION B) ---
// This version is self-contained and does not depend on Buffer::Dump.
void Result::Dump(Communicator *c) {
  // 1. Write the fixed-size fields
  c->Write((char *)&mExitCode, sizeof(int));
  c->Write(reinterpret_cast<const char *>(&mTimeTaken), sizeof(mTimeTaken));

  // 2. Manually handle the output buffer
  if (mpOutputBuffer != nullptr && mpOutputBuffer->GetBufferSize() > 0) {
    size_t buffer_size = mpOutputBuffer->GetBufferSize();
    c->Write(reinterpret_cast<const char *>(&buffer_size), sizeof(buffer_size));
    c->Write(mpOutputBuffer->GetBuffer(), buffer_size);
  } else {
    size_t buffer_size = 0;
    c->Write(reinterpret_cast<const char *>(&buffer_size), sizeof(buffer_size));
  }
  
  // 3. Sync to ensure data is sent
  c->Sync();
}

void Result::TimeTaken(double time_taken) {
  mTimeTaken = time_taken;
}

double Result::TimeTaken() const {
  return mTimeTaken;
}
void Result::DumpAsync(std::shared_ptr<IAsyncCommunicator> c) {
    if (!c) return;

    // 1. Create a temporary buffer to serialize the entire response message.
    Buffer response_buffer;

    // 2. Serialize the fixed-size fields first.
    response_buffer.Add(mExitCode);
    response_buffer.Add(mTimeTaken);

    // 3. Serialize the variable-size output buffer.
    // The protocol is: write the size, then write the data.
    if (mpOutputBuffer != nullptr && mpOutputBuffer->GetBufferSize() > 0) {
        size_t output_size = mpOutputBuffer->GetBufferSize();
        response_buffer.Add(output_size);
        response_buffer.Add(mpOutputBuffer->GetBuffer(), output_size);
    } else {
        // If there is no output buffer, just write a size of 0.
        size_t output_size = 0;
        response_buffer.Add(output_size);
    }

    // 4. Convert the serialized buffer to a DataChunk and send it asynchronously.
    // We use a null callback because we don't need to know when the write completes.
    c->AsyncWrite(response_buffer.to_datachunk(), nullptr);
}