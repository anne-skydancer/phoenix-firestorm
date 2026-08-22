/**
 * @file llimagej2cgrok.h
 * @brief JPEG 2000 encode/decode using Grok.
 *
 * Copyright (C) 2026 The Phoenix Firestorm Project, Inc.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef LL_LLIMAGEJ2CGROK_H
#define LL_LLIMAGEJ2CGROK_H

#include "llimagej2c.h"

class LLImageJ2CGrok final : public LLImageJ2CImpl
{
public:
    LLImageJ2CGrok();
    ~LLImageJ2CGrok() override;

protected:
    bool getMetadata(LLImageJ2C& base) override;
    bool decodeImpl(LLImageJ2C& base, LLImageRaw& raw_image, F32 decode_time,
                    S32 first_channel, S32 max_channel_count) override;
    bool encodeImpl(LLImageJ2C& base, const LLImageRaw& raw_image,
                    const char* comment_text, F32 encode_time = 0.f,
                    bool reversible = false) override;
    bool initDecode(LLImageJ2C& base, LLImageRaw& raw_image,
                    int discard_level = -1, int* region = nullptr) override;
    bool initEncode(LLImageJ2C& base, LLImageRaw& raw_image,
                    int blocks_size = -1, int precincts_size = -1,
                    int levels = 0) override;
    std::string getEngineInfo() const override;
};

#endif
