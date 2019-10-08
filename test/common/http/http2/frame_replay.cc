#include "test/common/http/http2/frame_replay.h"

#include "common/common/hex.h"
#include "common/common/macros.h"

#include "test/common/http/common.h"
#include "test/test_common/environment.h"

namespace Envoy {
namespace Http {
namespace Http2 {

FileFrame::FileFrame(absl::string_view path) : api_(Api::createApiForTest()) {
  const std::string contents = api_->fileSystem().fileReadToEnd(
      TestEnvironment::runfilesPath("test/common/http/http2/" + std::string(path)));
  frame_.resize(contents.size());
  contents.copy(reinterpret_cast<char*>(frame_.data()), frame_.size());
}

std::unique_ptr<std::istream> FileFrame::istream() {
  const std::string frame_string{reinterpret_cast<char*>(frame_.data()), frame_.size()};
  return std::make_unique<std::istringstream>(frame_string);
}

const Frame& WellKnownFrames::clientConnectionPrefaceFrame() {
  CONSTRUCT_ON_FIRST_USE(std::vector<uint8_t>,
                         {0x50, 0x52, 0x49, 0x20, 0x2a, 0x20, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x32,
                          0x2e, 0x30, 0x0d, 0x0a, 0x0d, 0x0a, 0x53, 0x4d, 0x0d, 0x0a, 0x0d, 0x0a});
}

const Frame& WellKnownFrames::defaultSettingsFrame() {
  CONSTRUCT_ON_FIRST_USE(std::vector<uint8_t>,
                         {0x00, 0x00, 0x0c, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
                          0x7f, 0xff, 0xff, 0xff, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00});
}

const Frame& WellKnownFrames::initialWindowUpdateFrame() {
  CONSTRUCT_ON_FIRST_USE(std::vector<uint8_t>, {0x00, 0x00, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00,
                                                0x00, 0x0f, 0xff, 0x00, 0x01});
}

void FrameUtils::fixupHeaders(Frame& frame) {
  constexpr size_t frame_header_len = 9; // from RFC 7540
  while (frame.size() < frame_header_len) {
    frame.emplace_back(0x00);
  }
  size_t headers_len = frame.size() - frame_header_len;
  frame[2] = headers_len & 0xff;
  headers_len >>= 8;
  frame[1] = headers_len & 0xff;
  headers_len >>= 8;
  frame[0] = headers_len & 0xff;
  // HEADERS frame with END_STREAM | END_HEADERS for stream 1.
  size_t offset = 3;
  for (const uint8_t b : {0x01, 0x05, 0x00, 0x00, 0x00, 0x01}) {
    frame[offset++] = b;
  }
}

CodecFrameInjector::CodecFrameInjector(const std::string& injector_name)
    : injector_name_(injector_name) {
  settings_.hpack_table_size_ = Http2Settings::DEFAULT_HPACK_TABLE_SIZE;
  settings_.max_concurrent_streams_ = Http2Settings::DEFAULT_MAX_CONCURRENT_STREAMS;
  settings_.initial_stream_window_size_ = Http2Settings::DEFAULT_INITIAL_STREAM_WINDOW_SIZE;
  settings_.initial_connection_window_size_ = Http2Settings::DEFAULT_INITIAL_CONNECTION_WINDOW_SIZE;
  settings_.allow_metadata_ = false;
}

ClientCodecFrameInjector::ClientCodecFrameInjector() : CodecFrameInjector("server") {
  auto client = std::make_unique<TestClientConnectionImpl>(
      client_connection_, client_callbacks_, stats_store_, settings_,
      Http::DEFAULT_MAX_REQUEST_HEADERS_KB, Http::DEFAULT_MAX_HEADERS_COUNT);
  request_encoder_ = &client->newStream(response_decoder_);
  connection_ = std::move(client);
  ON_CALL(client_connection_, write(_, _))
      .WillByDefault(Invoke([&](Buffer::Instance& data, bool) -> void {
        ENVOY_LOG_MISC(
            trace, "client write: {}",
            Hex::encode(static_cast<uint8_t*>(data.linearize(data.length())), data.length()));
        data.drain(data.length());
      }));
  request_encoder_->getStream().addCallbacks(client_stream_callbacks_);
  // Setup a single stream to inject frames as a reply to.
  TestHeaderMapImpl request_headers;
  HttpTestUtility::addDefaultHeaders(request_headers);
  request_encoder_->encodeHeaders(request_headers, true);
}

ServerCodecFrameInjector::ServerCodecFrameInjector() : CodecFrameInjector("client") {
  connection_ = std::make_unique<TestServerConnectionImpl>(
      server_connection_, server_callbacks_, stats_store_, settings_,
      Http::DEFAULT_MAX_REQUEST_HEADERS_KB, Http::DEFAULT_MAX_HEADERS_COUNT);
  EXPECT_CALL(server_callbacks_, newStream(_, _))
      .WillRepeatedly(Invoke([&](StreamEncoder& encoder, bool) -> StreamDecoder& {
        encoder.getStream().addCallbacks(server_stream_callbacks_);
        return request_decoder_;
      }));
  ON_CALL(server_connection_, write(_, _))
      .WillByDefault(Invoke([&](Buffer::Instance& data, bool) -> void {
        ENVOY_LOG_MISC(
            trace, "server write: {}",
            Hex::encode(static_cast<uint8_t*>(data.linearize(data.length())), data.length()));
        data.drain(data.length());
      }));
}

void CodecFrameInjector::write(const Frame& frame) {
  Buffer::OwnedImpl buffer;
  buffer.add(frame.data(), frame.size());
  ENVOY_LOG_MISC(trace, "{} write: {}", injector_name_, Hex::encode(frame.data(), frame.size()));
  while (buffer.length() > 0) {
    connection_->dispatch(buffer);
  }
}

} // namespace Http2
} // namespace Http
} // namespace Envoy