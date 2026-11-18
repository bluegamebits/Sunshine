/**
 * @file src/compat/moonlight.h
 * @brief Import Moonlight's internal C declarations with C++ atomic support.
 */
#pragma once

// standard includes
// C++23's compatibility header defines atomic types using C++ templates. Load it before the C linkage block.
#include <stdatomic.h>

// lib includes
extern "C" {
#include <moonlight-common-c/src/Limelight-internal.h>
}

// Microphone passthrough protocol extension. Newer moonlight-common-c revisions provide these
// definitions; they're duplicated here so Sunshine also builds against older submodule revisions.
// See SS_MIC_PACKET_HEADER in moonlight-common-c/src/Video.h for the wire format.
#ifndef LI_FF_MICROPHONE_V1
  #define LI_FF_MICROPHONE_V1 0x40000000  ///< Host feature flag: accepts microphone packets.
#endif
#ifndef ML_FF_MICROPHONE_V1
  #define ML_FF_MICROPHONE_V1 0x40000000  ///< Client feature flag: sends microphone packets.
#endif
#ifndef SS_MIC_PACKET_MAGIC
  #define SS_MIC_PACKET_MAGIC 0xFF4D4943  ///< Big-endian magic that starts every microphone packet.
  #define SS_MIC_PACKET_VERSION 1  ///< Microphone packet format version.
  #define SS_MIC_SAMPLE_RATE 48000  ///< Microphone Opus sample rate.
  #define SS_MIC_CHANNELS 1  ///< Microphone Opus channel count.
  #define SS_MIC_MAX_OPUS_PAYLOAD 1000  ///< Largest Opus payload carried by one microphone packet.

  #pragma pack(push, 1)

/**
 * @brief Header of a client microphone packet; an AES-GCM encrypted Opus packet follows.
 */
typedef struct _SS_MIC_PACKET_HEADER {
  uint32_t magic;  ///< SS_MIC_PACKET_MAGIC (big-endian).
  uint8_t version;  ///< SS_MIC_PACKET_VERSION.
  uint8_t flags;  ///< Reserved, zero.
  uint16_t reserved;  ///< Reserved, zero.
  char sessionId[16];  ///< Audio ping payload identifying the session.
  uint32_t sequenceNumber;  ///< Packet sequence number (big-endian), also the AES-GCM IV counter.
  uint8_t tag[16];  ///< AES-GCM tag; the ciphertext follows immediately.
} SS_MIC_PACKET_HEADER, *PSS_MIC_PACKET_HEADER;

  #pragma pack(pop)
#endif
