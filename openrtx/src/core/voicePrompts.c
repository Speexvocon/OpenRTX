/***************************************************************************
 *   Copyright (C) 2022 by Federico Amedeo Izzo IU2NUO,                    *
 *                         Niccolò Izzo IU2KIN,                            *
 *                         Silvano Seva IU2KWO                             *
 *                         Joseph Stephen VK7JS                            *
 *                         Roger Clark  VK3KYY                             *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/

#include <interfaces/keyboard.h>
#include <interfaces/audio.h>
#include <ui/ui_strings.h>
#include <voicePrompts.h>
#include <audio_codec.h>
#include <ctype.h>
#include <state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t VOICE_PROMPTS_DATA_MAGIC   = 0x5056;  //'VP'
static const uint32_t VOICE_PROMPTS_DATA_VERSION = 0x1000;  // v1000 OpenRTX

#define VOICE_PROMPTS_TOC_SIZE 350
#define CODEC2_HEADER_SIZE     7
#define VP_SEQUENCE_BUF_SIZE   128

typedef struct
{
    uint32_t magic;
    uint32_t version;
}
vpHeader_t;

typedef struct
{
    const char* userWord;
    const voicePrompt_t vp;
}
userDictEntry_t;

typedef struct
{
    uint16_t buffer[VP_SEQUENCE_BUF_SIZE];  // Buffer of individual prompt indices.
    uint16_t pos;                           // Index into above buffer.
    uint16_t length;                        // Number of entries in above buffer.
    uint32_t c2DataStart;
    uint32_t c2DataIndex;                   // Index into current codec2 data
    uint32_t c2DataLength;                  // Length of codec2 data for current prompt.
}
vpSequence_t;

static const userDictEntry_t userDictionary[] =
{
    {"hotspot",   PROMPT_CUSTOM1},  // Hotspot
    {"clearnode", PROMPT_CUSTOM2},  // ClearNode
    {"sharinode", PROMPT_CUSTOM3},  // ShariNode
    {"microhub",  PROMPT_CUSTOM4},  // MicroHub
    {"openspot",  PROMPT_CUSTOM5},  // Openspot
    {"repeater",  PROMPT_CUSTOM6},  // repeater
    {"blindhams", PROMPT_CUSTOM7},  // BlindHams
    {"allstar",   PROMPT_CUSTOM8},  // Allstar
    {"parrot",    PROMPT_CUSTOM9},  // Parrot
    {"channel",   PROMPT_CHANNEL},  // Channel
    {0, 0}
};

static vpSequence_t vpCurrentSequence =
{
    .pos          = 0,
    .length       = 0,
    .c2DataStart  = 0,
    .c2DataIndex  = 0,
    .c2DataLength = 0
};

static uint32_t tableOfContents[VOICE_PROMPTS_TOC_SIZE];
static bool     vpDataLoaded      = false;
static bool     voicePromptActive = false;

#ifdef VP_USE_FILESYSTEM
static FILE *vpFile = NULL;
#else
extern unsigned char _vpdata_start asm("_voiceprompts_start");
extern unsigned char _vpdata_end asm("_voiceprompts_end");
unsigned char        *vpData = &_vpdata_start;
#endif

/**
 * \internal
 * Load voice prompts header.
 *
 * @param header: pointer toa vpHeader_t data structure.
 */
static void loadVpHeader(vpHeader_t *header)
{
    #ifdef VP_USE_FILESYSTEM
    fseek(vpFile, 0L, SEEK_SET);
    fread(header, sizeof(vpHeader_t), 1, vpFile);
    #else
    memcpy(header, vpData, sizeof(vpHeader_t));
    #endif
}

/**
 * \internal
 * Load voice prompts' table of contents.
 */
static void loadVpToC()
{
    #ifdef VP_USE_FILESYSTEM
    fread(&tableOfContents, sizeof(tableOfContents), 1, vpFile);
    size_t vpDataOffset = ftell(vpFile);

    if(vpDataOffset == (sizeof(vpHeader_t) + sizeof(tableOfContents)))
        vpDataLoaded = true;
    #else
    uint8_t *tocPtr = vpData + sizeof(vpHeader_t);
    memcpy(&tableOfContents, tocPtr, sizeof(tableOfContents));
    vpDataLoaded = true;
    #endif
}

/**
 * \internal
 * Load Codec2 data for a voice prompt.
 *
 * @param offset: offset relative to the start of the voice prompt data.
 * @param length: data length in bytes.
 */
static void fetchCodec2Data(uint8_t *data, const size_t offset)
{
    if (vpDataLoaded == false)
        return;

    #ifdef VP_USE_FILESYSTEM
    if (vpFile == NULL)
        return;

    size_t start = sizeof(vpHeader_t)
                 + sizeof(tableOfContents)
                 + CODEC2_HEADER_SIZE;

    fseek(vpFile, start + offset, SEEK_SET);
    fread(data, 8, 1, vpFile);
    #else
    uint8_t *dataPtr = vpData
                     + sizeof(vpHeader_t)
                     + sizeof(tableOfContents)
                     + CODEC2_HEADER_SIZE;

    if((dataPtr + 8) >= &_vpdata_end)
    {
        memset(data, 0x00, 8);
        return;
    }

    memcpy(data, dataPtr + offset, 8);
    #endif
}

