// AUTO-GENERATED from db/ by tools/gen_profiles.py. DO NOT EDIT.
// Field order: active_profile_t in src/nand_profile.h (schema v1).
#pragma once
#include "nand_profile.h"

static const active_profile_t NAND_RESIDENT[] = {
  { // DS35Q1GA: dosilicon profile, generic2 ECC
    "DS35Q1GA", 0xE5, 0x71, 0x00, 0x00,
    2112, 64, 64, 1024,
    0x13, 0x0B, 0x6B, 0x0F, 0x1F, 0xC0, 0xB0,
    4, 4, 0x03, { 0, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 },
    0x00, 1, 2, 0xB0, 0x01, 0, 1, 0,
    3300,
    0, 2, 0xFF, 4, 4, 0x01,
    { 2, 6, 16, 8, 32, 8, 48, 8 },
    { 8, 8, 24, 8, 40, 8, 56, 8 },
    { 0, 0 } },
  { // MT29F2G01ABAGD: micron profile, micron3 ECC
    "MT29F2G01ABAGD", 0x2C, 0x24, 0x00, 0x00,
    2176, 128, 64, 2048,
    0x13, 0x0B, 0x6B, 0x0F, 0x1F, 0xC0, 0xB0,
    4, 4, 0x07, { 0, 1, 3, 2, 3, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 },
    0x00, 1, 2, 0x00, 0x00, 0, 2, 0,
    3300,
    0, 1, 0xFF, 1, 1, 0x01,
    { 2, 62, 0, 0, 0, 0, 0, 0 },
    { 64, 64, 0, 0, 0, 0, 0, 0 },
    { 0, 0 } }
};
static const unsigned NAND_RESIDENT_COUNT =
    sizeof(NAND_RESIDENT) / sizeof(NAND_RESIDENT[0]);
