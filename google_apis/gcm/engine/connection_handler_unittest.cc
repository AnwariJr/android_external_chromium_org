// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "google_apis/gcm/engine/connection_handler.h"

#include "base/bind.h"
#include "base/memory/scoped_ptr.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/test_timeouts.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google_apis/gcm/base/mcs_util.h"
#include "google_apis/gcm/base/socket_stream.h"
#include "google_apis/gcm/protocol/mcs.pb.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/stream_socket.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace gcm {
namespace {

typedef scoped_ptr<google::protobuf::MessageLite> ScopedMessage;
typedef std::vector<net::MockRead> ReadList;
typedef std::vector<net::MockWrite> WriteList;

const uint64 kAuthId = 54321;
const uint64 kAuthToken = 12345;
const char kMCSVersion = 38;  // The protocol version.
const int kMCSPort = 5228;    // The server port.
const char kDataMsgFrom[] = "data_from";
const char kDataMsgCategory[] = "data_category";
const char kDataMsgFrom2[] = "data_from2";
const char kDataMsgCategory2[] = "data_category2";
const char kDataMsgFromLong[] =
    "this is a long from that will result in a message > 128 bytes";
const char kDataMsgCategoryLong[] =
    "this is a long category that will result in a message > 128 bytes";
const char kDataMsgFromLong2[] =
    "this is a second long from that will result in a message > 128 bytes";
const char kDataMsgCategoryLong2[] =
    "this is a second long category that will result in a message > 128 bytes";

// ---- Helpers for building messages. ----

// Encode a protobuf packet with protobuf type |tag| and serialized protobuf
// bytes |proto| into the MCS message form (tag + varint size + bytes).
std::string EncodePacket(uint8 tag, const std::string& proto) {
  std::string result;
  google::protobuf::io::StringOutputStream string_output_stream(&result);
  google::protobuf::io::CodedOutputStream coded_output_stream(
      &string_output_stream);
  const unsigned char tag_byte[1] = {tag};
  coded_output_stream.WriteRaw(tag_byte, 1);
  coded_output_stream.WriteVarint32(proto.size());
  coded_output_stream.WriteRaw(proto.c_str(), proto.size());
  return result;
}

// Encode a handshake request into the MCS message form.
std::string EncodeHandshakeRequest() {
  std::string result;
  const char version_byte[1] = {kMCSVersion};
  result.append(version_byte, 1);
  ScopedMessage login_request(BuildLoginRequest(kAuthId, kAuthToken));
  result.append(EncodePacket(kLoginRequestTag,
                             login_request->SerializeAsString()));
  return result;
}

// Build a serialized login response protobuf.
std::string BuildLoginResponse() {
  std::string result;
  mcs_proto::LoginResponse login_response;
  login_response.set_id("id");
  result.append(login_response.SerializeAsString());
  return result;
}

// Encoode a handshake response into the MCS message form.
std::string EncodeHandshakeResponse() {
  std::string result;
  const char version_byte[1] = {kMCSVersion};
  result.append(version_byte, 1);
  result.append(EncodePacket(kLoginResponseTag, BuildLoginResponse()));
  return result;
}

// Build a serialized data message stanza protobuf.
std::string BuildDataMessage(const std::string& from,
                             const std::string& category) {
  std::string result;
  mcs_proto::DataMessageStanza data_message;
  data_message.set_from(from);
  data_message.set_category(category);
  return data_message.SerializeAsString();
}

class GCMConnectionHandlerTest : public testing::Test {
 public:
  GCMConnectionHandlerTest();
  virtual ~GCMConnectionHandlerTest();

  net::StreamSocket* BuildSocket(const ReadList& read_list,
                                 const WriteList& write_list);

  // Pump |message_loop_|, resetting |run_loop_| after completion.
  void PumpLoop();

  ConnectionHandler* connection_handler() { return &connection_handler_; }
  base::MessageLoop* message_loop() { return &message_loop_; };
  net::DelayedSocketData* data_provider() { return data_provider_.get(); }
  int last_error() const { return last_error_; }

  // Initialize the connection handler, setting |dst_proto| as the destination
  // for any received messages.
  void Connect(ScopedMessage* dst_proto);

  // Runs the message loop until a message is received.
  void WaitForMessage();

 private:
  void ReadContinuation(ScopedMessage* dst_proto, ScopedMessage new_proto);
  void WriteContinuation();
  void ConnectionContinuation(int error);

  // SocketStreams and their data provider.
  ReadList mock_reads_;
  WriteList mock_writes_;
  scoped_ptr<net::DelayedSocketData> data_provider_;
  scoped_ptr<SocketInputStream> socket_input_stream_;
  scoped_ptr<SocketOutputStream> socket_output_stream_;

  // The connection handler being tested.
  ConnectionHandler connection_handler_;

  // The last connection error received.
  int last_error_;

  // net:: components.
  scoped_ptr<net::StreamSocket> socket_;
  net::MockClientSocketFactory socket_factory_;
  net::AddressList address_list_;