/**
 * \internal
 * Check validity of voice prompt header.
 *
 * @param header: voice prompt header to be checked.
 * @return true if header is valid
 */
static inline bool checkVpHeader(const vpHeader_t* header)
{
    return ((header->magic == VOICE_PROMPTS_DATA_MAGIC) &&
            (header->version == VOICE_PROMPTS_DATA_VERSION));
}

/**
 * \internal
 * Perform a string lookup inside user dictionary.
 *
 * @param ptr: string to be searched.
 * @param advanceBy: final offset with respect of dictionary beginning.
 * @return index of user dictionary's voice prompt.
 */
static uint16_t userDictLookup(const char* ptr, int* advanceBy)
{
    if ((ptr == NULL) || (*ptr == '\0'))
        return 0;

    for(uint32_t index = 0; userDictionary[index].userWord != 0; index++)
    {
        int len = strlen(userDictionary[index].userWord);
        if (strncasecmp(userDictionary[index].userWord, ptr, len) == 0)
        {
            *advanceBy = len;
            return userDictionary[index].vp;
        }
    }

    return 0;
}

static bool GetSymbolVPIfItShouldBeAnnounced(char symbol,
                                             vpFlags_t flags,
                                             voicePrompt_t* vp)
{
    *vp = PROMPT_SILENCE;

    const char indexedSymbols[] =
        "%.+-*#!,@:?()~/[]<>=$'`&|_^{}";  // Must match order of symbols in
                                          // voicePrompt_t enum.
    const char commonSymbols[] = "%.+-*#";

    bool announceCommonSymbols =
        (flags & vpAnnounceCommonSymbols) ? true : false;
    bool announceLessCommonSymbols =
        (flags & vpAnnounceLessCommonSymbols) ? true : false;

    char* symbolPtr = strchr(indexedSymbols, symbol);

    if (symbolPtr == NULL)
    {  // we don't have a prompt for this character.
        return (flags & vpAnnounceASCIIValueForUnknownChars) ? true : false;
    }

    bool commonSymbol = strchr(commonSymbols, symbol) != NULL;

    *vp = PROMPT_PERCENT + (symbolPtr - indexedSymbols);

    return ((commonSymbol && announceCommonSymbols) ||
            (!commonSymbol && announceLessCommonSymbols));
}


void vp_init()
{
    #ifdef VP_USE_FILESYSTEM
    if(vpFile == NULL)
        vpFile = fopen("voiceprompts.vpc", "r");

    if(vpFile == NULL)
        return;
    #else
    if(&_vpdata_start == &_vpdata_end)
        return;
    #endif

    // Read header
    vpHeader_t header;
    loadVpHeader(&header);

    if(checkVpHeader(&header) == true)
    {
        loadVpToC();
    }

    if (vpDataLoaded)
    {
        // If the hash key is down, set vpLevel to high, if beep or less.
        if ((kbd_getKeys() & KEY_HASH) && (state.settings.vpLevel <= vpBeep))
            state.settings.vpLevel = vpHigh;
    }
    else
    {
        // ensure we at least have beeps in the event no voice prompts are
        // loaded.
        if (state.settings.vpLevel > vpBeep)
            state.settings.vpLevel = vpBeep;
    }

    // Initialize codec2 module
    codec_init();
}

void vp_terminate()
{
    if (voicePromptActive)
    {
        audio_disableAmp();
        codec_stop();

        vpCurrentSequence.pos = 0;
        voicePromptActive     = false;
    }

    codec_terminate();

    #ifdef VP_USE_FILESYSTEM
    fclose(vpFile);
    #endif
}

void vp_clearCurrPrompt()
{
    voicePromptActive              = false;
    vpCurrentSequence.length       = 0;
    vpCurrentSequence.pos          = 0;
    vpCurrentSequence.c2DataIndex  = 0;
    vpCurrentSequence.c2DataLength = 0;
}

void vp_queuePrompt(const uint16_t prompt)
{
    if (state.settings.vpLevel < vpLow)
        return;

    if (voicePromptActive)
        vp_clearCurrPrompt();

    if (vpCurrentSequence.length < VP_SEQUENCE_BUF_SIZE)
    {
        vpCurrentSequence.buffer[vpCurrentSequence.length] = prompt;
        vpCurrentSequence.length++;
    }
}

