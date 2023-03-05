/*
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Lzma.h>

namespace Compress {

u32 LzmaHeader::dictionary_size() const
{
    // "If the value of dictionary size in properties is smaller than (1 << 12),
    //  the LZMA decoder must set the dictionary size variable to (1 << 12)."
    constexpr u32 minimum_dictionary_size = (1 << 12);
    if (m_dictionary_size < minimum_dictionary_size)
        return minimum_dictionary_size;

    return m_dictionary_size;
}

Optional<u64> LzmaHeader::uncompressed_size() const
{
    // We are making a copy of the packed field here because we would otherwise
    // pass an unaligned reference to the constructor of Optional, which is
    // undefined behavior.
    auto uncompressed_size = m_uncompressed_size;

    // "If "Uncompressed size" field contains ones in all 64 bits, it means that
    //  uncompressed size is unknown and there is the "end marker" in stream,
    //  that indicates the end of decoding point."
    if (uncompressed_size == UINT64_MAX)
        return {};

    // "In opposite case, if the value from "Uncompressed size" field is not
    //  equal to ((2^64) - 1), the LZMA stream decoding must be finished after
    //  specified number of bytes (Uncompressed size) is decoded. And if there
    //  is the "end marker", the LZMA decoder must read that marker also."
    return uncompressed_size;
}

ErrorOr<void> LzmaHeader::decode_model_properties(u8& literal_context_bits, u8& literal_pos_bits, u8& pos_bits) const
{
    // "Decodes the following values from the encoded model properties field:
    //
    //     name  Range          Description
    //       lc  [0, 8]         the number of "literal context" bits
    //       lp  [0, 4]         the number of "literal pos" bits
    //       pb  [0, 4]         the number of "pos" bits
    //
    //  Encoded using `((pb * 5 + lp) * 9 + lc)`."

    u8 input_bits = m_model_properties;

    if (input_bits >= (9 * 5 * 5))
        return Error::from_string_literal("Encoded model properties value is larger than the highest possible value");

    literal_context_bits = input_bits % 9;
    input_bits /= 9;

    literal_pos_bits = input_bits % 5;
    input_bits /= 5;

    pos_bits = input_bits;

    return {};
}

ErrorOr<LzmaDecompressorOptions> LzmaHeader::as_decompressor_options() const
{
    u8 literal_context_bits { 0 };
    u8 literal_position_bits { 0 };
    u8 position_bits { 0 };

    TRY(decode_model_properties(literal_context_bits, literal_position_bits, position_bits));

    return LzmaDecompressorOptions {
        .literal_context_bits = literal_context_bits,
        .literal_position_bits = literal_position_bits,
        .position_bits = position_bits,
        .dictionary_size = dictionary_size(),
        .uncompressed_size = uncompressed_size(),
    };
}

void LzmaDecompressor::initialize_to_default_probability(Span<Probability> span)
{
    for (auto& entry : span)
        entry = default_probability;
}

ErrorOr<NonnullOwnPtr<LzmaDecompressor>> LzmaDecompressor::create_from_container(MaybeOwned<Stream> stream)
{
    auto header = TRY(stream->read_value<LzmaHeader>());

    return TRY(LzmaDecompressor::create_from_raw_stream(move(stream), TRY(header.as_decompressor_options())));
}

ErrorOr<NonnullOwnPtr<LzmaDecompressor>> LzmaDecompressor::create_from_raw_stream(MaybeOwned<Stream> stream, LzmaDecompressorOptions const& options)
{
    // "The LZMA Encoder always writes ZERO in initial byte of compressed stream.
    //  That scheme allows to simplify the code of the Range Encoder in the
    //  LZMA Encoder. If initial byte is not equal to ZERO, the LZMA Decoder must
    //  stop decoding and report error."
    {
        auto byte = TRY(stream->read_value<u8>());
        if (byte != 0)
            return Error::from_string_literal("Initial byte of data stream is not zero");
    }

    auto output_buffer = TRY(CircularBuffer::create_empty(options.dictionary_size));

    // "The LZMA Decoder uses (1 << (lc + lp)) tables with CProb values, where each table contains 0x300 CProb values."
    auto literal_probabilities = TRY(FixedArray<Probability>::create(literal_probability_table_size * (1 << (options.literal_context_bits + options.literal_position_bits))));

    auto decompressor = TRY(adopt_nonnull_own_or_enomem(new (nothrow) LzmaDecompressor(move(stream), options, move(output_buffer), move(literal_probabilities))));

    // Read the initial bytes into the range decoder.
    for (size_t i = 0; i < 4; i++) {
        auto byte = TRY(decompressor->m_stream->read_value<u8>());
        decompressor->m_range_decoder_code = decompressor->m_range_decoder_code << 8 | byte;
    }

    return decompressor;
}

LzmaDecompressor::LzmaDecompressor(MaybeOwned<Stream> stream, LzmaDecompressorOptions options, CircularBuffer output_buffer, FixedArray<Probability> literal_probabilities)
    : m_stream(move(stream))
    , m_options(move(options))
    , m_output_buffer(move(output_buffer))
    , m_literal_probabilities(move(literal_probabilities))
{
    initialize_to_default_probability(m_literal_probabilities.span());

    for (auto& array : m_length_to_position_states)
        initialize_to_default_probability(array);

    for (auto& array : m_binary_tree_distance_probabilities)
        initialize_to_default_probability(array);

    initialize_to_default_probability(m_alignment_bit_probabilities);

    initialize_to_default_probability(m_is_match_probabilities);
    initialize_to_default_probability(m_is_rep_probabilities);
    initialize_to_default_probability(m_is_rep_g0_probabilities);
    initialize_to_default_probability(m_is_rep_g1_probabilities);
    initialize_to_default_probability(m_is_rep_g2_probabilities);
    initialize_to_default_probability(m_is_rep0_long_probabilities);
}

ErrorOr<void> LzmaDecompressor::normalize_range_decoder()
{
    // "The value of the "Range" variable before each bit decoding can not be smaller
    //  than ((UInt32)1 << 24). The Normalize() function keeps the "Range" value in
    //  described range."
    constexpr u32 minimum_range_value = 1 << 24;

    if (m_range_decoder_range >= minimum_range_value)
        return {};

    m_range_decoder_range <<= 8;
    m_range_decoder_code <<= 8;

    m_range_decoder_code |= TRY(m_stream->read_value<u8>());

    VERIFY(m_range_decoder_range >= minimum_range_value);

    return {};
}

ErrorOr<u8> LzmaDecompressor::decode_direct_bit()
{
    m_range_decoder_range >>= 1;
    m_range_decoder_code -= m_range_decoder_range;

    u32 temp = 0 - (m_range_decoder_code >> 31);

    m_range_decoder_code += m_range_decoder_range & temp;

    if (m_range_decoder_code == m_range_decoder_range)
        return Error::from_string_literal("Reached an invalid state while decoding LZMA stream");

    TRY(normalize_range_decoder());

    return temp + 1;
}

ErrorOr<u8> LzmaDecompressor::decode_bit_with_probability(Probability& probability)
{
    // "The LZMA decoder provides the pointer to CProb variable that contains
    //  information about estimated probability for symbol 0 and the Range Decoder
    //  updates that CProb variable after decoding."

    // The significance of the shift width is not explained and appears to be a magic constant.
    constexpr size_t probability_shift_width = 5;

    u32 bound = (m_range_decoder_range >> probability_bit_count) * probability;

    if (m_range_decoder_code < bound) {
        probability += ((1 << probability_bit_count) - probability) >> probability_shift_width;
        m_range_decoder_range = bound;
        TRY(normalize_range_decoder());
        return 0;
    } else {
        probability -= probability >> probability_shift_width;
        m_range_decoder_code -= bound;
        m_range_decoder_range -= bound;
        TRY(normalize_range_decoder());
        return 1;
    }
}

ErrorOr<u16> LzmaDecompressor::decode_symbol_using_bit_tree(size_t bit_count, Span<Probability> probability_tree)
{
    VERIFY(bit_count <= sizeof(u16) * 8);
    VERIFY(probability_tree.size() >= 1ul << bit_count);

    // This has been modified from the reference implementation to unlink the result and the tree index,
    // which should allow for better readability.

    u16 result = 0;
    size_t tree_index = 1;

    for (size_t i = 0; i < bit_count; i++) {
        u16 next_bit = TRY(decode_bit_with_probability(probability_tree[tree_index]));
        result = (result << 1) | next_bit;
        tree_index = (tree_index << 1) | next_bit;
    }

    return result;
}

ErrorOr<u16> LzmaDecompressor::decode_symbol_using_reverse_bit_tree(size_t bit_count, Span<Probability> probability_tree)
{
    VERIFY(bit_count <= sizeof(u16) * 8);
    VERIFY(probability_tree.size() >= 1ul << bit_count);

    u16 result = 0;
    size_t tree_index = 1;

    for (size_t i = 0; i < bit_count; i++) {
        u16 next_bit = TRY(decode_bit_with_probability(probability_tree[tree_index]));
        result |= next_bit << i;
        tree_index = (tree_index << 1) | next_bit;
    }

    return result;
}

ErrorOr<void> LzmaDecompressor::decode_literal_to_output_buffer()
{
    u8 previous_byte = 0;
    if (m_total_decoded_bytes > 0) {
        auto read_bytes = MUST(m_output_buffer.read_with_seekback({ &previous_byte, sizeof(previous_byte) }, 1));
        VERIFY(read_bytes.size() == sizeof(previous_byte));
    }

    // "To select the table for decoding it uses the context that consists of
    //  (lc) high bits from previous literal and (lp) low bits from value that
    //  represents current position in outputStream."
    u16 literal_state_bits_from_position = m_total_decoded_bytes & ((1 << m_options.literal_position_bits) - 1);
    u16 literal_state_bits_from_output = previous_byte >> (8 - m_options.literal_context_bits);
    u16 literal_state = literal_state_bits_from_position << m_options.literal_context_bits | literal_state_bits_from_output;

    Span<Probability> selected_probability_table = m_literal_probabilities.span().slice(literal_probability_table_size * literal_state, literal_probability_table_size);

    // The result is defined as u16 here and initialized to 1, but we will cut off the top bits before queueing them into the output buffer.
    // The top bit is only used to track how much we have decoded already, and to select the correct probability table.
    u16 result = 1;

    // "If (State > 7), the Literal Decoder also uses "matchByte" that represents
    //  the byte in OutputStream at position the is the DISTANCE bytes before
    //  current position, where the DISTANCE is the distance in DISTANCE-LENGTH pair
    //  of latest decoded match."
    // Note: The specification says `(State > 7)`, but the reference implementation does `(State >= 7)`, which is a mismatch.
    //       Testing `(State > 7)` with actual test files yields errors, so the reference implementation appears to be the correct one.
    if (m_state >= 7) {
        u8 matched_byte = 0;
        auto read_bytes = TRY(m_output_buffer.read_with_seekback({ &matched_byte, sizeof(matched_byte) }, m_rep0 + 1));
        VERIFY(read_bytes.size() == sizeof(matched_byte));

        do {
            u8 match_bit = (matched_byte >> 7) & 1;
            matched_byte <<= 1;

            u8 decoded_bit = TRY(decode_bit_with_probability(selected_probability_table[((1 + match_bit) << 8) + result]));
            result = result << 1 | decoded_bit;

            if (match_bit != decoded_bit)
                break;
        } while (result < 0x100);
    }

    while (result < 0x100)
        result = (result << 1) | TRY(decode_bit_with_probability(selected_probability_table[result]));

    u8 actual_result = result - 0x100;

    size_t written_bytes = m_output_buffer.write({ &actual_result, sizeof(actual_result) });
    VERIFY(written_bytes == sizeof(actual_result));
    m_total_decoded_bytes += sizeof(actual_result);

    return {};
}

LzmaDecompressor::LzmaLengthDecoderState::LzmaLengthDecoderState()
{
    for (auto& array : m_low_length_probabilities)
        initialize_to_default_probability(array);

    for (auto& array : m_medium_length_probabilities)
        initialize_to_default_probability(array);

    initialize_to_default_probability(m_high_length_probabilities);
}

ErrorOr<u16> LzmaDecompressor::decode_normalized_match_length(LzmaLengthDecoderState& length_decoder_state)
{
    // "LZMA uses "posState" value as context to select the binary tree
    //  from LowCoder and MidCoder binary tree arrays:"
    u16 position_state = m_total_decoded_bytes & ((1 << m_options.position_bits) - 1);

    // "The following scheme is used for the match length encoding:
    //
    //   Binary encoding    Binary Tree structure    Zero-based match length
    //   sequence                                    (binary + decimal):
    //
    //   0 xxx              LowCoder[posState]       xxx
    if (TRY(decode_bit_with_probability(length_decoder_state.m_first_choice_probability)) == 0)
        return TRY(decode_symbol_using_bit_tree(3, length_decoder_state.m_low_length_probabilities[position_state].span()));

    //   1 0 yyy            MidCoder[posState]       yyy + 8
    if (TRY(decode_bit_with_probability(length_decoder_state.m_second_choice_probability)) == 0)
        return TRY(decode_symbol_using_bit_tree(3, length_decoder_state.m_medium_length_probabilities[position_state].span())) + 8;

    //   1 1 zzzzzzzz       HighCoder                zzzzzzzz + 16"
    return TRY(decode_symbol_using_bit_tree(8, length_decoder_state.m_high_length_probabilities.span())) + 16;
}

ErrorOr<u32> LzmaDecompressor::decode_normalized_match_distance(u16 normalized_match_length)
{
    // "LZMA uses normalized match length (zero-based length)
    //  to calculate the context state "lenState" do decode the distance value."
    u16 length_state = min(normalized_match_length, number_of_length_to_position_states - 1);

    // "At first stage the distance decoder decodes 6-bit "posSlot" value with bit
    //  tree decoder from PosSlotDecoder array."
    u16 position_slot = TRY(decode_symbol_using_bit_tree(6, m_length_to_position_states[length_state].span()));

    // "The encoding scheme for distance value is shown in the following table:
    //
    //  posSlot (decimal) /
    //       zero-based distance (binary)
    //  0    0
    //  1    1
    //  2    10
    //  3    11
    //
    //  4    10 x
    //  5    11 x
    //  6    10 xx
    //  7    11 xx
    //  8    10 xxx
    //  9    11 xxx
    //  10    10 xxxx
    //  11    11 xxxx
    //  12    10 xxxxx
    //  13    11 xxxxx
    //
    //  14    10 yy zzzz
    //  15    11 yy zzzz
    //  16    10 yyy zzzz
    //  17    11 yyy zzzz
    //  ...
    //  62    10 yyyyyyyyyyyyyyyyyyyyyyyyyy zzzz
    //  63    11 yyyyyyyyyyyyyyyyyyyyyyyyyy zzzz
    //
    //  where
    //   "x ... x" means the sequence of binary symbols encoded with binary tree and
    //       "Reverse" scheme. It uses separated binary tree for each posSlot from 4 to 13.
    //   "y" means direct bit encoded with range coder.
    //   "zzzz" means the sequence of four binary symbols encoded with binary
    //       tree with "Reverse" scheme, where one common binary tree "AlignDecoder"
    //       is used for all posSlot values."

    // "If (posSlot < 4), the "dist" value is equal to posSlot value."
    if (position_slot < first_position_slot_with_binary_tree_bits)
        return position_slot;

    // From here on, the first bit of the distance is always set and the second bit is set if the last bit of the position slot is set.
    u32 distance_prefix = ((1 << 1) | ((position_slot & 1) << 0));

    // "If (posSlot >= 4), the decoder uses "posSlot" value to calculate the value of
    //   the high bits of "dist" value and the number of the low bits.
    //   If (4 <= posSlot < kEndPosModelIndex), the decoder uses bit tree decoders.
    //     (one separated bit tree decoder per one posSlot value) and "Reverse" scheme."
    if (position_slot < first_position_slot_with_direct_encoded_bits) {
        size_t number_of_bits_to_decode = (position_slot / 2) - 1;
        auto& selected_probability_tree = m_binary_tree_distance_probabilities[position_slot - first_position_slot_with_binary_tree_bits];
        return (distance_prefix << number_of_bits_to_decode) | TRY(decode_symbol_using_reverse_bit_tree(number_of_bits_to_decode, selected_probability_tree));
    }

    // "  if (posSlot >= kEndPosModelIndex), the middle bits are decoded as direct
    //     bits from RangeDecoder and the low 4 bits are decoded with a bit tree
    //     decoder "AlignDecoder" with "Reverse" scheme."
    size_t number_of_direct_bits_to_decode = ((position_slot - first_position_slot_with_direct_encoded_bits) / 2) + 2;
    for (size_t i = 0; i < number_of_direct_bits_to_decode; i++) {
        distance_prefix = (distance_prefix << 1) | TRY(decode_direct_bit());
    }
    return (distance_prefix << number_of_alignment_bits) | TRY(decode_symbol_using_reverse_bit_tree(number_of_alignment_bits, m_alignment_bit_probabilities));
}

ErrorOr<Bytes> LzmaDecompressor::read_some(Bytes bytes)
{
    while (m_output_buffer.used_space() < bytes.size() && m_output_buffer.empty_space() != 0) {
        if (m_found_end_of_stream_marker) {
            if (m_options.uncompressed_size.has_value() && m_total_decoded_bytes < m_options.uncompressed_size.value())
                return Error::from_string_literal("Found end-of-stream marker earlier than expected");

            break;
        }

        if (m_options.uncompressed_size.has_value() && m_total_decoded_bytes >= m_options.uncompressed_size.value()) {
            // FIXME: This should validate that either EOF or the 'end of stream' marker follow immediately.
            //        Both of those cases count as the 'end of stream' marker being found and should check for a clean decoder state.
            break;
        }

        // "The decoder calculates "state2" variable value to select exact variable from
        //  "IsMatch" and "IsRep0Long" arrays."
        u16 position_state = m_total_decoded_bytes & ((1 << m_options.position_bits) - 1);
        u16 state2 = (m_state << maximum_number_of_position_bits) + position_state;

        auto update_state_after_literal = [&] {
            if (m_state < 4)
                m_state = 0;
            else if (m_state < 10)
                m_state -= 3;
            else
                m_state -= 6;
        };

        auto update_state_after_match = [&] {
            if (m_state < 7)
                m_state = 7;
            else
                m_state = 10;
        };

        auto update_state_after_rep = [&] {
            if (m_state < 7)
                m_state = 8;
            else
                m_state = 11;
        };

        auto update_state_after_short_rep = [&] {
            if (m_state < 7)
                m_state = 9;
            else
                m_state = 11;
        };

        auto copy_match_to_buffer = [&](u16 real_length) -> ErrorOr<void> {
            VERIFY(!m_leftover_match_length.has_value());

            if (m_options.uncompressed_size.has_value() && m_options.uncompressed_size.value() < m_total_decoded_bytes + real_length)
                return Error::from_string_literal("Tried to copy match beyond expected uncompressed file size");

            while (real_length > 0) {
                if (m_output_buffer.empty_space() == 0) {
                    m_leftover_match_length = real_length;
                    break;
                }

                u8 byte;
                auto read_bytes = TRY(m_output_buffer.read_with_seekback({ &byte, sizeof(byte) }, m_rep0 + 1));
                VERIFY(read_bytes.size() == sizeof(byte));

                auto written_bytes = m_output_buffer.write({ &byte, sizeof(byte) });
                VERIFY(written_bytes == sizeof(byte));
                m_total_decoded_bytes++;

                real_length--;
            }

            return {};
        };

        // If we have a leftover part of a repeating match, we should finish that first.
        if (m_leftover_match_length.has_value()) {
            TRY(copy_match_to_buffer(m_leftover_match_length.release_value()));
            continue;
        }

        // "The decoder uses the following code flow scheme to select exact
        //  type of LITERAL or MATCH:
        //
        //  IsMatch[state2] decode
        //   0 - the Literal"
        if (TRY(decode_bit_with_probability(m_is_match_probabilities[state2])) == 0) {
            // "At first the LZMA decoder must check that it doesn't exceed
            //  specified uncompressed size."
            // This is already checked for at the beginning of the loop.

            // "Then it decodes literal value and puts it to sliding window."
            TRY(decode_literal_to_output_buffer());

            // "Then the decoder must update the "state" value."
            update_state_after_literal();
            continue;
        }

        // " 1 - the Match
        //     IsRep[state] decode
        //       0 - Simple Match"
        if (TRY(decode_bit_with_probability(m_is_rep_probabilities[m_state])) == 0) {
            // "The distance history table is updated with the following scheme:"
            m_rep3 = m_rep2;
            m_rep2 = m_rep1;
            m_rep1 = m_rep0;

            // "The zero-based length is decoded with "LenDecoder"."
            u16 normalized_length = TRY(decode_normalized_match_length(m_length_decoder));

            // "The state is update with UpdateState_Match function."
            update_state_after_match();

            // "and the new "rep0" value is decoded with DecodeDistance."
            m_rep0 = TRY(decode_normalized_match_distance(normalized_length));

            // "If the value of "rep0" is equal to 0xFFFFFFFF, it means that we have
            //  "End of stream" marker, so we can stop decoding and check finishing
            //  condition in Range Decoder"
            if (m_rep0 == 0xFFFFFFFF) {
                // The range decoder condition is checked after breaking out of the loop.
                m_found_end_of_stream_marker = true;
                continue;
            }

            // "If uncompressed size is defined, LZMA decoder must check that it doesn't
            //  exceed that specified uncompressed size."
            // This is being checked for in the common "copy to buffer" implementation.

            // "Also the decoder must check that "rep0" value is not larger than dictionary size
            //  and is not larger than the number of already decoded bytes."
            if (m_rep0 > min(m_options.dictionary_size, m_total_decoded_bytes))
                return Error::from_string_literal("rep0 value is larger than the possible lookback size");

            // "Then the decoder must copy match bytes as described in
            //  "The match symbols copying" section."
            TRY(copy_match_to_buffer(normalized_length + normalized_to_real_match_length_offset));

            continue;
        }

        // "     1 - Rep Match
        //         IsRepG0[state] decode
        //           0 - the distance is rep0"
        if (TRY(decode_bit_with_probability(m_is_rep_g0_probabilities[m_state])) == 0) {
            // "LZMA doesn't update the distance history."

            // "       IsRep0Long[state2] decode
            //           0 - Short Rep Match"
            if (TRY(decode_bit_with_probability(m_is_rep0_long_probabilities[state2])) == 0) {
                // "If the subtype is "Short Rep Match", the decoder updates the state, puts
                //  the one byte from window to current position in window and goes to next
                //  MATCH/LITERAL symbol."
                update_state_after_short_rep();

                u8 byte;
                auto read_bytes = TRY(m_output_buffer.read_with_seekback({ &byte, sizeof(byte) }, m_rep0 + 1));
                VERIFY(read_bytes.size() == sizeof(byte));

                auto written_bytes = m_output_buffer.write({ &byte, sizeof(byte) });
                VERIFY(written_bytes == sizeof(byte));
                m_total_decoded_bytes++;

                continue;
            }
            // "         1 - Rep Match 0"
            // Intentional fallthrough, we just need to make sure to not run the detection for other match types and to not switch around the distance history.
        } else {
            // "     1 -
            //         IsRepG1[state] decode
            //           0 - Rep Match 1"
            if (TRY(decode_bit_with_probability(m_is_rep_g1_probabilities[m_state])) == 0) {
                u32 distance = m_rep1;
                m_rep1 = m_rep0;
                m_rep0 = distance;
            }

            // "         1 -
            //             IsRepG2[state] decode
            //               0 - Rep Match 2"
            else if (TRY(decode_bit_with_probability(m_is_rep_g2_probabilities[m_state])) == 0) {
                u32 distance = m_rep2;
                m_rep2 = m_rep1;
                m_rep1 = m_rep0;
                m_rep0 = distance;
            }

            // "             1 - Rep Match 3"
            else {
                u32 distance = m_rep3;
                m_rep3 = m_rep2;
                m_rep2 = m_rep1;
                m_rep1 = m_rep0;
                m_rep0 = distance;
            }
        }

        // "In other cases (Rep Match 0/1/2/3), it decodes the zero-based
        //  length of match with "RepLenDecoder" decoder."
        u16 normalized_length = TRY(decode_normalized_match_length(m_rep_length_decoder));

        // "Then it updates the state."
        update_state_after_rep();

        // "Then the decoder must copy match bytes as described in
        //  "The Match symbols copying" section."
        TRY(copy_match_to_buffer(normalized_length + normalized_to_real_match_length_offset));
    }

    if (m_found_end_of_stream_marker) {
        if (m_range_decoder_code != 0)
            return Error::from_string_literal("LZMA stream ends in an unclean state");
    }

    return m_output_buffer.read(bytes);
}

ErrorOr<size_t> LzmaDecompressor::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

bool LzmaDecompressor::is_eof() const
{
    if (m_output_buffer.used_space() > 0)
        return false;

    if (m_options.uncompressed_size.has_value())
        return m_total_decoded_bytes >= m_options.uncompressed_size.value();

    return m_found_end_of_stream_marker;
}

bool LzmaDecompressor::is_open() const
{
    return true;
}

void LzmaDecompressor::close()
{
}

}
