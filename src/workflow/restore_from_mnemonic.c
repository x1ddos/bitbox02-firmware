// Copyright 2019 Shift Cryptosecurity AG
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "restore_from_mnemonic.h"

#include "blocking.h"
#include "confirm.h"
#include "password.h"
#include "status.h"
#include "trinary_input.h"
#include "unlock_bip39.h"

#include <hardfault.h>
#include <keystore.h>
#include <memory/memory.h>
#include <securechip/securechip.h>
#include <ui/component.h>
#include <ui/components/trinary_choice.h>
#include <ui/components/trinary_input_string.h>
#include <ui/screen_stack.h>
#include <ui/ui_util.h>
#include <util.h>
#include <workflow/confirm_time.h>

#include <wally_bip39.h> // for BIP39_WORDLIST_LEN

#include <stdio.h>
#include <string.h>

#define WORKFLOW_RESTORE_FROM_MNEMONIC_MAX_WORDS 24

static trinary_choice_t _number_of_words_choice;
static void _number_of_words_picked(component_t* trinary_choice, trinary_choice_t choice)
{
    (void)trinary_choice;
    _number_of_words_choice = choice;
    workflow_blocking_unblock();
}

/**
 * Workflow to pick how many words.
 * @param[out] number_of_words_out 12, 18 or 24.
 */
static void _pick_number_of_words(uint8_t* number_of_words_out)
{
    ui_screen_stack_push(
        trinary_choice_create("How many words?", "12", "18", "24", _number_of_words_picked, NULL));
    workflow_blocking_block();
    ui_screen_stack_pop();
    switch (_number_of_words_choice) {
    case TRINARY_CHOICE_LEFT:
        *number_of_words_out = 12;
        break;
    case TRINARY_CHOICE_MIDDLE:
        *number_of_words_out = 18;
        break;
    case TRINARY_CHOICE_RIGHT:
        *number_of_words_out = 24;
        break;
    default:
        Abort("restore_from_mnemonic: unreachable");
    }
}

static void _cleanup_wordlist(char*** wordlist)
{
    for (size_t i = 0; i < BIP39_WORDLIST_LEN; i++) {
        if ((*wordlist)[i] != NULL) {
            free((*wordlist)[i]);
            (*wordlist)[i] = NULL;
        }
    }
}

static bool _get_mnemonic(char* mnemonic_out)
{
    char* wordlist[BIP39_WORDLIST_LEN] = {0};
    char** __attribute__((__cleanup__(_cleanup_wordlist))) __attribute__((unused)) wordlist_clean =
        wordlist;
    for (size_t i = 0; i < BIP39_WORDLIST_LEN; i++) {
        if (!keystore_get_bip39_word(i, &wordlist[i])) {
            return false;
        }
    }

    uint8_t num_words;
    _pick_number_of_words(&num_words);
    char num_words_success_msg[20];
    snprintf(num_words_success_msg, sizeof(num_words_success_msg), "Enter %d words", num_words);
    workflow_status_blocking(num_words_success_msg, true);

    char words[WORKFLOW_RESTORE_FROM_MNEMONIC_MAX_WORDS]
              [WORKFLOW_TRINARY_INPUT_MAX_WORD_LENGTH + 1] = {0};

    uint8_t word_idx = 0;
    while (word_idx < num_words) {
        // This is at the same time the preset (word already filled out) if it is not empty, and
        // also the result of the user input.
        // This allows the user the edit the previous word
        // (delete one, the previous word is already preset).
        char* word = words[word_idx];

        workflow_trinary_input_result_t result = workflow_trinary_input_wordlist(
            word_idx,
            (const char* const*)wordlist,
            BIP39_WORDLIST_LEN,
            strlen(word) ? word : NULL,
            word);
        if (result == WORKFLOW_TRINARY_INPUT_RESULT_CANCEL) {
            return false;
        }
        if (result == WORKFLOW_TRINARY_INPUT_RESULT_DELETE) {
            if (word_idx > 0) {
                word_idx--;
            }
            continue;
        }
        word_idx++;
    }
    for (word_idx = 0; word_idx < num_words; word_idx++) {
        if (word_idx != 0) {
            strcat(mnemonic_out, " "); // NOLINT (gcc and clang cannot agree on best practice here)
        }
        strncat(mnemonic_out, words[word_idx], WORKFLOW_TRINARY_INPUT_MAX_WORD_LENGTH);
    }
    return true;
}

bool workflow_restore_from_mnemonic(const RestoreFromMnemonicRequest* request)
{
    // same as: MAX_WORD_LENGTH * MAX_WORDS + (MAX_WORDS - 1) + 1
    // (chars per word without null terminator) * (max words) + (spaces between words) + (null
    // terminator)
    char mnemonic
        [(WORKFLOW_TRINARY_INPUT_MAX_WORD_LENGTH + 1) * WORKFLOW_RESTORE_FROM_MNEMONIC_MAX_WORDS] =
            {0};
    UTIL_CLEANUP_STR(mnemonic);
    if (!_get_mnemonic(mnemonic)) {
        return false;
    }
    uint8_t seed[32];
    UTIL_CLEANUP_32(seed);
    size_t seed_len = 0;
    if (!keystore_bip39_mnemonic_to_seed(mnemonic, seed, &seed_len)) {
        workflow_status_blocking("Recovery words\ninvalid", false);
        return false;
    }

    workflow_status_blocking("Recovery words\nvalid", true);

    char password[SET_PASSWORD_MAX_PASSWORD_LENGTH] = {0};
    UTIL_CLEANUP_STR(password);
    // If entering password fails (repeat password does not match the first), we don't want to abort
    // the process immediately. We break out only if the user confirms.
    while (true) {
        if (!password_set(password)) {
            const confirm_params_t params = {
                .title = "",
                .body = "Passwords\ndo not match.\nTry again?",
            };

            if (!workflow_confirm_blocking(&params)) {
                return false;
            }
            continue;
        }
        break;
    }
    if (!keystore_encrypt_and_store_seed(seed, seed_len, password)) {
        workflow_status_blocking("Could not\nrestore backup", false);
        return false;
    }
#if APP_U2F == 1
    if (!workflow_confirm_time(request->timestamp, request->timezone_offset, false)) {
        return false;
    }
    if (!securechip_u2f_counter_set(request->timestamp)) {
        // ignore error
    }
#else
    (void)request;
#endif
    if (!memory_set_initialized()) {
        return false;
    }
    uint8_t remaining_attempts;
    if (keystore_unlock(password, &remaining_attempts) != KEYSTORE_OK) {
        // This should/can never happen, but let's check anyway.
        Abort("workflow_restore_from_mnemonic: unlock failed");
    }
    workflow_unlock_bip39_blocking();
    return true;
}