void vp_queueString(const char* string, vpFlags_t flags)
{
    if (state.settings.vpLevel < vpLow)
        return;

    if (voicePromptActive)
        vp_clearCurrPrompt();

    if (state.settings.vpPhoneticSpell)
        flags |= vpAnnouncePhoneticRendering;

    while (*string != '\0')
    {
        int advanceBy    = 0;
        voicePrompt_t vp = userDictLookup(string, &advanceBy);

        if (vp != 0)
        {
            vp_queuePrompt(vp);
            string += advanceBy;
            continue;
        }
        else if ((*string >= '0') && (*string <= '9'))
        {
            vp_queuePrompt(*string - '0' + PROMPT_0);
        }
        else if ((*string >= 'A') && (*string <= 'Z'))
        {
            if (flags & vpAnnounceCaps)
                vp_queuePrompt(PROMPT_CAP);
            if (flags & vpAnnouncePhoneticRendering)
                vp_queuePrompt((*string - 'A') + PROMPT_A_PHONETIC);
            else
                vp_queuePrompt(*string - 'A' + PROMPT_A);
        }
        else if ((*string >= 'a') && (*string <= 'z'))
        {
            if (flags & vpAnnouncePhoneticRendering)
                vp_queuePrompt((*string - 'a') + PROMPT_A_PHONETIC);
            else
                vp_queuePrompt(*string - 'a' + PROMPT_A);
        }
        else if ((*string == ' ') && (flags & vpAnnounceSpace))
        {
            vp_queuePrompt(PROMPT_SPACE);
        }
        else if (GetSymbolVPIfItShouldBeAnnounced(*string, flags, &vp))
        {
            if (vp != PROMPT_SILENCE)
                vp_queuePrompt(vp);
            else
            {
                // announce ASCII
                int32_t val = *string;
                vp_queuePrompt(PROMPT_CHARACTER);
                vp_queueInteger(val);
            }
        }
        else
        {
            // otherwise just add silence
            vp_queuePrompt(PROMPT_SILENCE);
        }

        string++;
    }

    if (flags & vpqAddSeparatingSilence)
        vp_queuePrompt(PROMPT_SILENCE);
}

void vp_queueInteger(const int value)
{
    if (state.settings.vpLevel < vpLow)
        return;

    char buf[12] = {0};  // min: -2147483648, max: 2147483647
    snprintf(buf, 12, "%d", value);
    vp_queueString(buf, 0);
}

void vp_queueStringTableEntry(const char* const* stringTableStringPtr)
{
    /*
     * This function looks up a voice prompt corresponding to a string table
     * entry. These are stored in the voice data after the voice prompts with no
     * corresponding string table entry, hence the offset calculation:
     * NUM_VOICE_PROMPTS + (stringTableStringPtr - currentLanguage->languageName)
     */

    if (state.settings.vpLevel < vpLow)
        return;

    if (stringTableStringPtr == NULL)
        return;

    uint16_t pos = NUM_VOICE_PROMPTS
                 + (stringTableStringPtr - &currentLanguage->languageName);

    vp_queuePrompt(pos);
}

void vp_play()
{
    if (state.settings.vpLevel < vpLow)
        return;

    if (voicePromptActive)
        return;

    if (vpCurrentSequence.length <= 0)
        return;

    voicePromptActive = true;

    codec_startDecode(SINK_SPK);
    audio_enableAmp();
}

void vp_tick()
{
    if (voicePromptActive == false)
        return;

    while(vpCurrentSequence.pos < vpCurrentSequence.length)
    {
        // get the codec2 data for the current prompt if needed.
        if (vpCurrentSequence.c2DataLength == 0)
        {
            // obtain the data for the prompt.
            int promptNumber = vpCurrentSequence.buffer[vpCurrentSequence.pos];

            vpCurrentSequence.c2DataIndex  = 0;
            vpCurrentSequence.c2DataStart  = tableOfContents[promptNumber];
            vpCurrentSequence.c2DataLength = tableOfContents[promptNumber + 1]
                                           - tableOfContents[promptNumber];
        }

        while (vpCurrentSequence.c2DataIndex < vpCurrentSequence.c2DataLength)
        {
            // push the codec2 data in lots of 8 byte frames.
            uint8_t c2Frame[8] = {0};

            fetchCodec2Data(c2Frame, vpCurrentSequence.c2DataStart +
                                     vpCurrentSequence.c2DataIndex);

            if (!codec_pushFrame(c2Frame, false))
                return;

            vpCurrentSequence.c2DataIndex += 8;
        }

        vpCurrentSequence.pos++;            // ready for next prompt in sequence.
        vpCurrentSequence.c2DataLength = 0; // flag that we need to get more data.
        vpCurrentSequence.c2DataIndex  = 0;
    }

    // see if we've finished.
    if(vpCurrentSequence.pos == vpCurrentSequence.length)
    {
        voicePromptActive     = false;
        vpCurrentSequence.pos = 0;
    }
}

bool vp_isPlaying()
{
    return voicePromptActive;
}

bool vp_sequenceNotEmpty()
{
    return (vpCurrentSequence.length > 0);
}
