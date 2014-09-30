// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/api/cast_channel/cast_framer.h"

#include <algorithm>

#include "extensions/common/api/cast_channel/cast_channel.pb.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace extensions {
namespace core_api {
namespace cast_channel {
class CastFramerTest : public testing::Test {
 public:
  CastFramerTest() {}
  virtual ~CastFramerTest() {}

  virtual void SetUp() OVERRIDE {
    cast_message_.set_protocol_version(CastMessage::CASTV2_1_0);
    cast_message_.set_source_id("source");
    cast_message_.set_destination_id("destination");
    cast_message_.set_namespace_("namespace");
    cast_message_.set_payload_type(CastMessage::STRING);
    cast_message_.set_payload_utf8("payload");
    ASSERT_TRUE(MessageFramer::Serialize(cast_message_, &cast_message_str_));

    buffer_ = new net::GrowableIOBuffer;
    buffer_->SetCapacity(MessageFramer::MessageHeader::max_message_size());
    framer_.reset(new MessageFramer(buffer_.get()));
  }

  void WriteToBuffer(const std::string& data) {
    memcpy(buffer_->StartOfBuffer(), data.data(), data.size());
  }

 protected:
  CastMessage cast_message_;
  std::string cast_message_str_;
  scoped_refptr<net::GrowableIOBuffer> buffer_;
  scoped_ptr<MessageFramer> framer_;
};

TEST_F(CastFramerTest, TestMessageFramerCompleteMessage) {
  ChannelError error;
  size_t message_length;

  WriteToBuffer(cast_message_str_);

  // Receive 1 byte of the header, framer demands 3 more bytes.
  EXPECT_EQ(4u, framer_->BytesRequested());
  EXPECT_EQ(NULL, framer_->Ingest(1, &message_length, &error).get());
  EXPECT_EQ(cast_channel::CHANNEL_ERROR_NONE, error);
  EXPECT_EQ(3u, framer_->BytesRequested());

  // Ingest remaining 3, expect that the framer has moved on to requesting the
  // body contents.
  EXPECT_EQ(NULL, framer_->Ingest(3, &message_length, &error).get());
  EXPECT_EQ(cast_channel::CHANNEL_ERROR_NONE, error);
  EXPECT_EQ(
      cast_message_str_.size() - MessageFramer::MessageHeader::header_size(),
      framer_->BytesRequested());

  // Remainder of packet sent over the wire.
  scoped_ptr<CastMessage> message;
  message = framer_->Ingest(framer_->BytesRequested(), &message_length, &error);
  EXPECT_NE(static_cast<CastMessage*>(NULL), message.get());
  EXPECT_EQ(cast_channel::CHANNEL_ERROR_NONE, error);
  EXPECT_EQ(message->SerializeAsString(), cast_message_.SerializeAsString());
  EXPECT_EQ(4u, framer_->BytesRequested());
  EXPECT_EQ(message->SerializeAsString().size(), message_length);
}

TEST_F(CastFramerTest, TestSerializeErrorMessageTooLarge) {
  std::string serialized;
  CastMessage big_message;
  big_message.CopyFrom(cast_message_);
  std::string payload;
  payload.append(MessageFramer::MessageHeader::max_message_size() + 1, 'x');
  big_message.set_payload_utf8(payload);
  EXPECT_FALSE(MessageFramer::Serialize(big_message, &serialized));
}

TEST_F(CastFramerTest, TestIngestIllegalLargeMessage) {
  std::string mangled_cast_message = cast_message_str_;
  mangled_cast_message[0] = 88;
  mangled_cast_message[1] = 88;
  mangled_cast_message[2] = 88;
  mangled_cast_message[3] = 88;
  WriteToBuffer(mangled_cast_message);

  size_t bytes_ingested;
  ChannelError error;
  EXPECT_EQ(4u, framer_->BytesRequested());
  EXPECT_EQ(NULL, framer_->Ingest(4, &bytes_ingested, &error).get());
  EXPECT_EQ(cast_channel::CHANNEL_ERROR_INVALID_MESSAGE, error);
  EXPECT_EQ(0u, framer_->BytesRequested());

  // Test that the parser enters a terminal error state.
  WriteToBuffer(cast_message_str_);
  EXPECT_EQ(0u, framer_->BytesRequested());
  EXPECT_EQ(NULL, framer_->Ingest(4, &bytes_ingested, &error).get());
  EXPECT_EQ(cast_channel::CHANNEL_ERROR_INVALID_MESSAGE, error);
  EXPECT_EQ(0u, framer_->BytesRequested());
}

TEST_F(CastFramerTest, TestUnparsableBodyProto) {
  // Message header is OK, but the body is replaced with "x"en.
  std::string mangled_cast_message = cast_message_str_;
  for (size_t i = MessageFramer::MessageHeader::header_size();
       i < mangled_cast_message.size();
       ++i) {
    std::fill(mangled_cast_message.begin() +
                  MessageFramer::MessageHeader::header_size(),
              mangled_cast_message.end(),
              'x');
  }
  WriteToBuffer(mangled_cast_message);

  // Send header.
  size_t message_length;
  ChannelError error;
  EXPECT_EQ(4u, framer_->BytesRequested());
  EXPECT_EQ(NULL, framer_->Ingest(4, &message_length, &error).get());
  EXPECT_EQ(cast_channel::CHANNEL_ERROR_NONE, error);
  EXPECT_EQ(cast_message_str_.size() - 4, framer_->BytesRequested());

  // Send body, expect an error.
  scoped_ptr<CastMessage> message;
  EXPECT_EQ(NULL,
            framer_->Ingest(framer_->BytesRequested(), &message_length, &error)
                .get());
  EXPECT_EQ(cast_channel::CHANNEL_ERROR_INVALID_MESSAGE, error);
}
}  // namespace cast_channel
}  // namespace core_api
}  // namespace extensions