  base::MessageLoopForIO message_loop_;
  scoped_ptr<base::RunLoop> run_loop_;
};

GCMConnectionHandlerTest::GCMConnectionHandlerTest()
    : connection_handler_(TestTimeouts::tiny_timeout()),
      last_error_(0) {
  net::IPAddressNumber ip_number;
  net::ParseIPLiteralToNumber("127.0.0.1", &ip_number);
  address_list_ = net::AddressList::CreateFromIPAddress(ip_number, kMCSPort);
}

GCMConnectionHandlerTest::~GCMConnectionHandlerTest() {
}

net::StreamSocket* GCMConnectionHandlerTest::BuildSocket(
    const ReadList& read_list,
    const WriteList& write_list) {
  mock_reads_ = read_list;
  mock_writes_ = write_list;
  data_provider_.reset(
      new net::DelayedSocketData(0,
                                 &(mock_reads_[0]), mock_reads_.size(),
                                 &(mock_writes_[0]), mock_writes_.size()));
  socket_factory_.AddSocketDataProvider(data_provider_.get());

  socket_ = socket_factory_.CreateTransportClientSocket(
      address_list_, NULL, net::NetLog::Source());
  socket_->Connect(net::CompletionCallback());

  run_loop_.reset(new base::RunLoop());
  PumpLoop();

  DCHECK(socket_->IsConnected());
  return socket_.get();
}

void GCMConnectionHandlerTest::PumpLoop() {
  run_loop_->RunUntilIdle();
  run_loop_.reset(new base::RunLoop());
}

void GCMConnectionHandlerTest::Connect(
    ScopedMessage* dst_proto) {
  connection_handler_.Init(
      socket_.Pass(),
      *BuildLoginRequest(kAuthId, kAuthToken),
      base::Bind(&GCMConnectionHandlerTest::ReadContinuation,
                 base::Unretained(this),
                 dst_proto),
      base::Bind(&GCMConnectionHandlerTest::WriteContinuation,
                 base::Unretained(this)),
      base::Bind(&GCMConnectionHandlerTest::ConnectionContinuation,
                 base::Unretained(this)));
}

void GCMConnectionHandlerTest::ReadContinuation(
    ScopedMessage* dst_proto,
    ScopedMessage new_proto) {
  *dst_proto = new_proto.Pass();
  run_loop_->Quit();
}

void GCMConnectionHandlerTest::WaitForMessage() {
  run_loop_->Run();
  run_loop_.reset(new base::RunLoop());
}

void GCMConnectionHandlerTest::WriteContinuation() {
  run_loop_->Quit();
}

void GCMConnectionHandlerTest::ConnectionContinuation(int error) {
  last_error_ = error;
  run_loop_->Quit();
}

// Initialize the connection handler and ensure the handshake completes
// successfully.
TEST_F(GCMConnectionHandlerTest, Init) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  ReadList read_list(1, net::MockRead(net::ASYNC,
                                      handshake_response.c_str(),
                                      handshake_response.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  Connect(&received_message);
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(BuildLoginResponse(), received_message->SerializeAsString());
  EXPECT_TRUE(connection_handler()->CanSendMessage());
}

// Simulate the handshake response returning an older version. Initialization
// should fail.
TEST_F(GCMConnectionHandlerTest, InitFailedVersionCheck) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  // Overwrite the version byte.
  handshake_response[0] = 37;
  ReadList read_list(1, net::MockRead(net::ASYNC,
                                      handshake_response.c_str(),
                                      handshake_response.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response. Should result in a connection error.
  EXPECT_FALSE(received_message.get());
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_FAILED, last_error());
}

// Attempt to initialize, but receive no server response, resulting in a time
// out.
TEST_F(GCMConnectionHandlerTest, InitTimeout) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  ReadList read_list(1, net::MockRead(net::SYNCHRONOUS,
                                      net::ERR_IO_PENDING));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response. Should result in a connection error.
  EXPECT_FALSE(received_message.get());
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_TIMED_OUT, last_error());
}

// Attempt to initialize, but receive an incomplete server response, resulting
// in a time out.
TEST_F(GCMConnectionHandlerTest, InitIncompleteTimeout) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size() / 2));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    net::ERR_IO_PENDING));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response. Should result in a connection error.
  EXPECT_FALSE(received_message.get());
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_TIMED_OUT, last_error());
}

// Reinitialize the connection handler after failing to initialize.
TEST_F(GCMConnectionHandlerTest, ReInit) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  ReadList read_list(1, net::MockRead(net::SYNCHRONOUS,
                                      net::ERR_IO_PENDING));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response. Should result in a connection error.
  EXPECT_FALSE(received_message.get());
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_TIMED_OUT, last_error());

  // Build a new socket and reconnect, successfully this time.
  std::string handshake_response = EncodeHandshakeResponse();
  read_list[0] = net::MockRead(net::ASYNC,
                               handshake_response.c_str(),
                               handshake_response.size());
  BuildSocket(read_list, write_list);
  Connect(&received_message);
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(BuildLoginResponse(), received_message->SerializeAsString());
  EXPECT_TRUE(connection_handler()->CanSendMessage());
}

