/**
 * @file tests/unit/test_stream_mic.cpp
 * @brief Test microphone passthrough packet handling in src/stream.*
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// local includes
#include <src/compat/moonlight.h>
#include <src/crypto.h>
#include <src/utility.h>

namespace stream {
  std::optional<std::string> test_route_mic_packet(const std::string &packet, const std::string &session_id);

  namespace mic {
    bool decrypt_packet(const std::string &packet, crypto::cipher::gcm_t &cipher, std::vector<std::uint8_t> &plaintext, std::uint32_t &seq);
  }  // namespace mic
}  // namespace stream

namespace {
  const crypto::aes_t test_key(16, 0x42);  ///< Remote input key shared by the test client and host.
  const std::string test_session_id = "0123456789abcdef";  ///< Audio ping payload identifying the test session.

  /**
   * @brief Build a microphone packet the same way moonlight-common-c does.
   *
   * @param opus Opus payload to encrypt.
   * @param seq Packet sequence number.
   * @param key AES-GCM key.
   * @return Serialized packet.
   */
  std::string make_mic_packet(const std::string &opus, std::uint32_t seq, const crypto::aes_t &key = test_key) {
    SS_MIC_PACKET_HEADER header {};
    header.magic = util::endian::big<std::uint32_t>(SS_MIC_PACKET_MAGIC);
    header.version = SS_MIC_PACKET_VERSION;
    std::memcpy(header.sessionId, test_session_id.data(), sizeof(header.sessionId));
    header.sequenceNumber = util::endian::big<std::uint32_t>(seq);

    crypto::aes_t iv(12);
    std::memcpy(iv.data(), &header.sequenceNumber, sizeof(header.sequenceNumber));
    iv[10] = 'C';
    iv[11] = 'M';

    std::vector<std::uint8_t> ciphertext(opus.size());
    crypto::cipher::gcm_t cipher {key, false};
    auto bytes = cipher.encrypt(opus, header.tag, ciphertext.data(), &iv);
    EXPECT_EQ(bytes, static_cast<int>(opus.size()));

    std::string packet {reinterpret_cast<const char *>(&header), sizeof(header)};
    packet.append(reinterpret_cast<const char *>(ciphertext.data()), ciphertext.size());
    return packet;
  }
}  // namespace

TEST(StreamMicTests, DecryptsAuthenticPacket) {
  crypto::cipher::gcm_t cipher {test_key, false};
  std::vector<std::uint8_t> plaintext;
  std::uint32_t seq = 0;

  ASSERT_TRUE(stream::mic::decrypt_packet(make_mic_packet("opus-frame", 0x01020304), cipher, plaintext, seq));
  EXPECT_EQ(std::string(plaintext.begin(), plaintext.end()), "opus-frame");
  EXPECT_EQ(seq, 0x01020304u);
}

TEST(StreamMicTests, RejectsTamperedPacket) {
  crypto::cipher::gcm_t cipher {test_key, false};
  std::vector<std::uint8_t> plaintext;
  std::uint32_t seq = 0;

  auto packet = make_mic_packet("opus-frame", 7);
  packet.back() ^= 0x01;
  EXPECT_FALSE(stream::mic::decrypt_packet(packet, cipher, plaintext, seq));
}

TEST(StreamMicTests, RejectsTamperedSequenceNumber) {
  crypto::cipher::gcm_t cipher {test_key, false};
  std::vector<std::uint8_t> plaintext;
  std::uint32_t seq = 0;

  // The sequence number is part of the IV, so changing it must break authentication
  auto packet = make_mic_packet("opus-frame", 7);
  packet[offsetof(SS_MIC_PACKET_HEADER, sequenceNumber) + 3] ^= 0x01;
  EXPECT_FALSE(stream::mic::decrypt_packet(packet, cipher, plaintext, seq));
}

TEST(StreamMicTests, RejectsPacketFromAnotherKey) {
  crypto::cipher::gcm_t cipher {test_key, false};
  std::vector<std::uint8_t> plaintext;
  std::uint32_t seq = 0;

  EXPECT_FALSE(stream::mic::decrypt_packet(make_mic_packet("opus-frame", 1, crypto::aes_t(16, 0x24)), cipher, plaintext, seq));
}

TEST(StreamMicTests, RejectsUnknownVersion) {
  crypto::cipher::gcm_t cipher {test_key, false};
  std::vector<std::uint8_t> plaintext;
  std::uint32_t seq = 0;

  auto packet = make_mic_packet("opus-frame", 1);
  packet[offsetof(SS_MIC_PACKET_HEADER, version)] = SS_MIC_PACKET_VERSION + 1;
  EXPECT_FALSE(stream::mic::decrypt_packet(packet, cipher, plaintext, seq));
}

TEST(StreamMicTests, RejectsTruncatedAndOversizedPackets) {
  crypto::cipher::gcm_t cipher {test_key, false};
  std::vector<std::uint8_t> plaintext;
  std::uint32_t seq = 0;

  auto packet = make_mic_packet("x", 1);
  EXPECT_FALSE(stream::mic::decrypt_packet(packet.substr(0, sizeof(SS_MIC_PACKET_HEADER)), cipher, plaintext, seq));
  EXPECT_FALSE(stream::mic::decrypt_packet(make_mic_packet(std::string(SS_MIC_MAX_OPUS_PAYLOAD + 1, 'x'), 1), cipher, plaintext, seq));
}

TEST(StreamMicTests, RoutesPacketToRegisteredSession) {
  auto packet = make_mic_packet("opus-frame", 1);
  auto delivered = stream::test_route_mic_packet(packet, test_session_id);
  ASSERT_TRUE(delivered.has_value());
  EXPECT_EQ(*delivered, packet);
}

TEST(StreamMicTests, DropsPacketForUnknownSession) {
  auto packet = make_mic_packet("opus-frame", 1);
  EXPECT_FALSE(stream::test_route_mic_packet(packet, "fedcba9876543210").has_value());
}