// Verify that messages can be received after initialization.
TEST_F(GCMConnectionHandlerTest, RecvMsg) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto = BuildDataMessage(kDataMsgFrom,
                                                    kDataMsgCategory);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
}

// Verify that if two messages arrive at once, they're treated appropriately.
TEST_F(GCMConnectionHandlerTest, Recv2Msgs) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto = BuildDataMessage(kDataMsgFrom,
                                                    kDataMsgCategory);
  std::string data_message_proto2 = BuildDataMessage(kDataMsgFrom2,
                                                     kDataMsgCategory2);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  data_message_pkt += EncodePacket(kDataMessageStanzaTag, data_message_proto2);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The first data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  received_message.reset();
  WaitForMessage();  // The second data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto2, received_message->SerializeAsString());
}

// Receive a long (>128 bytes) message.
TEST_F(GCMConnectionHandlerTest, RecvLongMsg) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto =
      BuildDataMessage(kDataMsgFromLong, kDataMsgCategoryLong);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  DCHECK_GT(data_message_pkt.size(), 128U);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
}

// Receive two long (>128 bytes) message.
TEST_F(GCMConnectionHandlerTest, Recv2LongMsgs) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto =
      BuildDataMessage(kDataMsgFromLong, kDataMsgCategoryLong);
  std::string data_message_proto2 =
      BuildDataMessage(kDataMsgFromLong2, kDataMsgCategoryLong2);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  data_message_pkt += EncodePacket(kDataMessageStanzaTag, data_message_proto2);
  DCHECK_GT(data_message_pkt.size(), 256U);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The first data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  received_message.reset();
  WaitForMessage();  // The second data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto2, received_message->SerializeAsString());
}

// Simulate a message where the end of the data does not arrive in time and the
// read times out.
TEST_F(GCMConnectionHandlerTest, ReadTimeout) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto = BuildDataMessage(kDataMsgFrom,
                                                    kDataMsgCategory);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  int bytes_in_first_message = data_message_pkt.size() / 2;
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    bytes_in_first_message));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    net::ERR_IO_PENDING));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str() +
                                        bytes_in_first_message,
                                    data_message_pkt.size() -
                                        bytes_in_first_message));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  received_message.reset();
  WaitForMessage();  // Should time out.
  EXPECT_FALSE(received_message.get());
  EXPECT_EQ(net::ERR_TIMED_OUT, last_error());
  EXPECT_FALSE(connection_handler()->CanSendMessage());

  // Finish the socket read. Should have no effect.
  data_provider()->ForceNextRead();
}

// Receive a message with zero data bytes.
TEST_F(GCMConnectionHandlerTest, RecvMsgNoData) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_pkt = EncodePacket(kHeartbeatPingTag, "");
  ASSERT_EQ(data_message_pkt.size(), 2U);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  received_message.reset();
  WaitForMessage();  // The heartbeat ping.
  EXPECT_TRUE(received_message.get());
  EXPECT_EQ(GetMCSProtoTag(*received_message), kHeartbeatPingTag);
  EXPECT_EQ(net::OK, last_error());
  EXPECT_TRUE(connection_handler()->CanSendMessage());
}

// Send a message after performing the handshake.
TEST_F(GCMConnectionHandlerTest, SendMsg) {
  mcs_proto::DataMessageStanza data_message;
  data_message.set_from(kDataMsgFrom);
  data_message.set_category(kDataMsgCategory);
  std::string handshake_request = EncodeHandshakeRequest();
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message.SerializeAsString());
  WriteList write_list;
  write_list.push_back(net::MockWrite(net::ASYNC,
                                      handshake_request.c_str(),
                                      handshake_request.size()));
  write_list.push_back(net::MockWrite(net::ASYNC,
                                      data_message_pkt.c_str(),
                                      data_message_pkt.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS, net::ERR_IO_PENDING));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  EXPECT_TRUE(connection_handler()->CanSendMessage());
  connection_handler()->SendMessage(data_message);
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  WaitForMessage();  // The message send.
  EXPECT_TRUE(connection_handler()->CanSendMessage());
}

// Attempt to send a message after the socket is disconnected due to a timeout.
TEST_F(GCMConnectionHandlerTest, SendMsgSocketDisconnected) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list;
  write_list.push_back(net::MockWrite(net::ASYNC,
                                      handshake_request.c_str(),
                                      handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS, net::ERR_IO_PENDING));
  net::StreamSocket* socket = BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  EXPECT_TRUE(connection_handler()->CanSendMessage());
  socket->Disconnect();
  mcs_proto::DataMessageStanza data_message;
  data_message.set_from(kDataMsgFrom);
  data_message.set_category(kDataMsgCategory);
  connection_handler()->SendMessage(data_message);
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  WaitForMessage();  // The message send. Should result in an error
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_CONNECTION_CLOSED, last_error());
}

}  // namespace
}  // namespace gcm